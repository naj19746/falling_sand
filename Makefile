CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra -std=c11
override CFLAGS += -D_DEFAULT_SOURCE
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
DBUS_CFLAGS := $(shell pkg-config --cflags dbus-1)
DBUS_LIBS   := $(shell pkg-config --libs dbus-1)

PREFIX ?= $(HOME)/.local

SRCS = main.c sim.c render.c x11.c audio.c config.c weather.c spotify.c
OBJS = $(SRCS:.c=.o)
HDRS = sim.h render.h x11.h audio.h config.h weather.h spotify.h

sandbg: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(SDL_LIBS) -lX11 -lXext -lpulse $(DBUS_LIBS) -lm

%.o: %.c $(HDRS)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) $(DBUS_CFLAGS) -c $<

run: sandbg
	./sandbg --windowed

desktop: sandbg
	./sandbg

install: sandbg
	install -Dm755 sandbg $(PREFIX)/bin/sandbg
	install -Dm755 sandbg-setup $(PREFIX)/bin/sandbg-setup
	install -Dm644 config.ini.example $(PREFIX)/share/sandbg/config.ini.example
	install -Dm755 git-hook $(PREFIX)/share/sandbg/git-hook

uninstall:
	rm -f $(PREFIX)/bin/sandbg $(PREFIX)/bin/sandbg-setup
	rm -rf $(PREFIX)/share/sandbg

clean:
	rm -f sandbg $(OBJS)

.PHONY: run desktop install uninstall clean
