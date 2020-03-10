
CC=gcc
CFLAGS=-std=c99 -g -Wall
LIBS=-lreadline

all: yash.o
	$(CC) $(CFLAGS) -o yash yash.o $(LIBS)

clean:
	rm *.o
	rm yash

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $< 	
