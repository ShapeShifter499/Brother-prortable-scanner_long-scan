#!/bin/bash
# br5longpaper-inject.sh
#
# Called by the pacman hook (after brscan5 is installed/upgraded) and by
# the brscan5-longpaper .install script (on first install of this package).
#
# Uses patchelf to add libbr5longpaper.so as a NEEDED dependency of the
# brscan5 SANE backend.  This causes the hook library to be loaded into any
# process that opens the brscan5 backend — and ONLY those processes.
#
# Effect:
#   - libusb_bulk_transfer is intercepted → USB commands patched with LONGPAPER
#   - C++ hooks fire (DeviceImageJpeg::SetHeight, BitmapImage::AppendWhiteLines)
#   - No injection into unrelated processes (unlike /etc/ld.so.preload)
#
# The SANE-level hooks (sane_control_option, sane_get_parameters) additionally
# require LD_PRELOAD at the application level; see the wrapper desktop files
# installed alongside this package for Simple Scan and other GUI frontends.

set -euo pipefail

LIB="libbr5longpaper.so"
TARGET="/usr/lib/sane/libsane-brother5.so.1.0.7"

if [[ ! -f "$TARGET" ]]; then
    echo "brscan5-longpaper: $TARGET not found — brscan5 not installed?" >&2
    echo "  Install brscan5 first, then re-install brscan5-longpaper." >&2
    exit 0
fi

if ! command -v patchelf >/dev/null 2>&1; then
    echo "brscan5-longpaper: patchelf not found — cannot inject" >&2
    exit 1
fi

# Idempotent: skip if already injected
if patchelf --print-needed "$TARGET" 2>/dev/null | grep -qxF "$LIB"; then
    echo "brscan5-longpaper: $LIB already present in $TARGET — skipping" >&2
    exit 0
fi

patchelf --add-needed "$LIB" "$TARGET"
echo "brscan5-longpaper: injected $LIB into $TARGET" >&2
