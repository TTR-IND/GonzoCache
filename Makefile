CC       ?= gcc
CFLAGS   ?= -O2 -Wall -Wextra -Wno-unused-parameter
LDLIBS   := -lm
PREFIX   ?= /usr/local
SBINDIR  := $(PREFIX)/sbin

.PHONY: all clean install uninstall

all: gonzocache

gonzocache: gonzocache.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

clean:
	rm -f gonzocache

install: gonzocache
	install -m 0755 -o root -g root gonzocache $(SBINDIR)/gonzocache
	@echo "Binary installed to $(SBINDIR)/gonzocache."
	@echo "For OpenRC service + login-autostart setup, use ./install.sh instead, or see the README."

uninstall:
	rm -f $(SBINDIR)/gonzocache
