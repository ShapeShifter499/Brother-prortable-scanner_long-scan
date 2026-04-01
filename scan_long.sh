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
COLOR_MODE="24bit Color[Fast]"
LENGTH_MM="1830"          # safety ceiling: just above DS-740D's 72" (1829mm) max
                          # the scan auto-stops on paper-exit — this is just a cap
DEVICE=""                  # auto-detect if empty
OUTPUT_FILE="scan_$(date +%Y%m%d_%H%M%S).png"
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
# scanimage PNG format does not support variable-length (lines=-1) scans.
# We always scan to a temporary PNM file (which handles variable height
# by buffering all rows before writing the header), then convert to the
# requested output format.  The PNM is removed after conversion.
PNM_TMP="${OUTPUT_FILE%.*}_tmp$$.pnm"

SCAN_ARGS=(
    --device  "${DEVICE}"
    --resolution "${RESOLUTION}"
    --mode    "${COLOR_MODE}"
    --format  pnm
    -o        "${PNM_TMP}"
)
# Note: DS-740D does not expose --br-y via SANE CLI; scan length is
# controlled by the hook (PTYPE=LONGPAPER_WIDE + LONG=ON + LSMD=ON +
# sane_start br-y push).  The scanner auto-stops when paper exits ADF.

# ADF source (DS-740D uses left-aligned; ADS-1200 may differ)
SCAN_ARGS+=(--source "Automatic Document Feeder(left aligned)")

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

if [[ ! -f "${PNM_TMP}" ]]; then
    echo "ERROR: scan produced no output file." >&2
    exit 1
fi

# ── Convert PNM → requested format (+ auto-trim trailing blank rows) ──────
#
# Long paper scans can include blank rows after the paper exits the ADF
# because brscan5 pads its internal buffer to the pre-configured scan
# height.  The hook suppresses BitmapImage::AppendWhiteLines in long mode,
# but as a belt-and-suspenders measure we also trim here with ImageMagick.
#
# Trimming strategy: compute the bottom edge of non-white content, then
# crop from the top of the image to that row only — left/right/top edges
# are preserved as-is so document margins are not affected.
# -fuzz 5% handles JPEG compression artifacts at the paper trailing edge.

do_trim_convert() {
    local src="$1" dst="$2"
    local conv_cmd
    if command -v magick >/dev/null 2>&1; then
        conv_cmd="magick"
    elif command -v convert >/dev/null 2>&1; then
        conv_cmd="convert"
    else
        return 1
    fi

    if [[ "${LONG_MODE}" != "OFF" ]]; then
        local orig_w orig_h bottom
        orig_w=$("${conv_cmd}" "${src}" -format "%w" info: 2>/dev/null)
        orig_h=$("${conv_cmd}" "${src}" -format "%h" info: 2>/dev/null)
        # page.y = top of content after trim; h = height of trimmed content.
        # page.y + h = bottom row of real content in the original image.
        bottom=$("${conv_cmd}" "${src}" -fuzz 5% -trim \
                     -format "%[fx:page.y+h]" info: 2>/dev/null)
        if [[ "${bottom}" =~ ^[0-9]+$ && "${bottom}" -gt 0 \
              && "${orig_h}" =~ ^[0-9]+$ && "${bottom}" -lt "${orig_h}" ]]; then
            local saved=$(( orig_h - bottom ))
            echo "Trimming trailing blank rows: ${orig_h} → ${bottom} px" \
                 "(removed ${saved} blank rows /" \
                 "$(echo "scale=1; ${saved} / ${RESOLUTION} * 25.4" | bc 2>/dev/null || echo '?') mm)"
            "${conv_cmd}" "${src}" \
                -gravity North -crop "${orig_w}x${bottom}+0+0" +repage "${dst}"
            return 0
        fi
    fi

    "${conv_cmd}" "${src}" "${dst}"
}

if [[ "${OUTPUT_FILE}" == *.pnm || "${OUTPUT_FILE}" == *.ppm ]]; then
    mv "${PNM_TMP}" "${OUTPUT_FILE}"
elif command -v magick >/dev/null 2>&1 || command -v convert >/dev/null 2>&1; then
    echo "Converting to ${OUTPUT_FILE}..."
    do_trim_convert "${PNM_TMP}" "${OUTPUT_FILE}"
    rm -f "${PNM_TMP}"
else
    # No converter available — keep the PNM
    PNM_FINAL="${OUTPUT_FILE%.*}.pnm"
    mv "${PNM_TMP}" "${PNM_FINAL}"
    echo "NOTE: ImageMagick not found; output kept as ${PNM_FINAL}" >&2
    echo "      Install ImageMagick to auto-convert:  sudo pacman -S imagemagick" >&2
    OUTPUT_FILE="${PNM_FINAL}"
fi

echo ""
echo "Scan complete: ${OUTPUT_FILE}"

# Report actual scanned dimensions
if command -v identify >/dev/null 2>&1; then
    PX_H=$(identify -ping -format "%h" "${OUTPUT_FILE}" 2>/dev/null)
    PX_W=$(identify -ping -format "%w" "${OUTPUT_FILE}" 2>/dev/null)
    if [[ -n "${PX_H}" && -n "${PX_W}" ]]; then
        MM_H=$(echo "scale=1; ${PX_H} / ${RESOLUTION} * 25.4" | bc 2>/dev/null || true)
        IN_H=$(echo "scale=2; ${PX_H} / ${RESOLUTION}" | bc 2>/dev/null || true)
        echo "Image dimensions: ${PX_W}×${PX_H} px"
        [[ -n "${MM_H}" ]] && echo "Scanned length:  ${MM_H} mm (${IN_H} inches) at ${RESOLUTION} DPI"
    fi
fi
