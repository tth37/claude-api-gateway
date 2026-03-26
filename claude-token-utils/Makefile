CC = gcc
CFLAGS = -O2 -Wall
LDFLAGS = -lcrypto
PREFIX = /usr/local/bin

all: claude-token-encrypt claude-token-verify

claude-token-encrypt: claude-token-encrypt.c crypto.c crypto.h
	$(CC) $(CFLAGS) -o $@ claude-token-encrypt.c crypto.c $(LDFLAGS)

claude-token-verify: claude-token-verify.c crypto.c crypto.h
	$(CC) $(CFLAGS) -o $@ claude-token-verify.c crypto.c $(LDFLAGS)

install: all
	install -m 755 claude-token-encrypt $(PREFIX)/claude-token-encrypt
	install -m 755 claude-token-verify $(PREFIX)/claude-token-verify

clean:
	rm -f claude-token-encrypt claude-token-verify

.PHONY: all install clean
