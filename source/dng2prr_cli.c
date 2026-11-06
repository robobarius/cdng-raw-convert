/*
 * dng2prr_cli.c: native (C only, no Python) version of dng2prr.py.
 * SIGMA fp CinemaDNG clip -> Apple ProRes RAW .mov, with the WAV as PCM audio.
 *
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Experimental; NOT Apple software. Produces the same tile data as dng2prr.py 0.3
 * (tests/compare_native.py checks this).
 *
 * Build:  cc -O2 -ffp-contract=off -o dng2prr dng2prr_cli.c prr_core.c dng_lj92.c dng_jpeg.c fsg2.c -lm -lpthread
 * Usage:  dng2prr OUT.mov CLIP_FOLDER [--scale 1|2|4|8] [--neutral R/G,B/G] [--gain G]
 *                 [--fps N/D] [--limit N] [--wav FILE] [--jobs N] [--progress-every N]
 *         dng2prr --list-clips PATH...
 */
#define _FILE_OFFSET_BITS 64
#define _USE_MATH_DEFINES
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifndef NO_GYRO
#include "fsg2.h"   /* GPL-3.0-or-later: build with -DNO_GYRO and without fsg2.c for an LGPL-only binary */
#endif
#include <unistd.h>
#include <pthread.h>

#define VERSION "0.6.10"

int prr_encode_tiles(const uint16_t *img, int stride, int w, int h, int align, int scale,
                     const uint8_t *qmat, uint8_t *out, size_t cap,
                     uint16_t *tile_sz, size_t *out_len, long *clamped);
void prr_set_dc_clamp(int on);
int dng_lj92_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride);
int dng_jpeg_decode(const uint8_t *data, size_t len, uint16_t *out, int out_w, int out_h, int stride);
int dng_lj92_encode(const uint16_t *tile, int tw, int th, int stride, int bits, uint8_t **out_p, size_t *out_len);
int dng_jpeg12_dct(const uint16_t *tile, int tw, int th, int stride, float *coef);
int dng_jpeg12_encode(const float *coef, int tw, int th, const uint16_t qt[64], uint8_t **out_p, size_t *out_len);
int prr_encode_tiles_pre(const int16_t *pre, int w, int h, int align, int scale, const uint8_t *qmat, uint8_t *out, size_t cap,
                         uint16_t *tile_sz, size_t *out_len, long *clamped);
void prr_qm(const uint8_t *qmat, int scale, int16_t qm[64]);
int dng_jpeg12_encode_q(const int16_t *q, int tw, int th, const uint16_t qt[64], uint8_t **out_p, size_t *out_len);
size_t dng_jpeg12_size_q(const int16_t *q, int tw, int th);
int dng_lj92_encode_pre(const int16_t *d, const uint8_t *s, int tw, int th, int bits, uint8_t **out_p, size_t *out_len);

/* ---- GPU (Metal, macOS): transform stages of all three outputs; the entropy coding stays on the CPU cores */
#ifdef FP_METAL
#include "fp_metal.h"
static FpGpu *g_gpu = NULL;
static int g_gpu_slots = 0, g_next_slot = 0;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;
static __thread int t_slot = -1;
static int my_slot(void) {          /* one slot of GPU buffers per encoder thread */
    if (t_slot < 0) { pthread_mutex_lock(&g_slot_mu); t_slot = g_next_slot++ % (g_gpu_slots ? g_gpu_slots : 1); pthread_mutex_unlock(&g_slot_mu); }
    return t_slot;
}
#endif

static void die(const char *fmt, const char *a) { fprintf(stderr, "dng2prr: "); fprintf(stderr, fmt, a); fprintf(stderr, "\n"); exit(1); }

/* ------------------------------------------------------------------ small helpers */
typedef struct { uint8_t *p; size_t n, cap; } Buf;
static void bput(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 1024; b->p = realloc(b->p, b->cap); if (!b->p) die("%s", "out of memory"); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void bu8(Buf *b, unsigned v) { uint8_t c = (uint8_t)v; bput(b, &c, 1); }
static void bu16(Buf *b, unsigned v) { uint8_t c[2] = { (uint8_t)(v >> 8), (uint8_t)v }; bput(b, c, 2); }
static void bu32(Buf *b, uint32_t v) { uint8_t c[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, c, 4); }
static void bu64(Buf *b, uint64_t v) { bu32(b, (uint32_t)(v >> 32)); bu32(b, (uint32_t)v); }
static void bf32(Buf *b, double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); bu32(b, u); }
static void bf64(Buf *b, double v) { uint64_t u; memcpy(&u, &v, 8); bu64(b, u); }
static void bzero_(Buf *b, size_t n) { while (n--) bu8(b, 0); }
static void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
/* atom helpers: start returns the offset of the size field, end patches it */
static size_t astart(Buf *b, const char *typ) { size_t o = b->n; bu32(b, 0); bput(b, typ, 4); return o; }
static void aend(Buf *b, size_t o) { uint32_t n = (uint32_t)(b->n - o); b->p[o] = n >> 24; b->p[o + 1] = n >> 16; b->p[o + 2] = n >> 8; b->p[o + 3] = n; }

static double now(void) { struct timeval t; gettimeofday(&t, NULL); return t.tv_sec + t.tv_usec * 1e-6; }
static int ends_with_ci(const char *s, const char *suf) { size_t a = strlen(s), b = strlen(suf); return a >= b && !strcasecmp(s + a - b, suf); }
static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }
static int cmpstr(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

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

typedef struct {
    int w, h, bits;
    uint16_t *raw;                 /* h x w */
    double black, white, cm1[9], cm2[9], neutral[3], baseline_exposure;
    int has_cm1, illum1, illum2;
    int fps_n, fps_d, crop[4];     /* l r t b */
    int iso; double fnumber, exposure_time; char lens[256];
    uint16_t *lin; int nlin;       /* LinearizationTable, kept when loaded with pixels == 2 */
    uint8_t tc[8]; int has_tc;     /* DNG TimeCodes (SMPTE 12M, BCD) */
} Frame;

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
    f->fnumber = tagv(&ex[E_FNUM], 0, 0); f->exposure_time = tagv(&ex[E_EXPT], 0, 0);
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
typedef struct { double wb_r, wb_b, M[9]; int cct; } Colour;
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
static const int BYCCT[14] = { 2500, 2727, 2800, 2856, 3000, 3333, 3750, 4000, 4286, 5000, 6000, 6500, 7500, 10000 };
static void bycct_tables(const Frame *f, Buf *wbt, Buf *mxt) {
    Colour c0 = colour_params(f);
    double n0[3] = { 1.0 / c0.wb_r, 1.0, 1.0 / c0.wb_b }, ln0[3], tint[3];
    locus_neutral(f, c0.cct, ln0);
    for (int i = 0; i < 3; i++) tint[i] = n0[i] / ln0[i];
    int temps[15], nt = 0, have = 0;
    for (int i = 0; i < 14; i++) { if (BYCCT[i] == c0.cct) have = 1; }
    for (int i = 0; i < 14; i++) {                       /* sorted merge of the clip's temperature */
        if (!have && c0.cct < BYCCT[i] && (nt == 0 || temps[nt-1] < c0.cct)) temps[nt++] = c0.cct;
        temps[nt++] = BYCCT[i];
    }
    if (!have && temps[nt-1] < c0.cct) temps[nt++] = c0.cct;
    bu16(wbt, nt); bu16(wbt, 1); bu16(mxt, nt); bu16(mxt, 1);
    for (int i = 0; i < nt; i++) {
        double T = temps[i], n[3], cm[9], icm[9], w[3], M[9];
        locus_neutral(f, T, n); for (int k = 0; k < 3; k++) n[k] *= tint[k];
        cm_at(f, T, cm); mat_inv(cm, icm); mat_vec(icm, n, w); double y = w[1]; w[0] /= y; w[1] = 1; w[2] /= y;
        adapt_matrix(w, cm, n, M);
        if (temps[i] == c0.cct) { memcpy(n, n0, sizeof n); memcpy(M, c0.M, sizeof M); }
        bu32(wbt, temps[i]); bf32(wbt, 1.0 / n[0]); bf32(wbt, 1.0 / n[2]);
        bu32(mxt, temps[i]); for (int k = 0; k < 9; k++) bf32(mxt, M[k]);
    }
}

/* ------------------------------------------------------------------ frame encode */
static const double LIN_CURVE[9] = { 0, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };
#define BLACK16 256.0
#define ALIGN 3
static int tile_quantiser(int scale) {
    switch (scale) { case 1: return 4; case 2: return 8; case 4: return 15; case 8: return 30; }
    int q = (int)nearbyint(3.75 * scale); return q < 2 ? 2 : q > 255 ? 255 : q;
}
static double level_scale(const Frame *f) {
    double span = f->white - f->black, k = 1.0;
    while (span * k * 2 <= 61440.0) k *= 2;
    return k;
}
static inline double interp_u(double x) {            /* numpy.interp(x, LIN_CURVE, knots) */
    if (x <= 0) return 0;
    if (x >= 65536.0) return 65536.0;
    int j = 0; while (j < 7 && x >= LIN_CURVE[j + 1]) j++;
    double slope = (8192.0 * (j + 1) - 8192.0 * j) / (LIN_CURVE[j + 1] - LIN_CURVE[j]);
    return slope * (x - LIN_CURVE[j]) + 8192.0 * j;
}

typedef struct { uint8_t *data; size_t len; long clamped; uint8_t *meta; size_t meta_len; } Encoded;
static int meta_blob(const char *path, uint8_t **out, size_t *len);   /* defined with the DNG writer */
static int g_extra = 1;                                                  /* --no-extra: no timecode / metadata tracks */
static uint32_t g_pasp_h = 1, g_pasp_v = 1;                              /* --desqueeze F: pixel aspect (pasp) */
static int parse_desqueeze(const char *s) {
    double x = atof(s); if (!(x >= 1.0 && x <= 4.0)) return 0;
    static const struct { double f; uint32_t h, v; } k[] = { {1.25, 5, 4}, {4.0 / 3, 4, 3}, {1.5, 3, 2}, {1.6, 8, 5}, {5.0 / 3, 5, 3}, {1.8, 9, 5}, {2.0, 2, 1} };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) if (fabs(x - k[i].f) < 0.011) { g_pasp_h = k[i].h; g_pasp_v = k[i].v; return 1; }
    g_pasp_h = (uint32_t)lround(x * 100); g_pasp_v = 100; return 1;
}
static int g_no_audio = 0;                                               /* --no-audio: leave the clip's WAV out */
static int g_gyro = 1;                                                   /* --no-gyro: no Gyroflow telemetry track */

static Encoded encode_frame(const Frame *f, const Colour *col, int scale, double gain) {
    int w = f->w, h = f->h, W = (w + 15) / 16 * 16, H = (h + 15) / 16 * 16;
    float k = (float)level_scale(f), blk = (float)f->black;
    uint8_t qmat[64]; memset(qmat, 1, 64);
    size_t cap = (size_t)W * H * 4 + 65536; uint8_t *out = malloc(cap);
    int max_tiles = ((W / 16) + 8) * (H / 16); uint16_t *ts = calloc(max_tiles, 2);
    size_t olen = 0; long clamped = 0;
    int nt = -1;
    prr_set_dc_clamp(0);
#ifdef FP_METAL
    if (g_gpu) {                    /* coded curve, DCT and quantisation on the GPU; entropy coding here */
        int16_t qm[64]; prr_qm(qmat, tile_quantiser(scale), qm);
        const int16_t *pre = fpgpu_prr(g_gpu, my_slot(), f->raw, w, h, blk, k, qm);
        nt = prr_encode_tiles_pre(pre, w, h, ALIGN, tile_quantiser(scale), qmat, out, cap, ts, &olen, &clamped);
    }
#endif
    if (nt < 0) {
    uint16_t *u = malloc((size_t)W * H * 2);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float lin = (float)BLACK16 + ((float)f->raw[(size_t)y * w + x] - blk) * k;   /* float32, as numpy does */
            if (lin < 0) lin = 0;
            if (lin > 65535) lin = 65535;
            double v = nearbyint(interp_u((double)lin));
            u[(size_t)y * W + x] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
        }
    for (int y = 0; y < h; y++) for (int x = w; x < W; x++) u[(size_t)y * W + x] = u[(size_t)y * W + (w - 2 + ((x - w) & 1))];
    for (int y = h; y < H; y++) memcpy(u + (size_t)y * W, u + (size_t)(h - 2 + ((y - h) & 1)) * W, (size_t)W * 2);

    nt = prr_encode_tiles(u, W, w, h, ALIGN, tile_quantiser(scale), qmat, out, cap, ts, &olen, &clamped);
    free(u);
    }
    if (nt < 0) die("%s", "tile encoder failed; try a smaller quality setting");

    Buf hb = {0};
    bu8(&hb, 0); bu8(&hb, 0); bstr(&hb, "fpsp"); bu16(&hb, w); bu16(&hb, h);
    for (int i = 0; i < 4; i++) { int c = f->crop[i]; bu8(&hb, c < 0 ? 0 : c > 255 ? 255 : c); }
    bu16(&hb, 0);                                         /* RGGB */
    bu16(&hb, (unsigned)nearbyint((f->white - f->black) * (double)k));
    bf32(&hb, col->wb_r); bf32(&hb, col->wb_b);
    for (int i = 0; i < 9; i++) bf32(&hb, col->M[i]);
    bf32(&hb, gain); bu16(&hb, col->cct); bu16(&hb, 1 | (ALIGN << 1));
    bput(&hb, qmat, 64);

    Buf fb = {0};
    bu32(&fb, 0); bstr(&fb, "prrf"); bu16(&fb, (unsigned)hb.n + 2); bput(&fb, hb.p, hb.n);
    for (int i = 0; i < nt; i++) bu16(&fb, ts[i]);
    bput(&fb, out, olen);
    bu32(&fb, 8); bstr(&fb, "eomd");
    size_t total = (fb.n + 15) / 16 * 16; bzero_(&fb, total - fb.n);
    fb.p[0] = total >> 24; fb.p[1] = total >> 16; fb.p[2] = total >> 8; fb.p[3] = total;
    free(out); free(ts); free(hb.p);
    Encoded e = { fb.p, fb.n, clamped };
    return e;
}

/* ------------------------------------------------------------------ QuickTime writer */
typedef struct { char key[96]; int type; Buf val; } MetaItem;     /* type 1 = UTF-8, 23 = float, 0 = binary */
typedef struct { MetaItem it[24]; int n; } Meta;
static void meta_str(Meta *m, const char *k, const char *v) { if (!v || !*v) return; MetaItem *i = &m->it[m->n++]; snprintf(i->key, sizeof i->key, "%s", k); i->type = 1; memset(&i->val, 0, sizeof(Buf)); bstr(&i->val, v); }
static void meta_f32(Meta *m, const char *k, double v) { MetaItem *i = &m->it[m->n++]; snprintf(i->key, sizeof i->key, "%s", k); i->type = 23; memset(&i->val, 0, sizeof(Buf)); bf32(&i->val, v); }
static void meta_bin(Meta *m, const char *k, Buf *b) { MetaItem *i = &m->it[m->n++]; snprintf(i->key, sizeof i->key, "%s", k); i->type = 0; i->val = *b; }

static void camera_metadata(Meta *m, const Frame *f, int cct, int fps_n, int fps_d) {
    char s[64];
    meta_str(m, "com.apple.proapps.manufacturer", "Sigma"); meta_str(m, "com.apple.proapps.modelname", "SIGMA fp");
    if (f->iso) { meta_f32(m, "com.apple.proapps.exif.{Exif}.ExposureIndex", f->iso); snprintf(s, sizeof s, "%d", f->iso); meta_str(m, "org.smpte.rdd18.camera.isosensitivity", s); }
    if (cct) { snprintf(s, sizeof s, "%dK", cct); meta_str(m, "org.smpte.rdd18.camera.whitebalance", s); }
    if (f->fnumber > 0) { meta_f32(m, "com.apple.proapps.exif.{Exif}.FNumber", f->fnumber); snprintf(s, sizeof s, "F%g", f->fnumber); meta_str(m, "org.smpte.rdd18.lens.irisfnumber", s); }
    if (f->exposure_time > 0) {
        meta_f32(m, "com.apple.proapps.exif.{Exif}.ShutterSpeedValue", -log2(f->exposure_time));
        double ang = 360.0 * f->exposure_time * fps_n / fps_d; if (ang > 360) ang = 360;
        snprintf(s, sizeof s, "%.1fdeg", ang); meta_str(m, "org.smpte.rdd18.camera.shutterspeed_angle", s);
    }
    Buf wbt = {0}, mxt = {0}; bycct_tables(f, &wbt, &mxt);
    meta_bin(m, "com.apple.proresraw.whitebalance.bycct.whitebalancefactors", &wbt);
    meta_bin(m, "com.apple.proresraw.whitebalance.bycct.colormatrices", &mxt);
    if (f->lens[0]) { meta_str(m, "org.smpte.rdd18.lens.lensmodel", f->lens); meta_str(m, "com.apple.proapps.exif.{ExifAux}.LensModel", f->lens); meta_str(m, "org.smpte.rdd18.lens.lensattributes", f->lens); }
    meta_str(m, "com.apple.proapps.image.{TIFF}.Make", "SIGMA"); meta_str(m, "com.apple.proapps.image.{TIFF}.Model", "fp");
    meta_str(m, "com.apple.proapps.image.{TIFF}.Software", "dng2prr (fpSup PoC)");
}

static void put_fixed16(Buf *b, double v) { bu32(b, (uint32_t)(int64_t)nearbyint(v * 65536)); }
static void put_matrix(Buf *b) { uint32_t m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) bu32(b, m[i]); }
static void put_hdlr(Buf *b, const char *ct, const char *sub, const char *name) {
    size_t o = astart(b, "hdlr"); bu32(b, 0); bput(b, ct, 4); bput(b, sub, 4);
    if (!strcmp(ct, "mhlr")) bstr(b, "appl"); else bu32(b, 0);
    bzero_(b, 8); bu8(b, strlen(name)); bstr(b, name); aend(b, o);
}
static void put_dinf(Buf *b) { size_t o = astart(b, "dinf"), o2 = astart(b, "dref"); bu32(b, 0); bu32(b, 1); size_t o3 = astart(b, "alis"); bu32(b, 1); aend(b, o3); aend(b, o2); aend(b, o); }
static void put_co64(Buf *b, const uint64_t *off, int n) { size_t o = astart(b, "co64"); bu32(b, 0); bu32(b, n); for (int i = 0; i < n; i++) bu64(b, off[i]); aend(b, o); }
static void put_stsc(Buf *b, const uint32_t *per, int n) {
    size_t o = astart(b, "stsc"); bu32(b, 0); size_t cpos = b->n; bu32(b, 0); int ne = 0; uint32_t last = 0;
    for (int i = 0; i < n; i++) if (i == 0 || per[i] != last) { bu32(b, i + 1); bu32(b, per[i]); bu32(b, 1); last = per[i]; ne++; }
    b->p[cpos] = ne >> 24; b->p[cpos+1] = ne >> 16; b->p[cpos+2] = ne >> 8; b->p[cpos+3] = ne; aend(b, o);
}

typedef struct { FILE *fh; int ch, sw, rate; uint32_t frames; long data_off; } Wav;
static int wav_open(const char *path, Wav *w) {
    memset(w, 0, sizeof *w); w->fh = fopen(path, "rb"); if (!w->fh) return -1;
    uint8_t h[12]; if (fread(h, 1, 12, w->fh) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { fclose(w->fh); return -1; }
    for (;;) {
        uint8_t c[8]; if (fread(c, 1, 8, w->fh) != 8) { fclose(w->fh); return -1; }
        uint32_t n = rd32(c + 4, 1);
        if (!memcmp(c, "fmt ", 4)) { uint8_t fm[40] = {0}; if (fread(fm, 1, n < 40 ? n : 40, w->fh) < 16) { fclose(w->fh); return -1; } if (n > 40) fseek(w->fh, n - 40, SEEK_CUR);
            if (rd16(fm, 1) != 1 && rd16(fm, 1) != 0xFFFE) { fclose(w->fh); return -2; }
            w->ch = rd16(fm + 2, 1); w->rate = rd32(fm + 4, 1); w->sw = rd16(fm + 14, 1) / 8; if (n & 1) fseek(w->fh, 1, SEEK_CUR); }
        else if (!memcmp(c, "data", 4)) { w->data_off = ftell(w->fh); w->frames = w->ch && w->sw ? n / (w->ch * w->sw) : 0; return w->ch ? 0 : -1; }
        else fseek(w->fh, n + (n & 1), SEEK_CUR);
    }
}

/* ------------------------------------------------------------------ parallel encoding */
typedef struct {
    char **files; int nfiles, scale, have_neutral, have_gain; double neutral[2], gain;
    Encoded *slots; int *ready; int next, done_upto, window, failed;
    pthread_mutex_t mu; pthread_cond_t cv;
} Job;

static void *worker(void *arg) {
    Job *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        while (j->next < j->nfiles && j->next >= j->done_upto + j->window && !j->failed) pthread_cond_wait(&j->cv, &j->mu);
        if (j->next >= j->nfiles || j->failed) { pthread_mutex_unlock(&j->mu); return NULL; }
        int i = j->next++;
        pthread_mutex_unlock(&j->mu);
        Frame f; Encoded e = { 0 };
        if (load_frame(j->files[i], &f, 1) == 0) {
            if (j->have_neutral) { f.neutral[0] = j->neutral[0]; f.neutral[1] = 1; f.neutral[2] = j->neutral[1]; }
            Colour c = colour_params(&f);
            e = encode_frame(&f, &c, j->scale, j->have_gain ? j->gain : pow(2.0, f.baseline_exposure));
            free(f.raw);
            if (g_extra && e.data && meta_blob(j->files[i], &e.meta, &e.meta_len)) { e.meta = NULL; e.meta_len = 0; }
        }
        pthread_mutex_lock(&j->mu);
        if (!e.data) j->failed = 1;
        j->slots[i % j->window] = e; j->ready[i % j->window] = 1;
        pthread_cond_broadcast(&j->cv);
        pthread_mutex_unlock(&j->mu);
    }
}

/* ------------------------------------------------------------------ clip discovery */
static int has_dng(const char *d) {
    DIR *dir = opendir(d); if (!dir) return 0; struct dirent *e; int r = 0;
    while ((e = readdir(dir))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".dng")) { r = 1; break; }
    closedir(dir); return r;
}
static void walk(const char *d, int lvl, char ***out, int *n) {
    if (has_dng(d)) { for (int i = 0; i < *n; i++) if (!strcmp((*out)[i], d)) return; *out = realloc(*out, sizeof(char *) * (*n + 1)); (*out)[(*n)++] = strdup(d); return; }
    if (lvl <= 0) return;
    DIR *dir = opendir(d); if (!dir) return; struct dirent *e; char **subs = NULL; int ns = 0;
    while ((e = readdir(dir))) if (e->d_name[0] != '.') { subs = realloc(subs, sizeof(char *) * (ns + 1)); size_t L = strlen(d) + strlen(e->d_name) + 2; subs[ns] = malloc(L); snprintf(subs[ns++], L, "%s/%s", d, e->d_name); }
    closedir(dir); qsort(subs, ns, sizeof(char *), cmpstr);
    for (int i = 0; i < ns; i++) { if (is_dir(subs[i])) walk(subs[i], lvl - 1, out, n); free(subs[i]); }
    free(subs);
}

static void usage(void) {
    fprintf(stderr, "dng2prr %s (cDNG Raw Convert encoder, native): SIGMA fp CinemaDNG -> Apple ProRes RAW or compressed CinemaDNG (experimental, not Apple software)\n"
        "usage: dng2prr OUT.mov CLIP_FOLDER|FILES.dng... [--scale 1|2|4|8] [--neutral R/G,B/G] [--gain G]\n"
        "               [--fps N/D] [--limit N] [--wav FILE | --no-audio] [--desqueeze F] [--jobs N] [--progress-every N]\n"
        "               [--no-gyro] [--gyro-jsonl] [--no-extra] [--cpu]\n"
        "         --cpu: don't use the GPU (macOS Metal builds do the transform stages there, for every output)\n"
        "         fpSup Gyro2 data in the DNGs goes into a Gyroflow Protobuf track (--no-gyro: leave it out;\n"
        "         --gyro-jsonl: also write OUT.jsonl). --no-extra: no timecode, metadata or gyro tracks.\n"
        "       dng2prr --cdng OUTDIR CLIP_FOLDER [--cdng-mode lossless|log10|vbr-hq|vbr-lt|3|4|5|7]\n"
        "               [--copy-other] [--verify] [--skip-existing] [--jobs N]   (compressed CinemaDNG)\n"
        "       dng2prr --extract-meta IN.mov OUTDIR   (each frame's DNG tags, as carried in the .mov)\n"
        "       dng2prr --list-clips PATH...\n", VERSION);
    exit(2);
}


/* ================================================================== compressed CinemaDNG output */
/* Rewrites each DNG with its image re-encoded as JPEG tiles, keeping every other tag.
   Modes: lossless (LJ92), log10 (10-bit noise-matched curve + LJ92), and lossy
   (12-bit square-root curve + DCT JPEG; fixed quality "vbr-hq"/"vbr-lt" or a target
   ratio 3/4/5/7 against the uncompressed image size). */

typedef struct { uint16_t tag, type; uint32_t count; uint8_t *val; size_t len; struct Ifd *sub; uint32_t src_off; } Entry;   /* src_off: where an out-of-line value was in the source file */
typedef struct Ifd { Entry *e; int n; } Ifd;

static int is_ifd_pointer(int tag) { return tag == 34665 || tag == 34853 || tag == 40965; }   /* EXIF, GPS, Interop */

static int parse_ifd(const uint8_t *d, size_t dlen, uint32_t off, int le, Ifd *ifd, int depth) {
    if (off + 2 > dlen || depth > 3) return -1;
    int n = rd16(d + off, le); if (off + 2 + 12 * (size_t)n > dlen) return -1;
    ifd->e = calloc(n, sizeof(Entry)); ifd->n = 0;
    for (int i = 0; i < n; i++) {
        const uint8_t *p = d + off + 2 + 12 * i;
        Entry *e = &ifd->e[ifd->n];
        e->tag = rd16(p, le); e->type = rd16(p + 2, le); e->count = rd32(p + 4, le);
        int sz = tsz(e->type); if (!sz) continue;
        e->len = (size_t)sz * e->count;
        const uint8_t *src = e->len <= 4 ? p + 8 : d + rd32(p + 8, le);
        if (e->len > 4 && rd32(p + 8, le) + e->len > dlen) continue;
        e->src_off = e->len > 4 ? rd32(p + 8, le) : 0;
        e->val = malloc(e->len + 4); memcpy(e->val, src, e->len);
        if (is_ifd_pointer(e->tag) && e->count == 1) {
            e->sub = calloc(1, sizeof(Ifd));
            if (parse_ifd(d, dlen, rd32(e->val, le), le, e->sub, depth + 1)) { free(e->sub); e->sub = NULL; continue; }
        }
        ifd->n++;
    }
    return 0;
}
static void free_ifd(Ifd *f) { for (int i = 0; i < f->n; i++) { free(f->e[i].val); if (f->e[i].sub) { free_ifd(f->e[i].sub); free(f->e[i].sub); } } free(f->e); }
static void wr16(uint8_t *p, unsigned v, int le) { if (le) { p[0] = v; p[1] = v >> 8; } else { p[0] = v >> 8; p[1] = v; } }
static void wr32(uint8_t *p, uint32_t v, int le) { if (le) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; } else { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; } }
static Entry mk_entry(uint16_t tag, uint16_t type, uint32_t count, const uint32_t *vals, int le) {
    Entry e = { tag, type, count, NULL, (size_t)tsz(type) * count, NULL };
    e.val = calloc(e.len + 4, 1);
    for (uint32_t i = 0; i < count; i++) { if (type == 3) wr16(e.val + 2 * i, vals[i], le); else wr32(e.val + 4 * i, vals[i], le); }
    return e;
}
static int cmp_entry(const void *a, const void *b) { return (int)((const Entry *)a)->tag - (int)((const Entry *)b)->tag; }

/* A SIGMA MakerNote ("SIGMA\0\0\0", its IFD at +10) gives its values' positions from the start of the FILE, so
   it can't be moved as an opaque block: fpSup Gyro2 v0.2.0+ keeps each frame's gyro block in its own entry
   (0x4653), and the Gyroflow (fpSup) plugin follows that offset. Moving the MakerNote from `from` to `to`, the
   offsets that point inside it are moved with it (ones pointing elsewhere in the source can't be, and are left).
   The writer also keeps `to` at the same position modulo 4: the plugin finds the block at the next word boundary. */
static int is_sigma_maker_note(const Entry *e) { return e->tag == 0x927C && e->src_off && e->len >= 16 && !memcmp(e->val, "SIGMA\0\0\0", 8); }
static void sigma_maker_note_move(uint8_t *mn, size_t len, uint32_t from, uint32_t to, int le) {
    if (len < 12) return;
    unsigned n = rd16(mn + 10, le);
    if (12 + 12 * (size_t)n + 4 > len) return;
    for (unsigned i = 0; i < n; i++) {
        uint8_t *p = mn + 12 + 12 * i;
        size_t sz = (size_t)tsz(rd16(p + 2, le)) * rd32(p + 4, le);
        uint32_t off = rd32(p + 8, le);
        if (sz > 4 && off >= from && (size_t)(off - from) + sz <= len) wr32(p + 8, off - from + to, le);
    }
    uint32_t next = rd32(mn + 12 + 12 * n, le);
    if (next >= from && next - from < len) wr32(mn + 12 + 12 * n, next - from + to, le);
}

/* size of an IFD with its out-of-line values and sub-IFDs */
static size_t ifd_size(const Ifd *f) {
    size_t s = 2 + 12 * (size_t)f->n + 4;
    for (int i = 0; i < f->n; i++) {
        if (f->e[i].len > 4) s += ((f->e[i].len + 1) & ~(size_t)1) + (is_sigma_maker_note(&f->e[i]) ? 3 : 0);
        if (f->e[i].sub) s += ifd_size(f->e[i].sub);
    }
    return s;
}
/* write IFD at buf+pos (file offset base+pos); returns new pos. Entries must be sorted. */
static size_t ifd_write(const Ifd *f, uint8_t *buf, size_t pos, int le) {
    size_t start = pos, vpos = pos + 2 + 12 * (size_t)f->n + 4;
    wr16(buf + pos, f->n, le);
    for (int i = 0; i < f->n; i++) {
        const Entry *e = &f->e[i]; uint8_t *p = buf + start + 2 + 12 * i;
        wr16(p, e->tag, le); wr16(p + 2, e->type, le); wr32(p + 4, e->count, le);
        if (e->sub) { size_t sp = vpos; vpos = ifd_write(e->sub, buf, sp, le); wr32(p + 8, (uint32_t)sp, le); }
        else if (e->len <= 4) { memset(p + 8, 0, 4); memcpy(p + 8, e->val, e->len); }
        else {
            int sigma = is_sigma_maker_note(e);
            if (sigma) while ((vpos & 3) != (e->src_off & 3)) buf[vpos++] = 0;
            memcpy(buf + vpos, e->val, e->len); wr32(p + 8, (uint32_t)vpos, le);
            if (sigma) sigma_maker_note_move(buf + vpos, e->len, e->src_off, (uint32_t)vpos, le);
            vpos += (e->len + 1) & ~(size_t)1;
        }
    }
    wr32(buf + start + 2 + 12 * f->n, 0, le);
    return vpos;
}

enum { M_LOSSLESS, M_LOG10, M_LOSSY };
static double LOSSY_TOE = 0.1;           /* toe of the lossy curve (see make_curve) */
static int TW = 256, TH = 256;          /* DNG tile size (DNG2PRR_TILE=WxH to override, for tests) */
typedef struct { int kind; double scale; double ratio; const char *name; } CdngMode;

/* fixed quantiser scales of the VBR modes (on the square-root-coded 12-bit data) */
static double VBR_HQ = 2.5, VBR_LT = 6.0;
static int parse_cdng_mode(const char *s, CdngMode *m) {
    if (getenv("DNG2PRR_TOE")) LOSSY_TOE = atof(getenv("DNG2PRR_TOE"));
    if (getenv("DNG2PRR_TILE")) sscanf(getenv("DNG2PRR_TILE"), "%dx%d", &TW, &TH);
    if (getenv("DNG2PRR_VBR_HQ")) VBR_HQ = atof(getenv("DNG2PRR_VBR_HQ"));      /* for calibration */
    if (getenv("DNG2PRR_VBR_LT")) VBR_LT = atof(getenv("DNG2PRR_VBR_LT"));
    memset(m, 0, sizeof *m); m->name = s;
    if (!strcmp(s, "lossless")) m->kind = M_LOSSLESS;
    else if (!strcmp(s, "log10")) m->kind = M_LOG10;
    else if (!strcmp(s, "vbr-hq")) { m->kind = M_LOSSY; m->scale = VBR_HQ; }
    else if (!strcmp(s, "vbr-lt")) { m->kind = M_LOSSY; m->scale = VBR_LT; }
    else if (atof(s) >= 1.5 && atof(s) <= 20) { m->kind = M_LOSSY; m->ratio = atof(s); }
    else return -1;
    return 0;
}
/* base quantisation table for the lossy mode (natural order): gentle frequency weighting */
static void quant_table(double s, uint16_t qt[64]) {
    for (int v = 0; v < 8; v++) for (int u = 0; u < 8; u++) {
        double q = s * (1.0 + 0.3 * (u + v));
        int iq = (int)lround(q); qt[8*v+u] = (uint16_t)(iq < 1 ? 1 : iq > 255 ? 255 : iq);
    }
}

/* tone curve: code -> linear sensor value. N = number of codes - 1. e = 0: pure square root. */
/* Codes ped..N follow v = black + span*((x+e)^2 - e^2)/((1+e)^2 - e^2), x = (c-ped)/(N-ped):
   a square root with a linear toe of slope set by e (e = 0: pure square root). Codes below
   ped continue the toe linearly below black (clamped at 0). */
static void make_curve(double black, double white, int N, double e, int ped, uint16_t *tab) {
    double span = white - black, den = (1 + e) * (1 + e) - e * e, M = N - ped;
    double slope0 = span * 2 * e / den / M;                    /* DN per code at black */
    for (int c = 0; c <= N; c++) {
        double v;
        if (c >= ped) { double t = (c - ped) / M + e; v = black + span * (t * t - e * e) / den; }
        else v = black - (ped - c) * slope0;
        tab[c] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : lround(v));
    }
}
/* inverse lookup: linear value -> code (table is monotonic). Where several codes give the
   same value, pick the middle one, so coding errors either side decode symmetrically
   (picking the end of a run biases the decoded mean). Otherwise the nearest code. */
static void make_inverse(const uint16_t *tab, int N, uint16_t *inv /* 65536 */) {
    int lo = 0;                                   /* first code with tab >= v */
    for (int v = 0; v < 65536; v++) {
        while (lo <= N && tab[lo] < v) lo++;
        int c;
        if (lo <= N && tab[lo] == v) {
            int hi = lo; while (hi < N && tab[hi + 1] == v) hi++;
            c = (lo + hi + ((lo + hi) & 1 ? (lo & 1) : 0)) / 2;
        } else if (lo > N) c = N;
        else if (lo == 0) c = 0;
        else { int dl = v - tab[lo - 1], dh = tab[lo] - v; c = dl < dh ? lo - 1 : dh < dl ? lo : ((lo % 2 == 0) ? lo : lo - 1); }
        inv[v] = (uint16_t)c;
    }
}

typedef struct {
    char **files; int nfiles; const char *outdir; CdngMode mode; int verify, skip_existing;
    int next, done, failed, skipped; long long in_bytes, out_bytes; double max_err_sum; int err_frames;
    pthread_mutex_t mu;
} CJob;


static int cdng_one(CJob *j, const char *path, long long *in_b, long long *out_b) {
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    char outp[4096], tmpp[4096];
    snprintf(outp, sizeof outp, "%s/%s", j->outdir, base);
    snprintf(tmpp, sizeof tmpp, "%s/.%s.partial", j->outdir, base);
    if (j->skip_existing && is_file(outp)) return 1;
    FILE *fh = fopen(path, "rb"); if (!fh) { fprintf(stderr, "dng2prr: can't read %s\n", path); return -1; }
    fseeko(fh, 0, SEEK_END); size_t dlen = (size_t)ftello(fh); fseeko(fh, 0, SEEK_SET);
    uint8_t *d = malloc(dlen); if (fread(d, 1, dlen, fh) != dlen) { fclose(fh); free(d); return -1; } fclose(fh);
    *in_b = (long long)dlen;
    int le = d[0] == 'I';
    Frame f; if (load_frame(path, &f, 2)) { free(d); return -1; }
    Ifd ifd = {0}; if (parse_ifd(d, dlen, rd32(d + 4, le), le, &ifd, 0)) { fprintf(stderr, "dng2prr: %s: can't parse IFD\n", path); free(d); free(f.raw); return -1; }

    int w = f.w, h = f.h, across = (w + TW - 1) / TW, down = (h + TH - 1) / TH, nt = across * down;
    /* code values to store, the JPEG precision and the new linearization table */
    int bits = f.bits, N = 0; uint16_t *tab = NULL;
    uint16_t *codes = f.raw;                        /* stored values, possibly re-coded below */
    if (j->mode.kind != M_LOSSLESS) {
        /* work from linear values: apply any existing table first */
        if (f.lin) { for (size_t i = 0; i < (size_t)w * h; i++) { uint32_t v = f.raw[i]; f.raw[i] = f.lin[v < (uint32_t)f.nlin ? v : (uint32_t)f.nlin - 1]; } }
        double span = f.white - f.black;
        N = j->mode.kind == M_LOG10 ? 1023 : 4095;
        tab = malloc(2 * (N + 1));
        if (j->mode.kind == M_LOG10) {
            double e = span > N ? N / (2 * span - 2.0 * N) : 1e6;  /* 1 DN per code at black; linear if it fits */
            make_curve(f.black, f.white, N, e, 0, tab);
        } else {
            /* lossy: square-root-like curve with a pedestal of 128 codes below black, so DCT
               errors around black stay symmetric (no clipping at code 0, no brightness bias) */
            make_curve(f.black, f.white, N, LOSSY_TOE, 128, tab);
        }
        uint16_t *inv = malloc(2 * 65536); make_inverse(tab, N, inv);
        for (size_t i = 0; i < (size_t)w * h; i++) codes[i] = inv[f.raw[i]];
        free(inv);
        bits = j->mode.kind == M_LOG10 ? 10 : 12;
    } else if (f.lin) { tab = f.lin; N = f.nlin - 1; f.lin = NULL; }
    if (bits > 16 || bits < 2) bits = 16;

    /* tiles (edge tiles padded by repeating the last Bayer pair) */
    uint16_t *tile = malloc(2 * (size_t)TW * TH);
    uint8_t **tdata = calloc(nt, sizeof(uint8_t *)); size_t *tlen = calloc(nt, sizeof(size_t));
    float *coef = NULL; int nblk = (TW / 8) * (TH / 16) * 2;
    int gpu = 0;
#ifdef FP_METAL
    if (g_gpu && TW % 16 == 0 && TH % 16 == 0) {
        int sl = my_slot();
        if (j->mode.kind == M_LOSSY) {          /* DCT once on the GPU; each rate-control step quantises there too */
            fpgpu_jpeg_dct(g_gpu, sl, codes, w, h, TW, TH);
            size_t raw_bytes = (size_t)w * h * f.bits / 8; double sc = j->mode.scale; uint16_t qt[64];
            if (j->mode.ratio > 0) {
                double target = raw_bytes / j->mode.ratio, lo = 0.25, hi = 128;
                /* estimates from GPU symbol statistics, corrected by the exact size of one encode (the first) */
                double corr = 1.0; int calibrated = 0;
                for (int it = 0; it < 14; it++) {
                    double mid = sqrt(lo * hi); quant_table(mid, qt);
                    double est = fpgpu_jpeg_estimate(g_gpu, sl, qt);
                    if (!calibrated) {
                        const int16_t *q = fpgpu_jpeg_quant(g_gpu, sl, qt); size_t tot = 0;
                        for (int t = 0; t < nt; t++) tot += dng_jpeg12_size_q(q + 64 * (size_t)nblk * t, TW, TH);
                        tot += tot / 256;               /* byte stuffing */
                        corr = est > 0 ? tot / est : 1; calibrated = 1;
                    }
                    if (est * corr > target) lo = mid; else hi = mid;
                    if (hi / lo < 1.03) break;
                }
                sc = hi;
            }
            quant_table(sc, qt);
            const int16_t *q = fpgpu_jpeg_quant(g_gpu, sl, qt);
            for (int t = 0; t < nt; t++) dng_jpeg12_encode_q(q + 64 * (size_t)nblk * t, TW, TH, qt, &tdata[t], &tlen[t]);
        } else {                                 /* residuals and their sizes on the GPU; Huffman coding here */
            const int16_t *d; const uint8_t *s;
            fpgpu_lj92(g_gpu, sl, codes, w, h, TW, TH, bits, &d, &s);
            for (int t = 0; t < nt; t++) dng_lj92_encode_pre(d + (size_t)TW * TH * t, s + (size_t)TW * TH * t, TW, TH, bits, &tdata[t], &tlen[t]);
        }
        gpu = 1;
    }
#endif
    if (!gpu && j->mode.kind == M_LOSSY) coef = malloc(sizeof(float) * 64 * (size_t)nblk * nt);
    for (int t = 0; t < nt && !gpu; t++) {
        int ty = t / across, tx = t % across;
        for (int y = 0; y < TH; y++) {
            int sy = ty * TH + y; if (sy >= h) sy = h - 2 + ((sy - h) & 1);
            for (int x = 0; x < TW; x++) {
                int sx = tx * TW + x; if (sx >= w) sx = w - 2 + ((sx - w) & 1);
                tile[y * TW + x] = codes[(size_t)sy * w + sx];
            }
        }
        if (j->mode.kind == M_LOSSY) dng_jpeg12_dct(tile, TW, TH, TW, coef + 64 * (size_t)nblk * t);
        else dng_lj92_encode(tile, TW, TH, TW, bits, &tdata[t], &tlen[t]);
    }
    if (j->mode.kind == M_LOSSY && !gpu) {
        size_t raw_bytes = (size_t)w * h * f.bits / 8;
        double sc = j->mode.scale;
        uint16_t qt[64];
        if (j->mode.ratio > 0) {                         /* bisection on the quantiser scale */
            double target = raw_bytes / j->mode.ratio, lo = 0.25, hi = 128;
            int step = nt >= 24 ? 3 : 1;                 /* estimate from every 3rd tile, then refine on all */
            for (int it = 0; it < 10; it++) {
                double mid = sqrt(lo * hi); size_t tot = 0; int used = 0;
                quant_table(mid, qt);
                int st = it < 6 ? step : 1;
                for (int t = 0; t < nt; t += st) { uint8_t *b; size_t n; dng_jpeg12_encode(coef + 64 * (size_t)nblk * t, TW, TH, qt, &b, &n); tot += n; free(b); used++; }
                double est = (double)tot * nt / used;
                if (est > target) lo = mid; else hi = mid;
                if (hi / lo < 1.03) break;
            }
            sc = hi;
        }
        quant_table(sc, qt);
        for (int t = 0; t < nt; t++) dng_jpeg12_encode(coef + 64 * (size_t)nblk * t, TW, TH, qt, &tdata[t], &tlen[t]);
        free(coef);
    }

    /* verification: decode every tile again and compare with what was meant to be stored */
    int bad = 0; double maxerr = 0;
    if (j->verify) {
        for (int t = 0; t < nt && !bad; t++) {
            int ty = t / across, tx = t % across;
            if (dng_jpeg_decode(tdata[t], tlen[t], tile, TW, TH, TW)) { bad = 1; break; }
            for (int y = 0; y < TH && ty * TH + y < h; y++)
                for (int x = 0; x < TW && tx * TW + x < w; x++) {
                    int a = tile[y * TW + x], b = codes[(size_t)(ty * TH + y) * w + tx * TW + x];
                    if (j->mode.kind != M_LOSSY) { if (a != b) bad = 1; }
                    else { double ea = fabs((double)tab[a] - tab[b]); if (ea > maxerr) maxerr = ea; }
                }
        }
        if (bad) fprintf(stderr, "dng2prr: %s: VERIFY FAILED (decoded tiles differ)\n", path);
    }

    /* new IFD0 */
    Ifd out = { calloc(ifd.n + 8, sizeof(Entry)), 0 };
    for (int i = 0; i < ifd.n; i++) {
        int tg = ifd.e[i].tag;
        if (tg == 273 || tg == 278 || tg == 279 || tg == 322 || tg == 323 || tg == 324 || tg == 325 ||
            tg == 259 || tg == 258 || tg == 50712 || tg == 50972 || tg == 51111) { free(ifd.e[i].val); if (ifd.e[i].sub) { free_ifd(ifd.e[i].sub); free(ifd.e[i].sub); } continue; }
        out.e[out.n++] = ifd.e[i];
    }
    uint32_t v1;
    v1 = (tab && getenv("DNG2PRR_BPS16")) ? 16 : bits;            /* BitsPerSample (test switch) */
    out.e[out.n++] = mk_entry(258, 3, 1, &v1, le);
    v1 = 7; out.e[out.n++] = mk_entry(259, 3, 1, &v1, le);
    v1 = TW; out.e[out.n++] = mk_entry(322, 4, 1, &v1, le);
    v1 = TH; out.e[out.n++] = mk_entry(323, 4, 1, &v1, le);
    uint32_t *offs = calloc(nt, 4), *cnts = calloc(nt, 4);
    for (int t = 0; t < nt; t++) cnts[t] = (uint32_t)tlen[t];
    out.e[out.n++] = mk_entry(324, 4, nt, offs, le);
    out.e[out.n++] = mk_entry(325, 4, nt, cnts, le);
    if (tab) { uint32_t *tv = malloc(4 * (N + 1)); for (int c = 0; c <= N; c++) tv[c] = tab[c]; out.e[out.n++] = mk_entry(50712, 3, N + 1, tv, le); free(tv); }
    qsort(out.e, out.n, sizeof(Entry), cmp_entry);
    size_t hdr = 8 + ifd_size(&out), total = hdr;
    for (int t = 0; t < nt; t++) total += (tlen[t] + 1) & ~(size_t)1;
    if (total > 0xFFFFFFF0u) { fprintf(stderr, "dng2prr: %s: output too large\n", path); bad = 1; }
    uint8_t *buf = calloc(total, 1);
    if (le) memcpy(buf, "II*\0", 4); else memcpy(buf, "MM\0*", 4);
    wr32(buf + 4, 8, le);
    size_t pos = hdr;
    for (int t = 0; t < nt; t++) { offs[t] = (uint32_t)pos; memcpy(buf + pos, tdata[t], tlen[t]); pos += (tlen[t] + 1) & ~(size_t)1; }
    for (int i = 0; i < out.n; i++) if (out.e[i].tag == 324) for (int t = 0; t < nt; t++) wr32(out.e[i].val + 4 * t, offs[t], le);
    ifd_write(&out, buf, 8, le);

    int rc = 0;
    if (!bad) {
        FILE *fo = fopen(tmpp, "wb");
        if (!fo || fwrite(buf, 1, total, fo) != total || fclose(fo)) { fprintf(stderr, "dng2prr: can't write %s\n", tmpp); rc = -1; unlink(tmpp); }
        else if (rename(tmpp, outp)) { fprintf(stderr, "dng2prr: can't rename to %s\n", outp); rc = -1; unlink(tmpp); }
    } else rc = -1;
    *out_b = (long long)total;
    if (j->verify && j->mode.kind == M_LOSSY) { pthread_mutex_lock(&j->mu); j->max_err_sum += maxerr; j->err_frames++; pthread_mutex_unlock(&j->mu); }

    for (int t = 0; t < nt; t++) free(tdata[t]);
    free(tdata); free(tlen); free(tile); free(offs); free(cnts); free(buf); free(d);
    if (tab != f.lin) free(tab);
    free(f.raw); free(f.lin);
    for (int i = 0; i < out.n; i++) { free(out.e[i].val); if (out.e[i].sub) { free_ifd(out.e[i].sub); free(out.e[i].sub); } }
    free(out.e); free(ifd.e);
    return rc;
}

static void *cdng_worker(void *arg) {
    CJob *j = arg;
    for (;;) {
        pthread_mutex_lock(&j->mu);
        if (j->next >= j->nfiles || j->failed) { pthread_mutex_unlock(&j->mu); return NULL; }
        int i = j->next++;
        pthread_mutex_unlock(&j->mu);
        long long ib = 0, ob = 0;
        int rc = cdng_one(j, j->files[i], &ib, &ob);
        pthread_mutex_lock(&j->mu);
        if (rc < 0) j->failed = 1;
        else if (rc == 1) j->skipped++;
        else { j->in_bytes += ib; j->out_bytes += ob; }
        j->done++;
        printf("  frame %d/%d  %.2f MB -> %.2f MB\n", j->done, j->nfiles, ib / 1e6, ob / 1e6); fflush(stdout);
        pthread_mutex_unlock(&j->mu);
    }
}

static int copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb"); if (!a) return -1;
    char tmp[4096]; snprintf(tmp, sizeof tmp, "%s.partial", dst);
    FILE *b = fopen(tmp, "wb"); if (!b) { fclose(a); return -1; }
    char buf[1 << 16]; size_t n; int rc = 0;
    while ((n = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, n, b) != n) { rc = -1; break; }
    fclose(a); if (fclose(b)) rc = -1;
    if (rc || rename(tmp, dst)) { unlink(tmp); return -1; }
    return 0;
}

static int mkdirs(const char *p) {
    char t[4096]; snprintf(t, sizeof t, "%s", p);
    for (char *q = t + 1; *q; q++) if (*q == '/') { *q = 0; mkdir(t, 0755); *q = '/'; }
    return mkdir(t, 0755) && !is_dir(t) ? -1 : 0;
}

static int run_cdng(const char *outdir, const char *mode_s, char **inputs, int ninputs, int jobs,
                    int copy_other, int verify, int skip_existing) {
    CdngMode mode; if (parse_cdng_mode(mode_s, &mode)) die("unknown --cdng-mode %s (lossless, log10, vbr-hq, vbr-lt, 3, 4, 5, 7)", mode_s);
    if (ninputs != 1 || !is_dir(inputs[0])) die("%s", "--cdng needs exactly one clip folder");
    if (mkdirs(outdir)) die("can't create %s", outdir);
    char **files = NULL; int nf = 0; char **other = NULL; int no = 0;
    DIR *dir = opendir(inputs[0]); struct dirent *e;
    while (dir && (e = readdir(dir))) {
        if (e->d_name[0] == '.') continue;
        size_t L = strlen(inputs[0]) + strlen(e->d_name) + 2; char *p = malloc(L); snprintf(p, L, "%s/%s", inputs[0], e->d_name);
        if (!is_file(p)) { free(p); continue; }
        if (ends_with_ci(e->d_name, ".dng")) { files = realloc(files, sizeof(char *) * (nf + 1)); files[nf++] = p; }
        else { other = realloc(other, sizeof(char *) * (no + 1)); other[no++] = p; }
    }
    if (dir) closedir(dir);
    if (!nf) die("%s", "no DNG files");
    qsort(files, nf, sizeof(char *), cmpstr);
    printf("%d frame(s), CinemaDNG %s\n", nf, mode.name); fflush(stdout);
    if (copy_other)
        for (int i = 0; i < no; i++) {
            const char *b = strrchr(other[i], '/') + 1; char dst[4096]; snprintf(dst, sizeof dst, "%s/%s", outdir, b);
            if (skip_existing && is_file(dst)) continue;
            if (copy_file(other[i], dst)) fprintf(stderr, "dng2prr: warning: could not copy %s\n", b);
            else printf("copied %s\n", b);
        }
    CJob j; memset(&j, 0, sizeof j);
    j.files = files; j.nfiles = nf; j.outdir = outdir; j.mode = mode; j.verify = verify; j.skip_existing = skip_existing;
    pthread_mutex_init(&j.mu, NULL);
    double t0 = now();
    pthread_t *th = malloc(sizeof(pthread_t) * jobs);
    for (int i = 0; i < jobs; i++) pthread_create(&th[i], NULL, cdng_worker, &j);
    for (int i = 0; i < jobs; i++) pthread_join(th[i], NULL);
    if (j.failed) die("%s", "a frame failed; stopping (frames already written are complete files)");
    double el = now() - t0;
    printf("done: %d written, %d skipped, %.1f MB -> %.1f MB (%.2f:1), %.1f fps%s\n",
           nf - j.skipped, j.skipped, j.in_bytes / 1e6, j.out_bytes / 1e6, j.out_bytes ? (double)j.in_bytes / j.out_bytes : 0,
           (nf - j.skipped) / (el > 0 ? el : 1), verify ? (mode.kind == M_LOSSY ? "" : ", verified lossless") : "");
    if (verify && mode.kind == M_LOSSY && j.err_frames) printf("verify: decoded fine; mean max error %.1f DN\n", j.max_err_sum / j.err_frames);
    return 0;
}

/* The frame's DNG minus its image: a small TIFF holding every tag of IFD0 and its sub-IFDs
   (EXIF incl. MakerNote, GPS) with the image-data tags removed. Carried per frame in the
   .mov's timed-metadata track, so nothing in the DNG headers is lost (e.g. per-frame
   gyro/lens telemetry, whatever its format). */
static int meta_blob(const char *path, uint8_t **out, size_t *len) {
    FILE *fh = fopen(path, "rb"); if (!fh) return -1;
    fseeko(fh, 0, SEEK_END); size_t dlen = (size_t)ftello(fh); fseeko(fh, 0, SEEK_SET);
    uint8_t *d = malloc(dlen); if (fread(d, 1, dlen, fh) != dlen) { fclose(fh); free(d); return -1; } fclose(fh);
    int le = d[0] == 'I';
    Ifd ifd = {0}; if (parse_ifd(d, dlen, rd32(d + 4, le), le, &ifd, 0)) { free(d); return -1; }
    Ifd o = { calloc(ifd.n + 1, sizeof(Entry)), 0 };
    for (int i = 0; i < ifd.n; i++) {
        int tg = ifd.e[i].tag;
        if (tg == 273 || tg == 279 || tg == 324 || tg == 325) { free(ifd.e[i].val); continue; }
        o.e[o.n++] = ifd.e[i];
    }
    size_t total = 8 + ifd_size(&o);
    uint8_t *b = calloc(total, 1);
    if (le) memcpy(b, "II*\0", 4); else memcpy(b, "MM\0*", 4);
    wr32(b + 4, 8, le); ifd_write(&o, b, 8, le);
    for (int i = 0; i < o.n; i++) { free(o.e[i].val); if (o.e[i].sub) { free_ifd(o.e[i].sub); free(o.e[i].sub); } }
    free(o.e); free(ifd.e); free(d);
    *out = b; *len = total;
    return 0;
}

/* SMPTE timecode from the DNG TimeCodes tag -> frame count at the given nominal rate */
static long tc_frames(const uint8_t tc[8], int nominal, int *drop) {
    int ff = (tc[0] & 0x0F) + 10 * ((tc[0] >> 4) & 0x03);
    int ss = (tc[1] & 0x0F) + 10 * ((tc[1] >> 4) & 0x07);
    int mm = (tc[2] & 0x0F) + 10 * ((tc[2] >> 4) & 0x07);
    int hh = (tc[3] & 0x0F) + 10 * ((tc[3] >> 4) & 0x03);
    *drop = (tc[0] >> 6) & 1;
    long total = ((long)hh * 3600 + mm * 60 + ss) * nominal + ff;
    if (*drop && (nominal == 30 || nominal == 60)) {           /* drop-frame: 2 (or 4) numbers per minute except every 10th */
        int dpm = nominal / 15; long mins = hh * 60L + mm;
        total -= dpm * (mins - mins / 10);
    }
    return total;
}

/* --extract-meta IN.mov OUTDIR: write each frame's carried DNG tags back out as
   OUTDIR/<frame number>.tif (a TIFF with all of that frame's tags and no image), so tools that
   read per-frame metadata from DNGs (e.g. gyro telemetry) can be pointed at a converted clip. */
static uint64_t be(const uint8_t *p, int n) { uint64_t v = 0; for (int i = 0; i < n; i++) v = v << 8 | p[i]; return v; }
static int find_box(const uint8_t *d, size_t start, size_t end, const char *typ, size_t *bs, size_t *be_) {
    size_t o = start;
    while (o + 8 <= end) {
        uint64_t sz = be(d + o, 4); size_t hl = 8;
        if (sz == 1) { sz = be(d + o + 8, 8); hl = 16; } else if (sz == 0) sz = end - o;
        if (sz < hl || o + sz > end) return 0;
        if (!memcmp(d + o + 4, typ, 4)) { *bs = o + hl; *be_ = o + sz; return 1; }
        o += sz;
    }
    return 0;
}
static int extract_meta(const char *in, const char *outdir) {
    FILE *fh = fopen(in, "rb"); if (!fh) die("can't read %s", in);
    fseeko(fh, 0, SEEK_END); size_t n = (size_t)ftello(fh);
    /* find moov (at the end of our files) by walking top-level boxes */
    size_t o = 0, ms = 0, me = 0; uint8_t h[16];
    while (o + 8 <= n) {
        fseeko(fh, (off_t)o, SEEK_SET); if (fread(h, 1, 16, fh) < 8) break;
        uint64_t sz = be(h, 4); if (sz == 1) sz = be(h + 8, 8); else if (sz == 0) sz = n - o;
        if (!memcmp(h + 4, "moov", 4)) { ms = o; me = o + sz; break; }
        if (sz < 8) break;
        o += sz;
    }
    if (!me) die("%s: no moov box", in);
    uint8_t *m = malloc(me - ms); fseeko(fh, (off_t)ms, SEEK_SET); if (fread(m, 1, me - ms, fh) != me - ms) die("%s", "read error");
    size_t pos = 8, end = me - ms, ts, te;
    while (find_box(m, pos, end, "trak", &ts, &te)) {
        size_t a, b2, c, d2, e2, f2, g, h2;
        if (find_box(m, ts, te, "mdia", &a, &b2) && find_box(m, a, b2, "minf", &c, &d2) && find_box(m, c, d2, "stbl", &e2, &f2) &&
            find_box(m, e2, f2, "stsd", &g, &h2) && h2 - g > 16 && !memcmp(m + g + 12, "mebx", 4)) {
            size_t zs = 0, ze = 0, cs = 0, ce = 0;
            if (!find_box(m, e2, f2, "stsz", &zs, &ze) || !find_box(m, e2, f2, "co64", &cs, &ce)) die("%s", "metadata track without stsz/co64");
            uint32_t cnt = (uint32_t)be(m + zs + 8, 4), nchunks = (uint32_t)be(m + cs + 4, 4);
            if (cnt != nchunks) die("%s", "unexpected metadata chunk layout");
            mkdirs(outdir); int written = 0;
            for (uint32_t i = 0; i < cnt; i++) {
                uint32_t sz = (uint32_t)be(m + zs + 12 + 4 * i, 4); uint64_t off = be(m + cs + 8 + 8 * (size_t)i, 8);
                if (sz <= 8) continue;
                uint8_t *buf = malloc(sz); fseeko(fh, (off_t)off, SEEK_SET); if (fread(buf, 1, sz, fh) != sz) die("%s", "read error");
                char p[4096]; snprintf(p, sizeof p, "%s/%06u.tif", outdir, i);
                FILE *fo = fopen(p, "wb"); if (fo) { fwrite(buf + 8, 1, sz - 8, fo); fclose(fo); written++; }
                free(buf);
            }
            printf("extracted %d frame metadata files to %s\n", written, outdir);
            fclose(fh); free(m); return 0;
        }
        pos = te;
    }
    die("%s: no fpSup metadata track", in); return 1;
}

int main(int argc, char **argv) {
    const char *out = NULL, *wavp = NULL; char **inputs = NULL; int ninputs = 0;
    int scale = 1, limit = 0, jobs = (int)sysconf(_SC_NPROCESSORS_ONLN), every = 25, list = 0, fps_n = 0, fps_d = 0;
    int have_neutral = 0, have_gain = 0; double neutral[2] = {1, 1}, gain = 1;
    const char *cdng_out = NULL, *cdng_mode = "lossless"; int copy_other = 0, verify = 0, skip_existing = 0, gyro_jsonl = 0, use_gpu = 1;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define NEXT() (i + 1 < argc ? argv[++i] : (usage(), ""))
        if (!strcmp(a, "--scale")) scale = atoi(NEXT());
        else if (!strcmp(a, "--limit")) limit = atoi(NEXT());
        else if (!strcmp(a, "--jobs")) jobs = atoi(NEXT());
        else if (!strcmp(a, "--progress-every")) every = atoi(NEXT());
        else if (!strcmp(a, "--wav")) wavp = NEXT();
        else if (!strcmp(a, "--no-audio")) g_no_audio = 1;
        else if (!strcmp(a, "--desqueeze")) { if (!parse_desqueeze(NEXT())) usage(); }
        else if (!strcmp(a, "--gain")) { gain = atof(NEXT()); have_gain = 1; }
        else if (!strcmp(a, "--neutral")) { if (sscanf(NEXT(), "%lf,%lf", &neutral[0], &neutral[1]) != 2) usage(); have_neutral = 1; }
        else if (!strcmp(a, "--fps")) { if (sscanf(NEXT(), "%d/%d", &fps_n, &fps_d) != 2) usage(); }
        else if (!strcmp(a, "--list-clips")) list = 1;
        else if (!strcmp(a, "--cdng")) cdng_out = NEXT();
        else if (!strcmp(a, "--cdng-mode")) cdng_mode = NEXT();
        else if (!strcmp(a, "--copy-other")) copy_other = 1;
        else if (!strcmp(a, "--verify")) verify = 1;
        else if (!strcmp(a, "--skip-existing")) skip_existing = 1;
        else if (!strcmp(a, "--no-extra")) g_extra = 0;
        else if (!strcmp(a, "--no-gyro")) g_gyro = 0;
        else if (!strcmp(a, "--cpu")) use_gpu = 0;
        else if (!strcmp(a, "--gyro-jsonl")) gyro_jsonl = 1;
        else if (!strcmp(a, "--extract-meta")) { if (i + 2 >= argc) usage(); return extract_meta(argv[i + 1], argv[i + 2]); }
        else if (!strcmp(a, "--version")) { printf("dng2prr %s (cDNG Raw Convert, native)\n", VERSION); return 0; }
        else if (a[0] == '-' && a[1] == '-') usage();
        else if (!out && !list && !cdng_out) out = a;
        else { inputs = realloc(inputs, sizeof(char *) * (ninputs + 1)); inputs[ninputs++] = (char *)a; }
    }
    if (every < 1) every = 1;
    if (jobs < 1) jobs = 1;
    if (list) {
        char **clips = NULL; int nc = 0;
        for (int i = 0; i < ninputs; i++) {
            char p[4096]; snprintf(p, sizeof p, "%s", inputs[i]); size_t L = strlen(p); while (L > 1 && p[L-1] == '/') p[--L] = 0;
            if (is_file(p) && ends_with_ci(p, ".dng")) { char *s = strrchr(p, '/'); if (s) *s = 0; else strcpy(p, "."); }
            char rp[4096]; if (realpath(p, rp) && is_dir(rp)) walk(rp, 3, &clips, &nc);
        }
        for (int i = 0; i < nc; i++) printf("%s\n", clips[i]);
        return 0;
    }
    (void)use_gpu;
#ifdef FP_METAL
    if (use_gpu && !list && !getenv("DNG2PRR_CPU")) {
        char err[256] = ""; g_gpu_slots = jobs > 0 ? jobs : 1;
        g_gpu = fpgpu_create(g_gpu_slots, err, sizeof err);
        if (g_gpu) printf("GPU: %s (transform stages; entropy coding on %d CPU threads)\n", fpgpu_name(g_gpu), jobs);
        else printf("GPU: not available (%s); CPU only\n", err);
    }
#endif
    if (cdng_out) return run_cdng(cdng_out, cdng_mode, inputs, ninputs, jobs, copy_other, verify, skip_existing);
    if (!out || !ninputs) usage();

    char **files = NULL; int nf = 0;
    for (int i = 0; i < ninputs; i++) {
        if (is_dir(inputs[i])) {
            DIR *dir = opendir(inputs[i]); struct dirent *e; char **list2 = NULL; int n2 = 0;
            while (dir && (e = readdir(dir))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".dng")) {
                size_t L = strlen(inputs[i]) + strlen(e->d_name) + 2; list2 = realloc(list2, sizeof(char *) * (n2 + 1)); list2[n2] = malloc(L); snprintf(list2[n2++], L, "%s/%s", inputs[i], e->d_name); }
            if (dir) closedir(dir);
            qsort(list2, n2, sizeof(char *), cmpstr);
            files = realloc(files, sizeof(char *) * (nf + n2)); memcpy(files + nf, list2, sizeof(char *) * n2); nf += n2; free(list2);
        } else { files = realloc(files, sizeof(char *) * (nf + 1)); files[nf++] = inputs[i]; }
    }
    if (limit > 0 && nf > limit) nf = limit;
    if (!nf) die("%s", "no DNG files");
    int w0 = -1, h0 = -1;
    for (int i = 0; i < nf; i++) {
        Tag t[NTAGS]; int le;
        if (read_header(files[i], t, NULL, &le, NULL)) die("%s: not a TIFF/DNG file", files[i]);
        int w = (int)tagv(&t[T_W], 0, 0), h = (int)tagv(&t[T_H], 0, 0); free_tags(t, NTAGS);
        if (w0 < 0) { w0 = w; h0 = h; } else if (w != w0 || h != h0) die("%s", "the DNGs in this folder have different sizes - not a single clip");
    }
    Frame first; if (load_frame(files[0], &first, 0)) return 1;
    if (have_neutral) { first.neutral[0] = neutral[0]; first.neutral[1] = 1; first.neutral[2] = neutral[1]; }
    if (!fps_n) { fps_n = first.fps_n; fps_d = first.fps_d; }
    printf("%d frame(s), %dx%d, white %g, black %g, fps %d/%d, scale %d\n", nf, first.w, first.h, first.white, first.black, fps_n, fps_d, scale);
    Wav wav; int have_wav = 0; char wavbuf[4096];
    if (g_no_audio) wavp = NULL;
    if (!g_no_audio && !wavp && is_dir(inputs[0])) {
        DIR *dir = opendir(inputs[0]); struct dirent *e; char *best = NULL;
        while (dir && (e = readdir(dir))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".wav")) if (!best || strcmp(e->d_name, best) < 0) { free(best); best = strdup(e->d_name); }
        if (dir) closedir(dir);
        if (best) { snprintf(wavbuf, sizeof wavbuf, "%s/%s", inputs[0], best); wavp = wavbuf; free(best); }
    }
    if (wavp) {
        if (wav_open(wavp, &wav) == 0) { have_wav = 1; printf("audio: %s\n", wavp); }
        else fprintf(stderr, "dng2prr: warning: could not read %s as PCM WAV; writing without audio\n", wavp);
    }
    /* Sigma's SPP_metadata.xmp (or any .xmp) in the clip folder: kept as movie metadata */
    Buf xmp = {0}; char xmpname[512] = "";
    if (g_extra && is_dir(inputs[0])) {
        DIR *dir = opendir(inputs[0]); struct dirent *e;
        while (dir && (e = readdir(dir))) if (e->d_name[0] != '.' && ends_with_ci(e->d_name, ".xmp") && !xmp.n) {
            char pth[4096]; snprintf(pth, sizeof pth, "%s/%s", inputs[0], e->d_name);
            FILE *xf = fopen(pth, "rb"); if (!xf) continue;
            char tmp[65536]; size_t n; while ((n = fread(tmp, 1, sizeof tmp, xf)) > 0 && xmp.n < (1u << 22)) bput(&xmp, tmp, n);
            fclose(xf); snprintf(xmpname, sizeof xmpname, "%s", e->d_name);
        }
        if (dir) closedir(dir);
    }
    /* fpSup Gyro2: each DNG's FSG2 block -> one Gyroflow Protobuf message per frame */
    uint8_t **gmsg = NULL; size_t *glen = NULL; int have_gyro = 0;
#ifndef NO_GYRO
    if (g_extra && g_gyro) {
        Fsg2Frame *gf = calloc(nf, sizeof(Fsg2Frame)); int ngot = 0, bad = 0; char gerr[640] = "";
        for (int i = 0; i < nf; i++) {
            char e1[256];
            if (fsg2_read(files[i], &gf[i], e1, sizeof e1) < 0) { if (!bad) snprintf(gerr, sizeof gerr, "%s: %s", files[i], e1); bad++; }
            else if (!gf[i].present && e1[0] && !gerr[0]) snprintf(gerr, sizeof gerr, "%s", e1);
            ngot += gf[i].present;
            if (!i && !gf[0].present && !bad) break;          /* no Gyro2 in this clip */
        }
        if (ngot && ngot < nf) fprintf(stderr, "dng2prr: warning: only %d of %d frames carry Gyro2 data%s%s; writing without gyro telemetry\n", ngot, nf, gerr[0] ? ": " : "", gerr);
        else if (ngot == nf) {
            FILE *js = NULL; char jsp[4096] = "";
            if (gyro_jsonl) { snprintf(jsp, sizeof jsp, "%s", out); char *dot = strrchr(jsp, '.'), *sl = strrchr(jsp, '/'); if (dot && (!sl || dot > sl)) *dot = 0; strncat(jsp, ".jsonl", sizeof jsp - strlen(jsp) - 1); js = fopen(jsp, "w"); }
            int pw = first.w - first.crop[0] - first.crop[1], ph = first.h - first.crop[2] - first.crop[3];   /* the decoded picture: DefaultCrop */
            if (pw <= 0 || ph <= 0) { pw = first.w; ph = first.h; }
            if (fsg2_messages(gf, nf, pw, ph, &gmsg, &glen, js, gerr, sizeof gerr) == 0) {
                long ns = 0; for (int i = 0; i < nf; i++) ns += gf[i].n_samples;
                have_gyro = 1;
                printf("gyro: fpSup Gyro2 data in every frame (%ld IMU samples at %.0f Hz), kept as a Gyroflow Protobuf track\n", ns, 1e12 / gf[0].period_ps);
                if (js) printf("gyro: also written to %s\n", jsp);
            } else {
                fprintf(stderr, "dng2prr: warning: Gyro2 data could not be converted (%s); writing without gyro telemetry\n", gerr);
                if (js) { fclose(js); js = NULL; unlink(jsp); }
            }
            if (js) fclose(js);
        } else if (bad) fprintf(stderr, "dng2prr: warning: %s; writing without gyro telemetry\n", gerr);
        for (int i = 0; i < nf; i++) fsg2_free(&gf[i]);
        free(gf);
    }
#endif
    (void)gyro_jsonl;
    int tc_drop = 0, tc_nom = (int)nearbyint((double)fps_n / fps_d); long tc_start = 0; int have_tc = g_extra && first.has_tc && tc_nom > 0;
    if (have_tc) {
        tc_start = tc_frames(first.tc, tc_nom, &tc_drop);
        printf("timecode: %02d:%02d:%02d%c%02d\n", (first.tc[3] & 15) + 10 * ((first.tc[3] >> 4) & 3), (first.tc[2] & 15) + 10 * ((first.tc[2] >> 4) & 7),
               (first.tc[1] & 15) + 10 * ((first.tc[1] >> 4) & 7), tc_drop ? ';' : ':', (first.tc[0] & 15) + 10 * ((first.tc[0] >> 4) & 3));
    }
    if (xmp.n) printf("metadata: %s kept in the .mov\n", xmpname);
    if (g_extra) printf("metadata: every frame's DNG tags kept in a timed-metadata track\n");
    fflush(stdout);
    Colour c0 = colour_params(&first);
    Meta meta = {0}; camera_metadata(&meta, &first, c0.cct, fps_n, fps_d);
    if (xmp.n) { Buf xs = {0}; bput(&xs, xmp.p, xmp.n); meta_str(&meta, "io.github.ijigen.fpsup.xmp", NULL); meta.n--;   /* placeholder to keep order simple */
                 MetaItem *it = &meta.it[meta.n++]; snprintf(it->key, sizeof it->key, "io.github.ijigen.fpsup.%s", xmpname); it->type = 1; it->val = xs; }
    int hq = tile_quantiser(scale) <= 15;

    FILE *fo = fopen(out, "wb"); if (!fo) die("can't write %s", out);
    Buf b = {0};
    size_t o = astart(&b, "ftyp"); bstr(&b, "qt  "); bu32(&b, 0x200); bstr(&b, "qt  "); aend(&b, o);
    uint64_t mdat_start = b.n; bu32(&b, 1); bstr(&b, "mdat"); bu64(&b, 0);
    fwrite(b.p, 1, b.n, fo); uint64_t pos = b.n; b.n = 0;

    uint64_t *voff = malloc(8 * nf), *aoff = malloc(8 * nf); uint32_t *vsz = malloc(4 * nf), *acnt = malloc(4 * nf); int na = 0;
    uint64_t *moff = calloc(nf, 8); uint32_t *msz = calloc(nf, 4); int have_meta = 0;
    uint64_t *goff = calloc(nf, 8);
    uint32_t a_done = 0; uint8_t *abuf = NULL;
    Job j; memset(&j, 0, sizeof j);
    j.files = files; j.nfiles = nf; j.scale = scale; j.have_neutral = have_neutral; j.have_gain = have_gain;
    j.neutral[0] = neutral[0]; j.neutral[1] = neutral[1]; j.gain = gain;
    j.window = jobs * 2; j.slots = calloc(j.window, sizeof(Encoded)); j.ready = calloc(j.window, sizeof(int));
    pthread_mutex_init(&j.mu, NULL); pthread_cond_init(&j.cv, NULL);
    pthread_t *th = malloc(sizeof(pthread_t) * jobs);
    for (int i = 0; i < jobs; i++) pthread_create(&th[i], NULL, worker, &j);
    double t0 = now(); long clamped = 0; int written = 0;
    for (int i = 0; i < nf; i++) {
        pthread_mutex_lock(&j.mu);
        while (!j.ready[i % j.window] && !j.failed) pthread_cond_wait(&j.cv, &j.mu);
        if (!j.ready[i % j.window]) { pthread_mutex_unlock(&j.mu); break; }
        Encoded e = j.slots[i % j.window]; j.ready[i % j.window] = 0; j.done_upto = i + 1;
        pthread_cond_broadcast(&j.cv);
        pthread_mutex_unlock(&j.mu);
        if (!e.data) break;
        voff[i] = pos; vsz[i] = (uint32_t)e.len; fwrite(e.data, 1, e.len, fo); pos += e.len; clamped += e.clamped; free(e.data);
        if (have_gyro) { goff[i] = pos; fwrite(gmsg[i], 1, glen[i], fo); pos += glen[i]; }
        if (g_extra) {                                    /* metadata sample: one item, local key 1 */
            uint8_t h[8]; uint32_t n = (uint32_t)(8 + e.meta_len);
            h[0] = n >> 24; h[1] = n >> 16; h[2] = n >> 8; h[3] = n; h[4] = 0; h[5] = 0; h[6] = 0; h[7] = 1;
            moff[i] = pos; msz[i] = n; fwrite(h, 1, 8, fo); if (e.meta_len) fwrite(e.meta, 1, e.meta_len, fo); pos += n;
            if (e.meta_len) have_meta = 1;
            free(e.meta);
        }
        if (have_wav) {
            uint32_t end = (uint32_t)nearbyint((double)(i + 1) * fps_d * wav.rate / fps_n);   /* Python round(): half to even */ if (end > wav.frames) end = wav.frames;
            if (end > a_done) {
                uint32_t n = end - a_done, bpf = wav.ch * wav.sw; abuf = realloc(abuf, (size_t)n * bpf);
                fseek(wav.fh, wav.data_off + (long)a_done * bpf, SEEK_SET);
                size_t got = fread(abuf, 1, (size_t)n * bpf, wav.fh); n = (uint32_t)(got / bpf);
                if (n) { aoff[na] = pos; acnt[na++] = n; fwrite(abuf, 1, (size_t)n * bpf, fo); pos += (size_t)n * bpf; }
                a_done += n;
            }
        }
        written++;
        if (i % every == 0 || i == nf - 1) {
            double el = now() - t0;
            printf("  frame %d/%d  %.2f MB  (%.2f fps)\n", i + 1, nf, e.len / 1e6, (i + 1) / (el > 0 ? el : 1e-9)); fflush(stdout);
        }
    }
    for (int i = 0; i < jobs; i++) pthread_join(th[i], NULL);
    if (written < nf) { fclose(fo); unlink(out); die("%s", "a frame failed to read or encode; no file written"); }

    uint64_t tc_off = 0;
    if (have_tc) { uint8_t t4[4] = { (uint8_t)(tc_start >> 24), (uint8_t)(tc_start >> 16), (uint8_t)(tc_start >> 8), (uint8_t)tc_start }; tc_off = pos; fwrite(t4, 1, 4, fo); pos += 4; }
    uint64_t mdat_end = pos; fseeko(fo, (off_t)mdat_start + 8, SEEK_SET);
    uint8_t m8[8]; uint64_t mdsz = mdat_end - mdat_start; for (int i = 0; i < 8; i++) m8[i] = (uint8_t)(mdsz >> (56 - 8 * i));
    fwrite(m8, 1, 8, fo); fseeko(fo, (off_t)mdat_end, SEEK_SET);

    /* moov */
    int vts = fps_n, vdur = fps_d; uint32_t mv_dur = (uint32_t)nf * vdur;
    size_t moov = astart(&b, "moov");
    o = astart(&b, "mvhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, vts); bu32(&b, mv_dur); put_fixed16(&b, 1.0);
    int tid_gyro = have_wav ? 3 : 2, tid_tc = tid_gyro + (have_gyro ? 1 : 0), tid_meta = tid_tc + (have_tc ? 1 : 0), next_tid = tid_meta + (g_extra ? 1 : 0);
    bu16(&b, 0x100); bzero_(&b, 10); put_matrix(&b); bzero_(&b, 24); bu32(&b, next_tid); aend(&b, o);
    /* metadata */
    {
        size_t om = astart(&b, "meta");
        size_t oh = astart(&b, "hdlr"); bzero_(&b, 8); bstr(&b, "mdta"); bzero_(&b, 12); aend(&b, oh);
        size_t ok = astart(&b, "keys"); bu32(&b, 0); bu32(&b, meta.n);
        for (int i = 0; i < meta.n; i++) { bu32(&b, 8 + strlen(meta.it[i].key)); bstr(&b, "mdta"); bstr(&b, meta.it[i].key); }
        aend(&b, ok);
        size_t oi = astart(&b, "ilst");
        for (int i = 0; i < meta.n; i++) {
            size_t d = meta.it[i].val.n;
            bu32(&b, 8 + 16 + d); bu32(&b, i + 1); bu32(&b, 16 + d); bstr(&b, "data"); bu32(&b, meta.it[i].type); bu32(&b, 0); bput(&b, meta.it[i].val.p, d);
        }
        aend(&b, oi); aend(&b, om);
    }
    /* video track */
    {
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0xF); bu32(&b, 0); bu32(&b, 0); bu32(&b, 1); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, first.w); put_fixed16(&b, first.h); aend(&b, o);
        if (have_tc) { size_t tf = astart(&b, "tref"); size_t t2 = astart(&b, "tmcd"); bu32(&b, tid_tc); aend(&b, t2); aend(&b, tf); }
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, vts); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "vide", "Video");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "vmhd"); bu32(&b, 1); bu16(&b, 0x40); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0x8000); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Data"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        const char *cname = hq ? "Apple ProRes RAW HQ" : "Apple ProRes RAW";
        size_t en = astart(&b, hq ? "aprh" : "aprn");
        bzero_(&b, 6); bu16(&b, 1); bu16(&b, 0); bu16(&b, 0); bstr(&b, "fpsp"); bu32(&b, 0); bu32(&b, 0x200);
        bu16(&b, first.w); bu16(&b, first.h); put_fixed16(&b, 72); put_fixed16(&b, 72); bu32(&b, 0); bu16(&b, 1);
        bu8(&b, strlen(cname)); bstr(&b, cname); bzero_(&b, 31 - strlen(cname)); bu16(&b, 24); bu16(&b, 0xFFFF);
        o = astart(&b, "fiel"); bu8(&b, 1); bu8(&b, 0); aend(&b, o);
        o = astart(&b, "pasp"); bu32(&b, g_pasp_h); bu32(&b, g_pasp_v); aend(&b, o);   /* --desqueeze: anamorphic */
        aend(&b, en); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, nf); bu32(&b, vdur); aend(&b, o);
        uint32_t *ones = malloc(4 * nf); for (int i = 0; i < nf; i++) ones[i] = 1; put_stsc(&b, ones, nf); free(ones);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 0); bu32(&b, nf); for (int i = 0; i < nf; i++) bu32(&b, vsz[i]); aend(&b, o);
        put_co64(&b, voff, nf);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (have_wav) {
        uint32_t ad = 0; for (int i = 0; i < na; i++) ad += acnt[i];
        int bpf = wav.ch * wav.sw;
        size_t tr = astart(&b, "trak");
        uint32_t a_mvdur = (uint32_t)nearbyint((double)ad * vts / wav.rate);
        o = astart(&b, "tkhd"); bu32(&b, 0xF); bu32(&b, 0); bu32(&b, 0); bu32(&b, 2); bu32(&b, 0); bu32(&b, a_mvdur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 1); bu16(&b, 0x100); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, wav.rate); bu32(&b, ad); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "soun", "Sound");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "smhd"); bu32(&b, 0); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Data"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "lpcm");
        bzero_(&b, 6); bu16(&b, 1); bu16(&b, 2); bu16(&b, 0); bzero_(&b, 4);
        bu16(&b, 3); bu16(&b, 16); bu16(&b, 0xFFFE); bu16(&b, 0); bu32(&b, 65536); bu32(&b, 72);
        bf64(&b, wav.rate); bu32(&b, wav.ch); bu32(&b, 0x7F000000); bu32(&b, wav.sw * 8); bu32(&b, 0x4 | 0x8); bu32(&b, bpf); bu32(&b, 1);
        aend(&b, en); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, ad); bu32(&b, 1); aend(&b, o);
        put_stsc(&b, acnt, na);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, bpf); bu32(&b, ad); aend(&b, o);
        put_co64(&b, aoff, na);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (have_gyro) {                                      /* Gyroflow Protobuf telemetry: one Main per video frame.
                                                             First metadata track in the file: telemetry-parser reads the first one. */
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0x3); bu32(&b, 0); bu32(&b, 0); bu32(&b, tid_gyro); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        size_t tf = astart(&b, "tref"); size_t t2 = astart(&b, "cdsc"); bu32(&b, 1); aend(&b, t2); aend(&b, tf);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, vts); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "meta", "GyroflowTelemetry");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "nmhd"); bu32(&b, 0); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Data"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "gyrf"); bzero_(&b, 6); bu16(&b, 1); aend(&b, en);
        aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, nf); bu32(&b, vdur); aend(&b, o);
        uint32_t *ones = malloc(4 * nf); for (int i = 0; i < nf; i++) ones[i] = 1; put_stsc(&b, ones, nf); free(ones);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 0); bu32(&b, nf); for (int i = 0; i < nf; i++) bu32(&b, (uint32_t)glen[i]); aend(&b, o);
        put_co64(&b, goff, nf);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (have_tc) {                                        /* QuickTime timecode track */
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0x3); bu32(&b, 0); bu32(&b, 0); bu32(&b, tid_tc); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, fps_n); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "tmcd", "Time Code");
        size_t mi = astart(&b, "minf");
        size_t gm = astart(&b, "gmhd");
        o = astart(&b, "gmin"); bu32(&b, 0); bu16(&b, 0x40); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0x8000); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        size_t tm = astart(&b, "tmcd");
        o = astart(&b, "tcmi"); bu32(&b, 0); bu16(&b, 0); bu16(&b, 0); bu16(&b, 12); bu16(&b, 0);
        bu16(&b, 0xFFFF); bu16(&b, 0xFFFF); bu16(&b, 0xFFFF); bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bu8(&b, 9); bstr(&b, "Helvetica"); aend(&b, o);
        aend(&b, tm); aend(&b, gm);
        put_hdlr(&b, "dhlr", "alis", "Data"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "tmcd");
        bzero_(&b, 6); bu16(&b, 1); bu32(&b, 0);
        bu32(&b, (tc_drop ? 0x1 : 0) | 0x2);           /* drop frame, 24-hour max */
        bu32(&b, fps_n); bu32(&b, fps_d); bu8(&b, tc_nom); bu8(&b, 0);
        aend(&b, en); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, 1); bu32(&b, mv_dur); aend(&b, o);
        uint32_t one = 1; put_stsc(&b, &one, 1);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 4); bu32(&b, 1); aend(&b, o);
        put_co64(&b, &tc_off, 1);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    if (g_extra) {                                        /* timed metadata: every frame's DNG tags */
        size_t tr = astart(&b, "trak");
        o = astart(&b, "tkhd"); bu32(&b, 0x3); bu32(&b, 0); bu32(&b, 0); bu32(&b, tid_meta); bu32(&b, 0); bu32(&b, mv_dur); bzero_(&b, 8);
        bu16(&b, 0); bu16(&b, 0); bu16(&b, 0); bzero_(&b, 2); put_matrix(&b); put_fixed16(&b, 0); put_fixed16(&b, 0); aend(&b, o);
        size_t tf = astart(&b, "tref"); size_t t2 = astart(&b, "cdsc"); bu32(&b, 1); aend(&b, t2); aend(&b, tf);
        size_t md = astart(&b, "mdia");
        o = astart(&b, "mdhd"); bu32(&b, 0); bu32(&b, 0); bu32(&b, 0); bu32(&b, vts); bu32(&b, mv_dur); bu16(&b, 0); bu16(&b, 0); aend(&b, o);
        put_hdlr(&b, "mhlr", "meta", "fpSup DNG metadata");
        size_t mi = astart(&b, "minf");
        o = astart(&b, "nmhd"); bu32(&b, 0); aend(&b, o);
        put_hdlr(&b, "dhlr", "alis", "Data"); put_dinf(&b);
        size_t st = astart(&b, "stbl");
        size_t sd = astart(&b, "stsd"); bu32(&b, 0); bu32(&b, 1);
        size_t en = astart(&b, "mebx"); bzero_(&b, 6); bu16(&b, 1);
        size_t ks = astart(&b, "keys");
        size_t k1 = b.n; bu32(&b, 0); bu32(&b, 1);              /* key atom, type = local key id 1 */
        const char *key = "io.github.ijigen.fpsup.dng.frame-tags";
        o = astart(&b, "keyd"); bstr(&b, "mdta"); bstr(&b, key); aend(&b, o);
        o = astart(&b, "dtyp"); bu32(&b, 0); bu32(&b, 0); aend(&b, o);   /* well-known type 0: binary */
        aend(&b, k1);
        aend(&b, ks); aend(&b, en); aend(&b, sd);
        o = astart(&b, "stts"); bu32(&b, 0); bu32(&b, 1); bu32(&b, nf); bu32(&b, vdur); aend(&b, o);
        uint32_t *ones = malloc(4 * nf); for (int i = 0; i < nf; i++) ones[i] = 1; put_stsc(&b, ones, nf); free(ones);
        o = astart(&b, "stsz"); bu32(&b, 0); bu32(&b, 0); bu32(&b, nf); for (int i = 0; i < nf; i++) bu32(&b, msz[i]); aend(&b, o);
        put_co64(&b, moff, nf);
        aend(&b, st); aend(&b, mi); aend(&b, md); aend(&b, tr);
    }
    (void)have_meta;
    aend(&b, moov);
    fwrite(b.p, 1, b.n, fo);
    if (fclose(fo)) die("error writing %s", out);
    struct stat sb; stat(out, &sb);
    printf("wrote %s  (%.1f MB)  dc-clamps: %ld\n", out, sb.st_size / 1e6, clamped);
    return 0;
}
