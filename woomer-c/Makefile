PREFIX ?= /usr/local
CC ?= gcc

CFLAGS ?= -std=c11 -O2 -Wall -Wextra
LDLIBS := $(shell pkg-config --libs raylib 2>/dev/null || echo -lraylib) -lm -lpthread -ldl

# Arch's raylib package usually needs these extra system libs when linking
# statically against GLFW's Wayland backend; harmless no-ops if unneeded.
LDLIBS += $(shell pkg-config --exists wayland-client && echo -lwayland-client) 2>/dev/null

SRC := src/main.c
BIN := woomer

.PHONY: all clean install uninstall run

all: $(BIN)

$(BIN): $(SRC)
	$(CC) $(CFLAGS) -o $(BIN) $(SRC) $(LDLIBS)

run: $(BIN)
	./$(BIN)

install: $(BIN)
	install -Dm755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -Dm644 shaders/spotlight.fs $(DESTDIR)$(PREFIX)/share/woomer/shaders/spotlight.fs

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -rf $(DESTDIR)$(PREFIX)/share/woomer

clean:
	rm -f $(BIN)
