CC = gcc
CFLAGS = -g
LDFLAGS = -lwayland-client -lxkbcommon -ludev
CPPFLAGS = -DUSE_SHM

WAYLAND_SCANNER = wayland-scanner
WAYLAND_PROTOCOLS_DIR = $(shell pkg-config --variable=pkgdatadir wayland-protocols)

TARGET = build/client
SOURCES = client.c xdg-shell-protocol.c xdg-decoration.c
OBJECTS = $(addprefix build/,$(SOURCES:.c=.o))
BUILD_DIR = build

GENERATED_FILES = xdg-shell-protocol.c xdg-shell-protocol.h xdg-decoration.c xdg-decoration.h

.PHONY: all clean

all: $(BUILD_DIR) $(TARGET)

$(TARGET): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(OBJECTS): $(GENERATED_FILES)

xdg-shell-protocol.c: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) public-code < $< > $@

xdg-shell-protocol.h: $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
	$(WAYLAND_SCANNER) client-header < $< > $@

xdg-decoration.c: $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	$(WAYLAND_SCANNER) public-code < $< > $@

xdg-decoration.h: $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
	$(WAYLAND_SCANNER) client-header < $< > $@

build/%.o: %.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD_DIR) $(GENERATED_FILES)
