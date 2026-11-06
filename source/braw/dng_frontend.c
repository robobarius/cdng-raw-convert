/*
 * dng_frontend.c: SIGMA fp CinemaDNG reader + DNG colour maths, shared by the dng2* encoders.
 *
 * Part of dng2braw / dng2prr (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Code extracted from dng2prr 0.2 (source/dng2prr_cli.c, lines 70-336), with the three entry
 * points made public under a dngf_ prefix. Only change: Frame.focal (EXIF FocalLength) is kept. Needs dng_jpeg.c + dng_lj92.c.
 */
#define _FILE_OFFSET_BITS 64
#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <sys/types.h>
#include "dng_frontend.h"

int dng_jpeg_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride);

/* ------------------------------------------------------------------ TIFF / DNG reading */
typedef struct { int present, type; uint32_t count; double *v; uint8_t *bytes; char *str; } Tag;
enum { T_W, T_H, T_BPS, T_COMP, T_PHOTO, T_STRIPOFF, T_ROWS, T_STRIPCNT, T_TILEW, T_TILEL, T_TILEOFF, T_TILECNT,
       T_CFA, T_BLACK, T_WHITE, T_CROPO, T_CROPS, T_CM1, T_CM2, T_NEUTRAL, T_BASEEXP, T_FRAMERATE, T_EXIF,
       T_ILL1, T_ILL2, T_LIN, T_TIMECODE, NTAGS };
static const int tagcode[NTAGS] = { 256, 257, 258, 259, 262, 273, 278, 279, 322, 323, 324, 325,
       33422, 50714, 50717, 50719, 50720, 50721, 50722, 50728, 50730, 51044, 34665,
       50778, 50779, 50712, 51043 };
enum { E_EXPT, E_FNUM, E_ISO, E_FOCAL, E_LENSMAKE, E_LENSMODEL, NEXIF };
static const int exifcode[NEXIF] = { 33434, 33437, 34855, 37386, 42035, 42036 };

static int tsz(int t) { static const int s[13] = {0,1,1,2,4,8,1,1,2,4,8,4,8}; return (t >= 1 && t <= 12) ? s[t] : 0; }
static uint32_t rd32(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24 : (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t rd16(const uint8_t *p, int le) { return le ? p[0] | p[1] << 8 : p[0] << 8 | p[1]; }

static void read_ifd(FILE *fh, int le, uint32_t off, const int *codes, int ncodes, Tag *tags) {
    uint8_t b2[2];
    if (fseeko(fh, off, SEEK_SET) || fread(b2, 1, 2, fh) != 2) return;
    int n = rd16(b2, le);
    uint8_t *ent = malloc((size_t)n * 12);
    if (fread(ent, 12, n, fh) != (size_t)n) { free(ent); return; }
    for (int i = 0; i < n; i++) {
        const uint8_t *e = ent + 12 * i;
        int code = rd16(e, le), typ = rd16(e + 2, le); uint32_t cnt = rd32(e + 4, le);
        int k = -1;
        for (int j = 0; j < ncodes; j++) if (codes[j] == code) k = j;
        if (k < 0 || !tsz(typ) || cnt > 50000000) continue;
        size_t nb = (size_t)tsz(typ) * cnt;
        uint8_t *raw = malloc(nb + 1);
        if (nb <= 4) memcpy(raw, e + 8, nb);
        else { fseeko(fh, rd32(e + 8, le), SEEK_SET); if (fread(raw, 1, nb, fh) != nb) { free(raw); continue; } }
        Tag *t = &tags[k]; t->present = 1; t->type = typ; t->count = cnt; t->bytes = raw;
        if (typ == 2) { raw[nb] = 0; t->str = (char *)raw; }
        t->v = malloc(sizeof(double) * (cnt ? cnt : 1));
        for (uint32_t c = 0; c < cnt; c++) {
            const uint8_t *q = raw + (size_t)tsz(typ) * c; double v = 0;
            switch (typ) {
                case 1: case 7: v = q[0]; break;
                case 2: v = q[0]; break;
                case 3: v = rd16(q, le); break;
                case 8: v = (int16_t)rd16(q, le); break;
                case 4: v = rd32(q, le); break;
                case 9: v = (int32_t)rd32(q, le); break;
                case 5: { uint32_t d = rd32(q + 4, le); v = d ? (double)rd32(q, le) / d : 0; } break;
                case 10: { int32_t d = (int32_t)rd32(q + 4, le); v = d ? (double)(int32_t)rd32(q, le) / d : 0; } break;
                case 11: { uint32_t u = rd32(q, le); float f; memcpy(&f, &u, 4); v = f; } break;
                case 12: { uint64_t u = (uint64_t)rd32(q, le) | (uint64_t)rd32(q + 4, le) << 32; if (!le) u = (uint64_t)rd32(q, le) << 32 | rd32(q + 4, le); double d; memcpy(&d, &u, 8); v = d; } break;
            }
            t->v[c] = v;
        }
    }
    free(ent);
}
static void free_tags(Tag *t, int n) { for (int i = 0; i < n; i++) { free(t[i].v); free(t[i].bytes); } }

static int read_header(const char *path, Tag *tags, Tag *ex, int *le_out, FILE **fh_out) {
    FILE *fh = fopen(path, "rb"); if (!fh) return -1;
    uint8_t h[8]; if (fread(h, 1, 8, fh) != 8) { fclose(fh); return -2; }
    int le = h[0] == 'I';
    if (rd16(h + 2, le) != 42) { fclose(fh); return -3; }
    memset(tags, 0, sizeof(Tag) * NTAGS);
    read_ifd(fh, le, rd32(h + 4, le), tagcode, NTAGS, tags);
    if (ex) { memset(ex, 0, sizeof(Tag) * NEXIF); if (tags[T_EXIF].present) read_ifd(fh, le, (uint32_t)tags[T_EXIF].v[0], exifcode, NEXIF, ex); }
    *le_out = le;
    if (fh_out) *fh_out = fh; else fclose(fh);
    return 0;
}

static void unpack(const uint8_t *b, int w, int h, int bps, int le16, uint16_t *out) {
    size_t rowbytes = ((size_t)w * bps + 7) / 8;
    for (int y = 0; y < h; y++) {
        const uint8_t *r = b + rowbytes * y; uint16_t *o = out + (size_t)y * w;
        if (bps == 16) { for (int x = 0; x < w; x++) o[x] = le16 ? r[2*x] | r[2*x+1] << 8 : r[2*x] << 8 | r[2*x+1]; continue; }
        if (bps == 12 && !(w & 1)) { for (int x = 0; x < w; x += 2) { const uint8_t *g = r + 3 * (x / 2); o[x] = g[0] << 4 | g[1] >> 4; o[x+1] = (g[1] & 15) << 8 | g[2]; } continue; }
        uint64_t acc = 0; int nacc = 0; size_t p = 0;               /* generic MSB-first */
        for (int x = 0; x < w; x++) {
            while (nacc < bps) { acc = acc << 8 | r[p++]; nacc += 8; }
            o[x] = (uint16_t)((acc >> (nacc - bps)) & ((1u << bps) - 1)); nacc -= bps;
        }
    }
}

static double tagv(Tag *t, int i, double def) { return (t->present && (uint32_t)i < t->count) ? t->v[i] : def; }

static int load_frame(const char *path, Frame *f, int pixels) {
    Tag tags[NTAGS], ex[NEXIF]; int le; FILE *fh;
    if (read_header(path, tags, ex, &le, &fh)) { fprintf(stderr, "dng2prr: %s: not a TIFF/DNG file\n", path); return -1; }
    memset(f, 0, sizeof *f);
    int ok = 1;
    if ((int)tagv(&tags[T_PHOTO], 0, 0) != 32803) { fprintf(stderr, "dng2prr: %s: not a CFA DNG\n", path); ok = 0; }
    if (ok && !(tags[T_CFA].present && tags[T_CFA].count == 4 && !memcmp(tags[T_CFA].bytes, "\0\1\1\2", 4))) { fprintf(stderr, "dng2prr: %s: only RGGB is supported\n", path); ok = 0; }
    if (!ok) { fclose(fh); free_tags(tags, NTAGS); free_tags(ex, NEXIF); return -1; }
    f->w = (int)tagv(&tags[T_W], 0, 0); f->h = (int)tagv(&tags[T_H], 0, 0);
    f->bits = (int)tagv(&tags[T_BPS], 0, 16);
    if (pixels) {
        int comp = (int)tagv(&tags[T_COMP], 0, 1);
        f->raw = calloc((size_t)f->w * f->h, 2);
        if (comp == 1) {
            size_t total = 0; Tag *so = &tags[T_STRIPOFF], *sc = &tags[T_STRIPCNT];
            for (uint32_t i = 0; i < sc->count; i++) total += (size_t)sc->v[i];
            uint8_t *buf = malloc(total + 16), *q = buf;
            for (uint32_t i = 0; i < so->count && i < sc->count; i++) { fseeko(fh, (off_t)so->v[i], SEEK_SET); q += fread(q, 1, (size_t)sc->v[i], fh); }
            size_t need = ((size_t)f->w * f->bits + 7) / 8 * f->h;
            if ((size_t)(q - buf) < need) { fprintf(stderr, "dng2prr: %s: truncated image data\n", path); ok = 0; }
            else unpack(buf, f->w, f->h, f->bits, le, f->raw);
            free(buf);
        } else if (comp == 7) {
            int tw, tl; Tag *to, *tc;
            if (tags[T_TILEOFF].present) { tw = (int)tagv(&tags[T_TILEW], 0, 0); tl = (int)tagv(&tags[T_TILEL], 0, 0); to = &tags[T_TILEOFF]; tc = &tags[T_TILECNT]; }
            else { tw = f->w; tl = (int)tagv(&tags[T_ROWS], 0, f->h); to = &tags[T_STRIPOFF]; tc = &tags[T_STRIPCNT]; }
            int across = (f->w + tw - 1) / tw, down = (f->h + tl - 1) / tl;
            uint16_t *full = calloc((size_t)across * tw * down * tl, 2);
            for (uint32_t i = 0; ok && i < to->count && i < tc->count; i++) {
                size_t n = (size_t)tc->v[i]; uint8_t *d = malloc(n);
                fseeko(fh, (off_t)to->v[i], SEEK_SET);
                if (fread(d, 1, n, fh) != n) ok = 0;
                int ty = i / across, tx = i % across;
                if (ok && dng_jpeg_decode(d, n, full + (size_t)ty * tl * across * tw + (size_t)tx * tw, tw, tl, across * tw)) {
                    fprintf(stderr, "dng2prr: %s: JPEG decode failed in tile %u\n", path, i); ok = 0; }
                free(d);
            }
            for (int y = 0; ok && y < f->h; y++) memcpy(f->raw + (size_t)y * f->w, full + (size_t)y * across * tw, (size_t)f->w * 2);
            free(full);
        } else { fprintf(stderr, "dng2prr: %s: DNG compression %d not supported\n", path, comp); ok = 0; }
        if (ok && tags[T_LIN].present && pixels == 2) {          /* keep stored values + the table */
            f->nlin = (int)tags[T_LIN].count; f->lin = malloc(2 * (size_t)f->nlin);
            for (int i = 0; i < f->nlin; i++) f->lin[i] = (uint16_t)tags[T_LIN].v[i];
        } else if (ok && tags[T_LIN].present) {
            Tag *lt = &tags[T_LIN]; size_t n = (size_t)f->w * f->h;
            for (size_t i = 0; i < n; i++) { uint32_t v = f->raw[i]; if (v >= lt->count) v = lt->count - 1; f->raw[i] = (uint16_t)lt->v[v]; }
        }
    }
    fclose(fh);
    /* levels */
    Tag *bl = &tags[T_BLACK]; double s = 0;
    if (bl->present && bl->count) { for (uint32_t i = 0; i < bl->count; i++) s += bl->v[i]; f->black = s / bl->count; }
    f->white = tagv(&tags[T_WHITE], 0, 4095);
    if (!tags[T_CM2].present || tags[T_CM2].count < 9 || !tags[T_NEUTRAL].present) { fprintf(stderr, "dng2prr: %s: missing colour tags\n", path); ok = 0; }
    else {
        for (int i = 0; i < 9; i++) f->cm2[i] = tags[T_CM2].v[i];
        f->has_cm1 = tags[T_CM1].present && tags[T_CM1].count >= 9;
        for (int i = 0; i < 9; i++) f->cm1[i] = f->has_cm1 ? tags[T_CM1].v[i] : f->cm2[i];
        for (int i = 0; i < 3; i++) f->neutral[i] = tagv(&tags[T_NEUTRAL], i, 1);
    }
    f->illum1 = (int)tagv(&tags[T_ILL1], 0, 17); f->illum2 = (int)tagv(&tags[T_ILL2], 0, 21);
    if (tags[T_TIMECODE].present && tags[T_TIMECODE].count >= 4) { memcpy(f->tc, tags[T_TIMECODE].bytes, tags[T_TIMECODE].count < 8 ? tags[T_TIMECODE].count : 8); f->has_tc = 1; }
    f->baseline_exposure = tagv(&tags[T_BASEEXP], 0, 0);
    if (tags[T_FRAMERATE].present && tags[T_FRAMERATE].type == 10 && tags[T_FRAMERATE].count >= 1) {
        /* keep numerator/denominator as stored */
        const uint8_t *q = tags[T_FRAMERATE].bytes; f->fps_n = (int32_t)rd32(q, le); f->fps_d = (int32_t)rd32(q + 4, le);
    } else { f->fps_n = 24000; f->fps_d = 1001; }
    if (f->fps_n <= 0 || f->fps_d <= 0) { f->fps_n = 24000; f->fps_d = 1001; }
    int cx = (int)tagv(&tags[T_CROPO], 0, 0), cy = (int)tagv(&tags[T_CROPO], 1, 0);
    int cw = (int)tagv(&tags[T_CROPS], 0, f->w), ch = (int)tagv(&tags[T_CROPS], 1, f->h);
    f->crop[0] = cx; f->crop[1] = f->w - cx - cw; f->crop[2] = cy; f->crop[3] = f->h - cy - ch;
    if (ex[E_ISO].present && ex[E_ISO].count == 1 && ex[E_ISO].type != 5 && ex[E_ISO].type != 10) f->iso = (int)ex[E_ISO].v[0];
    f->fnumber = tagv(&ex[E_FNUM], 0, 0); f->focal = tagv(&ex[E_FOCAL], 0, 0); f->exposure_time = tagv(&ex[E_EXPT], 0, 0);
    char mk[128] = "", md[128] = "";
    if (ex[E_LENSMAKE].str) snprintf(mk, sizeof mk, "%s", ex[E_LENSMAKE].str);
    if (ex[E_LENSMODEL].str) snprintf(md, sizeof md, "%s", ex[E_LENSMODEL].str);
    free_tags(tags, NTAGS); free_tags(ex, NEXIF);
    /* strip and join like Python: ' '.join(x.strip() for x in (make, model) if x.strip()) */
    char *parts[2] = { mk, md }; f->lens[0] = 0;
    for (int i = 0; i < 2; i++) {
        char *a = parts[i]; while (*a && isspace((unsigned char)*a)) a++;
        char *e = a + strlen(a); while (e > a && isspace((unsigned char)e[-1])) *--e = 0;
        if (*a) { if (f->lens[0]) strncat(f->lens, " ", sizeof f->lens - strlen(f->lens) - 1); strncat(f->lens, a, sizeof f->lens - strlen(f->lens) - 1); }
    }
    if (!ok) { free(f->raw); f->raw = NULL; return -1; }
    return 0;
}

/* ------------------------------------------------------------------ colour (same maths as dng2prr.py) */
static const double XYZ_D65[3] = { 0.95047, 1.0, 1.08883 };
static const double BRADFORD[9] = { 0.8951, 0.2664, -0.1614, -0.7502, 1.7135, 0.0367, 0.0389, -0.0685, 1.0296 };

static void mat_mul(const double *a, const double *b, double *o) {
    double t[9];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[3*i+j] = a[3*i] * b[j] + a[3*i+1] * b[3+j] + a[3*i+2] * b[6+j];
    memcpy(o, t, sizeof t);
}
static void mat_vec(const double *a, const double *v, double *o) {
    double t[3]; for (int i = 0; i < 3; i++) t[i] = a[3*i] * v[0] + a[3*i+1] * v[1] + a[3*i+2] * v[2]; memcpy(o, t, sizeof t);
}
static void mat_inv(const double *m, double *o) {
    double a = m[0], b = m[1], c = m[2], d = m[3], e = m[4], f = m[5], g = m[6], h = m[7], i = m[8];
    double A = e*i - f*h, B = -(d*i - f*g), C = d*h - e*g, det = a*A + b*B + c*C;
    double t[9] = { A/det, -(b*i - c*h)/det, (b*f - c*e)/det,
                    B/det, (a*i - c*g)/det, -(a*f - c*d)/det,
                    C/det, -(a*h - b*g)/det, (a*e - b*d)/det };
    memcpy(o, t, sizeof t);
}
static double illum_cct(int code, double def) {
    switch (code) { case 17: return 2856; case 21: return 6504; case 20: return 5503; case 23: return 5003; case 22: return 7504; case 10: return 6774; }
    return def;
}
static double cct_from_xy(double x, double y) {
    double n = (x - 0.3320) / (0.1858 - y);
    double t = 449 * n*n*n + 3525 * n*n + 6823.3 * n + 5520.33;
    return t < 1500 ? 1500 : t > 20000 ? 20000 : t;
}
static void cm_pair(const Frame *f, double *cm1, double *cm2, double *t1, double *t2) {
    memcpy(cm1, f->cm1, sizeof f->cm1); memcpy(cm2, f->cm2, sizeof f->cm2);
    *t1 = illum_cct(f->illum1, 2856); *t2 = illum_cct(f->illum2, 6504);
    if (*t1 > *t2) { double tmp[9]; memcpy(tmp, cm1, sizeof tmp); memcpy(cm1, cm2, sizeof tmp); memcpy(cm2, tmp, sizeof tmp); double x = *t1; *t1 = *t2; *t2 = x; }
}
static void cm_at(const Frame *f, double T, double *cm) {
    double c1[9], c2[9], t1, t2; cm_pair(f, c1, c2, &t1, &t2);
    double g = (1 / T - 1 / t2) / (1 / t1 - 1 / t2); g = g < 0 ? 0 : g > 1 ? 1 : g;
    for (int i = 0; i < 9; i++) cm[i] = g * c1[i] + (1 - g) * c2[i];
}
static void adapt_matrix(const double *w, const double *cm, const double *n, double *M) {
    /* M = Bradford(w -> D65) . inv(cm) . diag(n), normalised so M.[1,1,1] has Y = 1 */
    double bw[3], bd[3], D[9] = {0}, binv[9], cat[9], icm[9];
    mat_vec(BRADFORD, w, bw); mat_vec(BRADFORD, XYZ_D65, bd);
    for (int i = 0; i < 3; i++) D[4*i] = bd[i] / bw[i];
    mat_inv(BRADFORD, binv); mat_mul(binv, D, cat); mat_mul(cat, BRADFORD, cat);
    mat_inv(cm, icm); mat_mul(cat, icm, M);
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) M[3*r+c] *= n[c];
    double y = M[3] + M[4] + M[5];
    for (int i = 0; i < 9; i++) M[i] /= y;
}
static Colour colour_params(const Frame *f) {
    Colour c; double n[3] = { f->neutral[0] / f->neutral[1], 1.0, f->neutral[2] / f->neutral[1] };
    c.wb_r = 1.0 / n[0]; c.wb_b = 1.0 / n[2];
    double c1[9], c2[9], t1, t2, cm[9], icm[9], xyz[3], T = 5000.0; cm_pair(f, c1, c2, &t1, &t2);
    for (int it = 0; it < 20; it++) {
        double g = (1 / T - 1 / t2) / (1 / t1 - 1 / t2); g = g < 0 ? 0 : g > 1 ? 1 : g;
        for (int i = 0; i < 9; i++) cm[i] = g * c1[i] + (1 - g) * c2[i];
        mat_inv(cm, icm); mat_vec(icm, n, xyz);
        double s = xyz[0] + xyz[1] + xyz[2], Tn = cct_from_xy(xyz[0] / s, xyz[1] / s);
        if (fabs(Tn - T) < 1) { T = Tn; break; }
        T = Tn;
    }
    double w[3] = { xyz[0] / xyz[1], 1.0, xyz[2] / xyz[1] };
    adapt_matrix(w, cm, n, c.M);
    c.cct = (int)nearbyint(T);
    return c;
}
static void planck_xy(double T, double *x, double *y) {
    T = T < 1667 ? 1667 : T > 25000 ? 25000 : T;
    double X = T <= 4000 ? -0.2661239e9 / (T*T*T) - 0.2343589e6 / (T*T) + 0.8776956e3 / T + 0.179910
                         : -3.0258469e9 / (T*T*T) + 2.1070379e6 / (T*T) + 0.2226347e3 / T + 0.240390;
    double Y;
    if (T <= 2222) Y = -1.1063814 * X*X*X - 1.34811020 * X*X + 2.18555832 * X - 0.20219683;
    else if (T <= 4000) Y = -0.9549476 * X*X*X - 1.37418593 * X*X + 2.09137015 * X - 0.16748867;
    else Y = 3.0817580 * X*X*X - 5.87338670 * X*X + 3.75112997 * X - 0.37001483;
    *x = X; *y = Y;
}
static void locus_neutral(const Frame *f, double T, double *n) {
    double x, y, cm[9]; planck_xy(T, &x, &y);
    double w[3] = { x / y, 1.0, (1 - x - y) / y };
    cm_at(f, T, cm); mat_vec(cm, w, n); double g = n[1]; n[0] /= g; n[1] = 1; n[2] /= g;
}
/* per-temperature tables, tint-matched to the clip (see dng2prr.py calibration_by_cct) */

int dngf_load_frame(const char *path, Frame *f, int pixels) { return load_frame(path, f, pixels); }
void dngf_free_frame(Frame *f) { free(f->raw); free(f->lin); f->raw = NULL; f->lin = NULL; }
Colour dngf_colour_params(const Frame *f) { return colour_params(f); }
void dngf_locus_neutral(const Frame *f, double T, double *n) { locus_neutral(f, T, n); }
