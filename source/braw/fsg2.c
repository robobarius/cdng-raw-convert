/*
 * fsg2.c: fpSup Gyro2 telemetry (FSG2 block, payload kind 2, in each DNG's MakerNote tail)
 * turned into Gyroflow Protobuf messages, for embedding in a ProRes RAW .mov.
 *
 * Part of cDNG Raw Convert (fpSup community).
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a C port of ijigen/gyroflow-plugins common/src/fsg2.rs (decoder and
 * to_messages_with), lensfit.rs (the camera's distortion fit) and the MakerNote block
 * readers in cdng.rs, at tag fpsup-v0.1.10 (commit f0ad113, 2026-10-15: Gyro2 v0.2.0's
 * block version 2 in its own SIGMA MakerNote tag, payload version 2, zoomed takes;
 * first ported at 4b3dbac, 2026-10-08), kept to the same arithmetic so a
 * .mov made here carries the same messages the Gyroflow (fpSup) OpenFX plugin builds
 * from the DNG sequence. Those files are GPL-3.0-or-later, so this one is too; a binary
 * that links it is distributed under GPL-3.0-or-later (the LGPL-2.1+ parts allow that).
 *
 * Message schema: telemetry-parser src/gyroflow/gyroflow.proto (Gyroflow Protobuf v1).
 */
#include "fsg2.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define BLOCK_HEADER 12
#define HEADER_BYTES 48
#define MAX_PAYLOAD (4u << 20)
#define FLAG_DATA_LOST 1
#define FLAG_TRUNCATED 2
#define FLAG_OIS_ON 16
#define FLAG_EIS_ON 32
#define FLAG_LENS_TABLE 64
#define EVENT_LEVEL 1
#define EVENT_FOCUS 2
#define EVENT_VD 3
#define SENSOR_W 6000
#define SENSOR_H 4000
#define PIXEL_PITCH_NM 5983
#define STANDARD_GRAVITY 9.80665
#define VD_TO_READOUT_US 900.0
#define SENSOR_ACTIVE_MM 35.9
#define SENSOR_CLOCK_HZ 72.0e6           /* the IMX410's readout clock: a version 2 block's hmax counts it */
#define SAME_FOCAL_MM 0.05               /* focal lengths closer than this are one zoom position */
#define GYRO2_MAKER_NOTE_TAG 0x4653      /* Gyro2 v0.2.0+: its own entry in the SIGMA MakerNote (offsets from the file start) */

/* ------------------------------------------------------------------ CRC-32 (IEEE) -- */
static uint32_t crc32_ieee(const uint8_t *p, size_t n) {
    static uint32_t t[256]; static int init = 0;
    if (!init) { for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1; t[i] = c; } init = 1; }
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = t[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

/* ------------------------------------------------------------------ TIFF reading -- */
typedef struct { uint16_t tag, type; uint32_t count; uint8_t raw[4]; } TE;
typedef struct { FILE *fh; int le; } TR;
static uint32_t g32(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t g16(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; }
static int tsize(int t) { static const int s[13] = {0,1,1,2,4,8,1,1,2,4,8,4,8}; return (t >= 1 && t <= 12) ? s[t] : 0; }

static int read_ifd(TR *r, uint32_t off, TE **out, int *n) {
    uint8_t c[2];
    if (fseek(r->fh, off, SEEK_SET) || fread(c, 1, 2, r->fh) != 2) return -1;
    int cnt = g16(c, r->le); if (cnt <= 0 || cnt > 1000) return -1;
    TE *e = calloc(cnt, sizeof(TE));
    for (int i = 0; i < cnt; i++) {
        uint8_t b[12]; if (fread(b, 1, 12, r->fh) != 12) { free(e); return -1; }
        e[i].tag = g16(b, r->le); e[i].type = g16(b + 2, r->le); e[i].count = g32(b + 4, r->le); memcpy(e[i].raw, b + 8, 4);
    }
    *out = e; *n = cnt; return 0;
}
static const TE *find(const TE *e, int n, int tag) { for (int i = 0; i < n; i++) if (e[i].tag == tag) return &e[i]; return NULL; }
/* the value bytes of an entry, malloc'd (NULL if too big or unreadable) */
static uint8_t *bytes_of(TR *r, const TE *e, size_t *len) {
    if (!e) return NULL;
    size_t n = (size_t)tsize(e->type) * e->count; if (!n || n > MAX_PAYLOAD * 4) return NULL;
    uint8_t *b = malloc(n);
    if (n <= 4) memcpy(b, e->raw, n);
    else if (fseek(r->fh, g32(e->raw, r->le), SEEK_SET) || fread(b, 1, n, r->fh) != n) { free(b); return NULL; }
    if (len) *len = n;
    return b;
}
static void ascii_of(TR *r, const TE *e, char *dst, size_t cap) {
    dst[0] = 0; size_t n; uint8_t *b = bytes_of(r, e, &n); if (!b || e->type != 2) { free(b); return; }
    size_t k = 0; while (k < n && b[k]) k++;
    size_t s = 0; while (s < k && (b[s] == ' ' || b[s] == '\t')) s++;
    while (k > s && (b[k - 1] == ' ' || b[k - 1] == '\t' || b[k - 1] == '\n')) k--;
    size_t m = k - s < cap - 1 ? k - s : cap - 1; memcpy(dst, b + s, m); dst[m] = 0; free(b);
}
/* first numeric value of an entry, or NAN */
static double num_of(TR *r, const TE *e) {
    if (!e || !e->count) return NAN;
    size_t n; uint8_t *b = bytes_of(r, e, &n); if (!b) return NAN;
    double v = NAN; int le = r->le;
    switch (e->type) {
        case 1: v = b[0]; break;
        case 3: v = g16(b, le); break;
        case 4: v = g32(b, le); break;
        case 8: v = (int16_t)g16(b, le); break;
        case 9: v = (int32_t)g32(b, le); break;
        case 5: v = (double)g32(b, le) / (double)g32(b + 4, le); break;
        case 10: v = (double)(int32_t)g32(b, le) / (double)(int32_t)g32(b + 4, le); break;
        case 11: { uint32_t u = g32(b, le); float f; memcpy(&f, &u, 4); v = f; } break;
        case 12: { uint64_t u = le ? (uint64_t)g32(b + 4, le) << 32 | g32(b, le) : (uint64_t)g32(b, le) << 32 | g32(b + 4, le); memcpy(&v, &u, 8); } break;
    }
    free(b); return v;
}
static double positive(double v) { return (isfinite(v) && v > 0) ? v : 0; }

/* lensfit.rs: valid / warp_rectilinear */
static int kr_valid(const double kr[4]) { for (int i = 0; i < 4; i++) if (!isfinite(kr[i])) return 0; return kr[0] > 0.5 && kr[0] < 1.5; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static double be64f(const uint8_t *p) { uint64_t u = (uint64_t)be32(p) << 32 | be32(p + 4); double d; memcpy(&d, &u, 8); return d; }
static int warp_rectilinear(const uint8_t *l, size_t n, double kr[4]) {
    if (n < 4) return 0;
    uint32_t count = be32(l); size_t at = 4;
    for (uint32_t k = 0; k < count && k < 64; k++) {
        if (at + 16 > n) return 0;
        uint32_t id = be32(l + at), size = be32(l + at + 12); size_t body = at + 16;
        if (id == 1) {
            if (body + 4 > n) return 0;
            uint32_t planes = be32(l + body); if (!planes || planes > 4) return 0;
            size_t base = body + 4 + (planes >= 3 ? 1 : 0) * 6 * 8;
            if (base + 32 > n) return 0;
            for (int i = 0; i < 4; i++) kr[i] = be64f(l + base + 8 * i);
            return kr_valid(kr);
        }
        at = body + size;
    }
    return 0;
}

static int parse_payload(const uint8_t *p, size_t n, Fsg2Frame *f, char *err, size_t el) {
    #define NEED(k, what) do { if (at + (k) > n) { snprintf(err, el, "kind 2 payload truncated in %s", what); return -1; } } while (0)
    size_t at = 0;
    NEED(HEADER_BYTES, "header");
    if (p[0] != 1 && p[0] != 2) { snprintf(err, el, "unsupported kind 2 format version %d", p[0]); return -1; }
    if (p[1] != 0) { snprintf(err, el, "unknown kind 2 orientation code %d", p[1]); return -1; }
    f->fmt_version = p[0];
    f->flags = g16(p + 2, 1); f->clip_id = g32(p + 4, 1); f->frame_seq = g32(p + 8, 1); f->period_ps = g32(p + 12, 1);
    uint32_t u = g32(p + 16, 1); memcpy(&f->gscale, &u, 4); u = g32(p + 20, 1); memcpy(&f->ascale, &u, 4);
    if (f->fmt_version == 1) {
        f->readout_ns = g32(p + 24, 1); f->mode_id = g16(p + 28, 1);
        for (int i = 0; i < 4; i++) f->crop[i] = g16(p + 30 + 2 * i, 1);
    } else {
        /* version 2: the sensor mode as the camera's accessors give it; readout and window derived (fsg2.rs SensorMode) */
        f->mode_id = g16(p + 24, 1); f->hmax = g16(p + 26, 1); f->read_w = g16(p + 28, 1); f->lines = g16(p + 30, 1);
        f->bin_h = g16(p + 32, 1); f->bin_v = g16(p + 34, 1); f->calib2 = g16(p + 36, 1);
        if (f->mode_id == 0) { f->readout_ns = 0; memset(f->crop, 0, sizeof f->crop); }
        else {
            f->readout_ns = (uint32_t)llround((double)f->hmax * f->lines * 1.0e9 / SENSOR_CLOCK_HZ);
            uint32_t w = (uint32_t)f->read_w * (f->bin_h ? f->bin_h : 1), hh = (uint32_t)f->lines * (f->bin_v ? f->bin_v : 1);
            if (w > SENSOR_W) { w = SENSOR_W; }
            if (hh > SENSOR_H) { hh = SENSOR_H; }
            f->crop[0] = (uint16_t)((SENSOR_W - w) / 2); f->crop[1] = (uint16_t)((SENSOR_H - hh) / 2); f->crop[2] = (uint16_t)w; f->crop[3] = (uint16_t)hh;
        }
    }
    f->frame_mark = g16(p + 38, 1); f->base_idx = g32(p + 40, 1); f->n_samples = g16(p + 44, 1); f->n_events = g16(p + 46, 1);
    at = HEADER_BYTES;
    if (!f->period_ps) { snprintf(err, el, "kind 2 sample period is zero"); return -1; }
    if (!(isfinite(f->gscale) && f->gscale > 0) || !(isfinite(f->ascale) && f->ascale > 0)) { snprintf(err, el, "kind 2 gyro/accelerometer scale is not a positive number"); return -1; }
    NEED((size_t)f->n_samples * 6, "samples");
    f->samples = malloc(sizeof(int16_t[3]) * (f->n_samples ? f->n_samples : 1));
    for (int i = 0; i < f->n_samples; i++, at += 6) for (int k = 0; k < 3; k++) f->samples[i][k] = (int16_t)g16(p + at + 2 * k, 1);
    f->events = calloc(f->n_events ? f->n_events : 1, sizeof(Fsg2Event));
    for (int i = 0; i < f->n_events; i++) {
        NEED(4, "events");
        Fsg2Event *e = &f->events[i]; e->pos = g16(p + at, 1); e->kind = p[at + 2]; e->len = p[at + 3]; at += 4;
        NEED(e->len, "events");
        int expect = e->kind == EVENT_LEVEL ? 6 : (e->kind == EVENT_FOCUS || e->kind == EVENT_VD) ? 4 : -1;
        if (expect >= 0 && expect != e->len) { snprintf(err, el, "kind 2 event type %d has length %d", e->kind, e->len); return -1; }
        memcpy(e->v, p + at, e->len < 8 ? e->len : 8); at += e->len;
    }
    if (f->fmt_version == 1 && (f->flags & FLAG_LENS_TABLE)) {          /* version 2 has no lens table */
        NEED(4 + 20 + 5 * 3 * 4 * 8, "lens table");
        f->has_lens = 1; f->calib_focal = g32(p + at, 1); at += 4;
        for (int i = 0; i < 5; i++, at += 4) f->axis[i] = g32(p + at, 1);
        for (int a = 0; a < 5; a++) for (int b = 0; b < 3; b++) for (int c = 0; c < 4; c++, at += 8) {
            uint64_t v = (uint64_t)g32(p + at + 4, 1) << 32 | g32(p + at, 1); memcpy(&f->nodes[a][b][c], &v, 8);
        }
    }
    if (at != n) { snprintf(err, el, "kind 2 payload has %zu unexpected trailing bytes", n - at); return -1; }
    return 0;
    #undef NEED
}

void fsg2_free(Fsg2Frame *f) { free(f->samples); free(f->events); f->samples = NULL; f->events = NULL; }

int fsg2_read(const char *path, Fsg2Frame *f, char *err, size_t el) {
    memset(f, 0, sizeof *f); err[0] = 0;
    FILE *fh = fopen(path, "rb"); if (!fh) { snprintf(err, el, "cannot open"); return -1; }
    uint8_t h[8]; TR r = { fh, 1 }; TE *root = NULL, *exif = NULL; int nr = 0, nx = 0, rc = 0;
    if (fread(h, 1, 8, fh) != 8 || !((h[0] == 'I' && h[1] == 'I') || (h[0] == 'M' && h[1] == 'M'))) { fclose(fh); return 0; }
    r.le = h[0] == 'I';
    if (read_ifd(&r, g32(h + 4, r.le), &root, &nr)) { fclose(fh); return 0; }
    const TE *ex = find(root, nr, 0x8769);
    if (!ex || read_ifd(&r, g32(ex->raw, r.le), &exif, &nx)) { free(root); fclose(fh); return 0; }
    int found = 0; const uint8_t *payload = NULL; uint32_t plen = 0; int kind = 0; char rej[160] = "";
    uint8_t *mn = NULL, *tagv = NULL; size_t mn_len = 0;
    /* Gyro2 v0.2.0+ (cdng.rs gyro2_tag / parse_tag_block): a SIGMA MakerNote ("SIGMA\0\0\0", IFD at +10, offsets
       from the file start) with its own entry 0x4653, whose value starts (on the file's first word boundary) with a
       block of version 2 and no CRC. A MakerNote this can't walk is left to the tail search below. */
    const TE *mne = find(exif, nx, 0x927C);
    if (mne && mne->count >= 16 && (mne->type == 7 || mne->type == 1)) {
        uint32_t start = g32(mne->raw, r.le); uint8_t id[8]; TE *se = NULL; int ns = 0;
        if (!fseek(fh, start, SEEK_SET) && fread(id, 1, 8, fh) == 8 && !memcmp(id, "SIGMA\0\0\0", 8) && !read_ifd(&r, start + 10, &se, &ns)) {
            const TE *gt = NULL; int cnt = 0;
            for (int i = 0; i < ns; i++) if (se[i].tag == GYRO2_MAKER_NOTE_TAG) { gt = &se[i]; cnt++; }
            if (cnt > 1) { snprintf(err, el, "more than one Gyro2 MakerNote tag"); free(se); rc = -1; goto done; }
            if (gt) {
                size_t vl = 0; tagv = bytes_of(&r, gt, &vl);
                size_t pad = (4 - (g32(gt->raw, r.le) & 3)) & 3;
                const uint8_t *b = tagv ? tagv + pad : NULL; size_t bl = tagv && vl > pad ? vl - pad : 0;
                if (!b || bl < BLOCK_HEADER) { snprintf(err, el, "Gyro2 MakerNote tag is too short for a block"); free(se); rc = -1; goto done; }
                if (memcmp(b, "FSG2", 4)) { snprintf(err, el, "no telemetry block in the Gyro2 MakerNote tag (the camera had none for this frame)"); free(se); rc = -1; goto done; }
                if (b[4] != 2) { snprintf(err, el, "unsupported block version %d in the Gyro2 MakerNote tag", b[4]); free(se); rc = -1; goto done; }
                if (b[5] != 2) { snprintf(err, el, "unsupported block payload kind %d", b[5]); free(se); rc = -1; goto done; }
                uint32_t len = g32(b + 8, 1);
                if (!len || BLOCK_HEADER + (size_t)len > bl) { snprintf(err, el, "block payload length is invalid or exceeds the Gyro2 MakerNote tag"); free(se); rc = -1; goto done; }
                found = 1; kind = 2; payload = b + BLOCK_HEADER; plen = len; f->carrier = 2;
            }
        }
        free(se);
    }
    if (!found) mn = bytes_of(&r, mne, &mn_len);
    /* every "FSG2" must pass the checks; exactly one valid block */
    for (size_t at = 0; mn && at + BLOCK_HEADER <= mn_len; at++) {
        if (memcmp(mn + at, "FSG2", 4)) continue;
        const uint8_t *b = mn + at; uint32_t len = g32(b + 8, 1);
        if (b[4] != 1) { snprintf(rej, sizeof rej, "unsupported block version %d", b[4]); continue; }
        if (b[6] || b[7]) { snprintf(rej, sizeof rej, "reserved block bytes are not zero"); continue; }
        if (!len || len > MAX_PAYLOAD) { snprintf(rej, sizeof rej, "block payload length is invalid"); continue; }
        if (at + BLOCK_HEADER + (size_t)len + 4 > mn_len) { snprintf(rej, sizeof rej, "block exceeds MakerNote"); continue; }
        if (crc32_ieee(b, BLOCK_HEADER + len) != g32(b + BLOCK_HEADER + len, 1)) { snprintf(rej, sizeof rej, "block CRC mismatch"); continue; }
        if (b[5] != 1 && b[5] != 2) { snprintf(rej, sizeof rej, "unsupported block payload kind %d", b[5]); continue; }
        if (found) { snprintf(err, el, "more than one telemetry block in MakerNote"); rc = -1; goto done; }
        found = 1; kind = b[5]; payload = b + BLOCK_HEADER; plen = len; f->carrier = 1;
    }
    if (!found) { if (rej[0]) { snprintf(err, el, "invalid telemetry block in MakerNote: %s", rej); rc = -1; } goto done; }
    if (kind != 2) { snprintf(err, el, "telemetry block kind %d (a serialized Gyroflow message) is not converted", kind); goto done; }
    if (parse_payload(payload, plen, f, err, el)) { fsg2_free(f); rc = -1; goto done; }
    f->present = 1;
    ascii_of(&r, find(root, nr, 0x010F), f->make, sizeof f->make);
    ascii_of(&r, find(root, nr, 0x0110), f->model, sizeof f->model);
    ascii_of(&r, find(root, nr, 0x0131), f->software, sizeof f->software);
    ascii_of(&r, find(root, nr, 0xC62F), f->serial, sizeof f->serial);
    ascii_of(&r, find(exif, nx, 0xA434), f->lens_model, sizeof f->lens_model);
    f->fps = positive(num_of(&r, find(root, nr, 0xC764))); if (f->fps > 1000) f->fps = 0;
    f->exposure_s = positive(num_of(&r, find(exif, nx, 0x829A)));
    { double iso = positive(num_of(&r, find(exif, nx, 0x8827))); f->iso = iso > 0 ? (int)round(iso) : 0; }
    f->f_number = positive(num_of(&r, find(exif, nx, 0x829D)));
    f->focal_mm = positive(num_of(&r, find(exif, nx, 0x920A)));
    f->distance_m = positive(num_of(&r, find(exif, nx, 0x9206)));
    { size_t ol; uint8_t *op = bytes_of(&r, find(root, nr, 0xC74E), &ol); if (op) { f->has_warp = warp_rectilinear(op, ol, f->warp); free(op); } }
done:
    free(mn); free(tagv); free(root); free(exif); fclose(fh);
    return rc;
}

/* ------------------------------------------------------------------ lensfit.rs -- */
static const double RHO[3] = { 0.55, 0.85, 1.0 };
static double lf_focal_mm(int has_calib, uint32_t calib, double mount_mm) {
    int has_mount = isfinite(mount_mm) && mount_mm > 0; long long m = has_mount ? llround(mount_mm * 10.0) : 0, c = calib;
    if (has_calib && has_mount && c >= 10 && c <= 20000 && m >= 4 * llabs(c - m)) return c / 10.0;
    if (has_calib && !has_mount && c >= 10 && c <= 20000) return c / 10.0;
    if (has_mount) return m / 10.0;
    return 0;
}
static double lf_focal_px(int frame_w, double focal_mm, int cols) {
    double covered = cols == 0 ? SENSOR_W : (cols < SENSOR_W ? cols : SENSOR_W);
    return frame_w * focal_mm / (SENSOR_ACTIVE_MM * covered / SENSOR_W);
}
static int lf_interpolate(const uint32_t axis[5], const double nodes[5][3][4], double distance_mm, double kr[4]) {
    double a = (isfinite(distance_mm) && distance_mm > 0) ? 16777216.0 / distance_mm : 0.0;
    int i = 0; for (int j = 1; j < 4; j++) if (a >= axis[j]) i = j;
    double span = (double)axis[i + 1] - (double)axis[i]; if (span == 0) return 0;
    double t = (a - axis[i]) / span; if (t < 0) t = 0; if (t > 1) t = 1;
    for (int k = 0; k < 4; k++) kr[k] = nodes[i][1][k] + t * (nodes[i + 1][1][k] - nodes[i][1][k]);
    return kr_valid(kr);
}
static double det3(const double a[3][3]) {
    return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
}
static int lf_fit(const double kr[4], int w, int h, double focal_px, double k[4], double *fx) {
    if (!kr_valid(kr) || !(isfinite(focal_px) && focal_px > 0)) return 0;
    double s = hypot(w, h) / 2.0 / focal_px, m[3][3], b[3];
    for (int row = 0; row < 3; row++) {
        double u = RHO[row] * s, r2 = RHO[row] * RHO[row];
        double g = (kr[0] + kr[1] * r2 + kr[2] * r2 * r2 + kr[3] * r2 * r2 * r2) / kr[0];
        double th = atan(u), t2 = th * th;
        m[row][0] = th * t2; m[row][1] = th * t2 * t2; m[row][2] = th * t2 * t2 * t2; b[row] = g * u - th;
    }
    double d = det3(m); if (d == 0 || !isfinite(d)) return 0;
    for (int c = 0; c < 3; c++) { double n[3][3]; memcpy(n, m, sizeof n); for (int r = 0; r < 3; r++) n[r][c] = b[r]; k[c] = det3(n) / d; }
    k[3] = 0; *fx = focal_px * kr[0]; return 1;
}

/* ------------------------------------------------------------------ protobuf writer -- */
typedef struct { uint8_t *p; size_t n, cap; } PB;
static void pb_put(PB *b, const void *d, size_t n) { if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 64; b->p = realloc(b->p, b->cap); } memcpy(b->p + b->n, d, n); b->n += n; }
static void pb_varint(PB *b, uint64_t v) { uint8_t t[10]; int k = 0; do { t[k] = v & 0x7F; v >>= 7; if (v) t[k] |= 0x80; k++; } while (v); pb_put(b, t, k); }
static void pb_key(PB *b, int field, int wt) { pb_varint(b, (uint64_t)field << 3 | wt); }
static void pb_u32(PB *b, int f, uint32_t v) { pb_key(b, f, 0); pb_varint(b, v); }
static void pb_f32(PB *b, int f, float v) { uint32_t u; memcpy(&u, &v, 4); uint8_t t[4] = { u, u >> 8, u >> 16, u >> 24 }; pb_key(b, f, 5); pb_put(b, t, 4); }
static void pb_f64(PB *b, int f, double v) { uint64_t u; memcpy(&u, &v, 8); uint8_t t[8]; for (int i = 0; i < 8; i++) t[i] = (uint8_t)(u >> (8 * i)); pb_key(b, f, 1); pb_put(b, t, 8); }
static void pb_bytes(PB *b, int f, const void *d, size_t n) { pb_key(b, f, 2); pb_varint(b, n); pb_put(b, d, n); }
static void pb_str(PB *b, int f, const char *s) { pb_bytes(b, f, s, strlen(s)); }
static void pb_msg(PB *b, int f, PB *m) { pb_bytes(b, f, m->p, m->n); free(m->p); memset(m, 0, sizeof *m); }
static void pb_packed_f32(PB *b, int f, const float *v, int n) {
    PB t = {0}; for (int i = 0; i < n; i++) { uint32_t u; memcpy(&u, &v[i], 4); uint8_t c[4] = { u, u >> 8, u >> 16, u >> 24 }; pb_put(&t, c, 4); }
    pb_msg(b, f, &t);
}
/* proto3: implicit-presence scalars are omitted at their default */
static void pb_f32z(PB *b, int f, float v) { if (v != 0.0f) pb_f32(b, f, v); }   /* as prost: -0.0 is omitted too */

/* ------------------------------------------------------------------ JSONL writer -- */
static void js_str(FILE *o, const char *s) {
    fputc('"', o);
    for (; *s; s++) { unsigned char c = (unsigned char)*s; if (c == '"' || c == '\\') fprintf(o, "\\%c", c); else if (c < 0x20) fprintf(o, "\\u%04x", c); else fputc(c, o); }
    fputc('"', o);
}
static void js_f(FILE *o, double v, int dbl) { if (!isfinite(v) || v == 0) v = 0; fprintf(o, dbl ? "%.17g" : "%.9g", v); }

/* ------------------------------------------------------------------ vd_frame_positions -- */
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static int cmpl(const void *a, const void *b) { long long x = *(const long long *)a, y = *(const long long *)b; return x < y ? -1 : x > y; }

static int vd_frame_positions(Fsg2Frame *fr, int n, double fps, double *out, char *err, size_t el) {
    double period_s = fr[0].period_ps * 1e-12;
    if (!(period_s > 0 && fps > 0)) { snprintf(err, el, "no sample period or frame rate"); return -1; }
    double nominal = 1.0 / (fps * period_s);
    size_t nv = 0, cap = 64; double *vds = malloc(sizeof(double) * cap);
    for (int k = 0; k < n; k++) for (int e = 0; e < fr[k].n_events; e++) if (fr[k].events[e].kind == EVENT_VD) {
        if (nv == cap) { cap *= 2; vds = realloc(vds, sizeof(double) * cap); }
        vds[nv++] = (double)fr[k].base_idx + fr[k].events[e].pos;
    }
    qsort(vds, nv, sizeof(double), cmpd);
    size_t u = 0; for (size_t i = 0; i < nv; i++) if (!u || vds[i] != vds[u - 1]) vds[u++] = vds[i]; nv = u;
    if (nv < 2) { free(vds); snprintf(err, el, "this take has no Vd frame sync (it needs Gyro2 with the Vd hook)"); return -1; }
    double mi = 0, mv = 0, sii = 0, siv = 0;
    double *idx = malloc(sizeof(double) * nv);
    for (size_t i = 0; i < nv; i++) { idx[i] = round((vds[i] - vds[0]) / nominal); mi += idx[i]; mv += vds[i]; }
    mi /= nv; mv /= nv;
    for (size_t i = 0; i < nv; i++) { sii += (idx[i] - mi) * (idx[i] - mi); siv += (idx[i] - mi) * (vds[i] - mv); }
    free(idx); free(vds);
    if (!(sii > 0)) { snprintf(err, el, "Vd frame sync events do not span a frame"); return -1; }
    double slope = siv / sii;
    if (fabs(slope / nominal - 1.0) > 0.01) { snprintf(err, el, "Vd every %.2f samples does not match %.3f fps (%.2f)", slope, fps, nominal); return -1; }
    #define LINE(i) (mv + slope * ((i) - mi))
    double *hooks = malloc(sizeof(double) * n), *lags = malloc(sizeof(double) * n); int nl = 0;
    for (int k = 0; k < n; k++) {
        hooks[k] = (double)fr[k].base_idx + fr[k].frame_mark;
        double i = floor((hooks[k] - LINE(0.0)) / slope);
        if (i >= 0) lags[nl++] = hooks[k] - LINE(i);
    }
    if (!nl) { free(hooks); free(lags); snprintf(err, el, "no frame hook after a Vd"); return -1; }
    qsort(lags, nl, sizeof(double), cmpd);
    double lag = lags[nl / 2];
    long long *off = malloc(sizeof(long long) * n), win[9];
    for (int k = 0; k < n; k++) off[k] = (long long)round((hooks[k] - lag - LINE(0.0)) / slope) - (long long)fr[k].frame_seq;
    for (int k = 0; k < n; k++) {
        int lo = k >= 4 ? k - 4 : 0, hi = k + 5 < n ? k + 5 : n, m = hi - lo;
        memcpy(win, off + lo, sizeof(long long) * m); qsort(win, m, sizeof(long long), cmpl);
        out[k] = LINE((double)((long long)fr[k].frame_seq + win[m / 2]));
    }
    #undef LINE
    free(hooks); free(lags); free(off);
    return 0;
}

/* ------------------------------------------------------------------ to_messages -- */
static void accel_axes(const uint8_t *v, double a, float out[3]) {
    int16_t r0 = (int16_t)g16(v, 1), r1 = (int16_t)g16(v + 2, 1), r2 = (int16_t)g16(v + 4, 1);
    out[0] = (float)(r1 * a); out[1] = (float)(-(double)r0 * a); out[2] = (float)(r2 * a);
}
typedef struct { uint16_t pos; float a[3]; } Level;
static int cmp_level(const void *x, const void *y) { const Level *a = x, *b = y; return a->pos < b->pos ? -1 : a->pos > b->pos; }

int fsg2_messages(Fsg2Frame *fr, int n, int width, int height, uint8_t ***msgs_out, size_t **lens_out,
                  void *jsonl_file, char *err, size_t el) {
    FILE *js = (FILE *)jsonl_file;
    if (n < 1) { snprintf(err, el, "no kind 2 frames"); return -1; }
    Fsg2Frame *first = &fr[0];
    double fps = first->fps;
    if (!(fps > 0)) { snprintf(err, el, "first DNG has no FrameRate tag"); return -1; }
    for (int k = 0; k < n; k++) {
        if (!fr[k].present) { snprintf(err, el, "frame %d has no Gyro2 block", k + 1); return -1; }
        if (fr[k].clip_id != first->clip_id) { snprintf(err, el, "frame %d belongs to another take (clip id %#x, expected %#x)", k + 1, fr[k].clip_id, first->clip_id); return -1; }
        if (fr[k].period_ps != first->period_ps || fr[k].gscale != first->gscale || fr[k].ascale != first->ascale) { snprintf(err, el, "frame %d: sample period or scale changed within the take", k + 1); return -1; }
    }
    int ois = 0, eis = 0, trunc = 0, lost = 0;
    for (int k = 0; k < n; k++) { ois |= fr[k].flags & FLAG_OIS_ON; eis |= fr[k].flags & FLAG_EIS_ON; trunc += !!(fr[k].flags & FLAG_TRUNCATED); lost += !!(fr[k].flags & FLAG_DATA_LOST); }
    if (ois) fprintf(stderr, "dng2prr: gyro: lens OIS was on during this take; Gyroflow cannot undo it, so stabilisation will fight it\n");
    if (eis) fprintf(stderr, "dng2prr: gyro: in-camera electronic stabilisation was on during this take; its warp is not recorded\n");
    if (trunc) fprintf(stderr, "dng2prr: gyro: %d frame(s) had the gyro block truncated by the camera\n", trunc);
    if (lost) fprintf(stderr, "dng2prr: gyro: %d frame(s) flagged gyro data lost\n", lost);

    double period_us = first->period_ps / 1.0e6;
    double gyro_dps = (double)first->gscale * 180.0 / M_PI;
    double accel_ms2 = (double)first->ascale * STANDARD_GRAVITY;
    float held[3] = { 0, 0, 0 };
    for (int k = 0, got = 0; k < n && !got; k++) for (int e = 0; e < fr[k].n_events; e++) if (fr[k].events[e].kind == EVENT_LEVEL) { accel_axes(fr[k].events[e].v, accel_ms2, held); got = 1; break; }
    const Fsg2Frame *table = NULL; for (int k = 0; k < n; k++) if (fr[k].has_lens) { table = &fr[k]; break; }
    /* fsg2.rs zoom_start: the mount's focal length on the take's first frame, when the frames don't all report one */
    int zoomed_take = 0; double zoom_mm = 0;
    { int have = 0; for (int k = 0; k < n; k++) { double mm = fr[k].focal_mm; if (!(isfinite(mm) && mm > 0)) continue;
        if (!have) { zoom_mm = mm; have = 1; } else if (fabs(mm - zoom_mm) >= SAME_FOCAL_MM) zoomed_take = 1; } }

    double *vd = malloc(sizeof(double) * n);
    if (vd_frame_positions(fr, n, fps, vd, err, el)) { free(vd); return -1; }

    /* Header */
    PB hdr = {0}, cam = {0}, clip = {0};
    pb_str(&cam, 1, first->make[0] ? first->make : "SIGMA");
    pb_str(&cam, 2, first->model[0] ? first->model : "fp");
    if (first->serial[0]) pb_str(&cam, 3, first->serial);
    if (first->software[0]) pb_str(&cam, 4, first->software);
    if (first->lens_model[0]) pb_str(&cam, 6, first->lens_model);
    pb_u32(&cam, 7, PIXEL_PITCH_NM); pb_u32(&cam, 8, PIXEL_PITCH_NM); pb_u32(&cam, 9, SENSOR_W); pb_u32(&cam, 10, SENSOR_H);
    pb_str(&cam, 13, "xyz");
    double duration_us = n * 1.0e6 / fps; uint32_t imu_rate = (uint32_t)llround(1.0e12 / first->period_ps);
    double readout_us = first->readout_ns / 1000.0;
    pb_u32(&clip, 1, width); pb_u32(&clip, 2, height); pb_f64(&clip, 3, duration_us);
    pb_f32(&clip, 4, (float)fps); pb_f32(&clip, 5, (float)fps); pb_f32(&clip, 6, (float)fps);
    pb_u32(&clip, 8, imu_rate); pb_f32(&clip, 10, 1.0f); if (readout_us != 0) pb_f64(&clip, 11, readout_us);
    pb_msg(&hdr, 1, &cam); pb_msg(&hdr, 2, &clip);

    uint8_t **msgs = calloc(n, sizeof *msgs); size_t *lens = calloc(n, sizeof *lens);
    uint64_t next_global = 0;
    for (int k = 0; k < n; k++) {
        Fsg2Frame *r = &fr[k]; uint64_t base = r->base_idx;
        double start = vd[k] * period_us + VD_TO_READOUT_US, end = start + r->readout_ns / 1000.0;
        Level *lv = malloc(sizeof(Level) * (r->n_events + 1)); int nlv = 0, li = 0;
        for (int e = 0; e < r->n_events; e++) if (r->events[e].kind == EVENT_LEVEL) { lv[nlv].pos = r->events[e].pos; accel_axes(r->events[e].v, accel_ms2, lv[nlv].a); nlv++; }
        /* stable sort by position (insertion sort; a handful of events) */
        for (int i = 1; i < nlv; i++) { Level t = lv[i]; int j = i - 1; while (j >= 0 && cmp_level(&lv[j], &t) > 0) { lv[j + 1] = lv[j]; j--; } lv[j + 1] = t; }

        PB fm = {0}; PB imus = {0}; int nimu = 0;
        if (js) fprintf(js, "{\"magic_string\":\"GyroflowProtobuf\",\"protocol_version\":1,");
        if (js && k == 0) {
            fprintf(js, "\"header\":{\"camera\":{\"camera_brand\":"); js_str(js, first->make[0] ? first->make : "SIGMA");
            fprintf(js, ",\"camera_model\":"); js_str(js, first->model[0] ? first->model : "fp");
            if (first->serial[0]) { fprintf(js, ",\"camera_serial_number\":"); js_str(js, first->serial); }
            if (first->software[0]) { fprintf(js, ",\"firmware_version\":"); js_str(js, first->software); }
            if (first->lens_model[0]) { fprintf(js, ",\"lens_model\":"); js_str(js, first->lens_model); }
            fprintf(js, ",\"pixel_pitch_x_nm\":%d,\"pixel_pitch_y_nm\":%d,\"sensor_pixel_width\":%d,\"sensor_pixel_height\":%d,\"imu_orientation\":\"xyz\"},",
                    PIXEL_PITCH_NM, PIXEL_PITCH_NM, SENSOR_W, SENSOR_H);
            fprintf(js, "\"clip\":{\"frame_width\":%d,\"frame_height\":%d,\"duration_us\":", width, height); js_f(js, duration_us, 1);
            fprintf(js, ",\"record_frame_rate\":"); js_f(js, (float)fps, 0);
            fprintf(js, ",\"sensor_frame_rate\":"); js_f(js, (float)fps, 0);
            fprintf(js, ",\"file_frame_rate\":"); js_f(js, (float)fps, 0);
            fprintf(js, ",\"imu_sample_rate\":%u,\"pixel_aspect_ratio\":1,\"frame_readout_time_us\":", imu_rate); js_f(js, readout_us, 1);
            fprintf(js, ",\"frame_readout_direction\":\"TopToBottom\"}},");
        }
        if (js) { fprintf(js, "\"frame\":{\"start_timestamp_us\":"); js_f(js, start, 1); fprintf(js, ",\"end_timestamp_us\":"); js_f(js, end, 1); fprintf(js, ",\"frame_number\":%d", k + 1);
                  if (r->iso) fprintf(js, ",\"iso\":%d", r->iso);
                  if (r->exposure_s > 0) { fprintf(js, ",\"exposure_time_us\":"); js_f(js, r->exposure_s * 1.0e6, 1); }
                  fprintf(js, ",\"crop_x\":%u,\"crop_y\":%u,\"crop_width\":%u,\"crop_height\":%u", r->crop[0], r->crop[1], r->crop[2], r->crop[3]); }
        pb_f64(&fm, 1, start); pb_f64(&fm, 2, end); pb_u32(&fm, 3, k + 1);
        if (r->iso) pb_u32(&fm, 4, r->iso);
        if (r->exposure_s > 0) pb_f64(&fm, 5, r->exposure_s * 1.0e6);
        pb_f32(&fm, 12, r->crop[0]); pb_f32(&fm, 13, r->crop[1]); pb_f32(&fm, 14, r->crop[2]); pb_f32(&fm, 15, r->crop[3]);

        /* lens_data */
        {
            PB ld = {0}; int any = 0; double dist_mm = r->distance_m > 0 ? r->distance_m * 1000.0 : 0;
            float intr[9]; int has_intr = 0; float coef[4];
            float fl = 0, fno = r->f_number > 0 ? (float)r->f_number : 0, fd = dist_mm > 0 ? (float)dist_mm : 0; int has_fl = 0;
            if (r->focal_mm > 0) {
                double kr[4]; int has_kr; double fmm;
                /* a frame of a zoomed take away from its starting focal length takes its own (fsg2.rs lens_data) */
                int unzoomed = !zoomed_take || fabs(r->focal_mm - zoom_mm) < SAME_FOCAL_MM;
                const Fsg2Frame *tb = unzoomed ? table : NULL;
                if (tb) { has_kr = lf_interpolate(tb->axis, tb->nodes, dist_mm > 0 ? dist_mm : NAN, kr); fmm = lf_focal_mm(1, tb->calib_focal, r->focal_mm); }
                else { has_kr = r->has_warp; memcpy(kr, r->warp, sizeof kr); fmm = lf_focal_mm(r->calib2 != 0 && unzoomed, r->calib2, r->focal_mm); }
                fl = (float)(fmm > 0 ? fmm : r->focal_mm); has_fl = 1;
                double k4[4], fx;
                if (fmm > 0 && has_kr && lf_fit(kr, width, height, lf_focal_px(width, fmm, r->crop[2]), k4, &fx)) {
                    float f = (float)fx, cx = width / 2.0f, cy = height / 2.0f;
                    float m[9] = { f, 0, cx, 0, f, cy, 0, 0, 1 }; memcpy(intr, m, sizeof m); has_intr = 1;
                    for (int i = 0; i < 4; i++) coef[i] = (float)k4[i];
                }
            } else fd = 0;   /* a lens without contacts: only the aperture */
            if (has_intr) { pb_packed_f32(&ld, 1, intr, 9); any = 1; }
            if (has_fl) { pb_f32(&ld, 2, fl); any = 1; }
            if (fno > 0) { pb_f32(&ld, 3, fno); any = 1; }
            if (fd > 0) { pb_f32(&ld, 4, fd); any = 1; }
            if (has_intr) { PB fe = {0}; pb_packed_f32(&fe, 1, coef, 4); pb_msg(&ld, 6, &fe); }
            if (any) {
                pb_msg(&fm, 16, &ld);
                if (js) {
                    fprintf(js, ",\"lens\":[{"); int c = 0;
                    if (has_intr) { fprintf(js, "\"camera_intrinsic_matrix\":["); for (int i = 0; i < 9; i++) { if (i) fputc(',', js); js_f(js, intr[i], 0); } fputc(']', js); c = 1; }
                    if (has_fl) { fprintf(js, "%s\"focal_length_mm\":", c ? "," : ""); js_f(js, fl, 0); c = 1; }
                    if (fno > 0) { fprintf(js, "%s\"f_number\":", c ? "," : ""); js_f(js, fno, 0); c = 1; }
                    if (fd > 0) { fprintf(js, "%s\"focus_distance_mm\":", c ? "," : ""); js_f(js, fd, 0); c = 1; }
                    if (has_intr) { fprintf(js, ",\"opencv_fisheye\":{\"coefficients\":["); for (int i = 0; i < 4; i++) { if (i) fputc(',', js); js_f(js, coef[i], 0); } fprintf(js, "]}"); }
                    fprintf(js, "}]");
                }
            } else free(ld.p);
        }

        if (js) fprintf(js, ",\"imu\":[");
        if (!(r->flags & FLAG_DATA_LOST)) {
            for (int s = 0; s < r->n_samples; s++) {
                while (li < nlv && lv[li].pos <= (uint16_t)s) { memcpy(held, lv[li].a, sizeof held); li++; }
                uint64_t global = base + (uint64_t)s;
                if (global < next_global) continue;
                next_global = global + 1;
                double ts = (double)(base + (uint64_t)s) * period_us;
                float g[3]; for (int a = 0; a < 3; a++) g[a] = (float)(r->samples[s][a] * gyro_dps);
                PB im = {0};
                pb_f64(&im, 1, ts);
                pb_f32z(&im, 2, g[0]); pb_f32z(&im, 3, g[1]); pb_f32z(&im, 4, g[2]);
                pb_f32z(&im, 5, held[0]); pb_f32z(&im, 6, held[1]); pb_f32z(&im, 7, held[2]);
                pb_msg(&imus, 17, &im);
                if (js) {
                    fprintf(js, "%s{\"sample_timestamp_us\":", nimu ? "," : ""); js_f(js, ts, 1);
                    fprintf(js, ",\"gyroscope_x\":"); js_f(js, g[0], 0); fprintf(js, ",\"gyroscope_y\":"); js_f(js, g[1], 0); fprintf(js, ",\"gyroscope_z\":"); js_f(js, g[2], 0);
                    fprintf(js, ",\"accelerometer_x\":"); js_f(js, held[0], 0); fprintf(js, ",\"accelerometer_y\":"); js_f(js, held[1], 0); fprintf(js, ",\"accelerometer_z\":"); js_f(js, held[2], 0);
                    fputc('}', js);
                }
                nimu++;
            }
        }
        for (; li < nlv; li++) memcpy(held, lv[li].a, sizeof held);
        free(lv);
        if (js) fprintf(js, "]}}\n");
        if (imus.n) pb_put(&fm, imus.p, imus.n);
        free(imus.p);

        PB mm = {0};
        pb_str(&mm, 1, "GyroflowProtobuf"); pb_u32(&mm, 2, 1);
        if (k == 0) pb_bytes(&mm, 3, hdr.p, hdr.n);
        pb_msg(&mm, 4, &fm);
        msgs[k] = mm.p; lens[k] = mm.n;
    }
    free(hdr.p); free(vd);
    *msgs_out = msgs; *lens_out = lens;
    return 0;
}

/* ------------------------------------------------------------------ fsg2_imu -- */
/* The same readings fsg2_messages puts in its IMU messages (camera clock, gyro in deg/s, accelerometer as the
   held level in m/s², both in the message axes), plus each frame's start_timestamp_us. For writers that need
   the motion in another container (dng2braw's Blackmagic motion tracks). */
int fsg2_imu(Fsg2Frame *fr, int n, Fsg2Imu *out, char *err, size_t el) {
    memset(out, 0, sizeof *out);
    if (n < 1) { snprintf(err, el, "no kind 2 frames"); return -1; }
    Fsg2Frame *first = &fr[0];
    double fps = first->fps;
    if (!(fps > 0)) { snprintf(err, el, "first DNG has no FrameRate tag"); return -1; }
    double period_us = first->period_ps / 1.0e6;
    double gyro_dps = (double)first->gscale * 180.0 / M_PI;
    double accel_ms2 = (double)first->ascale * STANDARD_GRAVITY;
    float held[3] = { 0, 0, 0 };
    for (int k = 0, got = 0; k < n && !got; k++) for (int e = 0; e < fr[k].n_events; e++) if (fr[k].events[e].kind == EVENT_LEVEL) { accel_axes(fr[k].events[e].v, accel_ms2, held); got = 1; break; }
    double *vd = malloc(sizeof(double) * n);
    if (vd_frame_positions(fr, n, fps, vd, err, el)) { free(vd); return -1; }
    long cap = 0; for (int k = 0; k < n; k++) cap += fr[k].n_samples;
    out->ts_us = malloc(sizeof(double) * (cap + 1)); out->gyro = malloc(sizeof(float[3]) * (cap + 1)); out->accel = malloc(sizeof(float[3]) * (cap + 1));
    out->frame_start_us = malloc(sizeof(double) * n);
    out->fps = fps; out->readout_us = first->readout_ns / 1000.0; out->exposure_us = first->exposure_s > 0 ? first->exposure_s * 1.0e6 : 0;
    uint64_t next_global = 0;
    for (int k = 0; k < n; k++) {
        Fsg2Frame *r = &fr[k]; uint64_t base = r->base_idx;
        out->frame_start_us[k] = vd[k] * period_us + VD_TO_READOUT_US;
        Level *lv = malloc(sizeof(Level) * (r->n_events + 1)); int nlv = 0, li = 0;
        for (int e = 0; e < r->n_events; e++) if (r->events[e].kind == EVENT_LEVEL) { lv[nlv].pos = r->events[e].pos; accel_axes(r->events[e].v, accel_ms2, lv[nlv].a); nlv++; }
        for (int i = 1; i < nlv; i++) { Level t = lv[i]; int j = i - 1; while (j >= 0 && cmp_level(&lv[j], &t) > 0) { lv[j + 1] = lv[j]; j--; } lv[j + 1] = t; }
        if (!(r->flags & FLAG_DATA_LOST)) {
            for (int s = 0; s < r->n_samples; s++) {
                while (li < nlv && lv[li].pos <= (uint16_t)s) { memcpy(held, lv[li].a, sizeof held); li++; }
                uint64_t global = base + (uint64_t)s;
                if (global < next_global) continue;
                next_global = global + 1;
                long i = out->n++;
                out->ts_us[i] = (double)global * period_us;
                for (int a = 0; a < 3; a++) { out->gyro[i][a] = (float)(r->samples[s][a] * gyro_dps); out->accel[i][a] = held[a]; }
            }
        }
        for (; li < nlv; li++) memcpy(held, lv[li].a, sizeof held);
        free(lv);
    }
    free(vd);
    return 0;
}
void fsg2_imu_free(Fsg2Imu *m) { free(m->ts_us); free(m->gyro); free(m->accel); free(m->frame_start_us); memset(m, 0, sizeof *m); }
