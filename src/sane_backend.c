/*
 * sane_backend.c — SANE backend "brother5lp" for long paper scanning
 *
 * Registered as 'brother5lp' in /etc/sane.d/dll.conf alongside the
 * normal 'brother5' backend.  GUI apps (Simple Scan, XSane, gscan2pdf,
 * GIMP) see the scanner as an additional device:
 *
 *   "Brother DS-740D [Long Paper]"   ← our backend, brother5lp:...
 *   "Brother DS-740D"                ← original brscan5, brother5:...
 *
 * Users select the [Long Paper] device once; no env vars, no LD_PRELOAD,
 * no patchelf.  Everything is handled internally.
 *
 * How the USB/C++ hooks work without LD_PRELOAD:
 *
 *   1. SANE's dll frontend loads backends with dlopen(RTLD_LOCAL).
 *      A __attribute__((constructor)) immediately promotes our library
 *      to RTLD_GLOBAL using dlopen(RTLD_NOLOAD|RTLD_GLOBAL), placing
 *      our symbols (libusb_bulk_transfer, C++ method overrides) in the
 *      process-wide global symbol table.
 *
 *   2. sane_init() loads brscan5 with dlopen(RTLD_NOW|RTLD_GLOBAL).
 *      Because our library is already in the global table, brscan5's
 *      PLT entries for libusb_bulk_transfer, DeviceImageJpeg::SetHeight,
 *      DecodeParameter::SetHeight, and BitmapImage::AppendWhiteLines all
 *      resolve to our hook functions.
 *
 *   3. All sane_* functions are implemented here and delegate to brscan5
 *      via explicit function pointers obtained from dlsym — with our
 *      long paper patches applied inline (extend br-y range, push 1829mm,
 *      lines=-1, NARROW/WIDE detection from br-x).
 *
 * Build:
 *   See Makefile target 'backend'
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

/* ── Constants ─────────────────────────────────────────────────── */

#define LONG_PAPER_MAX_MM  5000.0   /* exposed br-y max (mm) */
#define NARROW_WIDTH_MM     110.0   /* br-x below this → NARROW mode */
#define MAX_CMD_BUF        8192

/* ── Types ──────────────────────────────────────────────────────── */

typedef int  (*libusb_bulk_fn)(libusb_device_handle *, unsigned char,
                                unsigned char *, int, int *, unsigned int);
typedef void (*cpp_setheight_fn)(void *, unsigned int);
typedef void (*cpp_appendwhite_fn)(void *, unsigned int);

/* brscan5 SANE function pointer types */
typedef SANE_Status          (*fn_init)      (SANE_Int *, SANE_Auth_Callback);
typedef void                  (*fn_exit)      (void);
typedef SANE_Status          (*fn_get_devs)  (const SANE_Device ***, SANE_Bool);
typedef SANE_Status          (*fn_open)      (SANE_String_Const, SANE_Handle *);
typedef void                  (*fn_close)     (SANE_Handle);
typedef const SANE_Option_Descriptor *
                              (*fn_get_opt)  (SANE_Handle, SANE_Int);
typedef SANE_Status          (*fn_ctrl)      (SANE_Handle, SANE_Int,
                                               SANE_Action, void *, SANE_Int *);
typedef SANE_Status          (*fn_get_params)(SANE_Handle, SANE_Parameters *);
typedef SANE_Status          (*fn_start)     (SANE_Handle);
typedef SANE_Status          (*fn_read)      (SANE_Handle, SANE_Byte *,
                                               SANE_Int, SANE_Int *);
typedef void                  (*fn_cancel)    (SANE_Handle);
typedef SANE_Status          (*fn_set_io)    (SANE_Handle, SANE_Bool);
typedef SANE_Status          (*fn_get_fd)    (SANE_Handle, SANE_Int *);
typedef SANE_String_Const    (*fn_strstatus) (SANE_Status);

/* ── brscan5 function pointers ─────────────────────────────────── */

static void         *g_brscan5      = NULL;

static fn_init       b5_init        = NULL;
static fn_exit       b5_exit        = NULL;
static fn_get_devs   b5_get_devs    = NULL;
static fn_open       b5_open        = NULL;
static fn_close      b5_close       = NULL;
static fn_get_opt    b5_get_opt     = NULL;
static fn_ctrl       b5_ctrl        = NULL;
static fn_get_params b5_get_params  = NULL;
static fn_start      b5_start       = NULL;
static fn_read       b5_read        = NULL;
static fn_cancel     b5_cancel      = NULL;
static fn_set_io     b5_set_io      = NULL;
static fn_get_fd     b5_get_fd      = NULL;
static fn_strstatus  b5_strstatus   = NULL;

/* ── Hook function pointers (lazily resolved via RTLD_NEXT) ─────── */

static libusb_bulk_fn     real_usb_bulk       = NULL;
static cpp_setheight_fn   real_devimg_sh      = NULL;
static cpp_setheight_fn   real_decodeparam_sh = NULL;
static cpp_appendwhite_fn real_append_white   = NULL;

/* ── Per-session scan state ─────────────────────────────────────── */

static int        g_mode             = 1;   /* 1=WIDE, 2=NARROW; always ON for this backend */
static SANE_Fixed g_br_y_user_val    = 0;   /* br-y requested by frontend (mm, SANE_Fixed)  */
static SANE_Fixed g_br_x_user_val    = 0;   /* br-x requested by frontend                   */
static SANE_Fixed g_br_y_backend_max = 0;   /* brscan5's original br-y maximum              */
static int        g_resolution       = 300; /* DPI, updated from sane_control_option         */
static SANE_Int   g_br_y_opt_num     = -1;
static SANE_Int   g_br_x_opt_num     = -1;
static SANE_Int   g_res_opt_num      = -1;

/* Extended br-y option descriptor */
static SANE_Option_Descriptor g_br_y_desc;
static SANE_Range              g_br_y_range;

/* ── Constructor: promote to RTLD_GLOBAL ────────────────────────── */
/*
 * SANE's dll frontend calls dlopen("libsane-brother5lp.so.1", RTLD_LOCAL).
 * We need our symbols in the global table BEFORE brscan5 is loaded so that
 * brscan5's PLT bindings for libusb_bulk_transfer and the C++ methods
 * resolve to our hooks.  dlopen with (RTLD_NOLOAD|RTLD_GLOBAL) on an
 * already-resident library promotes it from local to global scope.
 * We find our own path via dladdr so no filename is hard-coded.
 */
__attribute__((constructor))
static void br5lp_promote_global(void)
{
    Dl_info info;
    if (dladdr((void *)br5lp_promote_global, &info) && info.dli_fname) {
        void *h = dlopen(info.dli_fname, RTLD_LAZY | RTLD_GLOBAL | RTLD_NOLOAD);
        if (h)
            fprintf(stderr, "[brother5lp] promoted to RTLD_GLOBAL\n");
        else
            fprintf(stderr, "[brother5lp] WARNING: RTLD_GLOBAL promotion"
                    " failed: %s\n", dlerror());
    }
}

/* ══════════════════════════════════════════════════════════════════
 * USB command patching (mirrors longpaper_hook.c; kept self-contained)
 * ════════════════════════════════════════════════════════════════ */

static uint8_t *buf_find(const uint8_t *hay, int hlen, const char *needle)
{
    int nlen = (int)strlen(needle);
    for (int i = 0; i <= hlen - nlen; i++)
        if (memcmp(hay + i, needle, nlen) == 0)
            return (uint8_t *)(hay + i);
    return NULL;
}

static int buf_replace_val(uint8_t *buf, int len, int max_len,
                            const char *key, const char *new_val)
{
    uint8_t *p = buf_find(buf, len, key);
    if (!p) return -1;
    uint8_t *vs = p + strlen(key);
    uint8_t *ve = vs;
    while (ve < buf + len && *ve != '\r' && *ve != '\n' && *ve != '\0') ve++;
    int olen = (int)(ve - vs), nlen = (int)strlen(new_val), delta = nlen - olen;
    if (len + delta > max_len) return -2;
    memmove(vs + nlen, ve, (buf + len) - ve);
    memcpy(vs, new_val, nlen);
    return len + delta;
}

static int buf_insert_kv(uint8_t *buf, int len, int max_len,
                          const char *key, const char *val)
{
    if (buf_find(buf, len, key)) return len;
    char token[128];
    int tlen = snprintf(token, sizeof(token), "%s%s\r\n", key, val);
    if (tlen <= 0 || tlen >= (int)sizeof(token) || len + tlen > max_len) return -1;
    int ins = (len > 0 && buf[len - 1] == 0x80) ? len - 1 : len;
    memmove(buf + ins + tlen, buf + ins, len - ins);
    memcpy(buf + ins, token, tlen);
    return len + tlen;
}

static bool cmd_is(const uint8_t *buf, int len, const char *hdr)
{
    int hlen = (int)strlen(hdr);
    return len >= hlen && memcmp(buf, hdr, hlen) == 0;
}

static bool is_scan_cmd(const uint8_t *data, int length)
{
    if (length < 6 || length > MAX_CMD_BUF) return false;
    return cmd_is(data, length, "\x1bSSP\n") || cmd_is(data, length, "\x1bXSC\n");
}

static int patch_xsc_area(uint8_t *buf, int len, int max_len)
{
    uint8_t *p = buf_find(buf, len, "AREA=");
    if (!p) return len;
    uint8_t *vs = p + 5, *ve = vs;
    while (ve < buf + len && *ve != '\r' && *ve != '\n' && *ve != '\0') ve++;
    int vlen = (int)(ve - vs);
    if (vlen <= 0 || vlen >= 64) return len;
    char val[64]; memcpy(val, vs, vlen); val[vlen] = '\0';
    long x1, y1, x2, y2;
    if (sscanf(val, "%ld,%ld,%ld,%ld", &x1, &y1, &x2, &y2) != 4) return len;

    long y2_new;
    if (g_br_y_user_val > 0) {
        double mm = SANE_UNFIX(g_br_y_user_val);
        long calc  = (long)(mm / 25.4 * g_resolution + 0.5);
        y2_new = (calc < 999999) ? calc : 999999;
        fprintf(stderr, "[brother5lp] XSC AREA y2: %ld → %ld (%.1fmm @ %ddpi)\n",
                y2, y2_new, mm, g_resolution);
    } else {
        y2_new = 999999;
        fprintf(stderr, "[brother5lp] XSC AREA y2: %ld → %ld (scanner ALLEND)\n",
                y2, y2_new);
    }

    char newval[64];
    int nvlen = snprintf(newval, sizeof(newval), "%ld,%ld,%ld,%ld", x1, y1, x2, y2_new);
    int delta = nvlen - vlen;
    if (len + delta > max_len) return -1;
    memmove(vs + nvlen, ve, (buf + len) - ve);
    memcpy(vs, newval, nvlen);
    return len + delta;
}

static int patch_cmd_long_paper(uint8_t *buf, int len, int max_len, int mode)
{
    if (cmd_is(buf, len, "\x1bXSC\n"))
        return patch_xsc_area(buf, len, max_len);

    if (cmd_is(buf, len, "\x1bSSP\n")) {
        const char *ptype = (mode == 2) ? "LONGPAPER_NARROW" : "LONGPAPER_WIDE";
        int r = buf_replace_val(buf, len, max_len, "PTYPE=", ptype);
        if (r == -2) return -1;
        if (r == -1) { len = buf_insert_kv(buf, len, max_len, "PTYPE=", ptype); if (len < 0) return -1; }
        else len = r;

        r = buf_replace_val(buf, len, max_len, "LONG=", "ON");
        if (r == -2) return -1;
        if (r == -1) { r = buf_insert_kv(buf, len, max_len, "LONG=", "ON"); if (r < 0) return -1; len = r; }
        else len = r;

        r = buf_replace_val(buf, len, max_len, "LSMD=", "ON");
        if (r == -2) return -1;
        if (r == -1) { r = buf_insert_kv(buf, len, max_len, "LSMD=", "ON"); if (r < 0) return -1; len = r; }
        else len = r;

        return len;
    }
    return len;
}

/* ══════════════════════════════════════════════════════════════════
 * Hook functions — libusb + C++ methods
 *
 * These are in the global symbol table (via br5lp_promote_global).
 * When brscan5 is subsequently loaded with RTLD_GLOBAL, its PLT
 * entries for these symbols bind to our versions here.
 *
 * RTLD_NEXT finds the real implementations because brscan5's
 * dependencies (libusb, libLxBsScanCoreApi) are loaded AFTER us.
 * ════════════════════════════════════════════════════════════════ */

int libusb_bulk_transfer(libusb_device_handle *dev_handle,
                          unsigned char endpoint,
                          unsigned char *data, int length,
                          int *actual_length, unsigned int timeout)
{
    if (!real_usb_bulk)
        real_usb_bulk = (libusb_bulk_fn)dlsym(RTLD_NEXT, "libusb_bulk_transfer");

    if (!(endpoint & LIBUSB_ENDPOINT_IN) && is_scan_cmd(data, length)) {
        int extra = 256;
        uint8_t *mod = malloc(length + extra);
        if (mod) {
            memcpy(mod, data, length);
            int new_len = patch_cmd_long_paper(mod, length, length + extra, g_mode);
            if (new_len > 0) {
                const char *ct = cmd_is(mod, length, "\x1bSSP\n") ? "SSP" : "XSC";
                fprintf(stderr, "[brother5lp] USB patch %s: LONGPAPER_%s (%d→%d bytes)\n",
                        ct, g_mode == 2 ? "NARROW" : "WIDE", length, new_len);
                int rc = real_usb_bulk(dev_handle, endpoint, mod,
                                       new_len, actual_length, timeout);
                free(mod);
                return rc;
            }
            free(mod);
        }
    }

    return real_usb_bulk
           ? real_usb_bulk(dev_handle, endpoint, data, length, actual_length, timeout)
           : LIBUSB_ERROR_OTHER;
}

/* Extend height in scan pipeline beyond brscan5's ADF cap (4200px / 355mm) */
void _ZN15DeviceImageJpeg9SetHeightEj(void *self, unsigned int h)
{
    if (!real_devimg_sh)
        real_devimg_sh = (cpp_setheight_fn)dlsym(RTLD_NEXT,
                             "_ZN15DeviceImageJpeg9SetHeightEj");
    if (h > 0 && h < 30000) {
        fprintf(stderr, "[brother5lp] DeviceImageJpeg::SetHeight %u → 65536\n", h);
        h = 65536;
    }
    if (real_devimg_sh) real_devimg_sh(self, h);
}

void _ZN15DecodeParameter9SetHeightEj(void *self, unsigned int h)
{
    if (!real_decodeparam_sh)
        real_decodeparam_sh = (cpp_setheight_fn)dlsym(RTLD_NEXT,
                                  "_ZN15DecodeParameter9SetHeightEj");
    if (h > 0 && h < 30000) {
        fprintf(stderr, "[brother5lp] DecodeParameter::SetHeight %u → 65536\n", h);
        h = 65536;
    }
    if (real_decodeparam_sh) real_decodeparam_sh(self, h);
}

/* Suppress white-row padding after ALLEND — only real scan data remains */
void _ZN11BitmapImage16AppendWhiteLinesEj(void *self, unsigned int n)
{
    if (!real_append_white)
        real_append_white = (cpp_appendwhite_fn)dlsym(RTLD_NEXT,
                                "_ZN11BitmapImage16AppendWhiteLinesEj");
    fprintf(stderr,
        "[brother5lp] BitmapImage::AppendWhiteLines(%u) suppressed\n", n);
    /* no-op — caller does not get white padding rows */
}

/* ══════════════════════════════════════════════════════════════════
 * brscan5 loading
 * ════════════════════════════════════════════════════════════════ */

#define B5_LOAD(var, sym) \
    do { \
        (var) = dlsym(g_brscan5, (sym)); \
        if (!(var)) { \
            fprintf(stderr, "[brother5lp] ERROR: brscan5 missing symbol: %s\n", (sym)); \
            return SANE_STATUS_UNSUPPORTED; \
        } \
    } while (0)

static SANE_Status load_brscan5(void)
{
    if (g_brscan5) return SANE_STATUS_GOOD;   /* already loaded */

    g_brscan5 = dlopen("libsane-brother5.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!g_brscan5) {
        fprintf(stderr, "[brother5lp] ERROR: cannot load brscan5: %s\n", dlerror());
        fprintf(stderr, "[brother5lp]   Is the 'brscan5' package installed?\n");
        return SANE_STATUS_UNSUPPORTED;
    }
    fprintf(stderr, "[brother5lp] loaded brscan5 via dlopen(RTLD_GLOBAL)\n");

    B5_LOAD(b5_init,       "sane_init");
    B5_LOAD(b5_exit,       "sane_exit");
    B5_LOAD(b5_get_devs,   "sane_get_devices");
    B5_LOAD(b5_open,       "sane_open");
    B5_LOAD(b5_close,      "sane_close");
    B5_LOAD(b5_get_opt,    "sane_get_option_descriptor");
    B5_LOAD(b5_ctrl,       "sane_control_option");
    B5_LOAD(b5_get_params, "sane_get_parameters");
    B5_LOAD(b5_start,      "sane_start");
    B5_LOAD(b5_read,       "sane_read");
    B5_LOAD(b5_cancel,     "sane_cancel");
    b5_set_io    = dlsym(g_brscan5, "sane_set_io_mode");    /* optional */
    b5_get_fd    = dlsym(g_brscan5, "sane_get_select_fd");  /* optional */
    b5_strstatus = dlsym(g_brscan5, "sane_strstatus");      /* optional */

    return SANE_STATUS_GOOD;
}

#undef B5_LOAD

/* ══════════════════════════════════════════════════════════════════
 * Device list — clone brscan5's list, append " [Long Paper]" to model
 * ════════════════════════════════════════════════════════════════ */

static const SANE_Device **g_dev_list = NULL;

static void free_dev_list(void)
{
    if (!g_dev_list) return;
    for (int i = 0; g_dev_list[i]; i++) {
        free((void *)g_dev_list[i]->name);
        free((void *)g_dev_list[i]->vendor);
        free((void *)g_dev_list[i]->model);
        free((void *)g_dev_list[i]->type);
        free((void *)g_dev_list[i]);
    }
    free(g_dev_list);
    g_dev_list = NULL;
}

static SANE_Status build_dev_list(const SANE_Device **src)
{
    free_dev_list();
    int n = 0;
    while (src[n]) n++;

    g_dev_list = calloc(n + 1, sizeof(SANE_Device *));
    if (!g_dev_list) return SANE_STATUS_NO_MEM;

    for (int i = 0; i < n; i++) {
        SANE_Device *d = malloc(sizeof(SANE_Device));
        if (!d) { free_dev_list(); return SANE_STATUS_NO_MEM; }

        char model_buf[256];
        snprintf(model_buf, sizeof(model_buf), "%s [Long Paper]",
                 src[i]->model ? src[i]->model : "Scanner");

        d->name   = strdup(src[i]->name   ? src[i]->name   : "");
        d->vendor  = strdup(src[i]->vendor ? src[i]->vendor : "");
        d->model   = strdup(model_buf);
        d->type    = strdup(src[i]->type   ? src[i]->type   : "");

        if (!d->name || !d->vendor || !d->model || !d->type) {
            free((void*)d->name); free((void*)d->vendor);
            free((void*)d->model); free((void*)d->type); free(d);
            free_dev_list();
            return SANE_STATUS_NO_MEM;
        }
        g_dev_list[i] = d;
    }
    g_dev_list[n] = NULL;
    return SANE_STATUS_GOOD;
}

/* ══════════════════════════════════════════════════════════════════
 * SANE backend API — required exports
 * ════════════════════════════════════════════════════════════════ */

SANE_Status sane_init(SANE_Int *version_code, SANE_Auth_Callback authorize)
{
    SANE_Status r = load_brscan5();
    if (r != SANE_STATUS_GOOD) return r;
    return b5_init(version_code, authorize);
}

void sane_exit(void)
{
    if (b5_exit) b5_exit();
    free_dev_list();
    if (g_brscan5) { dlclose(g_brscan5); g_brscan5 = NULL; }
}

SANE_Status sane_get_devices(const SANE_Device ***device_list,
                               SANE_Bool local_only)
{
    const SANE_Device **raw = NULL;
    SANE_Status r = b5_get_devs(&raw, local_only);
    if (r != SANE_STATUS_GOOD || !raw) return r;

    r = build_dev_list(raw);
    if (r != SANE_STATUS_GOOD) return r;

    *device_list = g_dev_list;
    return SANE_STATUS_GOOD;
}

SANE_Status sane_open(SANE_String_Const devicename, SANE_Handle *handle)
{
    /* Reset per-session state on each open */
    g_br_y_user_val    = 0;
    g_br_x_user_val    = 0;
    g_br_y_backend_max = 0;
    g_resolution       = 300;
    g_br_y_opt_num     = -1;
    g_br_x_opt_num     = -1;
    g_res_opt_num      = -1;
    g_mode             = 1; /* default WIDE; updated to NARROW if br-x is narrow */

    return b5_open(devicename, handle);
}

void sane_close(SANE_Handle handle)
{
    b5_close(handle);
}

const SANE_Option_Descriptor *
sane_get_option_descriptor(SANE_Handle handle, SANE_Int option)
{
    const SANE_Option_Descriptor *d = b5_get_opt(handle, option);
    if (!d || !d->name) return d;

    /* Track option indices for use in sane_control_option */
    if      (strcmp(d->name, "br-x")      == 0) g_br_x_opt_num = option;
    else if (strcmp(d->name, "resolution") == 0) g_res_opt_num  = option;

    if (strcmp(d->name, "br-y") == 0) {
        g_br_y_opt_num = option;

        if (d->constraint_type == SANE_CONSTRAINT_RANGE &&
            d->type            == SANE_TYPE_FIXED &&
            d->constraint.range) {

            g_br_y_backend_max = d->constraint.range->max;

            memcpy(&g_br_y_desc,  d,                   sizeof(g_br_y_desc));
            memcpy(&g_br_y_range, d->constraint.range, sizeof(g_br_y_range));
            g_br_y_range.max             = SANE_FIX(LONG_PAPER_MAX_MM);
            g_br_y_desc.constraint.range = &g_br_y_range;

            fprintf(stderr,
                "[brother5lp] br-y range extended: %.1fmm → %.0fmm\n",
                SANE_UNFIX(g_br_y_backend_max), LONG_PAPER_MAX_MM);
            return &g_br_y_desc;
        }
    }

    return d;
}

SANE_Status sane_control_option(SANE_Handle handle, SANE_Int option,
                                 SANE_Action action, void *value,
                                 SANE_Int *info)
{
    if (action == SANE_ACTION_SET_VALUE && value) {

        /* Track resolution */
        if (option == g_res_opt_num)
            g_resolution = (int)*(SANE_Int *)value;

        /* Track br-x; pick NARROW mode if receipt-width paper */
        if (option == g_br_x_opt_num) {
            g_br_x_user_val = *(SANE_Fixed *)value;
            double w = SANE_UNFIX(g_br_x_user_val);
            int new_mode = (w > 0.0 && w < NARROW_WIDTH_MM) ? 2 : 1;
            if (new_mode != g_mode) {
                g_mode = new_mode;
                fprintf(stderr,
                    "[brother5lp] br-x=%.1fmm → mode=%s\n",
                    w, g_mode == 1 ? "WIDE" : "NARROW");
            }
        }

        /* br-y: save true requested value, then push 1829mm to backend
         * so brscan5 allocates a full-length read buffer.              */
        if (option == g_br_y_opt_num) {
            g_br_y_user_val = *(SANE_Fixed *)value;

            SANE_Fixed hw_max = SANE_FIX(1829.0);
            SANE_Int   inf    = 0;
            SANE_Status r = b5_ctrl(handle, option, action, &hw_max, &inf);
            if (r == SANE_STATUS_GOOD) {
                SANE_Fixed actual = 0;
                b5_ctrl(handle, option, SANE_ACTION_GET_VALUE, &actual, NULL);
                fprintf(stderr,
                    "[brother5lp] br-y SET: requested %.1fmm,"
                    " pushed 1829mm → stored %.1fmm%s\n",
                    SANE_UNFIX(g_br_y_user_val), SANE_UNFIX(actual),
                    (inf & SANE_INFO_INEXACT) ? " [clamped]" : "");
                if (info) *info = inf;
                return SANE_STATUS_GOOD;
            }
            /* backend rejected 1829mm — fall through with original value */
        }
    }

    return b5_ctrl(handle, option, action, value, info);
}

SANE_Status sane_get_parameters(SANE_Handle handle, SANE_Parameters *params)
{
    SANE_Status r = b5_get_params(handle, params);
    if (r == SANE_STATUS_GOOD && params && params->lines > 0) {
        fprintf(stderr,
            "[brother5lp] sane_get_parameters: lines %d → -1"
            " (streaming until scanner ALLEND)\n", params->lines);
        params->lines = -1;
    }
    return r;
}

SANE_Status sane_start(SANE_Handle handle)
{
    /* Push the hardware maximum to brscan5 right before starting so it
     * allocates the largest possible read buffer.  patch_xsc_area then
     * caps the AREA y2 at the value actually requested by the frontend. */
    if (g_br_y_opt_num >= 0 && g_br_y_backend_max > 0) {
        SANE_Fixed hw  = SANE_FIX(1829.0);
        SANE_Int   inf = 0;
        SANE_Status r = b5_ctrl(handle, g_br_y_opt_num,
                                SANE_ACTION_SET_VALUE, &hw, &inf);
        if (r == SANE_STATUS_GOOD) {
            SANE_Fixed actual = 0;
            b5_ctrl(handle, g_br_y_opt_num, SANE_ACTION_GET_VALUE, &actual, NULL);
            fprintf(stderr,
                "[brother5lp] sane_start: br-y pushed → %.1fmm%s\n",
                SANE_UNFIX(actual), (inf & SANE_INFO_INEXACT) ? " [clamped]" : "");
        }
    }
    return b5_start(handle);
}

SANE_Status sane_read(SANE_Handle handle, SANE_Byte *data,
                       SANE_Int max_length, SANE_Int *length)
{
    return b5_read(handle, data, max_length, length);
}

void sane_cancel(SANE_Handle handle)
{
    b5_cancel(handle);
}

SANE_Status sane_set_io_mode(SANE_Handle handle, SANE_Bool non_blocking)
{
    return b5_set_io ? b5_set_io(handle, non_blocking) : SANE_STATUS_UNSUPPORTED;
}

SANE_Status sane_get_select_fd(SANE_Handle handle, SANE_Int *fd)
{
    return b5_get_fd ? b5_get_fd(handle, fd) : SANE_STATUS_UNSUPPORTED;
}

SANE_String_Const sane_strstatus(SANE_Status status)
{
    return b5_strstatus ? b5_strstatus(status) : "unknown status";
}
