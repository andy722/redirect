CC = gcc
LD = gcc

TYPE = THREADS

CFLAGS = -DCOLORS
DEBUG = -g -Wall -DDEBUG
RELEASE = $(CFLAGS) -O2
TARGET = redir
PATH = /usr/bin
MANPATH = /usr/share/man/man8

all:	
	@echo -e "Compile with \"make <type>[-debug]\", type is one of the following: aio, threads, select, redir.\nIf in trouble, choose redir.\nInstall with \"make install\""
redir:	threads
redir-debug:	threads-debug

### DEBUG ###
aio-debug:	main.c
	@echo "Compiling debug version with support of async. calls"
	$(CC) $(DEBUG) $(CFLAGS) -DAIO -c -g main.c
	$(LD) -lrt main.o -o $(TARGET)
threads-debug:main.c
	@echo "Compiling multithreaded version"
	$(CC) $(DEBUG) $(CFLAGS) -DTHREADS -D_REENTRANT -c -g main.c
	$(LD) -lpthread main.o -o $(TARGET)
select-debug:	main.c
	@echo "Compiling with support of SELECT calls"
	$(CC) $(DEBUG) $(CFLAGS) -DSELECT -c -g main.c
	$(LD)  main.o -o $(TARGET)

### RELEASE ###
aio:	main.c
	@echo "Compiling with support of async. calls"
	$(CC) $(RELEASE) -DAIO -c -g main.c
	$(LD) -lrt main.o -o $(TARGET)
threads:main.c
	@echo "Compiling multithreaded version"
	$(CC) $(RELEASE) -DTHREADS -D_REENTRANT -c -g main.c
	$(LD) -lpthread main.o -o $(TARGET)
select:	main.c
	@echo "Compiling with support of SELECT calls"
	$(CC) $(RELEASE) -DSELECT -c -g main.c
	$(LD)  main.o -o $(TARGET)

### GLOBAL ###
clean:
	/bin/rm -f *.o
#release:main.c
#	$(CC) $(RELEASE) -c -g main.c
#	$(LD) main.o -o $(TARGET)
#	strip $(TARGET)
install:
	/bin/cp -f $(TARGET) $(PATH)
	cp redir.man $(MANPATH)
