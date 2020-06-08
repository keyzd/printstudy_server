CC = gcc
CFLAGS = -Wall -Wextra
LDFLAGS = `mariadb_config --cflags --libs`

PROGNAME = ps_serv
SRCMODULES = ps_serv.c

all: $(SRCMODULES)
	$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $(PROGNAME)

