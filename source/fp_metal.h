/*
 * fp_metal.h: Apple GPU (Metal) back end for dng2prr's per-frame transform stages, for all three of its outputs:
 *   ProRes RAW         coded-domain curve + 8x8 DCT + quantisation of every Bayer-site block;
 *   lossy CinemaDNG    8x8 DCT of every tile (once per frame) and quantisation (once per rate-control step);
 *   lossless CinemaDNG predictor residuals and their size categories.
 * The entropy coding (adaptive ProRes RAW codebooks, JPEG Huffman) stays on the CPU, where it runs on every core.
 * Each encoder thread has its own slot of unified-memory buffers, so the GPU's results are read in place.
 * macOS only; dng2prr falls back to the CPU path when Metal is unavailable (or with --cpu).
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later.
 */
#ifndef FP_METAL_H
#define FP_METAL_H
#include <stdint.h>
#include <stddef.h>

typedef struct FpGpu FpGpu;
FpGpu *fpgpu_create(int slots, char *err, size_t errlen);   /* NULL: no usable Metal device */
const char *fpgpu_name(FpGpu *g);
void fpgpu_destroy(FpGpu *g);

/* ProRes RAW: raw (w x h) -> coded curve -> padded W x H (multiples of 16) -> quantised DCT blocks.
   Returns a pointer into the slot: int16 coefficients [comp 0..3][by < H/16][bx < W/16][64], raster order. */
const int16_t *fpgpu_prr(FpGpu *g, int slot, const uint16_t *raw, int w, int h, float black, float k, const int16_t qm[64]);

/* Lossy CinemaDNG: DCT of every TW x TH tile of `codes` (w x h, edge tiles padded as the CPU does).
   Returns float coefficients in dng_jpeg12_dct's layout, tile after tile (nblk = (TW/8)*(TH/16)*2 blocks each). */
const float *fpgpu_jpeg_dct(FpGpu *g, int slot, const uint16_t *codes, int w, int h, int TW, int TH);
/* ... then quantise them (all tiles of the last fpgpu_jpeg_dct call) with qt (natural order): int16 zig-zag. */
const int16_t *fpgpu_jpeg_quant(FpGpu *g, int slot, const uint16_t qt[64]);

/* ... or only estimate the bytes all tiles would take at qt (symbol statistics on the GPU), for rate control. */
double fpgpu_jpeg_estimate(FpGpu *g, int slot, const uint16_t qt[64]);

/* Lossless CinemaDNG: per tile, each sample's predictor-1 residual (int16, modulo 2^16) and size category (0..16).
   Laid out tile after tile, TW x TH each. */
int fpgpu_lj92(FpGpu *g, int slot, const uint16_t *codes, int w, int h, int TW, int TH, int bits, const int16_t **d, const uint8_t **s);
#endif
