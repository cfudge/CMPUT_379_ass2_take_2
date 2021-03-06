CC=gcc
FLAGS=-M32
all: server_f server_p server_s
.PHONY: all
server_f:server_f.o
	$(CC) -o  server_f server_f.o
server_f.o:server_f.c server_f.h
	$(CC) -c server_f.c server_f.h
server_p:server_p.o
	$(CC) -o  server_p server_p.o
server_p.o:server_p.c server_p.h
	$(CC) -c server_p.c server_p.h
server_s:server_s.o
	$(CC) -o  server_s server_s.o
server_s.o:server_s.c server_s.h
	$(CC) -c server_s.c server_s.h