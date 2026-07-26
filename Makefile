CC       ?= cc
CFLAGS   ?= -O2 -Wall -Wextra
PKGS     := libpng x11
CFLAGS   += $(shell pkg-config --cflags $(PKGS))
LDLIBS   := $(shell pkg-config --libs $(PKGS)) -lm

# Extra libs only needed for -static: X11/libpng pull these in transitively
# when dynamically linked, but a static link needs them named explicitly.
STATIC_LIBS := -lxcb -lXau -lXdmcp -lz

PREFIX   ?= /usr/local
BINDIR   := $(PREFIX)/bin

TARGET   := redact
SRC      := redact.c

.PHONY: all static install uninstall clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

static: $(SRC)
	$(CC) $(CFLAGS) -static -o $(TARGET) $< $(LDLIBS) $(STATIC_LIBS)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)

clean:
	rm -f $(TARGET)
