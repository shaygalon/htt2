/*
 * Copyright (c) 2011-2012 Yaroslav Stavnichiy <yarosla@gmail.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <assert.h>
#include <malloc.h>
#include <string.h>
#include <time.h>
#include <stdarg.h>
#include <signal.h>
#include <pthread.h>
#include <sys/stat.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <netdb.h>
#include <math.h>

//#define WITH_SSL
static int has_fastopen=1;

#ifdef WITH_SSL
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>
#endif

#include <ev.h>
static int first=9;
#define VERSION "1.1"

#if (__SIZEOF_POINTER__==8)
typedef uint64_t int_to_ptr;
#else
typedef uint32_t int_to_ptr;
#endif

#define MEM_GUARD 128
#define MAX_URLS 1024
#define MAX_SESSIONS 128
#define MAX_REQ_SIZE 4096

time_t start_time_rg;
time_t current_time;

struct config {
  int num_connections;
  int num_requests;
  int num_threads;
  int progress_step;
  struct addrinfo *saddr;
  const char* uri_path;
  const char* uri_host;
  const char* ssl_cipher_priority;
  char request_data[MAX_REQ_SIZE];
  int request_length;
  char *request_data_arr[MAX_URLS];
  int request_length_arr[MAX_URLS];
  int num_urls;
  int sessions[MAX_SESSIONS];
  const char* session_host[MAX_SESSIONS];
  struct addrinfo *session_saddr[MAX_SESSIONS];
  int last_session;
  int infinite;
  int run_time;
#ifdef WITH_SSL
  gnutls_certificate_credentials_t ssl_cred;
  gnutls_priority_t priority_cache;
#endif

  char _padding0[MEM_GUARD]; // guard from false sharing
  int keep_alive;
  int secure;
  int quiet;

  char _padding1[MEM_GUARD]; // guard from false sharing
  volatile int request_counter;
  char _padding2[MEM_GUARD];
};

static struct config config;
static char host_string[512];

enum nxweb_chunked_decoder_state_code {CDS_CR1=-2, CDS_LF1=-1, CDS_SIZE=0, CDS_LF2, CDS_DATA};

typedef struct nxweb_chunked_decoder_state {
  enum nxweb_chunked_decoder_state_code state;
  unsigned short final_chunk:1;
  unsigned short monitor_only:1;
  int64_t chunk_bytes_left;
} nxweb_chunked_decoder_state;

enum connection_state {C_CONNECTING, C_HANDSHAKING, C_WRITING, C_READING_HEADERS, C_READING_BODY};

typedef struct connection {
  struct ev_loop* loop;
  struct thread_config* tdata;
  int fd;
  ev_io watch_read;
  ev_io watch_write;
  ev_tstamp last_activity;

  nxweb_chunked_decoder_state cdstate;

#ifdef WITH_SSL
  gnutls_session_t session;
#endif

  int write_pos;
  int read_pos;
  int bytes_to_read;
  int bytes_received;
  int alive_count;
  int success_count;

  int keep_alive:1;
  int chunked:1;
  int done:1;
  int secure:1;

  char buf[32768];
  char* body_ptr;

  enum connection_state state;
  struct addrinfo *saddr;
  const char* uri_path;
  const char* uri_host;
  int session_id;
  int num_urls;
  char **urls;
  int *request_length_arr;
} connection;

typedef struct thread_config {
  pthread_t tid;
  connection *conns;
  int id;
  int num_conn;
  struct ev_loop* loop;
  ev_tstamp start_time;
  ev_timer watch_heartbeat;

  int shutdown_in_progress;

  int num_success;
  int num_fail;
  long num_bytes_received;
  long num_overhead_received;
  int num_connect;
  ev_tstamp avg_req_time;

#ifdef WITH_SSL
  _Bool ssl_identified;
  _Bool ssl_dhe;
  _Bool ssl_ecdh;
  gnutls_kx_algorithm_t ssl_kx;
  gnutls_credentials_type_t ssl_cred;
  int ssl_dh_prime_bits;
  gnutls_ecc_curve_t ssl_ecc_curve;
  gnutls_protocol_t ssl_protocol;
  gnutls_certificate_type_t ssl_cert_type;
  gnutls_x509_crt_t ssl_cert;
  gnutls_compression_method_t ssl_compression;
  gnutls_cipher_algorithm_t ssl_cipher;
  gnutls_mac_algorithm_t ssl_mac;
#endif
} thread_config;

static int print_all_cpu_stats=0;
static volatile int stop_cpu_stats;
typedef struct cpu_info_s {
	long double max,min,avg;
	pthread_t tid;
} cpu_info_t;

void strip_newline(char *line) {
	while (*line) {
		if ((*line == '\n') || (*line == '\r'))
			*line=0;
		line++;
	}
}
int empty_line(char *line) {
	while (*line) {
		if ((*line != ' ') || (*line != '\t'))
			return 0;
		line++;
	}
	return 1;
}
void nxweb_die(const char* fmt, ...) {
  va_list ap;
  fprintf(stderr, "FATAL: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  exit(EXIT_FAILURE);
}

static inline const char* get_current_time(char* buf, int max_buf_size) {
  time_t t;
  struct tm tm;
  time(&t);
  localtime_r(&t, &tm);
  strftime(buf, max_buf_size, "%F %T", &tm); // %F=%Y-%m-%d %T=%H:%M:%S
  return buf;
}

void nxweb_log_error(const char* fmt, ...) {
  char cur_time[32];
  va_list ap;

  get_current_time(cur_time, sizeof(cur_time));
  flockfile(stderr);
  fprintf(stderr, "%s [%u:%p]: ", cur_time, getpid(), (void*)pthread_self());
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
  fflush(stderr);
  funlockfile(stderr);
}

static inline int setup_socket(int fd) {
  int flags=fcntl(fd, F_GETFL);
  if (flags<0) return flags;
  if (fcntl(fd, F_SETFL, flags|=O_NONBLOCK)<0) return -1;

  int nodelay=1;
  int qlen = 5;                            
  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay))) return -1;
  //try fastopen
  if (setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen))  != 0) {
	  has_fastopen=0;
	  nxweb_log_error("%s","Cannot set fastopen option on socket.\n");
  }

//  struct linger linger;
//  linger.l_onoff=1;
//  linger.l_linger=10; // timeout for completing reads/writes
//  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));

  return 0;
}

static inline void _nxweb_close_good_socket(int fd) {
//  struct linger linger;
//  linger.l_onoff=0; // gracefully shutdown connection
//  linger.l_linger=0;
//  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
//  shutdown(fd, SHUT_RDWR);
  close(fd);
}

static inline void _nxweb_close_bad_socket(int fd) {
  struct linger linger;
  linger.l_onoff=1;
  linger.l_linger=0; // timeout for completing writes
  setsockopt(fd, SOL_SOCKET, SO_LINGER, &linger, sizeof(linger));
  close(fd);
}

static inline void sleep_ms(int ms) {
  struct timespec req;
  time_t sec=ms/1000;
  ms%=1000;
  req.tv_sec=sec;
  req.tv_nsec=ms*1000000L;
  while(nanosleep(&req, &req)==-1) continue;
}

static inline void inc_success(connection* conn) {
  conn->success_count++;
  conn->tdata->num_success++;
  conn->tdata->num_bytes_received+=conn->bytes_received;
  conn->tdata->num_overhead_received+=(conn->body_ptr-conn->buf);
}

static inline void inc_fail(connection* conn) {
  conn->tdata->num_fail++;
}

static inline void inc_connect(connection* conn) {
  conn->tdata->num_connect++;
}

enum {ERR_AGAIN=-2, ERR_ERROR=-1, ERR_RDCLOSED=-3};

static inline ssize_t conn_read(connection* conn, void* buf, size_t size) {
#ifdef WITH_SSL
  if (conn->secure) {
    ssize_t ret=gnutls_record_recv(conn->session, buf, size);
    if (ret>0) return ret;
    if (ret==GNUTLS_E_AGAIN) return ERR_AGAIN;
    if (ret==0) return ERR_RDCLOSED;
    return ERR_ERROR;
  }
  else
#endif
  {
    ssize_t ret=read(conn->fd, buf, size);
    if (ret>0) return ret;
    if (ret==0) return ERR_RDCLOSED;
    if (errno==EAGAIN) return ERR_AGAIN;
    return ERR_ERROR;
  }
}

static inline ssize_t conn_write(connection* conn, void* buf, size_t size) {
#ifdef WITH_SSL
  if (conn->secure) {
    ssize_t ret=gnutls_record_send(conn->session, buf, size);
    if (ret>=0) return ret;
    if (ret==GNUTLS_E_AGAIN) return ERR_AGAIN;
    return ERR_ERROR;
  }
  else
#endif
  if (has_fastopen) {
    ssize_t ret=send(conn->fd, buf, size,0);
    if (ret>=0) return ret;
    if (errno==EAGAIN) return ERR_AGAIN;
    return ERR_ERROR;
  } else  {
    ssize_t ret=write(conn->fd, buf, size);
    if (ret>=0) return ret;
    if (errno==EAGAIN) return ERR_AGAIN;
    return ERR_ERROR;
  }
}

static inline void conn_close(connection* conn, int good) {
#ifdef WITH_SSL
  if (conn->secure) gnutls_deinit(conn->session);
#endif
  if (good) _nxweb_close_good_socket(conn->fd);
  else _nxweb_close_bad_socket(conn->fd);
}

static int open_socket(connection* conn);
static void rearm_socket(connection* conn);

#ifdef WITH_SSL
static void retrieve_ssl_session_info(connection* conn) {
  if (conn->tdata->ssl_identified) return; // already retrieved
  conn->tdata->ssl_identified=1;
  gnutls_session_t session=conn->session;
  conn->tdata->ssl_kx=gnutls_kx_get(session);
  conn->tdata->ssl_cred=gnutls_auth_get_type(session);
  int dhe=(conn->tdata->ssl_kx==GNUTLS_KX_DHE_RSA || conn->tdata->ssl_kx==GNUTLS_KX_DHE_DSS);
  int ecdh=(conn->tdata->ssl_kx==GNUTLS_KX_ECDHE_RSA || conn->tdata->ssl_kx==GNUTLS_KX_ECDHE_ECDSA);
  if (dhe) conn->tdata->ssl_dh_prime_bits=gnutls_dh_get_prime_bits(session);
  if (ecdh) conn->tdata->ssl_ecc_curve=gnutls_ecc_curve_get(session);
  conn->tdata->ssl_dhe=dhe;
  conn->tdata->ssl_ecdh=ecdh;
  conn->tdata->ssl_protocol=gnutls_protocol_get_version(session);
  conn->tdata->ssl_cert_type=gnutls_certificate_type_get(session);
  if (conn->tdata->ssl_cert_type==GNUTLS_CRT_X509) {
    const gnutls_datum_t *cert_list;
    unsigned int cert_list_size=0;
    cert_list=gnutls_certificate_get_peers(session, &cert_list_size);
    if (cert_list_size>0) {
      gnutls_x509_crt_init(&conn->tdata->ssl_cert);
      gnutls_x509_crt_import(conn->tdata->ssl_cert, &cert_list[0], GNUTLS_X509_FMT_DER);
    }
  }
  conn->tdata->ssl_compression=gnutls_compression_get(session);
  conn->tdata->ssl_cipher=gnutls_cipher_get(session);
  conn->tdata->ssl_mac=gnutls_mac_get(session);
}
#endif // WITH_SSL

static void write_cb(struct ev_loop *loop, ev_io *w, int revents) {
  connection *conn=((connection*)(((char*)w)-offsetof(connection, watch_write)));

  if (conn->state==C_CONNECTING) {
    conn->last_activity=ev_now(loop);
    conn->state=conn->secure? C_HANDSHAKING : C_WRITING;
  }

#ifdef WITH_SSL
  if (conn->state==C_HANDSHAKING) {
    conn->last_activity=ev_now(loop);
    int ret=gnutls_handshake(conn->session);
    if (ret==GNUTLS_E_SUCCESS) {
      retrieve_ssl_session_info(conn);
      conn->state=C_WRITING;
      // fall through to C_WRITING
    }
    else if (ret==GNUTLS_E_AGAIN || !gnutls_error_is_fatal(ret)) {
      if (ret!=GNUTLS_E_AGAIN) nxweb_log_error("gnutls handshake non-fatal error [%d] %s conn=%p", ret, gnutls_strerror(ret), conn);
      if (!gnutls_record_get_direction(conn->session)) {
        ev_io_stop(conn->loop, &conn->watch_write);
        ev_io_start(conn->loop, &conn->watch_read);
      }
      return;
    }
    else {
      nxweb_log_error("gnutls handshake error [%d] %s conn=%p", ret, gnutls_strerror(ret), conn);
      conn_close(conn, 0);
      inc_fail(conn);
      open_socket(conn);
      return;
    }
  }
#endif // WITH_SSL

  if (conn->state==C_WRITING) {
    int bytes_avail, bytes_sent;
	int req_index;
	char *data;
	int data_len;
	//use the session urls if in sessions mode
	if (config.num_urls>0) {
		req_index=rand() % conn->num_urls;
		data=conn->urls[req_index];
		data_len=conn->request_length_arr[req_index];
	} else {
		data=config.request_data;
		data_len=config.request_length;
	}
    do {
      bytes_avail=data_len - conn->write_pos;
      if (!bytes_avail) {
        conn->state=C_READING_HEADERS;
        conn->read_pos=0;
        ev_io_stop(conn->loop, &conn->watch_write);
        //ev_io_set(&conn->watch_read, conn->fd, EV_READ);
        ev_io_start(conn->loop, &conn->watch_read);
        ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
        return;
      }
      bytes_sent=conn_write(conn, data+conn->write_pos, bytes_avail);
      if (bytes_sent<0) {
        if (bytes_sent!=ERR_AGAIN) {
          strerror_r(errno, conn->buf, sizeof(conn->buf));
          nxweb_log_error("conn_write() returned %d: %d %s", bytes_sent, errno, conn->buf);
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        return;
      }
      if (bytes_sent) conn->last_activity=ev_now(loop);
      conn->write_pos+=bytes_sent;
    } while (bytes_sent==bytes_avail);
    return;
  }
}

static int decode_chunked_stream(nxweb_chunked_decoder_state* decoder_state, char* buf, int* buf_len) {
  char* p=buf;
  char* d=buf;
  char* end=buf+*buf_len;
  char c;
  while (p<end) {
    c=*p;
    switch (decoder_state->state) {
      case CDS_DATA:
        if (end-p>=decoder_state->chunk_bytes_left) {
          p+=decoder_state->chunk_bytes_left;
          decoder_state->chunk_bytes_left=0;
          decoder_state->state=CDS_CR1;
          d=p;
          break;
        }
        else {
          decoder_state->chunk_bytes_left-=(end-p);
          if (!decoder_state->monitor_only) *buf_len=(end-buf);
          return 0;
        }
      case CDS_CR1:
        if (c!='\r') return -1;
        p++;
        decoder_state->state=CDS_LF1;
        break;
      case CDS_LF1:
        if (c!='\n') return -1;
        if (decoder_state->final_chunk) {
          if (!decoder_state->monitor_only) *buf_len=(d-buf);
          return 1;
        }
        p++;
        decoder_state->state=CDS_SIZE;
        break;
      case CDS_SIZE: // read digits until CR2
        if (c=='\r') {
          if (!decoder_state->chunk_bytes_left) {
            // terminator found
            decoder_state->final_chunk=1;
          }
          p++;
          decoder_state->state=CDS_LF2;
        }
        else {
          if (c>='0' && c<='9') c-='0';
          else if (c>='A' && c<='F') c=c-'A'+10;
          else if (c>='a' && c<='f') c=c-'a'+10;
          else return -1;
          decoder_state->chunk_bytes_left=(decoder_state->chunk_bytes_left<<4)+c;
          p++;
        }
        break;
      case CDS_LF2:
        if (c!='\n') return -1;
        p++;
        if (!decoder_state->monitor_only) {
          memmove(d, p, end-p);
          end-=(p-d);
          p=d;
        }
        decoder_state->state=CDS_DATA;
        break;
    }
  }
  if (!decoder_state->monitor_only) *buf_len=(d-buf);
  return 0;
}

static char* find_end_of_http_headers(char* buf, int len, char** start_of_body) {
  if (len<4) return 0;
  char* p;
  for (p=memchr(buf+3, '\n', len-3); p; p=memchr(p+1, '\n', len-(p-buf)-1)) {
    if (*(p-1)=='\n') { *start_of_body=p+1; return p-1; }
    if (*(p-3)=='\r' && *(p-2)=='\n' && *(p-1)=='\r') { *start_of_body=p+1; return p-3; }
  }
  return 0;
}

static void parse_headers(connection* conn) {
  *(conn->body_ptr-1)='\0';

  conn->keep_alive=!strncasecmp(conn->buf, "HTTP/1.1", 8);
  conn->bytes_to_read=-1;
  char *p;
  for (p=strchr(conn->buf, '\n'); p; p=strchr(p, '\n')) {
    p++;
    if (!strncasecmp(p, "Content-Length:", 15)) {
      p+=15;
      while (*p==' ' || *p=='\t') p++;
      conn->bytes_to_read=atoi(p);
    }
    else if (!strncasecmp(p, "Transfer-Encoding:", 18)) {
      p+=18;
      while (*p==' ' || *p=='\t') p++;
      conn->chunked=!strncasecmp(p, "chunked", 7);
    }
    else if (!strncasecmp(p, "Connection:", 11)) {
      p+=11;
      while (*p==' ' || *p=='\t') p++;
      conn->keep_alive=!strncasecmp(p, "keep-alive", 10);
    }
  }

  if (conn->chunked) {
    conn->bytes_to_read=-1;
    memset(&conn->cdstate, 0, sizeof(conn->cdstate));
    conn->cdstate.monitor_only=1;
  }

  conn->bytes_received=conn->read_pos-(conn->body_ptr-conn->buf); // what already read
}

static void read_cb(struct ev_loop *loop, ev_io *w, int revents) {
  connection *conn=((connection*)(((char*)w)-offsetof(connection, watch_read)));

#ifdef WITH_SSL
  if (conn->state==C_HANDSHAKING) {
    conn->last_activity=ev_now(loop);
    int ret=gnutls_handshake(conn->session);
    if (ret==GNUTLS_E_SUCCESS) {
      retrieve_ssl_session_info(conn);
      conn->state=C_WRITING;
      ev_io_stop(conn->loop, &conn->watch_read);
      ev_io_start(conn->loop, &conn->watch_write);
      return;
    }
    else if (ret==GNUTLS_E_AGAIN || !gnutls_error_is_fatal(ret)) {
      if (ret!=GNUTLS_E_AGAIN) nxweb_log_error("gnutls handshake non-fatal error [%d] %s conn=%p", ret, gnutls_strerror(ret), conn);
      if (gnutls_record_get_direction(conn->session)) {
        ev_io_stop(conn->loop, &conn->watch_read);
        ev_io_start(conn->loop, &conn->watch_write);
      }
      return;
    }
    else {
      nxweb_log_error("gnutls handshake error [%d] %s conn=%p", ret, gnutls_strerror(ret), conn);
      conn_close(conn, 0);
      inc_fail(conn);
      open_socket(conn);
      return;
    }
  }
#endif // WITH_SSL

  if (conn->state==C_READING_HEADERS) {
    int room_avail, bytes_received;
    do {
      room_avail=sizeof(conn->buf)-conn->read_pos-1;
      if (!room_avail) {
        // headers too long
        nxweb_log_error("response headers too long");
        conn_close(conn, 0);
        inc_fail(conn);
        open_socket(conn);
        return;
      }
      bytes_received=conn_read(conn, conn->buf+conn->read_pos, room_avail);
      if (bytes_received<=0) {
        if (bytes_received==ERR_AGAIN) return;
        if (bytes_received==ERR_RDCLOSED) {
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        strerror_r(errno, conn->buf, sizeof(conn->buf));
        nxweb_log_error("headers [%d] conn_read() returned %d error: %d %s", conn->alive_count, bytes_received, errno, conn->buf);
        conn_close(conn, 0);
        inc_fail(conn);
        open_socket(conn);
        return;
      }
      conn->last_activity=ev_now(loop);
      conn->read_pos+=bytes_received;
      //conn->buf[conn->read_pos]='\0';
      if (find_end_of_http_headers(conn->buf, conn->read_pos, &conn->body_ptr)) {
        parse_headers(conn);
        if (conn->bytes_to_read<0 && !conn->chunked) {
          nxweb_log_error("response length unknown");
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        if (!conn->bytes_to_read) { // empty body
          rearm_socket(conn);
          return;
        }

        conn->state=C_READING_BODY;
        if (!conn->chunked) {
          if (conn->bytes_received>=conn->bytes_to_read) {
            // already read all
            rearm_socket(conn);
            return;
          }
        }
        else {
          int r=decode_chunked_stream(&conn->cdstate, conn->body_ptr, &conn->bytes_received);
          if (r<0) {
            nxweb_log_error("chunked encoding error");
            conn_close(conn, 0);
            inc_fail(conn);
            open_socket(conn);
            return;
          }
          else if (r>0) {
            // read all
            rearm_socket(conn);
            return;
          }
        }
        ev_feed_event(conn->loop, &conn->watch_read, EV_READ);
        return;
      }
    } while (bytes_received==room_avail);
    return;
  }

  if (conn->state==C_READING_BODY) {
    int room_avail, bytes_received, bytes_received2, r;
    conn->last_activity=ev_now(loop);
    do {
      room_avail=sizeof(conn->buf);
      if (conn->bytes_to_read>0) {
        int bytes_left=conn->bytes_to_read - conn->bytes_received;
        if (bytes_left<room_avail) room_avail=bytes_left;
      }
      bytes_received=conn_read(conn, conn->buf, room_avail);
      if (bytes_received<=0) {
        if (bytes_received==ERR_AGAIN) return;
        if (bytes_received==ERR_RDCLOSED) {
          nxweb_log_error("body [%d] read connection closed", conn->alive_count);
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        strerror_r(errno, conn->buf, sizeof(conn->buf));
        nxweb_log_error("body [%d] conn_read() returned %d error: %d %s", conn->alive_count, bytes_received, errno, conn->buf);
        conn_close(conn, 0);
        inc_fail(conn);
        open_socket(conn);
        return;
      }

      if (!conn->chunked) {
        conn->bytes_received+=bytes_received;
        if (conn->bytes_received>=conn->bytes_to_read) {
          // read all
          rearm_socket(conn);
          return;
        }
      }
      else {
        bytes_received2=bytes_received;
        r=decode_chunked_stream(&conn->cdstate, conn->buf, &bytes_received2);
        if (r<0) {
          nxweb_log_error("chunked encoding error after %d bytes received", conn->bytes_received);
          conn_close(conn, 0);
          inc_fail(conn);
          open_socket(conn);
          return;
        }
        else if (r>0) {
          conn->bytes_received+=bytes_received2;
          // read all
          rearm_socket(conn);
          return;
        }
      }

    } while (bytes_received==room_avail);
    return;
  }
}

static void shutdown_thread(thread_config* tdata) {
  int i;
  connection* conn;
  ev_tstamp now=ev_now(tdata->loop);
  ev_tstamp time_limit=tdata->avg_req_time*4;
  //fprintf(stderr, "[%.6lf]", time_limit);
  for (i=0; i<tdata->num_conn; i++) {
    conn=&tdata->conns[i];
    if (!conn->done) {
      if (ev_is_active(&conn->watch_read) || ev_is_active(&conn->watch_write)) {
        if ((now - conn->last_activity) > time_limit) {
          // kill this connection
          if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
          if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);
          conn_close(conn, 0);
          inc_fail(conn);
          conn->done=1;
          //fprintf(stderr, "*");
        }
        else {
          // don't kill this yet, but wake it up
          if (ev_is_active(&conn->watch_read)) {
            ev_feed_event(tdata->loop, &conn->watch_read, EV_READ);
          }
          if (ev_is_active(&conn->watch_write)) {
            ev_feed_event(tdata->loop, &conn->watch_write, EV_WRITE);
          }
          //fprintf(stderr, ".");
        }
      }
    }
  }
}

static int more_requests_to_run() {
  int rc;
  int print_flag = 0; 
  rc=__sync_add_and_fetch(&config.request_counter, 1);
  static time_t previous_time = 0;
  time_t elapsed_time;
	/* Infinite Mode */
  if (config.infinite==1) {  
      return 1;
      }
	/* Time Mode */
  else if(config.infinite==0){
	current_time  = time(NULL);
  	if(current_time < (config.run_time + start_time_rg)){
		elapsed_time = current_time-previous_time;
		if(elapsed_time > 4){
			print_flag = 1;
		        previous_time = current_time;
		}
		else{
			print_flag = 0;
		}
	}
	else{
		
		print_flag = 0;
		return 0;
	}
  }
	/*Requests Mode */
  else if(config.infinite==2){
	if (rc>config.num_requests) {
            return 0;
	}
	else{ 
	    if(config.progress_step>=10 && (rc%config.progress_step==0 || rc==config.num_requests)){
		print_flag = 1;
	    }
	    else{
		print_flag = 0;
	    }
	}
  } 

  if (!config.quiet && print_flag==1){
    	if(config.infinite==0)
		printf("%ld sec:  ",current_time-start_time_rg);
	printf("%d requests launched\n", rc);
	print_flag = 0;
  }
  return 1;
}

static void heartbeat_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  if (
	(config.request_counter>config.num_requests && config.infinite==2) || 
	((current_time-start_time_rg) > config.run_time && config.infinite==0)) 
  {
    thread_config *tdata=((thread_config*)(((char*)w)-offsetof(thread_config, watch_heartbeat)));
    if (!tdata->shutdown_in_progress) {
      ev_tstamp now=ev_now(tdata->loop);
      tdata->avg_req_time=tdata->num_success? (now-tdata->start_time) * tdata->num_conn / tdata->num_success : 0.1;
      if (tdata->avg_req_time>1.) tdata->avg_req_time=1.;
      tdata->shutdown_in_progress=1;
    }
    shutdown_thread(tdata);
  }
}

static void rearm_socket(connection* conn) {
  if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
  if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);

  inc_success(conn);

  if (!config.keep_alive || !conn->keep_alive) {
    conn_close(conn, 1);
    open_socket(conn);
  }
  else {
if (!more_requests_to_run()) {
      conn_close(conn, 1);
      conn->done=1;
      ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
      return;
    }
    conn->alive_count++;
    conn->state=C_WRITING;
    conn->write_pos=0;
    ev_io_start(conn->loop, &conn->watch_write);
    ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
  }
}

static int open_socket(connection* conn) {

  if (ev_is_active(&conn->watch_write)) ev_io_stop(conn->loop, &conn->watch_write);
  if (ev_is_active(&conn->watch_read)) ev_io_stop(conn->loop, &conn->watch_read);

  if (!more_requests_to_run(conn)) {
    conn->done=1;
    ev_feed_event(conn->tdata->loop, &conn->tdata->watch_heartbeat, EV_TIMER);
    return 1;
  }

  inc_connect(conn);
  
  //if sessions
	//choose session and set config.saddr to session saddr
	//if secure, set SSL stuff as well.

  conn->fd=socket(conn->saddr->ai_family, conn->saddr->ai_socktype, conn->saddr->ai_protocol);
  if (conn->fd==-1) {
    strerror_r(errno, conn->buf, sizeof(conn->buf));
    nxweb_log_error("can't open socket [%d] %s", errno, conn->buf);
    return -1;
  }
  if (setup_socket(conn->fd)) {
    nxweb_log_error("can't setup socket");
    return -1;
  }
  if (has_fastopen) {
    ssize_t ret=sendto(conn->fd, "", 0, MSG_FASTOPEN, conn->saddr->ai_addr, sizeof(struct sockaddr));
	if (ret<0) {
		if (errno!=EINPROGRESS && errno!=EALREADY && errno!=EISCONN) {
		  nxweb_log_error("can't connect with fastopen%d", errno);
		  return -1;
		}
	}
  } else {
	  if (connect(conn->fd, conn->saddr->ai_addr, conn->saddr->ai_addrlen)) {
		if (errno!=EINPROGRESS && errno!=EALREADY && errno!=EISCONN) {
		  nxweb_log_error("can't connect %d", errno);
		  return -1;
		}
	  } 
  }

#ifdef WITH_SSL
  if (config.secure) {
    gnutls_init(&conn->session, GNUTLS_CLIENT);
    gnutls_server_name_set(conn->session, GNUTLS_NAME_DNS, conn->uri_host, strlen(conn->uri_host));
    gnutls_priority_set(conn->session, config.priority_cache);
    gnutls_credentials_set(conn->session, GNUTLS_CRD_CERTIFICATE, config.ssl_cred);
    gnutls_transport_set_ptr(conn->session, (gnutls_transport_ptr_t)(int_to_ptr)conn->fd);
  }
#endif // WITH_SSL

  conn->state=C_CONNECTING;
  conn->write_pos=0;
  conn->alive_count=0;
  conn->done=0;
  ev_io_set(&conn->watch_write, conn->fd, EV_WRITE);
  ev_io_set(&conn->watch_read, conn->fd, EV_READ);
  ev_io_start(conn->loop, &conn->watch_write);
  ev_feed_event(conn->loop, &conn->watch_write, EV_WRITE);
  return 0;
}


static void* thread_main(void* pdata) {
  thread_config* tdata=(thread_config*)pdata;

  ev_timer_init(&tdata->watch_heartbeat, heartbeat_cb, 0.1, 0.1);
  ev_timer_start(tdata->loop, &tdata->watch_heartbeat);
  ev_unref(tdata->loop); // don't keep loop running just for heartbeat
  ev_run(tdata->loop, 0);
  stop_cpu_stats=1;

  ev_loop_destroy(tdata->loop);

  if (!config.quiet && config.num_threads>1) {
    printf("thread %d: %d connect, %d requests, %d success, %d fail, %ld bytes, %ld overhead\n",
         tdata->id, tdata->num_connect, tdata->num_success+tdata->num_fail,
         tdata->num_success, tdata->num_fail, tdata->num_bytes_received,
         tdata->num_overhead_received);
  }

  return 0;
}

//Scan /proc/stat for cpu load
static long double get_cpu_load() {
    long double a[4], b[4], loadavg=0;
    FILE *fp;
	int c=0;
	
        fp = fopen("/proc/stat","r");
        c+=fscanf(fp,"%*s %Lf %Lf %Lf %Lf",&a[0],&a[1],&a[2],&a[3]);
        fclose(fp);
        sleep(1);

        fp = fopen("/proc/stat","r");
        c+=fscanf(fp,"%*s %Lf %Lf %Lf %Lf",&b[0],&b[1],&b[2],&b[3]);
        fclose(fp);

		if (c != 8)
			return -1.0;
        loadavg = ((b[0]+b[1]+b[2]) - (a[0]+a[1]+a[2])) / ((b[0]+b[1]+b[2]+b[3]) - (a[0]+a[1]+a[2]+a[3]));
	return loadavg;
}


static void *cpu_stat_thread(void *pdata) {
    long double a[4], b[4], loadavg, total=0, count=0.0;
    FILE *fp;
	long double max,min;
	cpu_info_t *data=(cpu_info_t *)pdata;
	stop_cpu_stats=0;
	
    sleep(1);
	loadavg=get_cpu_load();
	if (loadavg < 0.0) {
		printf("Warning! Don't know how to process this /proc/stat!\n");
		data->min=0.0;
		data->max=0.0;
		data->avg=0.0;
		return data;
	}
	max=loadavg;
	min=loadavg;

    for(;;)
    {
        sleep(1);
		if (stop_cpu_stats)
			break;
		if (loadavg > max) 
			max=loadavg;
		if (loadavg < min)
			min=loadavg;
		count+=1.0;
		total+=loadavg;
		if (print_all_cpu_stats)
			printf("The current CPU utilization is : %Lf\n",loadavg);
    }
	data->max=	max*100.0;
	data->min=	min*100.0;
	data->avg=	total*100.0 / count;
	
	if (max > .95) 
		printf("Warning! Detected max cpu usage > 95%%\n");
	
    return data;
}

static int resolve_host(struct addrinfo** saddr, const char *host_and_port) {
  char* host=strdup(host_and_port);
  char* port=strchr(host, ':');
  if (port) *port++='\0';
  else port=config.secure? "443":"80";
  

	struct addrinfo hints, *res, *res_first, *res_last;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family=PF_UNSPEC;
	hints.ai_socktype=SOCK_STREAM;

    printf(" Resolving %s\n",host_and_port);

	if (getaddrinfo(host, port, &hints, &res_first)) goto ERR1;

	// search for an ipv4 address, no ipv6 yet
	res_last=0;
	for (res=res_first; res; res=res->ai_next) {
		if (res->ai_family==AF_INET) break;
		res_last=res;
	}

	if (!res) goto ERR2;
	if (res!=res_first) {
		// unlink from list and free rest
		res_last->ai_next = res->ai_next;
		freeaddrinfo(res_first);
		res->ai_next=0;
	}

  free(host);
  *saddr=res;
	return 0;

ERR2:
	freeaddrinfo(res_first);
ERR1:
  free(host);
  return -1;
}

static void show_help(void) {
	printf( "httpress <options> <url>\n"
          "  -n num   number of requests     (default: 1)\n"
          "  -t num   number of threads      (default: 1)\n"
          "  -c num   concurrent connections (default: 1)\n"
          "  -k       keep alive             (default: no)\n"
          "  -i        run forever           (default: no)\n"
          "  -q       no progress indication (default: no)\n"
          "  -z pri   GNUTLS cipher priority (default: NORMAL)\n"
          "  -r       Run for time in seconds(default: 120 seconds)\n"
          "  -h       show this help\n"
          //"  -v       show version\n"
          "\n"
          "example: httpress -n 10000 -c 100 -t 4 -k http://localhost:8080/index.html\n\n");
}

static char host_buf[1024];

static int parse_uri_to(const char* uri, const char **host, const char **path) {
  if (!strncmp(uri, "http://", 7)) uri+=7;
#ifdef WITH_SSL
  else if (!strncmp(uri, "https://", 8)) { uri+=8; config.secure=1; }
#endif
  else return -1;

  const char* p=strchr(uri, '/');
  if (!p) {
    config.uri_host=uri;
    config.uri_path="/";
    return 0;
  }
  if ((p-uri)>sizeof(host_buf)-1) return -1;
  strncpy(host_buf, uri, (p-uri));
  host_buf[(p-uri)]='\0';
  *host=host_buf;
  *path=p;
  return 0;
}

static int parse_uri(const char* uri) {
	return parse_uri_to(uri, &config.uri_host, &config.uri_path);
}



int main(int argc, char* argv[]) {
  config.num_connections=1;
  config.num_requests=1;
  config.num_threads=1;
  config.keep_alive=0;
  config.quiet=0;
  config.uri_path=0;
  config.uri_host=0;
  config.request_counter=0;
  config.infinite=2;
  config.ssl_cipher_priority="NORMAL"; // NORMAL:-CIPHER-ALL:+AES-256-CBC:-VERS-TLS-ALL:+VERS-TLS1.0:-KX-ALL:+DHE-RSA
  config.run_time=120;
  
  start_time_rg = time(NULL);
  int c;
  char *session_file=NULL;
  while ((c=getopt(argc, argv, ":hvkqin:r:f:t:c:z:"))!=-1) {
    switch (c) {
      case 'h':
        show_help();
        return 0;
      case 'v':
        printf("version:    " VERSION "\n");
        printf("build-date: " __DATE__ " " __TIME__ "\n\n");
        return 0;
      case 'k':
        config.keep_alive=1;
        break;
      case 'q':
        config.quiet=1;
        break;
      case 'n':
        config.num_requests=atoi(optarg);
        break;
      case 't':
        config.num_threads=atoi(optarg);
        break;
      case 'c':
        config.num_connections=atoi(optarg);
        break;
      case 'z':
        config.ssl_cipher_priority=optarg;
        break;
	  case 'f':
	    session_file=optarg;
		break;
	  case 'i':
		config.infinite=1;
		break;
      case 'r':
        config.run_time=atoi(optarg);
        config.infinite=0;      /*config.infinite = 0 means run_time is enabled and infinite disabled. 
                                   config.infinite = 1 means infinite enabled and run_time disabled.
                                   config.infinite = 2 means run_time is disabled and requests mode is enabled */
        break;
      case '?':
        fprintf(stderr, "unkown option: -%c\n\n", optopt);
        show_help();
        return EXIT_FAILURE;
    }
  }
  if (config.infinite == 0)
	config.num_requests = 10*config.num_threads*config.num_connections;
  if ((session_file==NULL) && (argc-optind)<1) {
    fprintf(stderr, "missing url argument\n\n");
    show_help();
    return EXIT_FAILURE;
  }
  else if ((argc-optind)>1) {
    fprintf(stderr, "too many arguments\n\n");
    show_help();
    return EXIT_FAILURE;
  }
  if ((config.run_time<1 || config.run_time>3600) && config.infinite==0){
   nxweb_die("Recheck run time. This value should be less than 1 hour"); 
  }
  if (config.num_requests<1 || config.num_requests>1000000000) nxweb_die("wrong number of requests");
  if (config.num_connections<1 || config.num_connections>1000000 || config.num_connections>config.num_requests) nxweb_die("wrong number of connections");
  if (config.num_threads<1 || config.num_threads>100000 || config.num_threads>config.num_connections) nxweb_die("wrong number of threads");

  config.progress_step=config.num_requests/4;
  if (config.progress_step>50000) config.progress_step=50000;

  if (session_file != NULL) {
	  int session_id=-1;
	  int num_urls=0;
	  int lineno=0;
	  char line[MAX_REQ_SIZE];
	  FILE *fp=fopen(session_file,"r");
	  config.uri_host=NULL;
	  while (!feof(fp)) {
		  lineno++;
		char *pline=fgets(line,MAX_REQ_SIZE,fp);
		if (pline==NULL) { 
			break;
		}
		strip_newline(line);
		if (empty_line(line))
			continue;
		//session start, update session id and close out last session if any
		if (strstr(line,"!start_req_sequence") != NULL) {
			if (session_id>=0)
				config.sessions[session_id]=num_urls;
			session_id++;
			if (session_id>MAX_SESSIONS) {
				printf("ERROR: Too many sessions!\n");
				exit(EXIT_FAILURE);
			}
			continue;
		}
		//hostname, capture and verify accessible.
		if (strstr(line,"host: ") != NULL) {
			sscanf(line,"host: %s",host_string);
			if (session_id<0) {
				nxweb_log_error("Host on line %d but no session started!", lineno);
				exit(EXIT_FAILURE);
			}
			parse_uri(host_string);
			config.session_host[session_id]=config.uri_host;
			if (resolve_host(&config.saddr, config.uri_host)) {
				nxweb_log_error("can't resolve host %s", config.uri_host);
				exit(EXIT_FAILURE);
			}
			config.session_saddr[session_id]=config.saddr;
		} else {
			//regular url to fetch, create the request string.
			if (config.uri_host==NULL) {
				nxweb_log_error("Session file does not set up host!");
				exit(EXIT_FAILURE);
			}
			char *data=(char *)malloc(sizeof(char) * MAX_REQ_SIZE);
			snprintf(data, sizeof(char) * MAX_REQ_SIZE,
				   "GET %s HTTP/1.1\r\n"
				   "Host: %s\r\n"
				   "Connection: %s\r\n"
				   "\r\n",
				   line, config.uri_host, config.keep_alive?"keep-alive":"close"
				  );
			config.request_length_arr[num_urls]=strlen(data);
			config.request_data_arr[num_urls]=data;
			num_urls++;
			if (num_urls > MAX_URLS) {
				printf("ERROR: Too many URLs!\n");
				exit(EXIT_FAILURE);
			}
			//some debug
			if (first<3) {printf("%s\n",data);first++;}
		}
	  }
	  //update final counts
	  config.sessions[session_id]=num_urls;
	  config.num_urls=num_urls;
	  config.last_session=session_id+1;
  } else 
  { /* single url */
	  if (parse_uri(argv[optind])) nxweb_die("can't parse url: %s", argv[optind]);
	  if (resolve_host(&config.saddr, config.uri_host)) {
		nxweb_log_error("can't resolve host %s", config.uri_host);
		exit(EXIT_FAILURE);
	  }

	  snprintf(config.request_data, sizeof(config.request_data),
			   "GET %s HTTP/1.1\r\n"
			   "Host: %s\r\n"
			   "Connection: %s\r\n"
			   "\r\n",
			   config.uri_path, config.uri_host, config.keep_alive?"keep-alive":"close"
			  );
	  config.request_length=strlen(config.request_data);
	if (first<3) { printf("%s\n",config.request_data); first++; }
	  
  } /* end of setting url(s) to test */
#ifdef WITH_SSL
  if (config.secure) {
    gnutls_global_init();
    gnutls_certificate_allocate_credentials(&config.ssl_cred);
    int ret=gnutls_priority_init(&config.priority_cache, config.ssl_cipher_priority, 0);
    if (ret) {
      fprintf(stderr, "invalid priority string: %s\n\n", config.ssl_cipher_priority);
      return EXIT_FAILURE;
    }
  }
#endif // WITH_SSL


  // Block signals for all threads
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGPIPE);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGQUIT);
  sigaddset(&set, SIGHUP);
  if (pthread_sigmask(SIG_BLOCK, &set, NULL)) {
    nxweb_log_error("can't set pthread_sigmask");
    exit(EXIT_FAILURE);
  }

  thread_config** threads=calloc(config.num_threads, sizeof(thread_config*));
  if (!threads) nxweb_die("can't allocate thread pool");

  ev_tstamp ts_start=ev_time();
  int i, j;
  int conns_allocated=0;
  thread_config* tdata;

  for (i=0; i<config.num_threads; i++) {
    threads[i]=
    tdata=memalign(MEM_GUARD, sizeof(thread_config)+MEM_GUARD);
    if (!tdata) nxweb_die("can't allocate thread data");
    memset(tdata, 0, sizeof(thread_config));
    tdata->id=i+1;
    tdata->start_time=ts_start;
    tdata->num_conn=(config.num_connections-conns_allocated)/(config.num_threads-i);
    conns_allocated+=tdata->num_conn;
    tdata->conns=memalign(MEM_GUARD, tdata->num_conn*sizeof(connection)+MEM_GUARD);
    if (!tdata->conns) nxweb_die("can't allocate thread connection pool");
    memset(tdata->conns, 0, tdata->num_conn*sizeof(connection));

    tdata->loop=ev_loop_new(0);

    connection* conn;
    for (j=0; j<tdata->num_conn; j++) {
      conn=&tdata->conns[j];
      conn->tdata=tdata;
      conn->loop=tdata->loop;
	  //if sessions, check session and set accordingly
	  if (config.last_session>0) {
		conn->session_id=j % config.last_session;
		conn->saddr=config.session_saddr[conn->session_id];
		conn->uri_host=config.session_host[conn->session_id];
		conn->uri_path=config.uri_path;
		int first_url=0; //first session starts at idx 0, otherwise stored in config.session
		if (conn->session_id>0) {
			int first_url=config.sessions[conn->session_id-1];
		} 
		conn->num_urls=config.sessions[conn->session_id] - first_url;
		conn->urls = &(config.request_data_arr[ first_url ]);
		conn->request_length_arr=&(config.request_length_arr[first_url]);
	  } else {
		conn->saddr=config.saddr;
		conn->uri_host=config.uri_host;
		conn->uri_path=config.uri_path;
	  }
      conn->secure=config.secure;
      ev_io_init(&conn->watch_write, write_cb, -1, EV_WRITE);
      ev_io_init(&conn->watch_read, read_cb, -1, EV_READ);
	  //also set connection props like saddr from session instead of from global config
      open_socket(conn);
    }

    pthread_create(&tdata->tid, 0, thread_main, tdata);
    //sleep_ms(10);
  }
  cpu_info_t cpustat;
  pthread_create(&(cpustat.tid), 0, cpu_stat_thread, &cpustat);
  
  // Unblock signals for the main thread;
  // other threads have inherited sigmask we set earlier
  sigdelset(&set, SIGPIPE); // except SIGPIPE
  if (pthread_sigmask(SIG_UNBLOCK, &set, NULL)) {
    nxweb_log_error("can't unset pthread_sigmask");
    exit(EXIT_FAILURE);
  }
  long total_success=0;
  long total_fail=0;
  long total_bytes=0;
  long total_overhead=0;
  long total_connect=0;

  for (i=0; i<config.num_threads; i++) {
    tdata=threads[i];
    pthread_join(threads[i]->tid, 0);
    total_success+=tdata->num_success;
    total_fail+=tdata->num_fail;
    total_bytes+=tdata->num_bytes_received;
    total_overhead+=tdata->num_overhead_received;
    total_connect+=tdata->num_connect;
  }

  int real_concurrency=0;
  int real_concurrency1=0;
  int real_concurrency1_threshold=config.num_requests/config.num_connections/10;
  if (real_concurrency1_threshold<2) real_concurrency1_threshold=2;
  for (i=0; i<config.num_threads; i++) {
    tdata=threads[i];
    for (j=0; j<tdata->num_conn; j++) {
      connection* conn=&tdata->conns[j];
      if (conn->success_count) real_concurrency++;
      if (conn->success_count>=real_concurrency1_threshold) real_concurrency1++;
    }
  }

  ev_tstamp ts_end=ev_time();
  if (ts_end<=ts_start) ts_end=ts_start+0.00001;
  ev_tstamp duration=ts_end-ts_start;
  int sec=duration;
  duration=(duration-sec)*1000;
  int millisec=duration;
  //duration=(duration-millisec)*1000;
  //int microsec=duration;
  double rps=total_success/(ts_end-ts_start);
  int kbps=(total_bytes+total_overhead) / (ts_end-ts_start) / 1024;
  ev_tstamp avg_req_time=total_success? (ts_end-ts_start) * config.num_connections / total_success : 0;

#ifdef WITH_SSL
  if (config.secure && !config.quiet) {
    for (i=0; i<config.num_threads; i++) {
      tdata=threads[i];
      if (tdata->ssl_identified) {
        printf("\nSSL INFO: %s\n", gnutls_cipher_suite_get_name(tdata->ssl_kx, tdata->ssl_cipher, tdata->ssl_mac));
        printf ("- Protocol: %s\n", gnutls_protocol_get_name(tdata->ssl_protocol));
        printf ("- Key Exchange: %s\n", gnutls_kx_get_name(tdata->ssl_kx));
        if (tdata->ssl_ecdh) printf ("- Ephemeral ECDH using curve %s\n",
                  gnutls_ecc_curve_get_name(tdata->ssl_ecc_curve));
        if (tdata->ssl_dhe) printf ("- Ephemeral DH using prime of %d bits\n",
                  tdata->ssl_dh_prime_bits);
        printf ("- Cipher: %s\n", gnutls_cipher_get_name(tdata->ssl_cipher));
        printf ("- MAC: %s\n", gnutls_mac_get_name(tdata->ssl_mac));
        printf ("- Compression: %s\n", gnutls_compression_get_name(tdata->ssl_compression));
        printf ("- Certificate Type: %s\n", gnutls_certificate_type_get_name(tdata->ssl_cert_type));
        if (tdata->ssl_cert) {
          gnutls_datum_t cinfo;
          if (!gnutls_x509_crt_print(tdata->ssl_cert, GNUTLS_CRT_PRINT_ONELINE, &cinfo)) {
            printf ("- Certificate Info: %s\n", cinfo.data);
            gnutls_free(cinfo.data);
          }
        }
        break;
      }
    }
  }
#endif // WITH_SSL

  if (!config.quiet) printf("\n");
  printf("TOTALS:  %ld connect, %ld requests, %ld success, %ld fail, %d (%d) real concurrency, keepalive %d\n",
         total_connect, total_success+total_fail, total_success, total_fail, real_concurrency, real_concurrency1, config.keep_alive);
  printf("TRAFFIC: %ld avg bytes, %ld avg overhead, %ld bytes, %ld overhead\n",
         total_success?total_bytes/total_success:0L, total_success?total_overhead/total_success:0L, total_bytes, total_overhead);
  printf("CPUSTAT:  max,%.1Lf,min,%.1Lf,avg,%.1Lf \n",
         cpustat.max, cpustat.min,cpustat.avg);
  if (rps > 100) {
	int irps=floor(rps);
  	printf("TIMING:  %d.%03d seconds, %d rps, %d kbps, %.1f ms avg req time\n",
         sec, millisec, /*microsec,*/ irps, kbps, (float)(avg_req_time*1000));
  }
  else {
  	printf("TIMING:  %d.%03d seconds, %.2f rps, %d kbps, %.1f ms avg req time\n",
         sec, millisec, /*microsec,*/ rps, kbps, (float)(avg_req_time*1000));
  }

  pthread_join(cpustat.tid,0);
		 
  freeaddrinfo(config.saddr);
  for (i=0; i<config.num_threads; i++) {
    tdata=threads[i];
    free(tdata->conns);
#ifdef WITH_SSL
    if (tdata->ssl_cert) gnutls_x509_crt_deinit(tdata->ssl_cert);
#endif // WITH_SSL
    free(tdata);
  }
  free(threads);

#ifdef WITH_SSL
  if (config.secure) {
    gnutls_certificate_free_credentials(config.ssl_cred);
    gnutls_priority_deinit(config.priority_cache);
    gnutls_global_deinit();
  }
#endif // WITH_SSL

  return EXIT_SUCCESS;
}
