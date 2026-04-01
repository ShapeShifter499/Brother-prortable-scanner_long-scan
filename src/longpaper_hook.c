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

typedef SANE_Status (*sane_get_params_fn)(SANE_Handle, SANE_Parameters *);

typedef SANE_Status (*sane_start_fn)(SANE_Handle);

/* C++ method type: void Class::SetHeight(unsigned int) — x86-64 ABI
 * passes `this` in rdi and the unsigned int argument in rsi.          */
typedef void (*cpp_setheight_fn)(void *self, unsigned int h);

/* ── Globals ───────────────────────────────────────────────────── */

static int  g_mode        = 0;     /* 0=OFF  1=WIDE  2=NARROW       */
static bool g_initialized = false;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

static libusb_bulk_fn     real_usb_bulk      = NULL;
static sane_get_opt_fn    real_get_opt       = NULL;
static sane_ctrl_fn       real_ctrl_opt      = NULL;
static sane_get_params_fn real_get_params    = NULL;
static sane_start_fn      real_sane_start    = NULL;
static cpp_setheight_fn   real_devimg_sh     = NULL; /* DeviceImageJpeg::SetHeight */
static cpp_setheight_fn   real_decodeparam_sh= NULL; /* DecodeParameter::SetHeight  */

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

    real_usb_bulk   = (libusb_bulk_fn)     dlsym(RTLD_NEXT,
                                                  "libusb_bulk_transfer");
    real_get_opt    = (sane_get_opt_fn)    dlsym(RTLD_NEXT,
                                                  "sane_get_option_descriptor");
    real_ctrl_opt   = (sane_ctrl_fn)       dlsym(RTLD_NEXT,
                                                  "sane_control_option");
    real_get_params     = (sane_get_params_fn) dlsym(RTLD_NEXT,
                                                    "sane_get_parameters");
    real_sane_start     = (sane_start_fn)      dlsym(RTLD_NEXT,
                                                    "sane_start");
    real_devimg_sh      = (cpp_setheight_fn)   dlsym(RTLD_NEXT,
                                                    "_ZN15DeviceImageJpeg9SetHeightEj");
    real_decodeparam_sh = (cpp_setheight_fn)   dlsym(RTLD_NEXT,
                                                    "_ZN15DecodeParameter9SetHeightEj");

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

/* Returns true if buf starts with the given literal header. */
static bool cmd_is(const uint8_t *buf, int len, const char *header) {
    int hlen = (int)strlen(header);
    return len >= hlen && memcmp(buf, header, hlen) == 0;
}

/*
 * Returns true if the outgoing USB payload looks like a brscan5 scan
 * command that we should patch (SSP or XSC, not CKD status checks).
 */
static bool is_scan_cmd(const uint8_t *data, int length) {
    if (length < 6 || length > MAX_CMD_BUF) return false;
    /* Target the two commands that control the scan: SSP (parameters)
     * and XSC (execute with area coordinates).  CKD is just a source
     * check and must not be patched. */
    if (cmd_is(data, length, "\x1bSSP\n")) return true;
    if (cmd_is(data, length, "\x1bXSC\n")) return true;
    return false;
}

/* ── Long paper command patch ──────────────────────────────────── */

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

/*
 * Patch the XSC command's AREA=x1,y1,x2,y2 by replacing y2 with a large
 * value so the scanner isn't told to stop at the normal-paper height.
 * The scanner's hardware limit (72" on the DS-740D) still applies.
 */
static int patch_xsc_area(uint8_t *buf, int len, int max_len) {
    uint8_t *p = buf_find(buf, len, "AREA=");
    if (!p) return len;                          /* no AREA= key, no change */

    uint8_t *vs = p + 5;                         /* start of value */
    uint8_t *ve = vs;
    while (ve < buf + len && *ve != '\r' && *ve != '\n' && *ve != '\0')
        ve++;

    int vlen = (int)(ve - vs);
    if (vlen <= 0 || vlen >= 64) return len;

    char val[64];
    memcpy(val, vs, vlen);
    val[vlen] = '\0';

    long x1, y1, x2, y2;
    if (sscanf(val, "%ld,%ld,%ld,%ld", &x1, &y1, &x2, &y2) != 4)
        return len;                              /* unexpected format, skip */

    /* Replace y2 with a value well past the scanner's hardware maximum.
     * DS-740D max = 72 inches; at 600 dpi that is 43 200 px.  We use
     * 999999 so the scanner's own ADF-exit (ALLEND) logic always fires
     * first regardless of resolution.                                     */
    char newval[64];
    int newvlen = snprintf(newval, sizeof(newval), "%ld,%ld,%ld,999999",
                           x1, y1, x2);

    int delta = newvlen - vlen;
    if (len + delta > max_len) return -1;

    memmove(vs + newvlen, ve, (buf + len) - ve);
    memcpy(vs, newval, newvlen);

    fprintf(stderr,
        "[br5longpaper] XSC AREA y2: %ld → 999999 (was %.1fmm at current dpi)\n",
        y2, (double)y2 / 300.0 * 25.4);

    return len + delta;
}

/*
 * Modify a brscan5 scan command in-place to request long paper scanning.
 *
 *   buf / len   — command to modify (already copied)
 *   max_len     — allocated size
 *   mode        — 1 = WIDE, 2 = NARROW
 *
 * Returns new length (>= original), or -1 on failure (caller uses original).
 *
 * Command-aware behaviour:
 *
 *   SSP — scan parameter setup:
 *     • PTYPE=NORMAL → LONGPAPER_WIDE or LONGPAPER_NARROW
 *     • LONG=OFF → LONG=ON  (uses replace, not insert, to handle existing key)
 *
 *   XSC — execute scan with area coordinates:
 *     • AREA=x1,y1,x2,y2 → replace y2 with 999999 so the hardware-limited
 *       ADF-exit detection (ALLEND) governs the stop instead of a pixel count
 *       that corresponds to ~355 mm.
 *
 *   CKD — source/capability check: left untouched (not a scan command).
 *
 * Auto-stop behaviour:
 *   With PTYPE=LONGPAPER_WIDE/NARROW and LONG=ON the scanner switches from
 *   coordinate-based stopping to ADF-exit detection: it scans until the
 *   paper physically exits the feed rollers, then sends ALLEND — identical
 *   to how the Windows driver works.
 *
 * Hardware maximums (scanner enforces regardless of software values):
 *   DS-740D:  72 inches (1828.8 mm)
 *   ADS-1200: verify with Brother specification / Windows driver
 */
static int patch_cmd_long_paper(uint8_t *buf, int len, int max_len,
                                 int mode) {
    /* XSC: extend scan-area height so the scanner isn't told to stop at
     * the normal-paper pixel limit (~355 mm).                             */
    if (cmd_is(buf, len, "\x1bXSC\n"))
        return patch_xsc_area(buf, len, max_len);

    /* SSP: set PTYPE and enable LONG flag. */
    if (cmd_is(buf, len, "\x1bSSP\n")) {
        const char *ptype = (mode == 1) ? "LONGPAPER_WIDE" : "LONGPAPER_NARROW";

        /* Replace or insert PTYPE= */
        int r = buf_replace_val(buf, len, max_len, "PTYPE=", ptype);
        if (r == -2) return -1;
        if (r == -1) {
            len = buf_insert_kv(buf, len, max_len, "PTYPE=", ptype);
            if (len < 0) return -1;
        } else {
            len = r;
        }

        /* Replace LONG= value (OFF → ON) or insert if absent */
        r = buf_replace_val(buf, len, max_len, "LONG=", "ON");
        if (r == -2) return -1;
        if (r == -1) {
            r = buf_insert_kv(buf, len, max_len, "LONG=", "ON");
            if (r < 0) return -1;
            len = r;
        } else {
            len = r;
        }

        /* Enable LSMD (Long Scan Mode Detect): scanner uses ADF-exit
         * detection to stop the scan, same as Windows long paper mode. */
        r = buf_replace_val(buf, len, max_len, "LSMD=", "ON");
        if (r == -2) return -1;
        if (r == -1) {
            r = buf_insert_kv(buf, len, max_len, "LSMD=", "ON");
            if (r < 0) return -1;
            len = r;
        } else {
            len = r;
        }

        return len;
    }

    /* Unknown command type that slipped through is_scan_cmd — leave alone. */
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
                const char *cmd_type =
                    cmd_is(mod, length, "\x1bSSP\n") ? "SSP" :
                    cmd_is(mod, length, "\x1bXSC\n") ? "XSC" : "???";
                fprintf(stderr,
                    "[br5longpaper] USB patch %s: LONGPAPER_%s (%d→%d bytes)\n",
                    cmd_type, g_mode == 1 ? "WIDE" : "NARROW",
                    length, new_len);
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
        g_br_y_user_val = requested;

        /* Always try to push 1829mm (DS-740D hardware max) into the
         * backend during the option-setup phase.  brscan5 may accept
         * any value (SANE_STATUS_GOOD) but clamp it internally; the
         * SANE_INFO_INEXACT flag and a subsequent GET reveal the truth. */
        SANE_Fixed hw_max  = SANE_FIX(1829.0);
        SANE_Int   inf     = 0;
        SANE_Status r = real_ctrl_opt(handle, option, action, &hw_max, &inf);

        if (r == SANE_STATUS_GOOD) {
            SANE_Fixed actual = 0;
            real_ctrl_opt(handle, option, SANE_ACTION_GET_VALUE, &actual, NULL);
            fprintf(stderr,
                "[br5longpaper] br-y SET: requested %.1fmm,"
                " pushed 1829mm → stored %.1fmm%s\n",
                SANE_UNFIX(requested), SANE_UNFIX(actual),
                (inf & SANE_INFO_INEXACT) ? " [CLAMPED by backend]" : " [exact]");
            if (info) *info = inf;
            return SANE_STATUS_GOOD;
        }

        /* Backend rejected 1829mm outright — pass the original value */
        fprintf(stderr,
            "[br5longpaper] br-y SET: 1829mm rejected (status %d),"
            " passing %.1fmm\n", r, SANE_UNFIX(requested));
    }

    return real_ctrl_opt(handle, option, action, value, info);
}

/* ── 4. sane_get_parameters ────────────────────────────────────── */
/*
 * brscan5 computes the expected scan height (lines) from br-y, which it
 * caps at its internal ADF maximum (~355mm = 4200px at 300dpi).  scanimage
 * calls sane_get_parameters() after sane_start() and uses the returned
 * lines field as an upper bound for how many rows it will read.  With
 * lines=4200, scanimage stops at 4200 rows even if the scanner has more
 * data to send (because we put the scanner in LONGPAPER mode).
 *
 * Returning lines=-1 (SANE's "unknown height" sentinel) makes scanimage
 * switch to a streaming read loop that continues until sane_read() itself
 * returns SANE_STATUS_EOF.  brscan5 returns EOF only when the scanner
 * sends ALLEND — which happens when the paper physically exits the ADF,
 * giving us the same auto-stop behaviour as the Windows driver.
 *
 * Output format: use --format pnm (not png) so scanimage buffers
 * all rows in a temp file before writing the header.  scan_long.sh
 * auto-converts to PNG after the scan.
 */
SANE_Status sane_get_parameters(SANE_Handle handle, SANE_Parameters *params) {
    ensure_init();

    if (!real_get_params) return SANE_STATUS_UNSUPPORTED;

    SANE_Status st = real_get_params(handle, params);

    if (st == SANE_STATUS_GOOD && g_mode && params && params->lines > 0) {
        fprintf(stderr,
            "[br5longpaper] sane_get_parameters: lines %d → -1"
            " (streaming until scanner ALLEND)\n", params->lines);
        params->lines = -1;
    }

    return st;
}

/* ── 5. sane_start ─────────────────────────────────────────────── */
/*
 * brscan5 reads all scanner JPEG data during sane_start(), using its
 * internal scan height (derived from br-y, typically capped at ~355mm =
 * 4200px at 300dpi).  We must push a larger br-y value into the backend
 * BEFORE sane_start() is called, so it allocates a bigger read buffer
 * and continues reading until the scanner's ALLEND signal.
 *
 * Strategy:
 *   Try increasing br-y in 10mm steps from backend_max up to DS-740D's
 *   hardware maximum (1829mm = 72").  Most values will be rejected with
 *   SANE_STATUS_INVAL; the first accepted value extends the line budget.
 *   Any value larger than the document will still produce correct output
 *   because brscan5 stops at ALLEND (paper exit), not the configured max.
 *
 *   If the backend refuses every value above its reported max, we fall
 *   back to normal behaviour (USB patches still ensure LONGPAPER mode;
 *   the output may still be capped at 4200 lines in that case).
 */
SANE_Status sane_start(SANE_Handle handle) {
    ensure_init();

    if (!real_sane_start) return SANE_STATUS_UNSUPPORTED;

    if (g_mode && real_ctrl_opt && g_br_y_opt_num >= 0 &&
        g_br_y_backend_max > 0) {

        /* DS-740D hardware maximum: 72 inches = 1829mm */
        SANE_Fixed hw_max = SANE_FIX(1829.0);
        SANE_Fixed target = hw_max;
        SANE_Int   info   = 0;
        SANE_Status r;

        /* Try the full 72" first; if rejected, fall back to 500mm
         * increments down to 50mm above the backend max.             */
        static const double try_mm[] =
            { 1829.0, 1500.0, 1200.0, 900.0, 600.0, 0.0 };

        bool pushed = false;
        for (int i = 0; try_mm[i] > 0.0 && !pushed; i++) {
            double mm = try_mm[i];
            if (SANE_FIX(mm) <= g_br_y_backend_max) continue;
            target = SANE_FIX(mm);
            r = real_ctrl_opt(handle, g_br_y_opt_num,
                              SANE_ACTION_SET_VALUE, &target, &info);
            if (r == SANE_STATUS_GOOD) {
                /* Read back what brscan5 actually stored */
                SANE_Fixed actual = 0;
                real_ctrl_opt(handle, g_br_y_opt_num,
                              SANE_ACTION_GET_VALUE, &actual, NULL);
                fprintf(stderr,
                    "[br5longpaper] sane_start: br-y push %.0fmm →"
                    " stored %.1fmm%s (info=0x%x)\n",
                    mm, SANE_UNFIX(actual),
                    (info & SANE_INFO_INEXACT) ? " [CLAMPED]" : " [exact]",
                    (unsigned)info);
                pushed = true;
            }
        }
        if (!pushed) {
            fprintf(stderr,
                "[br5longpaper] sane_start: backend rejected all br-y"
                " values > %.1fmm; USB patches still active\n",
                SANE_UNFIX(g_br_y_backend_max));
        }
    }

    return real_sane_start(handle);
}

/* ── 6 & 7. DeviceImageJpeg::SetHeight / DecodeParameter::SetHeight ───────
 *
 * brscan5 computes scan height in pixels from br-y (clamped by capability
 * table to 355.6mm = 4200px at 300dpi) and calls these two C++ methods in
 * libLxBsScanCoreApi.so to configure the scan pipeline.  By the time any
 * SANE-level hook fires, these values are already locked in.
 *
 * We intercept the C++ mangled symbols directly via LD_PRELOAD.  When
 * long-paper mode is active and the height is at or below the capability
 * cap (< 30 000px = ~100" at 300dpi), we replace it with 65 536px
 * (~218" at 300dpi, 109" at 600dpi) — well above DS-740D's 72" hardware
 * max in all cases.  The scanner's own ALLEND (paper-exit) signal stops
 * the scan at the true document length, so the extra headroom is never
 * consumed and causes no memory overhead (the buffer grows line-by-line).
 *
 * Symbol names (from nm -D libLxBsScanCoreApi.so.3.2.1):
 *   _ZN15DeviceImageJpeg9SetHeightEj   DeviceImageJpeg::SetHeight(unsigned)
 *   _ZN15DecodeParameter9SetHeightEj   DecodeParameter::SetHeight(unsigned)
 */

/* Extend height used by the scanner JPEG capture pipeline. */
void _ZN15DeviceImageJpeg9SetHeightEj(void *self, unsigned int h) {
    if (!real_devimg_sh)
        real_devimg_sh = (cpp_setheight_fn)dlsym(RTLD_NEXT,
                             "_ZN15DeviceImageJpeg9SetHeightEj");
    ensure_init();
    if (g_mode && h > 0 && h < 30000) {
        fprintf(stderr,
            "[br5longpaper] DeviceImageJpeg::SetHeight %u → 65536 px\n", h);
        h = 65536;
    }
    if (real_devimg_sh) real_devimg_sh(self, h);
}

/* Extend height used by the scan-decode / output pipeline. */
void _ZN15DecodeParameter9SetHeightEj(void *self, unsigned int h) {
    if (!real_decodeparam_sh)
        real_decodeparam_sh = (cpp_setheight_fn)dlsym(RTLD_NEXT,
                                  "_ZN15DecodeParameter9SetHeightEj");
    ensure_init();
    if (g_mode && h > 0 && h < 30000) {
        fprintf(stderr,
            "[br5longpaper] DecodeParameter::SetHeight %u → 65536 px\n", h);
        h = 65536;
    }
    if (real_decodeparam_sh) real_decodeparam_sh(self, h);
}
