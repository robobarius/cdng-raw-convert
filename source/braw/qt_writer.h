/*
 * qt_writer.h: byte-buffer and QuickTime atom helpers shared by the dng2* encoders.
 *
 * Part of dng2braw / dng2prr (fpSup community). LGPL-2.1-or-later, see LICENSE.txt.
 * Same helpers as dng2prr 0.2's dng2prr_cli.c ("small helpers" and "QuickTime writer"),
 * header-only so each encoder can include them without a link-time clash.
 */
#ifndef QT_WRITER_H
#define QT_WRITER_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

typedef struct { uint8_t *p; size_t n, cap; } Buf;
static inline void bput(Buf *b, const void *d, size_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 1024; b->p = realloc(b->p, b->cap); if (!b->p) { fprintf(stderr, "out of memory\n"); exit(1); } }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static inline void bu8(Buf *b, unsigned v) { uint8_t c = (uint8_t)v; bput(b, &c, 1); }
static inline void bu16(Buf *b, unsigned v) { uint8_t c[2] = { (uint8_t)(v >> 8), (uint8_t)v }; bput(b, c, 2); }
static inline void bu32(Buf *b, uint32_t v) { uint8_t c[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v }; bput(b, c, 4); }
static inline void bu64(Buf *b, uint64_t v) { bu32(b, (uint32_t)(v >> 32)); bu32(b, (uint32_t)v); }
static inline void bf32(Buf *b, double v) { float f = (float)v; uint32_t u; memcpy(&u, &f, 4); bu32(b, u); }
static inline void bf64(Buf *b, double v) { uint64_t u; memcpy(&u, &v, 8); bu64(b, u); }
static inline void bzero_(Buf *b, size_t n) { static const uint8_t z[256]; while (n) { size_t k = n < 256 ? n : 256; bput(b, z, k); n -= k; } }
static inline void bstr(Buf *b, const char *s) { bput(b, s, strlen(s)); }
static inline void bfree(Buf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static inline void wr16(uint8_t *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static inline unsigned be16(const uint8_t *p) { return p[0] << 8 | p[1]; }

/* atom helpers: start returns the offset of the size field, end patches it */
static inline size_t astart(Buf *b, const char *typ) { size_t o = b->n; bu32(b, 0); bput(b, typ, 4); return o; }
static inline void aend(Buf *b, size_t o) { wr32(b->p + o, (uint32_t)(b->n - o)); }

static inline void put_fixed16(Buf *b, double v) { bu32(b, (uint32_t)(int64_t)nearbyint(v * 65536)); }
static inline void put_matrix(Buf *b) { uint32_t m[9] = { 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000 }; for (int i = 0; i < 9; i++) bu32(b, m[i]); }
static inline void put_hdlr(Buf *b, const char *ct, const char *sub, const char *name) {
    size_t o = astart(b, "hdlr"); bu32(b, 0); bput(b, ct, 4); bput(b, sub, 4);
    if (!strcmp(ct, "mhlr")) bstr(b, "appl"); else bu32(b, 0);
    bzero_(b, 8); bu8(b, (unsigned)strlen(name)); bstr(b, name); aend(b, o);
}
static inline void put_dinf(Buf *b) { size_t o = astart(b, "dinf"), o2 = astart(b, "dref"); bu32(b, 0); bu32(b, 1); size_t o3 = astart(b, "alis"); bu32(b, 1); aend(b, o3); aend(b, o2); aend(b, o); }
static inline void put_co64(Buf *b, const uint64_t *off, int n) { size_t o = astart(b, "co64"); bu32(b, 0); bu32(b, n); for (int i = 0; i < n; i++) bu64(b, off[i]); aend(b, o); }
static inline void put_stsc_ones(Buf *b) { size_t o = astart(b, "stsc"); bu32(b, 0); bu32(b, 1); bu32(b, 1); bu32(b, 1); bu32(b, 1); aend(b, o); }

/* moov/meta key-value items (QuickTime 'mdta' metadata) */
typedef struct { char key[96]; int type; Buf val; } MetaItem;   /* well-known types: 1 UTF-8, 22 binary/uint, 23 f32,
                                                                    71 2xf32 size, 76 u16, 77 u32 */
typedef struct { MetaItem it[96]; int n; } Meta;
static inline MetaItem *meta_new(Meta *m, const char *k, int type) {
    if (m->n >= 96) return NULL;
    MetaItem *i = &m->it[m->n++]; snprintf(i->key, sizeof i->key, "%s", k); i->type = type; memset(&i->val, 0, sizeof(Buf)); return i;
}
static inline void meta_str(Meta *m, const char *k, const char *v) { MetaItem *i = meta_new(m, k, 1); if (i && v) bstr(&i->val, v); }
static inline void meta_f32(Meta *m, const char *k, double v) { MetaItem *i = meta_new(m, k, 23); if (i) bf32(&i->val, v); }
static inline void meta_f32v(Meta *m, const char *k, const double *v, int n) { MetaItem *i = meta_new(m, k, 23); if (i) for (int j = 0; j < n; j++) bf32(&i->val, v[j]); }
static inline void meta_size(Meta *m, const char *k, double a, double c) { MetaItem *i = meta_new(m, k, 71); if (i) { bf32(&i->val, a); bf32(&i->val, c); } }
static inline void meta_u16(Meta *m, const char *k, unsigned v) { MetaItem *i = meta_new(m, k, 76); if (i) bu16(&i->val, v); }
static inline void meta_u32(Meta *m, const char *k, uint32_t v) { MetaItem *i = meta_new(m, k, 77); if (i) bu32(&i->val, v); }
static inline void put_meta(Buf *b, const Meta *meta) {
    size_t om = astart(b, "meta");
    size_t oh = astart(b, "hdlr"); bzero_(b, 8); bstr(b, "mdta"); bzero_(b, 12); aend(b, oh);
    size_t ok = astart(b, "keys"); bu32(b, 0); bu32(b, meta->n);
    for (int i = 0; i < meta->n; i++) { bu32(b, 8 + (uint32_t)strlen(meta->it[i].key)); bstr(b, "mdta"); bstr(b, meta->it[i].key); }
    aend(b, ok);
    size_t oi = astart(b, "ilst");
    for (int i = 0; i < meta->n; i++) {
        size_t d = meta->it[i].val.n;
        bu32(b, (uint32_t)(8 + 16 + d)); bu32(b, i + 1); bu32(b, (uint32_t)(16 + d)); bstr(b, "data"); bu32(b, meta->it[i].type); bu32(b, 0);
        if (d) bput(b, meta->it[i].val.p, d);
    }
    aend(b, oi); aend(b, om);
}
#endif
