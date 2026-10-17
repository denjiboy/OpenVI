# OpenIV for Linux - native GTK3 front end for RAGE RPF archives.
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += $(shell pkg-config --cflags gtk+-3.0)
LDLIBS   := $(shell pkg-config --libs gtk+-3.0) -lcrypto -lz

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications

SRC      := src/rpf.c src/wbc.c src/game.c src/main.c
HDR      := src/rpf.h src/wbc.h src/wbc_net.h src/game.h
BIN      := openiv-native

all: $(BIN)

$(BIN): $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDLIBS)

run: $(BIN)
	./$(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 data/$(BIN).desktop $(DESTDIR)$(APPDIR)/$(BIN).desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(APPDIR)/$(BIN).desktop

clean:
	rm -f $(BIN)

.PHONY: all run install uninstall clean
