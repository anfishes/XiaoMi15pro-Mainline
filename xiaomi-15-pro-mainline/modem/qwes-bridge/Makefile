CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra
LDFLAGS ?= -pthread

all: qwes-bridge

qwes-bridge: qwes_bridge.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

install: qwes-bridge
	install -Dm755 qwes-bridge $(DESTDIR)/usr/local/bin/qwes-bridge

clean:
	rm -f qwes-bridge
