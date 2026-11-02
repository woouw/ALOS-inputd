# ALOS-inputd
# Cross-compile: make CC=aarch64-linux-musl-gcc
# On-device:    apk add build-base && make

CC     ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -D_POSIX_C_SOURCE=200809L
TARGET  = inputd

.PHONY: all clean install

all: $(TARGET)

$(TARGET): inputd.c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)/usr/local/bin/$(TARGET)
	install -Dm644 inputd.openrc $(DESTDIR)/etc/init.d/inputd
	install -Dm644 inputd.conf   $(DESTDIR)/etc/inputd.conf

clean:
	rm -f $(TARGET)
