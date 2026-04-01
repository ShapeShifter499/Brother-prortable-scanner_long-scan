#!/bin/bash
# br5longpaper-uninject.sh — reverse the patchelf injection on package removal.

set -euo pipefail

LIB="libbr5longpaper.so"
TARGET="/usr/lib/sane/libsane-brother5.so.1.0.7"

if [[ ! -f "$TARGET" ]]; then
    exit 0  # brscan5 already removed; nothing to undo
fi

if ! patchelf --print-needed "$TARGET" 2>/dev/null | grep -qxF "$LIB"; then
    exit 0  # not injected; nothing to undo
fi

patchelf --remove-needed "$LIB" "$TARGET"
echo "brscan5-longpaper: removed $LIB from $TARGET" >&2
