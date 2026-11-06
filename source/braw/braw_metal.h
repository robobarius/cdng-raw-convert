/*
 * braw_metal.h: Apple GPU (Metal) back end for dng2braw's per-frame transform stages: forward transform (FINDINGS 22),
 * luma pre-filter (30) and the luma/chroma DCT. Results land in unified-memory buffers that the CPU entropy coder reads
 * directly (no copies). Two slots let the GPU prepare one frame while the CPU codes the previous one.
 * macOS only; everything degrades to the CPU path when Metal is unavailable.
 * Part of dng2braw (fpSup community). LGPL-2.1-or-later.
 */
#ifndef BRAW_METAL_H
#define BRAW_METAL_H
#include <stdint.h>
#include <stddef.h>
#include "braw_encode.h"
#define BRAW_GPU_SLOTS 2
typedef struct BrawGpu BrawGpu;
BrawGpu *braw_gpu_create(int W, int H, char *err, size_t errlen);      /* NULL: no usable Metal device */
const char *braw_gpu_name(BrawGpu *g);
uint16_t *braw_gpu_cfa(BrawGpu *g, int slot);                          /* fill with the coded CFA frame (W x H) */
void braw_gpu_submit(BrawGpu *g, int slot, double black);              /* asynchronous */
int braw_gpu_wait(BrawGpu *g, int slot, BrawCoefs *K);                 /* 0 = ok; K points into the slot's buffers */
/* rate-control pass on the GPU: AC bits per block (luma raster, Cb, Cr) for reciprocal steps by scan position */
int braw_gpu_acbits(BrawGpu *g, int slot, const float ys[64], const float cs[32], const uint32_t **yb, const uint32_t **cb0, const uint32_t **cb1);
void braw_gpu_destroy(BrawGpu *g);
#endif
