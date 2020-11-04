CC=gcc
CFLAGS=-Wall -ggdb

default: master user

shared.o: shared.c master.h
	$(CC) $(CFLAGS) -c shared.c

blockedq.o: blockedq.c blockedq.h
	$(CC) $(CFLAGS) -c blockedq.c

feedbackq.o: feedbackq.c feedbackq.h
	$(CC) $(CFLAGS) -c feedbackq.c

master: master.c master.h feedbackq.o blockedq.o
	$(CC) $(CFLAGS) master.c feedbackq.o blockedq.o -o master

user: user.c master.h
	$(CC) $(CFLAGS) user.c -o user

clean:
	rm -f master user *.o
