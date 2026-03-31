#!/usr/bin/env bash
# discover_scanner.sh — finds Brother ADS/DS scanner USB PIDs and
# helps register them with brsaneconfig5.
#
# Usage:  ./discover_scanner.sh

set -euo pipefail

echo "=== Brother Scanner Discovery ==="
echo ""

# ── 1. Find Brother USB devices ──────────────────────────────────
echo "--- USB devices (lsusb) ---"
if ! command -v lsusb >/dev/null 2>&1; then
    echo "lsusb not found — install usbutils"
else
    BROTHER_DEVS=$(lsusb | grep -i "04f9" || true)
    if [[ -z "${BROTHER_DEVS}" ]]; then
        echo "No Brother devices found (is scanner plugged in?)"
    else
        echo "${BROTHER_DEVS}"
        echo ""
        echo "Vendor ID 04f9 is Brother Industries, Ltd."
        echo "Known ADS/DS scanner PIDs:"
        echo "  0x0459  ADS-1200"
        echo "  0x045a  ADS-1250W"
        echo "  0x045b  ADS-1700W"
        echo "  0x03b9  ADS-2800W"
        echo "  0x03b7  ADS-2400N"
        echo "  0x03ba  ADS-3600W"
        echo "  0x03b8  ADS-3000N"
        echo "  0x????  DS-740D  (PID unknown — match from your lsusb output above)"
    fi
fi

echo ""

# ── 2. Check brscan5 installation ────────────────────────────────
echo "--- brscan5 installation ---"

BRSANE5_BIN="/opt/brother/scanner/brscan5/brsaneconfig5"
if [[ -x "${BRSANE5_BIN}" ]]; then
    echo "brsaneconfig5 found: ${BRSANE5_BIN}"
    echo ""
    echo "Registered scanners:"
    "${BRSANE5_BIN}" -q 2>&1 || true
else
    echo "brsaneconfig5 not found at ${BRSANE5_BIN}"
    echo "Install brscan5 from AUR:  paru -S brscan5  or  yay -S brscan5"
fi

echo ""

# ── 3. Check SANE dll.conf ────────────────────────────────────────
echo "--- SANE dll.conf ---"
DLL_CONF="/etc/sane.d/dll.conf"
if [[ -f "${DLL_CONF}" ]]; then
    if grep -q "brother5" "${DLL_CONF}"; then
        echo "brother5 backend is enabled in ${DLL_CONF}"
    else
        echo "WARNING: 'brother5' not found in ${DLL_CONF}"
        echo "Add it with:  echo 'brother5' | sudo tee -a ${DLL_CONF}"
    fi
else
    echo "${DLL_CONF} not found"
fi

echo ""

# ── 4. Scan for available SANE devices ───────────────────────────
echo "--- SANE device list (scanimage -L) ---"
if command -v scanimage >/dev/null 2>&1; then
    scanimage -L 2>&1 || true
else
    echo "scanimage not found — install sane or sane-utils"
fi

echo ""
echo "=== Registration example ==="
echo ""
echo "If your DS-740D shows as e.g. 04f9:04ab in lsusb, register it with:"
echo ""
echo "  sudo brsaneconfig5 -a name=DS-740D model=DS-740D usb=0x04f9:0x04AB"
echo ""
echo "Replace 04AB with your actual PID from the lsusb output above."
echo ""
echo "Then test normal scanning:"
echo "  scanimage --device 'brother5:bus...' -L"
echo ""
echo "Then test long paper (run 'make' first to build the hook):"
echo "  ./scan_long.sh --length 1500 --output long_scan.tiff"
