# argonine-inputd
# Cross-compile: make CC=aarch64-linux-musl-gcc CFLAGS="-O2 -Wall -std=c11 -static"
# On-device:     apk add build-base linux-headers && make && make install

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11
TARGET  = inputd

.PHONY: all clean install

all: $(TARGET)

$(TARGET): inputd.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET)      $(DESTDIR)/usr/local/bin/$(TARGET)
	install -Dm755 inputd.openrc  $(DESTDIR)/etc/init.d/inputd
	[ -e $(DESTDIR)/etc/inputd.conf ] || install -Dm644 inputd.conf $(DESTDIR)/etc/inputd.conf

clean:
	rm -f $(TARGET)
