CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PKGS     := libpng x11
CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS)) -lm

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin

TARGET   := redact
SRC      := redact.c

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
