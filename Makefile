CC      = gcc
CFLAGS  = -Wall -O2 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION
LDFLAGS = -lws2_32

all: server.exe client.exe

server.exe: server.c sqlite3.c
	$(CC) $(CFLAGS) -o server.exe server.c sqlite3.c $(LDFLAGS)

client.exe: client.c
	$(CC) $(CFLAGS) -o client.exe client.c $(LDFLAGS)

clean:
	del /q server.exe client.exe 2>nul

.PHONY: all clean
