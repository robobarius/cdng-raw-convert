/*
 * fp_metal.m: Metal (Apple GPU) back end for dng2prr. See fp_metal.h.
 * Kernels are compiled at run time from source (no Xcode needed). Buffers are MTLStorageModeShared, so on Apple
 * silicon the CPU reads the GPU's results in place. Arithmetic is float32 (the CPU path is double); both give the
 * same integers apart from rare rounding ties, and either way the files are valid.
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later. Built with -fno-objc-arc.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "fp_metal.h"

static NSString *const kSrc = @
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct PP { uint w, h, W, H; float black, k; uint TW, TH; uint nt, across, bits, pad; };\n"
"constant float LIN[9] = { 0, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536 };\n"
"constant uchar ZZ[64] = { 0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,\n"
"  35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63 };\n"
/* ProRes RAW: coded curve (dng2prr's interp over LIN), edge-padded to W x H */
"kernel void k_prr_u(device const ushort *raw [[buffer(0)]], device ushort *u [[buffer(1)]], constant PP &p [[buffer(2)]], uint2 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p.W || g.y >= p.H) return; int x = g.x, y = g.y, w = p.w, h = p.h;\n"
"  if (x >= w) x = w - 2 + ((x - w) & 1); if (y >= h) y = h - 2 + ((y - h) & 1);\n"
"  float lin = clamp(256.0f + (float(raw[y * w + x]) - p.black) * p.k, 0.0f, 65535.0f), v;\n"
"  if (lin <= 0) v = 0; else { int j = 0; while (j < 7 && lin >= LIN[j + 1]) j++; v = 8192.0f / (LIN[j + 1] - LIN[j]) * (lin - LIN[j]) + 8192.0f * j; }\n"
"  u[g.y * p.W + g.x] = ushort(clamp(rint(v), 0.0f, 65535.0f)); }\n"
/* ProRes RAW: one 8x8 block of one Bayer site; orthonormal DCT / 8, quantised (round half away from zero) */
"kernel void k_prr_dct(device const ushort *u [[buffer(0)]], device short *o [[buffer(1)]], constant float *C [[buffer(2)]], constant short *qm [[buffer(3)]], constant PP &p [[buffer(4)]], uint3 g [[thread_position_in_grid]]) {\n"
"  uint BX = p.W / 16, BY = p.H / 16; if (g.x >= BX || g.y >= BY || g.z >= 4) return;\n"
"  int dy = g.z >> 1, dx = g.z & 1; float b[64], t[64];\n"
"  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) b[y * 8 + x] = float(u[(g.y * 16 + 2 * y + dy) * p.W + g.x * 16 + 2 * x + dx]) - 32768.0f;\n"
"  for (int y = 0; y < 8; y++) for (int k = 0; k < 8; k++) { float s = 0; for (int x = 0; x < 8; x++) s += C[k * 8 + x] * b[y * 8 + x]; t[y * 8 + k] = s; }\n"
"  device short *d = o + ((g.z * BY + g.y) * BX + g.x) * 64;\n"
"  for (int v = 0; v < 8; v++) for (int k = 0; k < 8; k++) { float s = 0; for (int y = 0; y < 8; y++) s += C[v * 8 + y] * t[y * 8 + k];\n"
"    float q = round(s / 8.0f / float(qm[v * 8 + k])); d[v * 8 + k] = short(clamp(q, -32768.0f, 32767.0f)); } }\n"
/* tile sample (tile t, row ty, column tx) with the CPU's edge padding */
"inline ushort tsample(device const ushort *c, constant PP &p, uint t, int ty, int tx) {\n"
"  int sy = int(t / p.across) * int(p.TH) + ty, sx = int(t % p.across) * int(p.TW) + tx, w = p.w, h = p.h;\n"
"  if (sy >= h) sy = h - 2 + ((sy - h) & 1); if (sx >= w) sx = w - 2 + ((sx - w) & 1); return c[sy * w + sx]; }\n"
/* lossy CinemaDNG: JPEG W = TW, H = TH/2, two components interleaved in DNG sample order; orthonormal DCT */
"kernel void k_jdct(device const ushort *c [[buffer(0)]], device float *o [[buffer(1)]], constant float *C [[buffer(2)]], constant PP &p [[buffer(3)]], uint3 g [[thread_position_in_grid]]) {\n"
"  uint W = p.TW, bw = W / 8, bh = p.TH / 16, nblk = bw * bh * 2; if (g.x >= bw * 2 || g.y >= bh || g.z >= p.nt) return;\n"
"  uint bx = g.x >> 1, cc = g.x & 1, by = g.y; float b[64], t[64];\n"
"  for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) { uint jy = by * 8 + y, jx = bx * 8 + x, idx = 2 * (jy * W + jx) + cc;\n"
"    b[8 * y + x] = float(tsample(c, p, g.z, int(idx / p.TW), int(idx % p.TW))) - 2048.0f; }\n"
"  for (int y = 0; y < 8; y++) for (int k = 0; k < 8; k++) { float s = 0; for (int x = 0; x < 8; x++) s += C[k * 8 + x] * b[8 * y + x]; t[8 * y + k] = s; }\n"
"  device float *d = o + (g.z * nblk + (by * bw + bx) * 2 + cc) * 64;\n"
"  for (int k = 0; k < 8; k++) for (int v = 0; v < 8; v++) { float s = 0; for (int y = 0; y < 8; y++) s += C[v * 8 + y] * t[8 * y + k]; d[8 * v + k] = s; } }\n"
"kernel void k_jq(device const float *cf [[buffer(0)]], device short *q [[buffer(1)]], constant ushort *qt [[buffer(2)]], constant uint &n [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
"  if (i >= n) return; uint b = i >> 6, z = i & 63, nn = ZZ[z];\n"
"  float r = round(cf[b * 64 + nn] / float(qt[nn])); q[i] = short(clamp(r, -16383.0f, 16383.0f)); }\n"
/* lossy CinemaDNG rate control: per-tile symbol statistics at a candidate quantiser, without writing anything */
"inline int qz(device const float *cf, uint b, uint z, constant ushort *qt) { uint nn = ZZ[z]; return int(clamp(round(cf[b * 64 + nn] / float(qt[nn])), -16383.0f, 16383.0f)); }\n"
"inline uint nb(int v) { uint a = uint(abs(v)); return a ? 32 - clz(a) : 0; }\n"
"kernel void k_jst(device const float *cf [[buffer(0)]], device atomic_uint *hist [[buffer(1)]], constant ushort *qt [[buffer(2)]], constant uint2 &dim [[buffer(3)]], uint i [[thread_position_in_grid]]) {\n"
"  uint nblk = dim.x, nt = dim.y; if (i >= nblk * nt) return; uint t = i / nblk, b = i % nblk;\n"
"  device atomic_uint *h = hist + t * 513; int dc = qz(cf, i, 0, qt), pr = b >= 2 ? qz(cf, i - 2, 0, qt) : 0;\n"
"  uint s = nb(dc - pr), mag = s; atomic_fetch_add_explicit(&h[s], 1, memory_order_relaxed); int run = 0;\n"
"  for (uint z = 1; z < 64; z++) { int v = qz(cf, i, z, qt); if (!v) { run++; continue; }\n"
"    while (run > 15) { atomic_fetch_add_explicit(&h[256 + 0xF0], 1, memory_order_relaxed); run -= 16; }\n"
"    uint sz = nb(v); atomic_fetch_add_explicit(&h[256 + ((run << 4) | sz)], 1, memory_order_relaxed); mag += sz; run = 0; }\n"
"  if (run) atomic_fetch_add_explicit(&h[256], 1, memory_order_relaxed);\n"
"  atomic_fetch_add_explicit(&h[512], mag, memory_order_relaxed); }\n"
/* lossless CinemaDNG: predictor 1 (left, same component; first column from the row above; corner = 2^(bits-1)) */
"kernel void k_lj(device const ushort *c [[buffer(0)]], device short *d [[buffer(1)]], device uchar *s [[buffer(2)]], constant PP &p [[buffer(3)]], uint3 g [[thread_position_in_grid]]) {\n"
"  if (g.x >= p.TW || g.y >= p.TH || g.z >= p.nt) return; int i = g.x, y = g.y;\n"
"  int v = tsample(c, p, g.z, y, i), pv = i >= 2 ? int(tsample(c, p, g.z, y, i - 2)) : (y == 0 ? (1 << (p.bits - 1)) : int(tsample(c, p, g.z, y - 1, i)));\n"
"  int dd = (v - pv) & 0xFFFF; if (dd >= 32768) dd -= 65536; uint a = uint(abs(dd));\n"
"  uint o = (g.z * p.TH + g.y) * p.TW + g.x; d[o] = short(dd); s[o] = dd == -32768 ? 16 : (a ? 32 - clz(a) : 0); }\n";

typedef struct { uint32_t w, h, W, H; float black, k; uint32_t TW, TH; uint32_t nt, across, bits, pad; } PP;
typedef struct {
    id<MTLBuffer> in, u, o16, cf, q, s, prm, qm, qt, hs;
    size_t in_n, u_n, o16_n, cf_n, q_n, s_n, hs_n;
    PP p; uint32_t nq;                       /* last jpeg dct dims, for quant */
} Slot;
struct FpGpu {
    id<MTLDevice> dev; id<MTLCommandQueue> q;
    id<MTLComputePipelineState> pU, pD, pJD, pJQ, pLJ, pJS;
    id<MTLBuffer> C8;
    Slot *slot; int nslots; char name[128];
};

static id<MTLBuffer> grow(FpGpu *g, id<MTLBuffer> b, size_t *have, size_t need) {
    if (b && *have >= need) return b;
    if (b) [b release];
    *have = need;
    return [g->dev newBufferWithLength:need ? need : 16 options:MTLResourceStorageModeShared];
}
static id<MTLComputePipelineState> pipe_(FpGpu *g, id<MTLLibrary> lib, NSString *n, char *err, size_t el) {
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithName:n];
    if (!f) { snprintf(err, el, "no kernel %s", n.UTF8String); return nil; }
    id<MTLComputePipelineState> p = [g->dev newComputePipelineStateWithFunction:f error:&e]; [f release];
    if (!p) snprintf(err, el, "%s", e.localizedDescription.UTF8String);
    return p;
}

FpGpu *fpgpu_create(int slots, char *err, size_t el) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { snprintf(err, el, "no Metal device"); return NULL; }
        FpGpu *g = calloc(1, sizeof *g); g->dev = dev; g->q = [dev newCommandQueue];
        NSError *e = nil; MTLCompileOptions *o = [[MTLCompileOptions new] autorelease];
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:o error:&e];
        if (!lib) { snprintf(err, el, "Metal compile: %s", e.localizedDescription.UTF8String); free(g); return NULL; }
        g->pU = pipe_(g, lib, @"k_prr_u", err, el); g->pD = pipe_(g, lib, @"k_prr_dct", err, el);
        g->pJD = pipe_(g, lib, @"k_jdct", err, el); g->pJQ = pipe_(g, lib, @"k_jq", err, el); g->pLJ = pipe_(g, lib, @"k_lj", err, el); g->pJS = pipe_(g, lib, @"k_jst", err, el);
        [lib release];
        if (!g->pU || !g->pD || !g->pJD || !g->pJQ || !g->pLJ || !g->pJS) { free(g); return NULL; }
        float C[64];
        for (int k = 0; k < 8; k++) for (int n = 0; n < 8; n++) C[k * 8 + n] = (float)((k == 0 ? sqrt(1.0 / 8) : sqrt(2.0 / 8)) * cos((2 * n + 1) * k * M_PI / 16.0));
        g->C8 = [dev newBufferWithBytes:C length:sizeof C options:MTLResourceStorageModeShared];
        g->nslots = slots < 1 ? 1 : slots; g->slot = calloc(g->nslots, sizeof(Slot));
        for (int i = 0; i < g->nslots; i++) {
            g->slot[i].prm = [dev newBufferWithLength:sizeof(PP) options:MTLResourceStorageModeShared];
            g->slot[i].qm = [dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
            g->slot[i].qt = [dev newBufferWithLength:128 options:MTLResourceStorageModeShared];
        }
        snprintf(g->name, sizeof g->name, "%s", dev.name.UTF8String);
        return g;
    }
}
const char *fpgpu_name(FpGpu *g) { return g->name; }
void fpgpu_destroy(FpGpu *g) { if (!g) return; free(g->slot); free(g); }

static void run(FpGpu *g, void (^enc)(id<MTLComputeCommandEncoder>)) {
    @autoreleasepool {
        id<MTLCommandBuffer> cb = [g->q commandBuffer];
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        enc(ce); [ce endEncoding]; [cb commit]; [cb waitUntilCompleted];
    }
}
static MTLSize tg2(id<MTLComputePipelineState> p) { NSUInteger w = p.threadExecutionWidth, h = p.maxTotalThreadsPerThreadgroup / w; return MTLSizeMake(w, h > 8 ? 8 : h, 1); }

const int16_t *fpgpu_prr(FpGpu *g, int si, const uint16_t *raw, int w, int h, float black, float k, const int16_t qm[64]) {
    Slot *s = &g->slot[si % g->nslots];
    int W = (w + 15) / 16 * 16, H = (h + 15) / 16 * 16;
    s->in = grow(g, s->in, &s->in_n, (size_t)w * h * 2); memcpy(s->in.contents, raw, (size_t)w * h * 2);
    s->u = grow(g, s->u, &s->u_n, (size_t)W * H * 2);
    s->o16 = grow(g, s->o16, &s->o16_n, (size_t)W * H * 2);           /* 4 sites x (W/16)(H/16) blocks x 64 = W*H */
    PP p = { (uint32_t)w, (uint32_t)h, (uint32_t)W, (uint32_t)H, black, k, 0, 0, 0, 0, 0, 0 };
    memcpy(s->prm.contents, &p, sizeof p); memcpy(s->qm.contents, qm, 128);
    run(g, ^(id<MTLComputeCommandEncoder> ce) {
        [ce setComputePipelineState:g->pU]; [ce setBuffer:s->in offset:0 atIndex:0]; [ce setBuffer:s->u offset:0 atIndex:1]; [ce setBuffer:s->prm offset:0 atIndex:2];
        [ce dispatchThreads:MTLSizeMake(W, H, 1) threadsPerThreadgroup:tg2(g->pU)];
        [ce setComputePipelineState:g->pD]; [ce setBuffer:s->u offset:0 atIndex:0]; [ce setBuffer:s->o16 offset:0 atIndex:1]; [ce setBuffer:g->C8 offset:0 atIndex:2];
        [ce setBuffer:s->qm offset:0 atIndex:3]; [ce setBuffer:s->prm offset:0 atIndex:4];
        [ce dispatchThreads:MTLSizeMake(W / 16, H / 16, 4) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
    });
    return (const int16_t *)s->o16.contents;
}

const float *fpgpu_jpeg_dct(FpGpu *g, int si, const uint16_t *codes, int w, int h, int TW, int TH) {
    Slot *s = &g->slot[si % g->nslots];
    uint32_t across = (w + TW - 1) / TW, down = (h + TH - 1) / TH, nt = across * down, nblk = (TW / 8) * (TH / 16) * 2;
    s->in = grow(g, s->in, &s->in_n, (size_t)w * h * 2); memcpy(s->in.contents, codes, (size_t)w * h * 2);
    s->cf = grow(g, s->cf, &s->cf_n, (size_t)nt * nblk * 64 * 4);
    PP p = { (uint32_t)w, (uint32_t)h, 0, 0, 0, 0, (uint32_t)TW, (uint32_t)TH, nt, across, 12, 0 };
    s->p = p; s->nq = nt * nblk * 64; memcpy(s->prm.contents, &p, sizeof p);
    run(g, ^(id<MTLComputeCommandEncoder> ce) {
        [ce setComputePipelineState:g->pJD]; [ce setBuffer:s->in offset:0 atIndex:0]; [ce setBuffer:s->cf offset:0 atIndex:1];
        [ce setBuffer:g->C8 offset:0 atIndex:2]; [ce setBuffer:s->prm offset:0 atIndex:3];
        [ce dispatchThreads:MTLSizeMake(TW / 8 * 2, TH / 16, nt) threadsPerThreadgroup:MTLSizeMake(16, 4, 1)];
    });
    return (const float *)s->cf.contents;
}
const int16_t *fpgpu_jpeg_quant(FpGpu *g, int si, const uint16_t qt[64]) {
    Slot *s = &g->slot[si % g->nslots];
    uint32_t n = s->nq;
    s->q = grow(g, s->q, &s->q_n, (size_t)n * 2); memcpy(s->qt.contents, qt, 128);
    run(g, ^(id<MTLComputeCommandEncoder> ce) {
        [ce setComputePipelineState:g->pJQ]; [ce setBuffer:s->cf offset:0 atIndex:0]; [ce setBuffer:s->q offset:0 atIndex:1];
        [ce setBuffer:s->qt offset:0 atIndex:2]; [ce setBytes:&n length:4 atIndex:3];
        [ce dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    });
    return (const int16_t *)s->q.contents;
}

/* estimated entropy-coded bytes of every tile (sum) at quantiser qt, from symbol statistics */
double fpgpu_jpeg_estimate(FpGpu *g, int si, const uint16_t qt[64]) {
    Slot *s = &g->slot[si % g->nslots];
    uint32_t nt = s->p.nt, nblk = (s->p.TW / 8) * (s->p.TH / 16) * 2, nblk_ = nblk;
    s->hs = grow(g, s->hs, &s->hs_n, (size_t)nt * 513 * 4); memset(s->hs.contents, 0, (size_t)nt * 513 * 4); memcpy(s->qt.contents, qt, 128);
    run(g, ^(id<MTLComputeCommandEncoder> ce) {
        [ce setComputePipelineState:g->pJS]; [ce setBuffer:s->cf offset:0 atIndex:0]; [ce setBuffer:s->hs offset:0 atIndex:1];
        [ce setBuffer:s->qt offset:0 atIndex:2]; uint32_t dim[2] = { nblk_, nt }; [ce setBytes:dim length:8 atIndex:3];
        [ce dispatchThreads:MTLSizeMake(nblk * nt, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    });
    const uint32_t *h = (const uint32_t *)s->hs.contents; double bits = 0;
    for (uint32_t t = 0; t < nt; t++, h += 513) {
        for (int part = 0; part < 2; part++) {              /* Huffman length ~ entropy of each table, at least 1 bit */
            const uint32_t *f = h + 256 * part; double N = 0; for (int k = 0; k < 256; k++) N += f[k];
            for (int k = 0; k < 256; k++) if (f[k]) { double L = -log2(f[k] / N); bits += f[k] * (L < 1 ? 1 : L); }
        }
        bits += h[512] + 8 * 400;                          /* magnitude bits; headers and tables */
    }
    return bits / 8;
}

int fpgpu_lj92(FpGpu *g, int si, const uint16_t *codes, int w, int h, int TW, int TH, int bits, const int16_t **d, const uint8_t **sz) {
    Slot *s = &g->slot[si % g->nslots];
    uint32_t across = (w + TW - 1) / TW, down = (h + TH - 1) / TH, nt = across * down;
    size_t n = (size_t)nt * TW * TH;
    s->in = grow(g, s->in, &s->in_n, (size_t)w * h * 2); memcpy(s->in.contents, codes, (size_t)w * h * 2);
    s->o16 = grow(g, s->o16, &s->o16_n, n * 2); s->s = grow(g, s->s, &s->s_n, n);
    PP p = { (uint32_t)w, (uint32_t)h, 0, 0, 0, 0, (uint32_t)TW, (uint32_t)TH, nt, across, (uint32_t)bits, 0 };
    memcpy(s->prm.contents, &p, sizeof p);
    run(g, ^(id<MTLComputeCommandEncoder> ce) {
        [ce setComputePipelineState:g->pLJ]; [ce setBuffer:s->in offset:0 atIndex:0]; [ce setBuffer:s->o16 offset:0 atIndex:1];
        [ce setBuffer:s->s offset:0 atIndex:2]; [ce setBuffer:s->prm offset:0 atIndex:3];
        [ce dispatchThreads:MTLSizeMake(TW, TH, nt) threadsPerThreadgroup:MTLSizeMake(32, 8, 1)];
    });
    *d = (const int16_t *)s->o16.contents; *sz = (const uint8_t *)s->s.contents;
    return (int)nt;
}
