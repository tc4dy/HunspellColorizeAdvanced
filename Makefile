CC      := gcc
PKGCONF := pkg-config

HUNSPELL_CFLAGS := $(shell $(PKGCONF) --cflags hunspell)
HUNSPELL_LIBS   := $(shell $(PKGCONF) --libs   hunspell)

CFLAGS  := -O2 -march=native -pipe \
           -Wall -Wextra -Wpedantic \
           -Wformat=2 -Wformat-security \
           -Wnull-dereference \
           -Wstack-protector \
           -Wshadow \
           -fstack-protector-strong \
           -D_FORTIFY_SOURCE=2 \
           $(HUNSPELL_CFLAGS)

LDFLAGS := -Wl,-z,relro -Wl,-z,now
LDLIBS  := $(HUNSPELL_LIBS)

PREFIX      := $(HOME)/.local
BINDIR      := $(PREFIX)/bin
MANDIR      := $(PREFIX)/share/man/man1

TARGET      := huncolor
SRC         := huncolor.c
OBJ         := $(SRC:.c=.o)
DEPFILE     := $(SRC:.c=.d)

.PHONY: all clean install uninstall strip debug

all: $(TARGET)

-include $(DEPFILE)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

debug: CFLAGS := -O0 -g3 -fsanitize=address,undefined \
                 -Wall -Wextra -Wpedantic \
                 $(HUNSPELL_CFLAGS)
debug: LDFLAGS :=
debug: clean $(TARGET)

strip: $(TARGET)
	strip --strip-all $<

install: $(TARGET)
	install -d $(BINDIR)
	install -m 755 $(TARGET) $(BINDIR)/$(TARGET)

uninstall:
	$(RM) $(BINDIR)/$(TARGET)

clean:
	$(RM) $(TARGET) $(OBJ) $(DEPFILE)
