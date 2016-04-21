# htt2
Http test tool based on httpress

## Usage:

	httpress <options> <url>
  	-n num   number of requests     (default: 1)
  	-t num   number of threads      (default: 1)
  	-c num   concurrent connections (default: 1)
  	-k       keep alive             (default: no)
  	-i       run forever            (default: no)
  	-f file  session file           (default: none)
  	-q       no progress indication (default: no)
  	-z pri   GNUTLS cipher priority (default: NORMAL)
  	-h       show this help


## Sample session file:

	!start_req_sequence
	host: http://192.168.223.140:8080
	/html/1000/1.html
	/html/1000/2.html
	/html/1000/3.html
	/html/1000/4.html
	/html/1000/5.html
	/html/1000/6.html
	/html/1000/7.html
	/html/1000/8.html


