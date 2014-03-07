/*
 * Copyright (c) 2008 Bob Beck <beck@obtuse.com>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * select_server.c - an example of using select to implement a non-forking
 * server. In this case this is an "echo" server - it simply reads
 * input from clients, and echoes it back again to them, one line at
 * a time.
 *
 * to use, cc -DDEBUG -o select_server select_server.c
 * or cc -o select_server select_server.c after you read the code :)
 *
 * then run with select_server PORT
 * where PORT is some numeric port you want to listen on.
 *
 * to connect to it, then use telnet or nc
 * i.e.
 * telnet localhost PORT
 * or
 * nc localhost PORT
 * 
 */


#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <netinet/in.h>

#include <err.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "server_s.h"

char *str_from_file(FILE *requested_file, int req_filed, int *file_size);

/* we use this structure to keep track of each connection to us */
struct con {
	int sd; 	/* the socket for this connection */
	int state; 	/* the state of the connection */
	struct sockaddr_in sa; /* the sockaddr of the connection */
	size_t  slen;   /* the sockaddr length of the connection */
	char *buf;	/* a buffer to store the characters read in */
	char *bp;	/* where we are in the buffer */
	size_t bs;	/* total size of the buffer */
	size_t bl;	/* how much we have left to read/write */
        char log_string[300];
        int is_OK;
        int file_size;
};

/*
 * we will accept a maximum of 256 simultaneous connections to us.
 * While you could make this a dynamically allocated array, and
 * use a variable for maxconn instead of a #define, that is left
 * as an exercise to the reader. The necessity of doing this
 * in the real world is debatable. Even the most monsterous of
 * daemons on real unix machines can typically only deal with several
 * thousand simultaeous connections, due to limitations of the
 * operating system and process limits. so it might not be worth it
 * in general to make this part fully dynamic, depending on your
 * application. For example, there is no point in allowing for
 * more connections than the kernel will allow your process to
 * have open files - so run 'ulimit -a' to see what that is as
 * an example of a typical reasonable number, and bear in mind
 * you may have a few more files open than just your sockets
 * in order to do anything really useful
 */
void get_date(char *date_time);
#define MAXCONN 256
struct con connections[MAXCONN];

#define BUF_ASIZE 1000 /* how much buf will we allocate at a time. */

/* states used in struct con. */
#define STATE_UNUSED 0
#define STATE_READING 1
#define STATE_WRITING 2

#define DEBUG 1

#define MAX_DATE_STRING 200
FILE * err_file;

static void usage()
{
	extern char * __progname;
	fprintf(stderr,"usage: %s portnumber host_path log_path\n",__progname);
	exit(1);
}


/*
 * get a free connection structure, to save a new connection in
 */
struct con * get_free_conn()
{
	int i;
	for (i = 0; i < MAXCONN; i++) {
		if (connections[i].state == STATE_UNUSED)
			return(&connections[i]);
	}
	return(NULL); /* we're all full - indicate this to our caller */
}



/*
 * close or initialize a connection - resets a connection to the default,
 * unused state.
 */
void closecon (struct con *cp, int initflag, char *argv[])
{
	if (!initflag) {
		if (cp->sd != -1)
			close(cp->sd); /* close the socket */
		free(cp->buf); /* free up our buffer */
	}
	memset(cp, 0, sizeof(struct con)); /* zero out the con struct */
	cp->buf = NULL; /* unnecessary because of memset above, but put here
			 * to remind you NULL is 0.
			 */
	cp->sd = -1;
}

/*
 * handlewrite - deal with a connection that we want to write stuff
 * to. assumes the caller has checked that cp->sd is writeable
 * by using select(). once we write everything out, change the
 * state of the connection to the reading state.
 */
void handlewrite(struct con *cp, char *argv[])
{
	ssize_t i;

	/*
	 * assuming before we are called, cp->sd was put into an fd_set
	 * and checked for writeability by select, we know that we can
	 * do one write() and write something. We are *NOT* guaranteed
	 * how much we can write. So while we will be able to write bytes
	 * we don't know if we will get a whole line, or even how much
	 * we will get - so we do *exactly* one write. and keep track
	 * of where we are. If we don't want to block, we can't do
	 * multiple writes to write everything out without calling
	 * select() again between writes.
	 */

	i = write(cp->sd, cp->bp, cp->bl);
	if (i == -1) {
		if (errno != EAGAIN) {
			/* the write failed */
			closecon(cp, 0, argv);
		}
		/*
		 * note if EAGAIN, we just return, and let our caller
		 * decide to call us again when socket is writable
		 */
		return;
	}
	/* otherwise, something ok happened */
	cp->bp += i; /* move where we are */
	cp->bl -= i; /* decrease how much we have left to write */
	if (cp->bl == 0) {
		/* we wrote it all out, hooray, so go back to reading */
		cp->state = STATE_READING;
		cp->bl = cp->bs; /* we can read up to this much */
		cp->bp = cp->buf;	    /* we'll start at the beginning */
	}
}

/*
 * handleread - deal with a connection that we want to read stuff
 * from. assumes the caller has checked that cp->sd is writeable
 * by using select(). If a newline is seen at the end of what we
 * are reading, change the state of this connection to the writing
 * state.
 */
void handleread(struct con *cp, char *argv[])
{
	ssize_t i;
	int file_size, req_filed;
	char c1, c2;
	size_t file_chars;
        char itoa[12];
	char *complete_path;
	char *file_start;
	char *request_type, *file_path, *protocol;
	FILE *requested_file;
	/*
	 * first, let's make sure we have enough room to do a
	 * decent sized read.
	 */

	if (cp->bl < 10) {
		char *tmp;
		tmp = realloc(cp->buf, (cp->bs + BUF_ASIZE) * sizeof(char));
		if (tmp == NULL) {
			/* we're out of memory */
			closecon(cp, 0, argv);
			return;
		}
		cp->buf = tmp;
		cp->bs += BUF_ASIZE;
		cp->bl += BUF_ASIZE;
		cp->bp = cp->buf + (cp->bs - cp->bl);
	}

	/*
	 * assuming before we are called, cp->sd was put into an fd_set
	 * and checked for readability by select, we know that we can
	 * do one read() and get something. We are *NOT* guaranteed
	 * how much we can get. So while we will be able to read bytes
	 * we don't know if we will get a whole line, or even how much
	 * we will get - so we do *exactly* one read. and keep track
	 * of where we are. If we don't want to block, we can't do
	 * multiple reads to read in a whole line without calling
	 * select() to check for readability between each read.
	 */
	i = read(cp->sd, cp->bp, cp->bl);
	if (i == 0) {
		/* 0 byte read means the connection got closed */
		closecon(cp, 0, argv);
		return;
	}
	if (i == -1) {
		if (errno != EAGAIN) {
			/* read failed */
			err(1, "read failed! sd %d\n", cp->sd);
			closecon(cp, 0, argv);
		}
		/*
		 * note if EAGAIN, we just return, and let our caller
		 * decide to call us again when socket is readable
		 */
		return;
	}
	/*
	 * ok we really got something read. change where we're
	 * pointing
	 */
	cp->bp += i;
	cp->bl -= i;

	/*
	 * now check to see if we should change state - i.e. we got
	 * a newline on the end of the buffer
	 */
	if (*(cp->bp - 1) == '\n') {
		cp->state = STATE_WRITING;
		cp->bl = cp->bp - cp->buf; 
		cp->bp = cp->buf;
		request_type = calloc(cp->bl, sizeof(char));
		file_path = calloc(cp->bl + strlen(argv[2]), sizeof(char));
		protocol = calloc(cp->bl, sizeof(char));
		sscanf(cp->buf, "%s %s %s", request_type, file_path, protocol);
                c1 = *(cp->buf + strlen(cp->buf) -2);
                c2 = *(cp->buf + strlen(cp->buf) -1);
                if(!( ((c1 == '\n') && (c2 == '\n')) || ((c1 =='\r') && (c2=='\n')) )){
                   char fail_string[1000];
                   char date_string[MAX_DATE_STRING];
                   memset(date_string, '\0', MAX_DATE_STRING);
                   memset(fail_string, '\0', 1000);
                   get_date(date_string);
                   strcpy(fail_string, "HTTP/1.1 400 Bad Request\nDate: ");
                   strcat(fail_string, date_string);
                   strcat(fail_string, "\nContent-Type: text/html\nContent-Length: 107\n\n<html><body>\n<h2>Malformed Request</h2>\nYour browser sent a request I could not understand.\n</body></html>");
		  free(cp->buf);
		  cp->bl = strlen(fail_string);
		  cp->bp = fail_string;
		  cp->buf = fail_string;
                  strcpy(cp->log_string, date_string);
                  strcat(cp->log_string, "\t");
                  //strcat(cp->log_string, inet_ntoa((cp->sa).sin_addr));
                  strcat(cp->log_string, "\t");
                  strcat(cp->log_string, request_type);
                  strcat(cp->log_string, " ");
                  strcat(cp->log_string, file_path);
                  strcat(cp->log_string, protocol);
                  strcat(cp->log_string, "\t");
                  strcat(cp->log_string, "400 Bad Request");
                  cp->is_OK = 0;
		  free(request_type);
		  free(file_path);
		  free(protocol);
                  return;
                  }
                else{
		complete_path = calloc((strlen(argv[1]) + strlen(file_path)+2), 
				       sizeof(char));
		strcpy(complete_path, ".");
		strcat(complete_path, argv[2]);
		strcat(complete_path, file_path);
                char date_string[MAX_DATE_STRING];
                memset(date_string, '\0', MAX_DATE_STRING);
                get_date(date_string);
		if(!strcmp(request_type,"GET") && !strcmp(protocol,"HTTP/1.1")){
		  if((requested_file = fopen(complete_path, "r")) == NULL){
		   char fail_string[1000];
                   memset(fail_string, '\0', 1000);

                   if(errno == EACCES){
                           strcpy(fail_string, "HTTP/1.1 403 Forbidden\nDate: ");
                           strcat(fail_string, date_string);
                           strcat(fail_string, "\nContent-Type: text/html\nContent-Length: 117\n\n<html><body>\n<h2>Permission Denied</h2>\nYou asked for a document you are not permitted to see. It sucks to be you.\n</body></html>");

	        	  free(cp->buf);
	        	  cp->bp = fail_string;
	        	  cp->buf = fail_string;
                          cp->bl = strlen(fail_string);
                          strcpy(cp->log_string, date_string);
                          strcat(cp->log_string, "\t");
                          /*char* ip = inet_ntoa(((cp->sa).sin_addr));
                          strcat(cp->log_string, ip);*/
                          strcat(cp->log_string, "\t");
                          strcat(cp->log_string, request_type);
                          strcat(cp->log_string, " ");
                          strcat(cp->log_string, file_path);
                          strcat(cp->log_string, protocol);
                          strcat(cp->log_string, "\t");
                          strcat(cp->log_string, "403 Forbidden");
                          cp->is_OK = 0;
	        	  free(request_type);
	        	  free(file_path);
	        	  free(protocol);
	        	  free(complete_path);
                          return;
                       }
                  else{
                           strcpy(fail_string, "HTTP/1.1 404 Not Found\nDate: ");
                           strcat(fail_string, date_string);
                           strcat(fail_string, "\nContent-Type: text/html\nContent-Length: 117\n\n<html><body>\n<h2>Document not found</h2>\nYou asked for a document that doesn\'t exist. That is so sad.\n</body></html>");
	        	  free(cp->buf);
	        	  cp->bp = fail_string;
	        	  cp->buf = fail_string;
                          cp->bl = strlen(fail_string);
                          strcpy(cp->log_string, date_string);
                          strcat(cp->log_string, "\t");
                          //strcat(cp->log_string, inet_ntoa((cp->sa).sin_addr));
                          strcat(cp->log_string, "\t");
                          strcat(cp->log_string, request_type);
                          strcat(cp->log_string, " ");
                          strcat(cp->log_string, file_path);
                          strcat(cp->log_string, protocol);
                          strcat(cp->log_string, "\t");
                          strcat(cp->log_string, "404 Not Found");
                          cp->is_OK = 0;
	        	  free(request_type);
	        	  free(file_path);
	        	  free(protocol);
	        	  free(complete_path);
                          return;
		  }
                }
                else{
		  if((req_filed = open(complete_path)) < 0)
		    file_start = str_from_file(requested_file, req_filed, &file_size);
		    fclose(requested_file);
		    free(cp->buf);
		    cp->bp = file_start;
		    cp->buf = file_start;
                    cp->bl = strlen(file_start);
                    strcpy(cp->log_string, date_string);
                    strcat(cp->log_string, "\t");
                    //strcat(cp->log_string, inet_ntoa((cp->sa).sin_addr));
                    strcat(cp->log_string, "\t");
                    strcat(cp->log_string, request_type);
                    strcat(cp->log_string, " ");
                    strcat(cp->log_string, file_path);
                    strcat(cp->log_string, protocol);
                    strcat(cp->log_string, "\t");
                    strcat(cp->log_string, "200 OK ");
                    snprintf(itoa,12, "%d", file_size);
                    //cp->file_size = itoa;
                    cp->is_OK = 1;
		    close(req_filed);
		    free(request_type);
		    free(file_path);
		    free(protocol);
		    free(complete_path);
                    fprintf(err_file, "After struct setup: %s\n", cp->buf);
                    fflush(err_file);
                    return;
                }
        }
		else{
		   char fail_string[1000];
                   char date_string[MAX_DATE_STRING];
                   memset(date_string, '\0', MAX_DATE_STRING);
                   memset(fail_string, '\0', 1000);
                   get_date(date_string);
                   strcpy(fail_string, "HTTP/1.1 400 Bad Request\nDate: ");
                   strcat(fail_string, date_string);
                   strcat(fail_string, "\nContent-Type: text/html\nContent-Length: 107\n\n<html><body>\n<h2>Malformed Request</h2>\nYour browser sent a request I could not understand.\n</body></html>");
		  free(cp->buf);
		  cp->bp = fail_string;
		  cp->buf = fail_string;
                  cp->bl = strlen(fail_string);
                  strcpy(cp->log_string, date_string);
                  strcat(cp->log_string, "\t");
                  //strcat(cp->log_string, inet_ntoa((cp->sa).sin_addr));
                  strcat(cp->log_string, "\t");
                  strcat(cp->log_string, request_type);
                  strcat(cp->log_string, " ");
                  strcat(cp->log_string, file_path);
                  strcat(cp->log_string, protocol);
                  strcat(cp->log_string, "\t");
                  strcat(cp->log_string, "400 Bad Request");
                  cp->is_OK = 0;
		  free(request_type);
		  free(file_path);
		  free(protocol);
		  free(complete_path);
                  return;
		}
            }      
        }
}
char *str_from_file(FILE *requested_file, int req_filed, int *file_size){
	char c;
	char *file_start, *file_position;
	struct stat file_stat;
	
	//getting a time string following the example on the man page for 
	//strftime
	char date_time[MAX_DATE_STRING]; 
        get_date(date_time);


	fstat(req_filed, &file_stat);
	*file_size = ((int)((u_long)file_stat.st_size));
	
	file_start = malloc(*file_size + 300);
	file_position = file_start;
	strcpy(file_start, "HTTP/1.1 200 OK\nDate: ");
	strcat(file_start, date_time);
        sprintf(file_start, "%s\nContent-Type: text/html\nContent-Length: %d\n", file_start, *file_size);
	*file_size += strlen(file_start);
	fprintf(err_file, "file size: %d\n", *file_size);
	file_position = file_start + strlen(file_start);
		    fprintf(err_file, "before loop\n");
		    fflush(err_file);
	while((c = fgetc(requested_file)) != EOF){
	  *file_position = c;
	  file_position++;
	}
	return file_start;
}

void get_date(char *date_time){
	time_t t;
	struct tm * tmp;
	t = time(NULL);
	tmp = localtime(&t);
	if(tmp == NULL){
	  perror("localtime");
	  exit(EXIT_FAILURE);
	}
	if(strftime(date_time, MAX_DATE_STRING, "%a %d %b %G %H:%M:%S %Z",tmp) == 0){
	  perror("strftime returned 0");
	  exit(EXIT_FAILURE);
	}
}

int main(int argc,  char *argv[])
{
	struct sockaddr_in sockname;
	int max = -1, omax;	     /* the biggest value sd. for select */
	int sd;			     /* our listen socket */
	fd_set *readable = NULL , *writable = NULL; /* fd_sets for select */
	u_short port;
	u_long p;
	char *ep;
	int i;
	err_file = fopen("err.txt", "w");
	/*
	 * first, figure out what port we will listen on - it should
	 * be our first parameter.
	 */

	if (argc != 4)
		usage();
		errno = 0;
        p = strtoul(argv[1], &ep, 10);
        if (*argv[1] == '\0' || *ep != '\0') {
		/* parameter wasn't a number, or was empty */
		fprintf(stderr, "%s - not a number\n", argv[1]);
		usage();
	}
        if ((errno == ERANGE && p == ULONG_MAX) || (p > USHRT_MAX)) {
		/* It's a number, but it either can't fit in an unsigned
		 * long, or is too big for an unsigned short
		 */
		fprintf(stderr, "%s - value out of range\n", argv[1]);
		usage();
	}
	/* now safe to do this */
	port = p;

	/* now before we get going, decide if we want to daemonize, that
	 * is, run in the background like a real system process
	 */
#if !DEBUG
	/* don't daemonize if we compile with -DDEBUG */
	if (daemon(1, 0) == -1)
		err(1, "daemon() failed");
#endif

	/* now off to the races - let's set up our listening socket */
	memset(&sockname, 0, sizeof(sockname));
	sockname.sin_family = AF_INET;
	sockname.sin_port = htons(port);
	sockname.sin_addr.s_addr = htonl(INADDR_ANY);
	sd=socket(AF_INET,SOCK_STREAM,0);
	if ( sd == -1)
		err(1, "socket failed");

	if (bind(sd, (struct sockaddr *) &sockname, sizeof(sockname)) == -1)
		err(1, "bind failed");

	if (listen(sd,3) == -1)
		err(1, "listen failed");

	/* 
	 * We're now bound, and listening for connections on "sd".
	 * Each call to "accept" will return us a descriptor talking to
	 * a connected client.
	 */

	/*
	 * finally - the main loop.  accept connections and deal with 'em
	 */
#if DEBUG
	/*
	 * since we'll be running as a daemon if we're not compiled with
	 * -DDEBUG, we better not be using printf - since stdout will be
	 * unusable
	 */
	printf("Server up and listening for connections on port %u\n", port);
#endif	
        
	/* initialize all our connection structures */
	for (i = 0; i < MAXCONN; i++)
		closecon(&connections[i], 1, argv);

	for(;;) {
		int i;
		int maxfd = -1; /* the biggest value sd we are interested in.*/

		/*
		 * first we have to initialize the fd_sets to keep
		 * track of readable and writable sockets. we have
		 * to make sure we have fd_sets that are big enough
		 * to hold our largest valued socket descriptor.
		 * so first, we find the max value by iterating through
		 * all the connections, and then we allocate fd sets
		 * that are big enough, if they aren't already.
		 */
		omax = max;
		max = sd; /* the listen socket */

		for (i = 0; i < MAXCONN; i++) {
			if (connections[i].sd > max)
				max = connections[i].sd;
		}

		if (max > omax) {
			/* we need bigger fd_sets allocated */

			/* free the old ones - does nothing if they are NULL */
			free(readable);
			free(writable);

			/*
			 * this is how to allocate fd_sets for select
			 */
			readable = (fd_set *)calloc(howmany(max + 1, NFDBITS),
			    sizeof(fd_mask));
			if (readable == NULL)
				err(1, "out of memory");
			writable = (fd_set *)calloc(howmany(max + 1, NFDBITS),
			    sizeof(fd_mask));
			if (writable == NULL)
				err(1, "out of memory");
			omax = max;
			/*
			 * note that calloc always returns 0'ed memory,
			 * (unlike malloc) so these sets are all set to 0
			 * and ready to go
			 */
		} else {
			/*
			 * our allocated sets are big enough, just make
			 * sure they are cleared to 0. 
			 */
			memset(readable, 0, howmany(max+1, NFDBITS) *
			    sizeof(fd_mask));
			memset(writable, 0, howmany(max+1, NFDBITS) *
			    sizeof(fd_mask));
		}

		/*
		 * Now, we decide which sockets we are interested
		 * in reading and writing, by setting the corresponding
		 * bit in the readable and writable fd_sets.
		 */

		/*
		 * we are always interesting in reading from the
		 * listening socket. so put it in the read set.
		 */

		FD_SET(sd, readable);
		if (maxfd < sd)
			maxfd = sd;

		/*
		 * now go through the list of connections, and if we
		 * are interested in reading from, or writing to, the
		 * connection's socket, put it in the readable, or
		 * writable fd_set - in preparation to call select
		 * to tell us which ones we can read and write to.
		 */
		for (i = 0; i<MAXCONN; i++) {
			if (connections[i].state == STATE_READING) {
				FD_SET(connections[i].sd, readable);
				if (maxfd < connections[i].sd)
					maxfd = connections[i].sd;
			}
			if (connections[i].state == STATE_WRITING) {
				FD_SET(connections[i].sd, writable);
				if (maxfd < connections[i].sd)
					maxfd = connections[i].sd;
			}
		}

		/*
		 * finally, we can call select. we have filled in "readable"
		 * and "writable" with everything we are interested in, and
		 * when select returns, it will indicate in each fd_set
		 * which sockets are readable and writable
		 */
		i = select(maxfd + 1, readable, writable, NULL,NULL);
		if (i == -1  && errno != EINTR)
			err(1, "select failed");
		if (i > 0) {

			/* something is readable or writable... */

			/*
			 * First things first.  check the listen socket.
			 * If it was readable - we have a new connection
			 * to accept.
			 */

			if (FD_ISSET(sd, readable)) {
				struct con *cp;
				int newsd;
				socklen_t slen;
				struct sockaddr_in sa;

				slen = sizeof(sa);
				newsd = accept(sd, (struct sockaddr *)&sa,
				    &slen);
				if (newsd == -1)
					err(1, "accept failed");

				cp = get_free_conn();
				if (cp == NULL) {
					/*
					 * we have no connection structures
					 * so we close connection to our
					 * client to not leave him hanging
					 * because we are too busy to
					 * service his request
					 */
					close(newsd);
				} else {
					/*
					 * ok, if this worked, we now have a
					 * new connection. set him up to be
					 * READING so we do something with him
					 */
					cp->state = STATE_READING;
					cp->sd = newsd;
					cp->slen = slen;
					memcpy(&cp->sa, &sa, sizeof(sa));
				}
			}
			/*
			 * now, iterate through all of our connections,
			 * check to see if they are readble or writable,
			 * and if so, do a read or write accordingly 
			 */
			for (i = 0; i<MAXCONN; i++) {
				if ((connections[i].state == STATE_READING) &&
				    FD_ISSET(connections[i].sd, readable))
				  handleread(&connections[i], argv);
				if ((connections[i].state == STATE_WRITING) &&
				    FD_ISSET(connections[i].sd, writable))
					handlewrite(&connections[i], argv);
			}
		}
	}
}
