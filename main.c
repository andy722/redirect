/*
 * Redirects data flow from local port to specified remote server and counter.
 *
 * Usage example: redir 2020 proxy.nsu.ru:8888
 */

#if defined(THREADS) && defined(SELECT) || defined(THREADS) && defined(AIO) || defined(SELECT) && defined(AIO)
#error More than one option from list SELECT, THREADS, AIO cannot be defined!
#endif

#include <stdlib.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

#include <fcntl.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <linux/tcp.h>

#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

#include <signal.h>

#ifndef DEBUG
#define NODEBUG
#endif

#ifdef THREADS
#include <pthread.h>
#endif

#define _difftime(t1, t2) abs(t1-t2)

#ifdef AIO
#include <aio.h>
#endif

/* support for highlighting */
#ifdef COLORS
#define BRIGHT_WHITE	"\x1B[0;1;37m"
#define NORMAL		"\x1B[0;37m"
#endif

#ifndef COLORS
#define BRIGHT_WHITE	""
#define NORMAL		""
#endif

//#define DEBUG		1	/* debugging output: true/false */
#define MAX_QUEUE	1024	/* maximal enqueued connections (for listen call) */
#define BLOCK		65535	/* size of data pieces to work with */
#define TIMEOUT		1	/* delay (in seconds) for socket operations */

unsigned long long total_data = 0;	// transmitted data size. protected with mutex if using threads

/* command line arguments */
int port_from, port_to;
char host_to[128];

/* sockets */
int sock_catcher; 		// waits for incoming connections
int sock_incoming; 		// serves connections
int sock_final;			// connects with <host>:<port2>
struct list *coming;

/* daemonize? */
static int daemonize = 0;

/* thread synchronising */
#ifdef THREADS
pthread_mutex_t sock_final_lock = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t total_data_lock = PTHREAD_MUTEX_INITIALIZER;
static void *connection(void *);
#endif

#define MAX(a, b) (((a)>(b)) ? (a) : (b) )

#ifdef AIO
struct conn_t {
	int sock_incoming;
	int sock_final;
	struct aiocb *aiocbp;				      
	char buf[BLOCK];
	int write;	// last operation: 1 - write, 0 - read				     
	time_t progr_time;
};

struct list {
	struct conn_t *elem;
	struct list* next;
	struct list* prev;
};

struct list *trans = NULL;
#endif /* AIO */

static inline void usage(char *prog) {
	fprintf(stderr, "Usage:\n\t%s [-d] <port1> <host>:<port2>\n", prog);
}

/* making socket nonblocking */
void nonblock(int sock) {
	/* at first, get all current flags of the socket */
	int flags = fcntl(sock, F_GETFL, 0);
	if(flags<0) {
		perror("fcntl");
		exit(EXIT_FAILURE);
	}

	/* then, add nonblocking flag and set new options */
	if(fcntl(sock, F_SETFL, flags | O_NONBLOCK)<0) {
		perror("fcntl");
		exit(EXIT_FAILURE);
	}

	//	 setting socket timeout 

/*
	struct timeval tv;
	tv.tv_sec = 1;
	tv.tv_usec = 0;
	if(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv))!=0) {
		perror("setsockopt");
		exit(EXIT_FAILURE);
	}
*/
}

/* closes connections */
void quit(int signame) {
	fprintf(stderr, "Killed by signal %i. Transferred " BRIGHT_WHITE "%Li" NORMAL " bytes\n", signame, total_data);
	shutdown(sock_catcher, SHUT_RDWR);
	shutdown(sock_final, SHUT_RDWR);
	close(sock_catcher);
	close(sock_final);
	exit(EXIT_SUCCESS);
}

/* initializes final socket - communication between server and redirector */
int init_sock_final(struct sockaddr_in *addr_final) {
	int sock_final;
	int rc;
	struct hostent *hp;

	sock_final = socket(PF_INET, SOCK_STREAM, 0);
	if(sock_final<0) {
		perror("socket");
		exit(errno);
	}

	memset(addr_final, 0, sizeof(*addr_final));
	addr_final->sin_family = AF_INET;
	addr_final->sin_port = htons(port_to);
	hp = gethostbyname(host_to);
	if(hp==NULL) {
		perror("gethostbyname");
		exit(errno);
	}
	memcpy(&(addr_final->sin_addr), hp->h_addr, hp->h_length);

	rc = connect(sock_final, (struct sockaddr *) addr_final, sizeof(*addr_final));
	if(rc) {
		perror("connect");
		exit(errno);
	}

	/* enabling TCP_NODELAY */
	rc = 1;
	if(setsockopt(sock_final, IPPROTO_TCP, TCP_NODELAY, &rc, sizeof(rc)) == -1)
		perror("setsockopt");
	return sock_final;
}

/* copies data between 2 given sockets. Used with thread support or select call */
#if defined(THREADS) || defined(SELECT)
void copy(int insock, int outsock) {
	fd_set iofds, c_iofds;
	int max_fd;
	char buf[BLOCK];
	unsigned long long bytes, local_data = 0;

#ifdef SELECT
	nonblock(insock);
	nonblock(outsock);
#endif

#if TIMEOUT
	struct timeval tv;
	tv.tv_sec = TIMEOUT;
	tv.tv_usec = 0;
#endif
	FD_ZERO(&iofds);
	FD_SET(insock, &iofds);
	FD_SET(outsock, &iofds);

	max_fd = MAX(insock, outsock);
	while(1) {
		memcpy(&c_iofds, &iofds, sizeof(iofds));
#if TIMEOUT
		tv.tv_sec = TIMEOUT;
#endif
		if (select(max_fd + 1, &c_iofds, NULL, NULL,
#if TIMEOUT
					&tv
#else
					NULL
#endif
			  ) <= 0) {
			break;
		}

		if(FD_ISSET(insock, &c_iofds)) {
			if((bytes = read(insock, buf, sizeof(buf))) <= 0)
				break;
			if(write(outsock, buf, bytes) != bytes)
				break;
			local_data+=bytes;
		}
		if(FD_ISSET(outsock, &c_iofds)) {
			if((bytes = read(outsock, buf, sizeof(buf))) <= 0)
				break;
			if(write(insock, buf, bytes) != bytes)
				break;
			local_data+=bytes;
		}
	}
	shutdown(insock, 0);
	shutdown(outsock, 0);
	close(insock);
	close(outsock);
#ifdef THREADS
	pthread_mutex_lock(&total_data_lock);
#endif
	total_data += local_data;
#ifdef THREADS
	pthread_mutex_unlock(&total_data_lock);
#endif
	return;
}
#endif

#ifdef AIO
void nop() {return;}
/* analog to copy(int, int), but used with aio */
int copy(struct conn_t *conn) {
	int rc = aio_error(conn->aiocbp);

	fd_set wr;
	FD_ZERO(&wr);
	FD_SET(conn->sock_final, &wr);
	if(select(conn->sock_final+1, NULL, &wr, NULL, NULL)==0) {
		nop();
		return EXIT_FAILURE;
	}

	if(rc==EINPROGRESS)	// ==115
		return EXIT_SUCCESS;
	
	int bytes = aio_return(conn->aiocbp);
	if(rc && !(bytes>=0) ) {
		// some kind of error
		perror("aio_error");
		return EXIT_FAILURE;
	} else {
		// all is ok

		if(conn->write) {
			// Previous operation - writing. Now reading from another socket
			fd_set rd;
			FD_ZERO(&rd);

			FD_SET(conn->sock_final, &rd);
			FD_SET(conn->sock_incoming, &rd);

			struct timeval tv;
			tv.tv_sec = 0;
			tv.tv_usec = 0;

			// nonblocking!
			rc = select(MAX(conn->sock_final, conn->sock_incoming) + 1, &rd, NULL, NULL, NULL);

			if(!rc) {
				return /*EXIT_SUCCESS*/EXIT_FAILURE;
			}
			if(FD_ISSET(conn->sock_final, &rd))
				conn->aiocbp->aio_fildes = conn->sock_final;
			if(FD_ISSET(conn->sock_incoming, &rd))
				conn->aiocbp->aio_fildes = conn->sock_incoming;


			memset(conn->buf, 0, BLOCK);
			conn->aiocbp->aio_nbytes = BLOCK;
			rc = aio_read(conn->aiocbp);
			if(rc) {
				perror("aio_read");
				return EXIT_FAILURE;
			}
#ifdef DEBUG
			fprintf(stderr, "Reading from sock %i\n", conn->aiocbp->aio_fildes);
#endif
		} else {
			// Previous operation - reading. Now writing to opposite socket
#ifdef DEBUG
			fprintf(stderr, "Read %i bytes from sock %i\n", bytes, conn->aiocbp->aio_fildes);
#endif
			if(!bytes) {
				return EXIT_FAILURE;
			} else {

				if(conn->aiocbp->aio_fildes == conn->sock_incoming) {
					memset(conn->aiocbp, 0, sizeof(conn->aiocbp));
					conn->aiocbp->aio_fildes = conn->sock_final;
				}
				else if(conn->aiocbp->aio_fildes == conn->sock_final) {
					memset(conn->aiocbp, 0, sizeof(conn->aiocbp));
					conn->aiocbp->aio_fildes = conn->sock_incoming;
				}
				conn->aiocbp->aio_buf = conn->buf;
				conn->aiocbp->aio_nbytes = bytes;
				total_data += bytes;

				rc = aio_write(conn->aiocbp);
				if(rc) {
					perror("aio_write");
					return EXIT_FAILURE;
				}
			}
		}
		conn->write = !conn->write;
		return EXIT_SUCCESS;
	}
}
#endif


#ifdef THREADS
void *connection(void *s) {
	int sock_incoming = (int) s;
	struct sockaddr_in addr_final;

	int sock_final = init_sock_final(&addr_final);

	copy(sock_final, sock_incoming);
	pthread_exit(NULL);
}
#endif /* THREADS */

static inline void sighandle() {
	if(signal(SIGTERM, &quit) == SIG_ERR)
		perror("signal (SIGTERM)");
	if(signal(SIGINT, &quit) == SIG_ERR)
		perror("signal (SIGINT)");
	if(signal(SIGPIPE, SIG_IGN) == SIG_ERR)
		perror("signal (SIGPIPE)");
}

int main(int argc, char *argv[]) {
	struct sockaddr_in addr_catcher;	// addr_catcher - local address (input, server)
	struct sockaddr_in addr_incoming;	// communication with client and redirector
	int rc;					// for return values

	if((argc>4) || ((argc==4) && strcmp(argv[1], "-d")) || (argc<3) ) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if(argc==3) {
		port_from = atoi(argv[1]);
		sscanf(argv[2], "%[^:]:%i", host_to, &port_to);
	} else {
		port_from = atoi(argv[2]);
		sscanf(argv[3], "%[^:]:%i", host_to, &port_to);
		daemonize = 1;
	}

	/* simple check for correctness */
	if(
			!( (port_from>0) && (port_from<65535) ) ||
			!( (port_to>0) && (port_to<65535) ) ||
			strlen(host_to)==0
	  ) {
		usage(argv[0]);
		return EXIT_SUCCESS;
	}
	if(daemonize) {
		if(daemon(0, 0)) {
			perror("daemon");
			exit(errno);
		}
	}

#ifdef DEBUG
	fprintf(stderr, "localhost:%i -> %s:%i\n", port_from, host_to, port_to);
#endif
	/* ok, at this moment all the args are OK */

	/* creating sock_catcher - it'll just wait for connections 
	 * and redirect them
	 */
	sock_catcher = socket(PF_INET, SOCK_STREAM, 0);
	if(sock_catcher<0) {
		perror("socket");
		exit(errno);
	}
	memset(&addr_catcher, 0, sizeof(addr_catcher));
	addr_catcher.sin_family = AF_INET;
	addr_catcher.sin_addr.s_addr = INADDR_ANY;
	addr_catcher.sin_port = htons(port_from);

	rc = bind(sock_catcher, (struct sockaddr *) &addr_catcher, sizeof(addr_catcher));
	if(rc) {
		perror("bind");
		exit(errno);
	}

	rc = 1;
	if(setsockopt(sock_catcher, IPPROTO_TCP, TCP_NODELAY, &rc, sizeof(rc)) == -1)
		perror("setsockopt");

	/* it doesn't work with the same remote address - and we use exactly this */
	/*
#ifdef DEBUG
	rc_c = 1;
	setsockopt(sock_catcher, SOL_SOCKET, SO_REUSEADDR, &rc_c, sizeof(rc_c));
	rc_c = 1;
	setsockopt(sock_final, SOL_SOCKET, SO_REUSEADDR, &rc_c, sizeof(rc_c));
#endif
*/
	/* all necessary sockets are created */
	sighandle();

	rc = listen(sock_catcher, MAX_QUEUE);
	if(rc) {
		perror("listen");
		exit(errno);
	}

#ifdef THREADS
	pthread_t pthr;				// return value for threads. not used, but necessary
	pthread_attr_t pattr;
	pthread_attr_init(&pattr);
	pthread_attr_setdetachstate(&pattr, PTHREAD_CREATE_DETACHED);
#endif 

#if defined(SELECT) || defined(AIO)
	struct sockaddr_in addr_final;
#endif
	int addr_incoming_len = sizeof(addr_incoming);

#ifdef AIO
	nonblock(sock_catcher);
#endif
	while(1) {
		sock_incoming = accept(sock_catcher, (struct sockaddr *) &addr_incoming, (socklen_t *) &addr_incoming_len);
#ifndef AIO
		if(sock_incoming<0) {
			switch(errno) {
				/* non-fatal errors */
				case EHOSTUNREACH:
				case ECONNRESET:
				case ETIMEDOUT:
#ifdef THREADS
					continue;
#endif
#if defined(SELECT)
					/* do nothing  */
					break;
#endif
				default:
					perror("accept");
					exit(errno);
			} /* switch */
		}
#endif

#ifdef THREADS
		if(sock_incoming==0)
			continue;
#endif

#ifdef DEBUG
#ifdef AIO
		if(sock_incoming>0)
#endif
			fprintf(stderr, "Accepted connection from %s:%i\n", 
					strdup(inet_ntoa(addr_incoming.sin_addr)), ntohs(addr_incoming.sin_port));
#endif

#ifdef THREADS
		rc = pthread_create(&pthr, &pattr, &connection, (void *) sock_incoming);
		if(rc) {
#ifdef DEBUG
			perror("pthread_create");
#endif
			if(rc!=EAGAIN)
				exit(errno);
		}
#else /* ifndef THREADS */

#ifdef SELECT
		if(sock_incoming>0)
			sock_final = init_sock_final(&addr_final);
		copy(sock_final, sock_incoming);
#endif

#ifdef AIO
		/* creating new transfer entry */
		if(sock_incoming>0) {
			// creating new entry
			sock_final = init_sock_final(&addr_final);
			
			struct conn_t *conn = (struct conn_t *) malloc(sizeof(struct conn_t));
			conn->sock_incoming = sock_incoming;
			conn->sock_final = sock_final;

			struct aiocb *aiocbp = (struct aiocb *) malloc(sizeof(struct aiocb));
			memset(aiocbp, 0, sizeof(struct aiocb));
			aiocbp->aio_fildes = sock_incoming;
			aiocbp->aio_buf = conn->buf;
			aiocbp->aio_nbytes = BLOCK;

			conn->aiocbp = aiocbp;
			conn->write = 1;

			// adding it to list
			struct list *current = (struct list *) malloc(sizeof(struct list));
			current->elem = conn;

			current->next = trans;
			current->prev = NULL;
			if(trans)
				trans->prev = current;
			trans = current;
		}

		struct list *_trans;
		for(_trans = trans; _trans; _trans = _trans->next) {
			rc = copy(_trans->elem);
			if(rc!=EXIT_SUCCESS) {
				// closing connection
				shutdown(_trans->elem->sock_final, 0);
				shutdown(_trans->elem->sock_incoming, 0);
				close(_trans->elem->sock_final);
				close(_trans->elem->sock_incoming);
				
				// deleting _trans
				if(_trans==trans) {
					trans = trans->next;
					if(trans)
						trans->prev = NULL;
					break;
				} else {
					struct list *n = _trans->next, *p = _trans->prev;
					if(p&&p->next)
						p->next = n;
					if(n&&n->prev)
						n->prev = p;
					break;
				}
			}
		} /* for */

#endif /* AIO */

#endif /* #ifndef THREADS */

	} /* while */
	return EXIT_SUCCESS;
}



