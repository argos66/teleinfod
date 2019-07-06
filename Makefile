CC=gcc
CFLAGS=-Os -Wextra -Wall -std=gnu11 -pedantic
LDFLAGS=-lpthread -lsqlite3
EXEC=teleinfod

all: $(EXEC)

teleinfod:
	$(CC) $(CFLAGS) -o teleinfod teleinfod.c $(LDFLAGS)

clean:
	rm -f *.o
	rm -f $(EXEC)
