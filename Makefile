# SPDX-License-Identifier: GPL-2.0-only
VERSION = 0.1.0
NAME := wch-isp

# Install paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man
UDEVPREFIX = $(PREFIX)/lib/udev

#make CROSS_COMPILE=i686-w64-mingw32- INCS="-Imingw32/include -Imingw32/include/libusb-1.0" LIBS="-Lmingw32/lib mingw32/bin/libusb-1.0.dll -lyaml"
ifneq ($(CROSS_COMPILE),)
CC = $(CROSS_COMPILE)gcc
LD = $(CROSS_COMPILE)ld
endif

#CFLAGS += -DPREFIX=\"$(PREFIX)\" -DNAME=\"$(NAME)\" -g
PKG_CONFIG = pkg-config

ifneq ($(OPTIONS),small)
# include and libs
INCS += `$(PKG_CONFIG) --cflags libusb-1.0`  `$(PKG_CONFIG) --cflags yaml-0.1`
LIBS += `$(PKG_CONFIG) --libs libusb-1.0`  `$(PKG_CONFIG) --libs yaml-0.1`
else
# include and libs
INCS += -DBUILD_SMALL
LIBS += 
endif

# Flags
WCHISP_CPPFLAGS = -DVERSION=\"$(VERSION)\" $(CPPFLAGS)
WCHISP_CFLAGS = -Wall -O2 $(INCS) $(CFLAGS)
WCHISP_LDFLAGS = $(LIBS) $(LDFLAGS)

SRC = main.c wch_if_usb.c wch_if_uart.c wch_yaml_parse.c
#SRC = $(NAME).c
HDR = arg.h devices.h
OBJ = $(SRC:.c=.o)
BIN = $(NAME)
MAN = $(NAME).1
DISTFILES = $(SRC) $(HDR) *.man 50-wchisp.rules Makefile


all: $(BIN) $(MAN)

%.1:	%.man
	sed "s/VERSION/$(VERSION)/g" < $< > $@

small:
	make OPTIONS="small" all

$(BIN): $(OBJ)
	$(CC) -o $@ $^ $(WCHISP_LDFLAGS)
.c.o:
	$(CC) $(WCHISP_CFLAGS) $(WCHISP_CPPFLAGS) -c $<

install: all
	install -D $(BIN) $(DESTDIR)$(PREFIX)/bin/${BIN}
	install -m644 -D $(MAN) $(DESTDIR)$(MANPREFIX)/man1/$(MAN)
	mkdir -p $(DESTDIR)$(PREFIX)/share/$(NAME)
	cp -a devices $(DESTDIR)$(PREFIX)/share/$(NAME)/
	install -D 50-wchisp.rules $(DESTDIR)$(UDEVPREFIX)/rules.d/50-wchisp.rules

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(MANPREFIX)/man1/$(MAN)
	rm -rf $(DESTDIR)$(PREFIX)/share/$(NAME)
	rm -f $(DESTDIR)$(UDEVPREFIX)/rules.d/50-wchisp.rules

dist:
	mkdir -p $(BIN)-$(VERSION)
	cp $(DISTFILES) $(BIN)-$(VERSION)
	tar -cf $(BIN)-$(VERSION).tar $(BIN)-$(VERSION)
	gzip $(BIN)-$(VERSION).tar
	rm -rf $(BIN)-$(VERSION)

clean:
	rm -f $(OBJ) $(BIN) $(MAN)

.PHONY: all install uninstall dist clean test
