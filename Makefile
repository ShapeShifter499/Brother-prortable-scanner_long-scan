# Makefile for libbr5longpaper.so
#
# Build:    make
# Install:  make install   (copies to /usr/local/lib)
# Clean:    make clean
# Check:    make check-deps

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
	$(CC) $(ALL_CFLAGS) -o $@ $^ $(ALL_LDFLAGS)
	@echo ""
	@echo "Built $(LIB)."
	@echo ""
	@echo "Quick test (check that br-y range is extended):"
	@echo "  BROTHER_LONG_MODE=WIDE LD_PRELOAD=\$$(pwd)/$(LIB) \\"
	@echo "    scanimage --device 'brother5:...' --help 2>&1 | grep 'br-y'"
	@echo ""
	@echo "Scan a long document:"
	@echo "  ./scan_long.sh --length 2000 --output scan.tiff"

install: $(LIB)
	install -Dm755 $(LIB) $(DESTDIR)$(LIBDIR)/$(LIB)
	@echo "Installed to $(LIBDIR)/$(LIB)"
	@echo "To use system-wide:"
	@echo "  echo $(LIBDIR)/$(LIB) | sudo tee -a /etc/ld.so.preload"
	@echo "  (Remove from ld.so.preload when not scanning long paper)"

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
