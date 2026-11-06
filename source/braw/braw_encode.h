/*
 * braw_encode.h: dng2braw's Blackmagic RAW slice encoder (5:1 profile of a Video Assist 12G + SIGMA fp).
 * Everything here was worked out by black-box decoding of our own recordings with Blackmagic's public
 * SDK API (docs/FINDINGS.md sections 21-30). Part of dng2braw (fpSup community). LGPL-2.1-or-later.
 * NOT Blackmagic software.
 */
#ifndef BRAW_ENCODE_H
#define BRAW_ENCODE_H
#include <stdint.h>
#include "qt_writer.h"

/* the decoded-buffer planes: Y W x H (u16), Cb and Cr W/2 x H/2 (i16), as the SDK's decoder produces them */
typedef struct { int W, H; uint16_t *Y; int16_t *Cb, *Cr; double *Ypre; } BrawPlanes;

extern double braw_raw_gain[3];             /* R, G, B gains on (photosite - black), FINDINGS 38 */
/* Forward transform (FINDINGS 22): coded CFA frame (RGGB at even/even, W x H) -> planes. black: DNG black level. */
int braw_planes_from_cfa(const uint16_t *cfa, int W, int H, double black, BrawPlanes *P);
/* Luma pre-filter (FINDINGS 30): inverse of the decoder's (3,10,3)/16 block-edge filter. Fills P->Ypre. */
int braw_prefilter(BrawPlanes *P);
void braw_planes_free(BrawPlanes *P);
/* Quantisation matrix (FINDINGS 32): header 0x1080-0x113f, 96 x u16 in raster order, luma 8x8 then chroma 4x8.
   Step = entry * 3/8 (luma AC), entry * 3/16 (chroma). Luma DC is fixed at 99/64 per code value of the block mean;
   the Video Assist writes 33 everywhere. */
typedef struct { uint16_t q[96]; } BrawQuant;
void braw_quant_scaled(BrawQuant *Q, double scale);       /* 33 * scale on all AC entries; DC entries stay 33 */

/* Per-frame DCT coefficients of the pre-filtered luma and the chroma, computed once (for rate control). */
typedef struct { int W, H, bw, bh; float *Y, *C[2]; int external; } BrawCoefs;   /* external: buffers owned by the GPU path */
int braw_coefs(const BrawPlanes *P, BrawCoefs *K);
void braw_coefs_free(BrawCoefs *K);
/* One slice (x0, y0, w, h in luma pixels) -> bitstream, padded to 64 bits. */
int braw_encode_slice_coefs(const BrawCoefs *K, const BrawQuant *Q, int x0, int y0, int w, int h, Buf *out);
/* All slices of a frame, in parallel: geo = n x (x0, y0, w, h); outs[i] receive the bits. Returns total bytes. */
size_t braw_encode_slices(const BrawCoefs *K, const BrawQuant *Q, int n, const int *geo, Buf *outs);
/* Same, counting bytes only (rate-control passes). */
#define BRAW_MAX_SLICES 512
size_t braw_count_slices(const BrawCoefs *K, const BrawQuant *Q, int n, const int *geo);
size_t braw_slice_size(const BrawCoefs *K, const BrawQuant *Q, int x0, int y0, int w, int h);
void braw_par_for(int n, void (*fn)(void *, int), void *ctx);
int braw_threads(void);
void braw_ac_lengths(uint8_t len[256]);
size_t braw_slice_fixed_bits(const BrawCoefs *K, int x0, int y0, int w, int h);
void braw_set_rounding(double r);
void braw_set_edge_strength(int v);        /* decoder edge filter strength written per edge, 0-3 (default 2) */
int braw_edge_strength(void);
void braw_set_chroma_factor(double f);     /* chroma AC matrix = luma x f (default 1) */          /* AC rounding offset, 0.5 = round to nearest (FINDINGS 36) */
void braw_recip_steps(const BrawQuant *Q, float ys[64], float cs[32]);
void braw_scan_tables(int zz8[64][2], int zzc[32][2]);   /* zigzag (luma) and 8x4 scan (chroma) */                     /* worker threads (CPU count, or DNG2BRAW_THREADS) */
/* Convenience: flat 33 matrix straight from planes (the prototype's behaviour). */
int braw_encode_slice_planes(const BrawPlanes *P, int x0, int y0, int w, int h, Buf *out);

/* The 5:1 frame-header words 0x08-0x23, as a Video Assist writes them (observed, FINDINGS 26/30). */
extern const uint8_t braw_hdr_5to1[0x24 - 0x08];
#endif
