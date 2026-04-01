#!/usr/bin/env bash
# scan_long.sh — wrapper to scan in long paper mode using brscan5 + the hook.
#
# Usage:
#   ./scan_long.sh [--narrow] [--resolution DPI] [--mode MODE] \
#                  [--length MM] [--device DEVICE] [--output FILE]
#
# Examples:
#   ./scan_long.sh                           # auto-detect scanner, WIDE, 300dpi
#   ./scan_long.sh --narrow --length 1000    # narrow receipt, 1000mm long
#   ./scan_long.sh --resolution 200 --output receipt.pdf
#   ./scan_long.sh --device "brother5:0x04f9:0x0459:..."
#
# Auto-stop behaviour (same as Windows):
#   The scan ends automatically when the paper exits the ADF — you do NOT
#   need to know the document length in advance.  The --length value is only
#   a safety ceiling.  The scanner sends an ALLEND status when the paper
#   runs out, just like the Windows driver.
#
# Hardware maximum lengths:
#   DS-740D:  72 inches = 1829 mm  (Brother specification)
#   ADS-1200: verify with your scanner; likely similar
#   The scanner enforces its own hardware limit regardless of --length.
#
# Requirements:
#   - brscan5 installed (libsane-brother5.so in SANE backend path)
#   - libbr5longpaper.so built (run 'make' first)
#   - scanimage (from sane-utils/sane-backends package)
#
# For ADS-1200 (USB PID 0x0459) and DS-740D (run `lsusb | grep 04f9` for PID)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HOOK_LIB="${SCRIPT_DIR}/libbr5longpaper.so"

# ── Check prerequisites ──────────────────────────────────────────
if [[ ! -f "${HOOK_LIB}" ]]; then
    echo "ERROR: ${HOOK_LIB} not found. Run 'make' first." >&2
    exit 1
fi

command -v scanimage >/dev/null 2>&1 || {
    echo "ERROR: 'scanimage' not found. Install sane-utils or sane-backends." >&2
    exit 1
}

# ── Defaults ─────────────────────────────────────────────────────
LONG_MODE="WIDE"
RESOLUTION="300"
COLOR_MODE="Color"
LENGTH_MM="1830"          # safety ceiling: just above DS-740D's 72" (1829mm) max
                          # the scan auto-stops on paper-exit — this is just a cap
DEVICE=""                  # auto-detect if empty
OUTPUT_FILE="scan_$(date +%Y%m%d_%H%M%S).tiff"
EXTRA_ARGS=()

# ── Parse arguments ───────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
    case "$1" in
        --narrow)        LONG_MODE="NARROW"; shift ;;
        --wide)          LONG_MODE="WIDE";   shift ;;
        --resolution|-r) RESOLUTION="$2"; shift 2 ;;
        --mode|-m)       COLOR_MODE="$2"; shift 2 ;;
        --length|-l)     LENGTH_MM="$2"; shift 2 ;;
        --device|-d)     DEVICE="$2"; shift 2 ;;
        --output|-o)     OUTPUT_FILE="$2"; shift 2 ;;
        --help|-h)
            sed -n '2,30p' "$0" | sed 's/^# \?//'
            exit 0 ;;
        *)
            EXTRA_ARGS+=("$1"); shift ;;
    esac
done

# ── Determine the scanner device string ──────────────────────────
if [[ -z "${DEVICE}" ]]; then
    echo "Detecting Brother scanner via brscan5..."
    # List all SANE devices and pick the first brother5 one
    DEVICE=$(BROTHER_LONG_MODE=OFF LD_PRELOAD="${HOOK_LIB}" \
        scanimage -L 2>/dev/null \
        | grep -i "brother5\|brother 5\|ADS-1200\|DS-740" \
        | head -1 \
        | sed "s/.*device[[:space:]]*['\`]*\([^'[:space:]]*\).*/\1/")

    if [[ -z "${DEVICE}" ]]; then
        echo "ERROR: No Brother ADS/DS scanner found." >&2
        echo "" >&2
        echo "Troubleshooting:" >&2
        echo "  1. Is brscan5 installed?" >&2
        echo "     pacman -S brscan5  (AUR)   or   dpkg -i brscan5-*.deb" >&2
        echo "  2. Is the scanner plugged in and powered on?" >&2
        echo "  3. Find your scanner's USB PID:" >&2
        echo "     lsusb | grep '04f9'" >&2
        echo "  4. Register it with brsaneconfig5:" >&2
        echo "     sudo brsaneconfig5 -a name=ADS-1200 model=ADS-1200 usb=0x04f9:0x0459" >&2
        exit 1
    fi
    echo "Found: ${DEVICE}"
fi

# ── Build scanimage arguments ─────────────────────────────────────
SCAN_ARGS=(
    --device  "${DEVICE}"
    --resolution "${RESOLUTION}"
    --mode    "${COLOR_MODE}"
    --br-y    "${LENGTH_MM}"
    --format  tiff
    -o        "${OUTPUT_FILE}"
)

# ADF source — use center-aligned (standard for ADS-1200 and DS-740D)
SCAN_ARGS+=(--source "Automatic Document Feeder(center aligned)")

[[ ${#EXTRA_ARGS[@]} -gt 0 ]] && SCAN_ARGS+=("${EXTRA_ARGS[@]}")

# ── Run ──────────────────────────────────────────────────────────
echo ""
echo "Scanning in LONG PAPER mode (${LONG_MODE})"
echo "  Device:     ${DEVICE}"
echo "  Resolution: ${RESOLUTION} DPI"
echo "  Mode:       ${COLOR_MODE}"
echo "  Max length: ${LENGTH_MM} mm (scan auto-stops when paper exits ADF)"
echo "  Output:     ${OUTPUT_FILE}"
echo ""

BROTHER_LONG_MODE="${LONG_MODE}" \
LD_PRELOAD="${HOOK_LIB}" \
    scanimage "${SCAN_ARGS[@]}"

echo ""
echo "Scan complete: ${OUTPUT_FILE}"
