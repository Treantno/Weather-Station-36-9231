CC      ?= cc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=c11 -Wall -Wextra -D_DEFAULT_SOURCE
PKG     ?= pkg-config
HIDAPI  ?= hidapi-hidraw

DEPS_CFLAGS := $(shell $(PKG) --cflags $(HIDAPI) libmosquitto)
DEPS_LIBS   := $(shell $(PKG) --libs $(HIDAPI) libmosquitto)

PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin

SRC = src/main.c src/protocol.c src/station.c src/mqtt.c

all: wsmqtt

wsmqtt: $(SRC) src/*.h
	$(CC) $(CFLAGS) $(DEPS_CFLAGS) -o $@ $(SRC) $(DEPS_LIBS) -lm $(LDFLAGS)

tests/test_decode: tests/test_decode.c src/protocol.c src/protocol.h
	$(CC) $(CFLAGS) -o $@ tests/test_decode.c src/protocol.c -lm

test: tests/test_decode
	./tests/test_decode

install: wsmqtt
	install -D -m 0755 wsmqtt $(DESTDIR)$(BINDIR)/wsmqtt
	install -D -m 0644 contrib/99-weatherstation.rules $(DESTDIR)/etc/udev/rules.d/99-weatherstation.rules
	install -D -m 0644 contrib/wsmqtt.service $(DESTDIR)/etc/systemd/system/wsmqtt.service
	test -e $(DESTDIR)/etc/wsmqtt.conf || install -D -m 0600 contrib/wsmqtt.conf.example $(DESTDIR)/etc/wsmqtt.conf

clean:
	rm -f wsmqtt tests/test_decode

.PHONY: all test install clean
