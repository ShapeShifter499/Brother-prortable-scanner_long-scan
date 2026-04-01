/*
 * longpaper_hook.c - LD_PRELOAD hook to enable long paper scanning for
 * Brother ADS-1200 and DS-740D scanners via brscan5 on Linux.
 *
 * Build:
 *   make  (or see Makefile)
 *
 * Usage:
 *   BROTHER_LONG_MODE=WIDE LD_PRELOAD=/path/to/libbr5longpaper.so \
 *     scanimage --device "brother5:..." --br-y 5000 ...
 *
 * BROTHER_LONG_MODE values:
 *   WIDE    - Wide long paper (A4/letter-width documents, long receipts)
 *   NARROW  - Narrow long paper (narrow receipts, tickets)
 *   OFF     - Disabled / passthrough (default)
 *
 * How it works:
 *   The brscan5 SANE backend (libsane-brother5.so) wraps libLxBsScanCoreApi.so
 *   which has full long paper support (LONGPAPER_NARROW, LONGPAPER_WIDE,
 *   ConvertUserSetting_LongPaper, MakeLongPaperModeString, etc.).
 *   However, the SANE layer:
 *     1. Constrains br-y to ~291–355 mm (normal paper max).
 *     2. Never sets PTYPE=LONGPAPER_WIDE/NARROW in USB commands.
 *
 *   This hook intercepts at two points:
 *
 *   A) sane_get_option_descriptor() — when br-y is queried, returns an
 *      extended range (up to 5000 mm) so frontends can accept large heights.
 *
 *   B) libusb_bulk_transfer() — when an outgoing command contains scan
 *      parameters (identified by RESO=, CLR=, PTYPE=, PSRC= keywords),
 *      injects LONG=ON and sets PTYPE=LONGPAPER_WIDE or LONGPAPER_NARROW.
 *      This directly patches the text-based QDI command the scanner understands.
 *
 *   C) sane_control_option() — tracks the br-y value the user sets; if it
 *      exceeds the backend's original max, we pass the backend's clamped max
 *      to avoid a SANE_STATUS_INVAL rejection while the USB patch handles
 *      the actual long scan length.
 *
 * Protocol notes (reverse-engineered from libLxBsScanCoreApi.so 3.2.1,
 * brscan5-1.5.1-0):
 *   - Text-based key=value pairs over USB bulk transfers
 *   - Long paper command: PTYPE=LONGPAPER_WIDE (or LONGPAPER_NARROW) + LONG=ON
 *   - Other relevant keys: RESO=, CLR=, AREA=, DPLX=, PSRC=, COMP=
 *   - See src/protocol_notes.md for full reverse-engineering notes
 *
 * USB IDs:
 *   Vendor:    0x04f9  (Brother Industries, Ltd)
 *   ADS-1200:  0x0459
 *   ADS-1250W: 0x045a
 *   ADS-1700W: 0x045b
 *   DS-740D:   0x0469  (confirmed)
 *
 * Copyright (C) 2026 — Released under GPLv2 (see LICENSE)
 */

#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#include <sane/sane.h>
#include <libusb-1.0/libusb.h>

/* ── Configuration ─────────────────────────────────────────────── */

/* Maximum br-y we expose via SANE (mm). Approximately the Windows
 * driver maximum for long paper mode (200 inches = 5080 mm).        */
#define LONG_PAPER_MAX_MM   5000.0

/* If br-y > this, we consider it a long paper scan and activate the
 * USB patch regardless of whether PTYPE already has a long value.   */
#define LONG_TRIGGER_MM     300.0

/* Maximum USB command buffer we will modify (bytes).
 * Commands larger than this are passed through unmodified.           */
#define MAX_CMD_BUF         8192

/* ── Types ─────────────────────────────────────────────────────── */

typedef int (*libusb_bulk_fn)(libusb_device_handle *dev_handle,
                               unsigned char endpoint,
                               unsigned char *data,
                               int length,
                               int *actual_length,
                               unsigned int timeout);

typedef const SANE_Option_Descriptor *(*sane_get_opt_fn)(SANE_Handle,
                                                          SANE_Int);

typedef SANE_Status (*sane_ctrl_fn)(SANE_Handle, SANE_Int, SANE_Action,
                                    void *, SANE_Int *);

/* ── Globals ───────────────────────────────────────────────────── */

static int  g_mode        = 0;     /* 0=OFF  1=WIDE  2=NARROW       */
static bool g_initialized = false;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static libusb_bulk_fn  real_usb_bulk   = NULL;
static sane_get_opt_fn real_get_opt    = NULL;
static sane_ctrl_fn    real_ctrl_opt   = NULL;

/* Per-handle tracking of br-y state (single-scanner assumption) */
static SANE_Fixed  g_br_y_user_val    = 0;  /* what the user requested */
static SANE_Fixed  g_br_y_backend_max = 0;  /* backend's hard limit    */
static SANE_Int    g_br_y_opt_num     = -1; /* SANE option number      */

/* Override descriptor returned when long mode is active */
static SANE_Option_Descriptor g_br_y_desc;
static SANE_Range              g_br_y_range;

/* ── Initialisation ────────────────────────────────────────────── */

static void do_init(void) {
    const char *env = getenv("BROTHER_LONG_MODE");
    if (env) {
        if      (strcasecmp(env, "WIDE")   == 0) g_mode = 1;
        else if (strcasecmp(env, "NARROW") == 0) g_mode = 2;
        else                                      g_mode = 0;
    }

    real_usb_bulk = (libusb_bulk_fn) dlsym(RTLD_NEXT,
                                            "libusb_bulk_transfer");
    real_get_opt  = (sane_get_opt_fn) dlsym(RTLD_NEXT,
                                             "sane_get_option_descriptor");
    real_ctrl_opt = (sane_ctrl_fn)    dlsym(RTLD_NEXT,
                                             "sane_control_option");

    if (g_mode) {
        fprintf(stderr,
            "[br5longpaper] active: mode=%s, max_br_y=%.0fmm\n",
            g_mode == 1 ? "WIDE" : "NARROW", LONG_PAPER_MAX_MM);
    }

    g_initialized = true;
}

static void ensure_init(void) {
    pthread_once(&g_once, do_init);
}

/* ── Buffer helpers ────────────────────────────────────────────── */

/* Find needle in binary haystack; return pointer or NULL. */
static uint8_t *buf_find(const uint8_t *hay, int hlen,
                          const char *needle) {
    int nlen = (int)strlen(needle);
    for (int i = 0; i <= hlen - nlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return (uint8_t *)(hay + i);
    return NULL;
}

/*
 * In-place replacement of the value of a KEY= token in a text command.
 *
 *   buf      — command buffer (mutable)
 *   len      — current length
 *   max_len  — allocated size of buf
 *   key      — e.g. "PTYPE="
 *   new_val  — new value string, e.g. "LONGPAPER_WIDE"
 *
 * Returns new length, or -1 on overflow or if key not present.
 */
static int buf_replace_val(uint8_t *buf, int len, int max_len,
                            const char *key, const char *new_val) {
    uint8_t *p = buf_find(buf, len, key);
    if (!p) return -1;                         /* key not present */

    uint8_t *vs = p + strlen(key);             /* start of old value */
    uint8_t *ve = vs;
    while (ve < buf + len && *ve != '\r' && *ve != '\n' && *ve != '\0')
        ve++;

    int olen  = (int)(ve - vs);
    int nlen  = (int)strlen(new_val);
    int delta = nlen - olen;

    if (len + delta > max_len) return -2;      /* overflow */

    memmove(vs + nlen, ve, (buf + len) - ve);
    memcpy(vs, new_val, nlen);
    return len + delta;
}

/*
 * Insert "KEY=VALUE\r\n" just before the last byte of buf
 * (which is typically a 0x80 end-of-command marker).
 * Returns new length, or -1 if key already present or overflow.
 */
static int buf_insert_kv(uint8_t *buf, int len, int max_len,
                          const char *key, const char *val) {
    if (buf_find(buf, len, key)) return len;   /* already present */

    char token[128];
    int tlen = snprintf(token, sizeof(token), "%s%s\r\n", key, val);
    if (tlen <= 0 || tlen >= (int)sizeof(token)) return -1;
    if (len + tlen > max_len) return -2;

    /* Insert before trailing 0x80 (end-of-command byte), if present */
    int ins = len;
    if (ins > 0 && buf[ins - 1] == 0x80) ins--;

    memmove(buf + ins + tlen, buf + ins, len - ins);
    memcpy(buf + ins, token, tlen);
    return len + tlen;
}

/* ── Command detection ─────────────────────────────────────────── */

/*
 * Returns true if the outgoing USB payload looks like a brscan5 scan
 * command (contains one of the known text parameter keys).
 */
static bool is_scan_cmd(const uint8_t *data, int length) {
    if (length < 6 || length > MAX_CMD_BUF) return false;
    static const char * const probes[] =
        { "RESO=", "CLR=", "PTYPE=", "PSRC=", "AREA=", NULL };
    for (int i = 0; probes[i]; i++)
        if (buf_find(data, length, probes[i]))
            return true;
    return false;
}

/* ── Long paper command patch ──────────────────────────────────── */

/*
 * Modify a brscan5 scan command in-place to request long paper scanning.
 *
 *   buf / len   — command to modify (already copied)
 *   max_len     — allocated size
 *   mode        — 1 = WIDE, 2 = NARROW
 *
 * Returns new length, or -1 on failure (caller should use original).
 *
 * What this sets and why:
 *
 *   PTYPE=LONGPAPER_WIDE (or NARROW)
 *     Tells the scanner "this is a long document scan."  The scanner
 *     switches from coordinate-based stopping to ADF-exit detection:
 *     it scans until the paper physically exits the feed rollers, then
 *     sends GetScanStatus()=ALLEND — identical to Windows auto-stop.
 *
 *   LONG=ON
 *     Explicit long-paper enable flag (bool, from MakeLongPaperModeString).
 *     Redundant with PTYPE=LONGPAPER_* but included for safety.
 *
 *   AREA=FULL
 *     Overrides coordinate-based scan area height ("scan the whole document,
 *     ignoring the height coordinate").  Without this, the backend clamps
 *     br-y to ~355mm and encodes that into the scan area coordinates;
 *     the scanner would stop at 355mm even with LONG=ON set.
 *     AREA=FULL is most likely what the Windows driver uses for long paper
 *     because it lets PTYPE/LONG flags drive end-of-scan instead of a fixed
 *     pixel coordinate.
 *
 * Hardware maximums (enforced by the scanner regardless of br-y ceiling):
 *   DS-740D:  72 inches (1828.8 mm) per Brother specification
 *   ADS-1200: verify with Windows driver or scanner manual
 *
 * The scan auto-stops when paper exits the ADF (ALLEND status), so the
 * br-y value you pass is only a safety ceiling — identical to Windows.
 */
/* Print printable content of a USB command buffer to stderr. */
static void log_cmd(const char *prefix, const uint8_t *buf, int len) {
    fprintf(stderr, "[br5longpaper] %s (%d bytes): ", prefix, len);
    for (int i = 0; i < len && i < 512; i++) {
        uint8_t c = buf[i];
        if (c == '\r') fprintf(stderr, "\\r");
        else if (c == '\n') fprintf(stderr, "\\n");
        else if (c == '\0') fprintf(stderr, "\\0");
        else if (c < 32 || c > 126) fprintf(stderr, "[%02x]", c);
        else fputc(c, stderr);
    }
    fprintf(stderr, "\n");
}

static int patch_cmd_long_paper(uint8_t *buf, int len, int max_len,
                                 int mode) {
    const char *ptype = (mode == 1) ? "LONGPAPER_WIDE" : "LONGPAPER_NARROW";

    /* Set PTYPE= to the long paper type (replace existing value or insert) */
    int r = buf_replace_val(buf, len, max_len, "PTYPE=", ptype);
    if (r == -2) return -1;
    if (r == -1) {
        len = buf_insert_kv(buf, len, max_len, "PTYPE=", ptype);
        if (len < 0) return -1;
    } else {
        len = r;
    }

    /* Enable long paper flag */
    r = buf_insert_kv(buf, len, max_len, "LONG=", "ON");
    if (r < 0 && r != -1) return -1;
    if (r > 0) len = r;

    /* NOTE: AREA=FULL was tried but caused "Document feeder jammed" errors.
     * Removed — PTYPE=LONGPAPER_WIDE + LONG=ON may be sufficient.
     * If the scanner still stops at 355mm, the height coordinate in the
     * command needs to be increased (requires knowing exact byte format). */

    return len;
}

/* ══════════════════════════════════════════════════════════════════
 * Intercepted functions
 * ════════════════════════════════════════════════════════════════ */

/* ── 1. libusb_bulk_transfer ───────────────────────────────────── */
/*
 * When long mode is ON and we see an outgoing scan command, we patch
 * PTYPE= and insert LONG=ON before forwarding to the scanner.
 */
int libusb_bulk_transfer(libusb_device_handle *dev_handle,
                          unsigned char endpoint,
                          unsigned char *data,
                          int length,
                          int *actual_length,
                          unsigned int timeout) {
    ensure_init();

    if (!real_usb_bulk) {
        fprintf(stderr, "[br5longpaper] FATAL: real libusb_bulk_transfer"
                " not found\n");
        return LIBUSB_ERROR_OTHER;
    }

    /* Log ALL outgoing transfers when debug mode is on */
    int debug = (getenv("BROTHER_SCAN_DEBUG") != NULL);

    if (!(endpoint & LIBUSB_ENDPOINT_IN)) {
        if (debug)
            log_cmd("OUT raw", data, length);
    }

    /* Only intercept outgoing (host→device) scan commands */
    if (g_mode && !(endpoint & LIBUSB_ENDPOINT_IN) &&
        is_scan_cmd(data, length)) {

        int extra = 256;
        uint8_t *mod = (uint8_t *)malloc(length + extra);
        if (mod) {
            memcpy(mod, data, length);
            int new_len = patch_cmd_long_paper(mod, length,
                                               length + extra, g_mode);
            if (new_len > 0) {
                fprintf(stderr,
                    "[br5longpaper] USB patch: PTYPE=LONGPAPER_%s LONG=ON"
                    " (%d→%d bytes)\n",
                    g_mode == 1 ? "WIDE" : "NARROW", length, new_len);
                if (debug)
                    log_cmd("OUT patched", mod, new_len);
                int rc = real_usb_bulk(dev_handle, endpoint, mod,
                                       new_len, actual_length, timeout);
                free(mod);
                return rc;
            }
            free(mod);
            fprintf(stderr, "[br5longpaper] patch failed, using original"
                    " command\n");
        }
    }

    return real_usb_bulk(dev_handle, endpoint, data, length,
                          actual_length, timeout);
}

/* ── 2. sane_get_option_descriptor ────────────────────────────── */
/*
 * The real SANE API: const SANE_Option_Descriptor *
 *   sane_get_option_descriptor(SANE_Handle, SANE_Int);
 *
 * When long mode is active, we intercept the br-y option and return
 * a modified descriptor with an extended max range (5000 mm).
 */
const SANE_Option_Descriptor *
sane_get_option_descriptor(SANE_Handle handle, SANE_Int option) {
    ensure_init();

    if (!real_get_opt) return NULL;

    const SANE_Option_Descriptor *d = real_get_opt(handle, option);

    if (!g_mode || !d || !d->name) return d;

    if (strcmp(d->name, "br-y") == 0) {
        g_br_y_opt_num = option;

        if (d->constraint_type == SANE_CONSTRAINT_RANGE &&
            d->type == SANE_TYPE_FIXED &&
            d->constraint.range) {

            /* Record the backend's actual maximum */
            g_br_y_backend_max = d->constraint.range->max;

            /* Build our extended descriptor once */
            memcpy(&g_br_y_desc,  d,                    sizeof(g_br_y_desc));
            memcpy(&g_br_y_range, d->constraint.range,  sizeof(g_br_y_range));
            g_br_y_range.max           = SANE_FIX(LONG_PAPER_MAX_MM);
            g_br_y_desc.constraint.range = &g_br_y_range;

            fprintf(stderr,
                "[br5longpaper] br-y range extended: %.1fmm → %.0fmm\n",
                SANE_UNFIX(g_br_y_backend_max), LONG_PAPER_MAX_MM);

            return &g_br_y_desc;
        }
    }

    return d;
}

/* ── 3. sane_control_option ────────────────────────────────────── */
/*
 * When the user sets br-y to a value larger than the backend's hard max,
 * we save the requested value (so we could theoretically encode it into
 * the USB scan area) and then pass the backend's own max to prevent
 * SANE_STATUS_INVAL.  The USB patch (above) activates LONG=ON regardless,
 * letting the scanner continue feeding until the document runs out.
 *
 * NOTE: Some Brother ADS scanners in LONG= mode ignore the scan height
 * and just keep scanning until the document exits the ADF.  If yours
 * stops early, the scan area coordinates in the USB command need adjustment
 * (see protocol_notes.md "Open Questions").
 */
SANE_Status sane_control_option(SANE_Handle handle, SANE_Int option,
                                 SANE_Action action, void *value,
                                 SANE_Int *info) {
    ensure_init();

    if (!real_ctrl_opt) return SANE_STATUS_UNSUPPORTED;

    if (g_mode && action == SANE_ACTION_SET_VALUE &&
        option == g_br_y_opt_num && value) {

        SANE_Fixed requested = *(SANE_Fixed *)value;
        double     mm        = SANE_UNFIX(requested);

        if (requested > g_br_y_backend_max && g_br_y_backend_max > 0) {
            /* Save the actual user request */
            g_br_y_user_val = requested;

            fprintf(stderr,
                "[br5longpaper] br-y %.1fmm > backend max %.1fmm:"
                " clamping to backend max; LONG=ON will be injected.\n",
                mm, SANE_UNFIX(g_br_y_backend_max));

            /* Pass the backend's own max instead */
            SANE_Fixed clamped = g_br_y_backend_max;
            return real_ctrl_opt(handle, option, action, &clamped, info);
        }

        g_br_y_user_val = requested;
    }

    return real_ctrl_opt(handle, option, action, value, info);
}
