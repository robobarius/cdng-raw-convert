/*
 * braw_metal.m: Metal (Apple GPU) back end for dng2braw's transform stages. See braw_metal.h.
 * Kernels are compiled at run time from source (no Xcode needed). Buffers are MTLStorageModeShared: on Apple silicon
 * the CPU and GPU see the same memory, so the CFA frame is written in place and the coefficients are read in place.
 * Arithmetic is float32 (Metal has no double); the CPU path is double. Both round to the same integer planes except
 * for rare ties. Part of dng2braw (fpSup community). LGPL-2.1-or-later. Built with -fno-objc-arc.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include "braw_metal.h"

static NSString *const kSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct P { uint W, H; float black; uint pad; float g0, g1, g2, g3; };\n"
"inline int site(int y, int x) { return ((y & 1) == 0 && (x & 1) == 0) ? 0 : ((y & 1) && (x & 1)) ? 2 : 1; }\n"
/* T(v) = 55.94 sqrt(v) + 71.2 (FINDINGS 22) */
"kernel void k_T(device const ushort *cfa [[buffer(0)]], device float *T [[buffer(1)]], constant P &p [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p.W || g.y >= p.H) return; uint i = g.y * p.W + g.x; int c = site(int(g.y), int(g.x));\n"
"  float v = (float(cfa[i]) - p.black) * (c == 0 ? p.g0 : c == 1 ? p.g1 : p.g2);\n"
"  T[i] = 55.94f * sqrt(max(v, 0.0f)) + 71.2f; }\n"
/* Y = T - T_own + L with same-colour 5x5 averages, edge-replicated window; written as the integer plane, pre-filter input */
"kernel void k_Y(device const float *T [[buffer(0)]], device float *Y [[buffer(1)]], constant P &p [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {\n"
"  int W = p.W, H = p.H, x = g.x, y = g.y; if (x >= W || y >= H) return;\n"
"  float a0 = 0, a1 = 0, a2 = 0; int n0 = 0, n1 = 0, n2 = 0;\n"
"  for (int dy = -2; dy <= 2; dy++) { int yy = clamp(y + dy, 0, H - 1);\n"
"    for (int dx = -2; dx <= 2; dx++) { int xx = clamp(x + dx, 0, W - 1); float t = T[yy * W + xx]; int c = site(yy, xx);\n"
"      if (c == 0) { a0 += t; n0++; } else if (c == 1) { a1 += t; n1++; } else { a2 += t; n2++; } } }\n"
"  float t0 = a0 / n0, t1 = a1 / n1, t2 = a2 / n2; int s = site(y, x); float own = s == 0 ? t0 : s == 1 ? t1 : t2;\n"
"  float v = T[y * W + x] - own + (0.29f * t0 + 0.571f * t1 + 0.293f * t2 - 12.7f);\n"
"  Y[y * W + x] = clamp(rint(min(v, 4095.0f)), 0.0f, 65535.0f); }\n"
"kernel void k_C(device const float *T [[buffer(0)]], device float *Cb [[buffer(1)]], device float *Cr [[buffer(2)]], constant P &p [[buffer(3)]], uint2 g [[thread_position_in_grid]]) {\n"
"  uint W = p.W; if (g.x >= W / 2 || g.y >= p.H / 2) return; uint a = (2 * g.y) * W + 2 * g.x;\n"
"  float R = T[a], G = (T[a + 1] + T[a + W]) * 0.5f, B = T[a + W + 1]; uint o = g.y * (W / 2) + g.x;\n"
"  Cb[o] = clamp(rint(0.563f * (B - G)), -32768.0f, 32767.0f); Cr[o] = clamp(rint(0.574f * (R - G)), -32768.0f, 32767.0f); }\n"
/* inverse of the decoder's (3,10,3)/16 edge filter (FINDINGS 30): boundary pair (b-1, b), b = 8, 16, ... */
"kernel void k_uh(device float *Y [[buffer(0)]], constant P &p [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
"  int W = p.W, b = 8 * (int(g.x) + 1); if (int(g.y) >= int(p.H) || b + 1 >= W) return; device float *r = Y + g.y * W;\n"
"  float v = float(p.pad), m = 16 - 2 * v, det = m * m - v * v; if (p.pad == 0) return;\n"
"  float A = 16 * r[b - 1] - v * r[b - 2], B = 16 * r[b] - v * r[b + 1]; r[b - 1] = (m * A - v * B) / det; r[b] = (m * B - v * A) / det; }\n"
"kernel void k_uv(device float *Y [[buffer(0)]], constant P &p [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
"  int W = p.W, H = p.H, b = 8 * (int(g.y) + 1), x = g.x; if (x >= W || b + 1 >= H) return;\n"
"  float v = float(p.pad), m = 16 - 2 * v, det = m * m - v * v; if (p.pad == 0) return;\n"
"  float A = 16 * Y[(b - 1) * W + x] - v * Y[(b - 2) * W + x], B = 16 * Y[b * W + x] - v * Y[(b + 1) * W + x];\n"
"  Y[(b - 1) * W + x] = (m * A - v * B) / det; Y[b * W + x] = (m * B - v * A) / det; }\n"
/* 8x8 luma DCT (orthonormal), mean in [0], zigzag AC after */
"kernel void k_dy(device const float *Y [[buffer(0)]], device float *o [[buffer(1)]], constant float *C8 [[buffer(2)]], constant int2 *zz [[buffer(3)]], constant P &p [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {\n"
"  uint bw = p.W / 8; if (g.x >= bw || g.y >= p.H / 8) return; float b[64], t[64], m = 0;\n"
"  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { float v = Y[(g.y * 8 + y) * p.W + g.x * 8 + x]; b[y * 8 + x] = v; m += v; }\n"
"  for (int k = 0; k < 8; k++) for (int x = 0; x < 8; x++) { float s = 0; for (int y = 0; y < 8; y++) s += C8[k * 8 + y] * b[y * 8 + x]; t[k * 8 + x] = s; }\n"
"  device float *d = o + (g.y * bw + g.x) * 64; d[0] = m / 64;\n"
"  for (int i = 1; i < 64; i++) { int k = zz[i].x, l = zz[i].y; float s = 0; for (int x = 0; x < 8; x++) s += t[k * 8 + x] * C8[l * 8 + x]; d[i] = s; } }\n"
/* 4x8 chroma DCT */
"kernel void k_dc(device const float *C [[buffer(0)]], device float *o [[buffer(1)]], constant float *C8 [[buffer(2)]], constant float *C4 [[buffer(3)]], constant int2 *zz [[buffer(4)]], constant P &p [[buffer(5)]], uint2 g [[thread_position_in_grid]]) {\n"
"  uint CW = p.W / 2, bw = p.W / 16; if (g.x >= bw || g.y >= p.H / 8) return; float b[32], t[32], m = 0;\n"
"  for (int y = 0; y < 4; y++) for (int x = 0; x < 8; x++) { float v = C[(g.y * 4 + y) * CW + g.x * 8 + x]; b[y * 8 + x] = v; m += v; }\n"
"  for (int k = 0; k < 4; k++) for (int x = 0; x < 8; x++) { float s = 0; for (int y = 0; y < 4; y++) s += C4[k * 4 + y] * b[y * 8 + x]; t[k * 8 + x] = s; }\n"
"  device float *d = o + (g.y * bw + g.x) * 32; d[0] = m / 32;\n"
"  for (int i = 1; i < 32; i++) { int k = zz[i].x, l = zz[i].y; float s = 0; for (int x = 0; x < 8; x++) s += t[k * 8 + x] * C8[l * 8 + x]; d[i] = s; } }\n"
/* AC bits of one block for given reciprocal steps (rate-control pass; FINDINGS 35) */
"inline uint acbits(device const float *o, int n, constant float *rs, constant uchar *len) {\n"
"  uint bits = 0; int run = 0;\n"
"  for (int i = 1; i < n; i++) { float x = o[i] * rs[i]; int v = int(copysign(trunc(fabs(x) + rs[0]), x)); v = clamp(v, -1023, 1023);\n"
"    if (v == 0) { run++; continue; }\n"
"    while (run > 15) { bits += len[0xF0]; run -= 16; }\n"
"    int a = abs(v), sz = 32 - clz(uint(a)); bits += len[(run << 4) | sz] + sz; run = 0; }\n"
"  return bits + len[0]; }\n"
"kernel void k_by(device const float *o [[buffer(0)]], device uint *out [[buffer(1)]], constant float *rs [[buffer(2)]], constant uchar *len [[buffer(3)]], constant uint &nb [[buffer(4)]], uint i [[thread_position_in_grid]]) {\n"
"  if (i < nb) out[i] = acbits(o + i * 64, 64, rs, len); }\n"
"kernel void k_bc(device const float *o [[buffer(0)]], device uint *out [[buffer(1)]], constant float *rs [[buffer(2)]], constant uchar *len [[buffer(3)]], constant uint &nb [[buffer(4)]], uint i [[thread_position_in_grid]]) {\n"
"  if (i < nb) out[i] = acbits(o + i * 32, 32, rs, len); }\n";

typedef struct { uint32_t W, H; float black; uint32_t pad; float g0, g1, g2, g3; } Params;
typedef struct { id<MTLBuffer> cfa, T, Y, Cb, Cr, cy, cc0, cc1, prm; id<MTLCommandBuffer> cb; } Slot;
struct BrawGpu {
    id<MTLDevice> dev; id<MTLCommandQueue> q;
    id<MTLComputePipelineState> pT, pY, pC, pH, pV, pDY, pDC, pBY, pBC;
    id<MTLBuffer> c8, c4, zz8, zzc, alen, rsy, rsc, by, bc0, bc1;
    int W, H; Slot s[BRAW_GPU_SLOTS]; char name[128];
};

static id<MTLComputePipelineState> mk_pipe(id<MTLDevice> d, id<MTLLibrary> lib, const char *fn, char *err, size_t errlen) {
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
    if (!f) { snprintf(err, errlen, "Metal: no function %s", fn); return nil; }
    id<MTLComputePipelineState> p = [d newComputePipelineStateWithFunction:f error:&e]; [f release];
    if (!p) snprintf(err, errlen, "Metal pipeline %s: %s", fn, e ? [[e localizedDescription] UTF8String] : "?");
    return p;
}

BrawGpu *braw_gpu_create(int W, int H, char *err, size_t errlen) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { snprintf(err, errlen, "no Metal device"); return NULL; }
        MTLCompileOptions *opt = [[MTLCompileOptions new] autorelease];
        if (@available(macOS 15.0, *)) opt.mathMode = MTLMathModeSafe;      /* IEEE sqrt/division/rint, closer to the CPU path */
        else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
        }
        NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:opt error:&e];
        if (!lib) { snprintf(err, errlen, "Metal compile: %s", e ? [[e localizedDescription] UTF8String] : "?"); [dev release]; return NULL; }
        BrawGpu *g = calloc(1, sizeof *g); g->dev = dev; g->q = [dev newCommandQueue]; g->W = W; g->H = H;
        snprintf(g->name, sizeof g->name, "%s", [[dev name] UTF8String]);
        if (!(g->pT = mk_pipe(dev, lib, "k_T", err, errlen)) || !(g->pY = mk_pipe(dev, lib, "k_Y", err, errlen)) || !(g->pC = mk_pipe(dev, lib, "k_C", err, errlen)) ||
            !(g->pH = mk_pipe(dev, lib, "k_uh", err, errlen)) || !(g->pV = mk_pipe(dev, lib, "k_uv", err, errlen)) ||
            !(g->pDY = mk_pipe(dev, lib, "k_dy", err, errlen)) || !(g->pDC = mk_pipe(dev, lib, "k_dc", err, errlen)) ||
            !(g->pBY = mk_pipe(dev, lib, "k_by", err, errlen)) || !(g->pBC = mk_pipe(dev, lib, "k_bc", err, errlen))) { [lib release]; free(g); return NULL; }
        [lib release];
        float c8[64], c4[16]; int z8[64][2], zc[32][2];
        for (int k = 0; k < 8; k++) for (int x = 0; x < 8; x++) c8[k * 8 + x] = (float)(sqrt((k ? 2.0 : 1.0) / 8) * cos(M_PI * (2 * x + 1) * k / 16));
        for (int k = 0; k < 4; k++) for (int x = 0; x < 4; x++) c4[k * 4 + x] = (float)(sqrt((k ? 2.0 : 1.0) / 4) * cos(M_PI * (2 * x + 1) * k / 8));
        braw_scan_tables(z8, zc);
        MTLResourceOptions sh = MTLResourceStorageModeShared;
        g->c8 = [dev newBufferWithBytes:c8 length:sizeof c8 options:sh]; g->c4 = [dev newBufferWithBytes:c4 length:sizeof c4 options:sh];
        g->zz8 = [dev newBufferWithBytes:z8 length:sizeof z8 options:sh]; g->zzc = [dev newBufferWithBytes:zc length:sizeof zc options:sh];
        size_t n = (size_t)W * H, nb = (size_t)(W / 8) * (H / 8);
        uint8_t al[256]; braw_ac_lengths(al);
        g->alen = [dev newBufferWithBytes:al length:256 options:sh];
        g->rsy = [dev newBufferWithLength:64 * 4 options:sh]; g->rsc = [dev newBufferWithLength:32 * 4 options:sh];
        g->by = [dev newBufferWithLength:nb * 4 options:sh]; g->bc0 = [dev newBufferWithLength:nb / 2 * 4 options:sh]; g->bc1 = [dev newBufferWithLength:nb / 2 * 4 options:sh];
        for (int i = 0; i < BRAW_GPU_SLOTS; i++) {
            Slot *s = &g->s[i];
            s->cfa = [dev newBufferWithLength:n * 2 options:sh]; s->T = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModePrivate];
            s->Y = [dev newBufferWithLength:n * 4 options:MTLResourceStorageModePrivate];
            s->Cb = [dev newBufferWithLength:n / 4 * 4 options:MTLResourceStorageModePrivate]; s->Cr = [dev newBufferWithLength:n / 4 * 4 options:MTLResourceStorageModePrivate];
            s->cy = [dev newBufferWithLength:nb * 64 * 4 options:sh]; s->cc0 = [dev newBufferWithLength:nb / 2 * 32 * 4 options:sh]; s->cc1 = [dev newBufferWithLength:nb / 2 * 32 * 4 options:sh];
            s->prm = [dev newBufferWithLength:sizeof(Params) options:sh];
            if (!s->cfa || !s->T || !s->Y || !s->Cb || !s->Cr || !s->cy || !s->cc0 || !s->cc1) { snprintf(err, errlen, "Metal: out of memory"); free(g); return NULL; }
        }
        return g;
    }
}
const char *braw_gpu_name(BrawGpu *g) { return g->name; }
uint16_t *braw_gpu_cfa(BrawGpu *g, int slot) { return (uint16_t *)[g->s[slot].cfa contents]; }

static void disp(id<MTLComputeCommandEncoder> e, id<MTLComputePipelineState> p, NSUInteger w, NSUInteger h) {
    [e setComputePipelineState:p];
    NSUInteger tw = p.threadExecutionWidth, th = p.maxTotalThreadsPerThreadgroup / tw; if (th > 8) th = 8;
    [e dispatchThreads:MTLSizeMake(w, h, 1) threadsPerThreadgroup:MTLSizeMake(tw, th, 1)];
}
void braw_gpu_submit(BrawGpu *g, int slot, double black) {
    @autoreleasepool {
        Slot *s = &g->s[slot]; int W = g->W, H = g->H;
        Params *pp = (Params *)[s->prm contents]; pp->W = W; pp->H = H; pp->black = (float)black; pp->pad = (uint32_t)braw_edge_strength();
        pp->g0 = (float)braw_raw_gain[0]; pp->g1 = (float)braw_raw_gain[1]; pp->g2 = (float)braw_raw_gain[2]; pp->g3 = 0;
        id<MTLCommandBuffer> cb = [g->q commandBuffer];
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];   /* serial dispatch: each stage sees the previous one */
        [e setBuffer:s->cfa offset:0 atIndex:0]; [e setBuffer:s->T offset:0 atIndex:1]; [e setBuffer:s->prm offset:0 atIndex:2]; disp(e, g->pT, W, H);
        [e setBuffer:s->T offset:0 atIndex:0]; [e setBuffer:s->Y offset:0 atIndex:1]; [e setBuffer:s->prm offset:0 atIndex:2]; disp(e, g->pY, W, H);
        [e setBuffer:s->T offset:0 atIndex:0]; [e setBuffer:s->Cb offset:0 atIndex:1]; [e setBuffer:s->Cr offset:0 atIndex:2]; [e setBuffer:s->prm offset:0 atIndex:3]; disp(e, g->pC, W / 2, H / 2);
        [e setBuffer:s->Y offset:0 atIndex:0]; [e setBuffer:s->prm offset:0 atIndex:1]; disp(e, g->pH, (W - 2) / 8, H);
        [e setBuffer:s->Y offset:0 atIndex:0]; [e setBuffer:s->prm offset:0 atIndex:1]; disp(e, g->pV, W, (H - 2) / 8);
        [e setBuffer:s->Y offset:0 atIndex:0]; [e setBuffer:s->cy offset:0 atIndex:1]; [e setBuffer:g->c8 offset:0 atIndex:2]; [e setBuffer:g->zz8 offset:0 atIndex:3]; [e setBuffer:s->prm offset:0 atIndex:4]; disp(e, g->pDY, W / 8, H / 8);
        for (int pl = 0; pl < 2; pl++) {
            [e setBuffer:(pl ? s->Cr : s->Cb) offset:0 atIndex:0]; [e setBuffer:(pl ? s->cc1 : s->cc0) offset:0 atIndex:1]; [e setBuffer:g->c8 offset:0 atIndex:2];
            [e setBuffer:g->c4 offset:0 atIndex:3]; [e setBuffer:g->zzc offset:0 atIndex:4]; [e setBuffer:s->prm offset:0 atIndex:5]; disp(e, g->pDC, W / 16, H / 8);
        }
        [e endEncoding]; [cb commit];
        s->cb = [cb retain];
    }
}
int braw_gpu_wait(BrawGpu *g, int slot, BrawCoefs *K) {
    Slot *s = &g->s[slot]; if (!s->cb) return -1;
    [s->cb waitUntilCompleted]; int bad = [s->cb status] != MTLCommandBufferStatusCompleted; [s->cb release]; s->cb = nil;
    if (bad) return -1;
    K->W = g->W; K->H = g->H; K->bw = g->W / 8; K->bh = g->H / 8; K->external = 1;
    K->Y = (float *)[s->cy contents]; K->C[0] = (float *)[s->cc0 contents]; K->C[1] = (float *)[s->cc1 contents];
    return 0;
}
/* AC bits per block for the slot's coefficients (must be waited already): luma blocks raster, then Cb, Cr */
int braw_gpu_acbits(BrawGpu *g, int slot, const float ys[64], const float cs[32], const uint32_t **yb, const uint32_t **cb0, const uint32_t **cb1) {
    @autoreleasepool {
        Slot *s = &g->s[slot]; uint32_t nb = (uint32_t)((g->W / 8) * (g->H / 8)), nc = nb / 2;
        memcpy([g->rsy contents], ys, 64 * 4); memcpy([g->rsc contents], cs, 32 * 4);
        id<MTLCommandBuffer> c = [g->q commandBuffer]; id<MTLComputeCommandEncoder> e = [c computeCommandEncoderWithDispatchType:MTLDispatchTypeConcurrent];
        NSUInteger tw = g->pBY.threadExecutionWidth * 4;
        [e setComputePipelineState:g->pBY]; [e setBuffer:s->cy offset:0 atIndex:0]; [e setBuffer:g->by offset:0 atIndex:1]; [e setBuffer:g->rsy offset:0 atIndex:2];
        [e setBuffer:g->alen offset:0 atIndex:3]; [e setBytes:&nb length:4 atIndex:4]; [e dispatchThreads:MTLSizeMake(nb, 1, 1) threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        [e setComputePipelineState:g->pBC];
        for (int pl = 0; pl < 2; pl++) {
            [e setBuffer:(pl ? s->cc1 : s->cc0) offset:0 atIndex:0]; [e setBuffer:(pl ? g->bc1 : g->bc0) offset:0 atIndex:1]; [e setBuffer:g->rsc offset:0 atIndex:2];
            [e setBuffer:g->alen offset:0 atIndex:3]; [e setBytes:&nc length:4 atIndex:4]; [e dispatchThreads:MTLSizeMake(nc, 1, 1) threadsPerThreadgroup:MTLSizeMake(tw, 1, 1)];
        }
        [e endEncoding]; [c commit]; [c waitUntilCompleted];
        if ([c status] != MTLCommandBufferStatusCompleted) return -1;
        *yb = (const uint32_t *)[g->by contents]; *cb0 = (const uint32_t *)[g->bc0 contents]; *cb1 = (const uint32_t *)[g->bc1 contents];
        return 0;
    }
}
void braw_gpu_destroy(BrawGpu *g) { if (g) free(g); }   /* process exit reclaims the Metal objects */
