# Needs the SQLite dev library:
#   Ubuntu/Codespaces:  sudo apt install libsqlite3-dev
.RECIPEPREFIX = >
CC = gcc
CFLAGS = -Wall -Wextra -O2

appointease: main.c
> $(CC) $(CFLAGS) main.c -o appointease -lsqlite3

clean:
> rm -f appointease appointease.db
