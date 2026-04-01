# Makefile for libbr5longpaper.so and libsane-brother5lp.so.1
#
# Build:               make
# AUR package:         cd pkg && makepkg -si
# Manual install:      sudo make install
# Clean:               make clean
# Check:               make check-deps
#
# Two outputs:
#   libbr5longpaper.so         — LD_PRELOAD hook for CLI use (scan_long.sh)
#   libsane-brother5lp.so.1    — SANE backend for GUI apps (Simple Scan etc.)
#
# The SANE backend is the preferred integration for GUI apps.  It registers
# as 'brother5lp' in /etc/sane.d/dll.conf; scanning apps then see the
# scanner as "Brother DS-740D [Long Paper]" alongside the regular device.
# No LD_PRELOAD or patchelf required.

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC -shared
LDFLAGS ?= -ldl -lpthread

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib
SANEDIR ?= $(LIBDIR)/sane

SRCS_HOOK    = src/longpaper_hook.c
SRCS_BACKEND = src/sane_backend.c

LIB_HOOK    = libbr5longpaper.so
LIB_BACKEND = libsane-brother5lp.so.1

# Try pkg-config first; fall back to common paths
LIBUSB_CFLAGS  := $(shell pkg-config --cflags libusb-1.0 2>/dev/null \
                           || echo -I/usr/include/libusb-1.0)
LIBUSB_LDFLAGS := $(shell pkg-config --libs   libusb-1.0 2>/dev/null \
                           || echo -lusb-1.0)

SANE_CFLAGS := -I/usr/include/sane

ALL_CFLAGS  = $(CFLAGS) $(LIBUSB_CFLAGS) $(SANE_CFLAGS)
ALL_LDFLAGS = $(LDFLAGS) $(LIBUSB_LDFLAGS)

.PHONY: all hook backend install clean check-deps

all: hook backend

hook: $(LIB_HOOK)

backend: $(LIB_BACKEND)

$(LIB_HOOK): $(SRCS_HOOK) | check-deps
	$(CC) $(ALL_CFLAGS) -Wl,-soname,$(LIB_HOOK) -o $@ $^ $(ALL_LDFLAGS)
	@echo "Built $(LIB_HOOK)  (LD_PRELOAD hook for CLI / scan_long.sh)"

$(LIB_BACKEND): $(SRCS_BACKEND) | check-deps
	$(CC) $(ALL_CFLAGS) -Wl,-soname,$(LIB_BACKEND) -o $@ $^ $(ALL_LDFLAGS)
	@echo "Built $(LIB_BACKEND)  (SANE backend — add 'brother5lp' to /etc/sane.d/dll.conf)"

install: all
	install -Dm755 $(LIB_HOOK)    $(DESTDIR)$(LIBDIR)/$(LIB_HOOK)
	install -Dm755 $(LIB_BACKEND) $(DESTDIR)$(SANEDIR)/$(LIB_BACKEND)
	ln -sf $(LIB_BACKEND) $(DESTDIR)$(SANEDIR)/libsane-brother5lp.so
	install -Dm755 scan_long.sh   $(DESTDIR)$(PREFIX)/bin/scan-long
	@echo ""
	@echo "Installed.  To activate the SANE backend:"
	@echo "  echo 'brother5lp' | sudo tee -a /etc/sane.d/dll.conf"
	@echo ""
	@echo "Then open Simple Scan / XSane and select"
	@echo "  'Brother DS-740D [Long Paper]'"

clean:
	rm -f $(LIB_HOOK) $(LIB_BACKEND)

check-deps:
	@echo "Checking build dependencies..."
	@command -v $(CC) >/dev/null 2>&1 || \
	    { echo "ERROR: $(CC) not found. Install gcc."; exit 1; }
	@test -f /usr/include/sane/sane.h || \
	    { echo "ERROR: sane/sane.h not found."; \
	      echo "  Arch:   sudo pacman -S sane"; \
	      echo "  Debian: sudo apt install libsane-dev"; \
	      exit 1; }
	@(pkg-config --exists libusb-1.0 2>/dev/null || \
	  test -f /usr/include/libusb-1.0/libusb.h) || \
	    { echo "ERROR: libusb-1.0 headers not found."; \
	      echo "  Arch:   sudo pacman -S libusb"; \
	      echo "  Debian: sudo apt install libusb-1.0-0-dev"; \
	      exit 1; }
	@echo "  OK"

