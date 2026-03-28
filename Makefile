CC      = cc
CFLAGS  = -Wall -Wextra -O2
LDFLAGS = -lssl -lcrypto -lpthread

TARGET  = claude-api-gateway
SRCS    = main.c encrypt.c setup.c verifier.c dashboard.c server.c service.c crypto.c httpserver.c render.c
OBJS    = $(SRCS:.c=.o)

PREFIX  = /usr/local/bin

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	install -m 755 $(TARGET) $(PREFIX)/$(TARGET)

uninstall:
	rm -f $(PREFIX)/$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install uninstall clean
