CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lpthread
PREFIX = /usr/local/bin

all: claude-ratelimit-dashboard

claude-ratelimit-dashboard: main.c render.c render.h
	$(CC) $(CFLAGS) -o $@ main.c render.c $(LDFLAGS)

install: all
	install -m 755 claude-ratelimit-dashboard $(PREFIX)/claude-ratelimit-dashboard

clean:
	rm -f claude-ratelimit-dashboard

.PHONY: all install clean
