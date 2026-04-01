# Makefile for libbr5longpaper.so
#
# Build:               make
# Install (manual):    make install   (copies .so to /usr/local/lib)
# AUR package:         cd pkg && makepkg -si
# Clean:               make clean
# Check:               make check-deps
#
# Preferred install method is the AUR package (pkg/PKGBUILD), which:
#   1. Installs libbr5longpaper.so to /usr/lib/
#   2. Uses patchelf to add it as a NEEDED dep of libsane-brother5.so so it
#      loads only for brscan5-using processes (not system-wide)
#   3. Installs a pacman hook to re-inject after brscan5 upgrades
#   4. Overrides Simple Scan's .desktop file with LD_PRELOAD for SANE hooks
#   5. Installs brother-scan-wrap for other GUI scanning apps

CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -fPIC -shared
LDFLAGS ?= -ldl -lpthread

PREFIX  ?= /usr/local
LIBDIR  ?= $(PREFIX)/lib

SRCS = src/longpaper_hook.c
LIB  = libbr5longpaper.so

# Try pkg-config first; fall back to common paths
LIBUSB_CFLAGS  := $(shell pkg-config --cflags libusb-1.0 2>/dev/null \
                           || echo -I/usr/include/libusb-1.0)
LIBUSB_LDFLAGS := $(shell pkg-config --libs   libusb-1.0 2>/dev/null \
                           || echo -lusb-1.0)

# SANE headers are typically in /usr/include/sane
SANE_CFLAGS    := -I/usr/include/sane

ALL_CFLAGS  = $(CFLAGS) $(LIBUSB_CFLAGS) $(SANE_CFLAGS)
ALL_LDFLAGS = $(LDFLAGS) $(LIBUSB_LDFLAGS)

.PHONY: all install clean check-deps

all: $(LIB)

$(LIB): $(SRCS) | check-deps
	$(CC) $(ALL_CFLAGS) -Wl,-soname,$(LIB) -o $@ $^ $(ALL_LDFLAGS)
	@echo ""
	@echo "Built $(LIB)."
	@echo ""
	@echo "Install via AUR package (recommended):"
	@echo "  cd pkg && makepkg -si"
	@echo ""
	@echo "Or manual install + CLI use:"
	@echo "  sudo make install"
	@echo "  ./scan_long.sh --output scan.png"

# Manual install — copies the .so to /usr/local/lib.
# For system-wide GUI support, use the AUR package instead.
install: $(LIB)
	install -Dm755 $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	install -Dm755 scan_long.sh $(DESTDIR)$(PREFIX)/bin/scan-long
	@echo "Installed to $(LIBDIR)/$(LIB)"
	@echo ""
	@echo "CLI use (explicit mode):"
	@echo "  BROTHER_LONG_MODE=WIDE LD_PRELOAD=$(LIBDIR)/$(LIB) scan-long --output scan.png"
	@echo ""
	@echo "GUI use (wrap any SANE app):"
	@echo "  LD_PRELOAD=$(LIBDIR)/$(LIB) simple-scan"
	@echo "  LD_PRELOAD=$(LIBDIR)/$(LIB) xsane"
	@echo ""
	@echo "For automatic injection without LD_PRELOAD, use the AUR package:"
	@echo "  cd pkg && makepkg -si"

clean:
	rm -f $(LIB)

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
