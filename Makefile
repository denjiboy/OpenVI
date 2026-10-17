# OpenIV for Linux - native GTK3 front end for RAGE RPF archives.
CC       ?= cc
CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter
CFLAGS   += $(shell pkg-config --cflags gtk+-3.0)
LDLIBS   := $(shell pkg-config --libs gtk+-3.0) -lcrypto -lz

PREFIX   ?= /usr/local
BINDIR   ?= $(PREFIX)/bin
APPDIR   ?= $(PREFIX)/share/applications

SRC      := $(wildcard src/*.c)
HDR      := $(wildcard src/*.h)
OBJ      := $(SRC:.c=.o)
BIN      := openiv-native

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJ) $(LDLIBS)

%.o: %.c $(HDR)
	$(CC) $(CFLAGS) -c -o $@ $<

run: $(BIN)
	./$(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	install -Dm644 data/$(BIN).desktop $(DESTDIR)$(APPDIR)/$(BIN).desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	rm -f $(DESTDIR)$(APPDIR)/$(BIN).desktop

clean:
	rm -f $(BIN) $(OBJ)

.PHONY: all run install uninstall clean
