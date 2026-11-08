CC = gcc
CFLAGS = -Wall -Wextra -O2
LDFLAGS = -lsqlite3

all: appointease

appointease: backend/main.c
	$(CC) $(CFLAGS) backend/main.c -o appointease $(LDFLAGS)

clean:
	rm -f appointease appointease.exe backend/appointease backend/appointease.exe *.db

.PHONY: all clean
