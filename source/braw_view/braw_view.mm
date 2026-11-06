/*
 * braw_view.mm: Blackmagic RAW playback helper for cDNG Raw Convert's viewer.
 *
 *   braw_view [--cpu] [--shm] [--gen N] [--gamma NAME] [--gamut NAME] CLIP.braw
 *   --gen/--gamma/--gamut override the clip's colour science generation, gamma and gamut, e.g. --gen 5 --gamma
 *   "Blackmagic Design Film" --gamut "Blackmagic Design"; without them, the clip's own settings.
 *   --cpu   decode on the CPU (the default is the SDK's Metal pipeline, falling back to the CPU without one)
 *   --shm   hand frames over in shared memory instead of the pipe (see below)
 *
 * Prints one JSON line about the clip (size, rate, frames, timecode, camera, decode pipeline, every clip metadata
 * entry; with --shm also "shm", "slots", "slot_bytes"), then reads commands on stdin, one per line:
 *   f INDEX SCALE    decode frame INDEX at 1/SCALE resolution (1, 2, 4 or 8); several may be in flight
 *   r SLOT           (--shm) the app has copied the frame out of SLOT; it can be reused
 *   q                finish and quit
 * Without --shm each decoded frame goes to stdout as: "BRVF", u32 index, u32 width, u32 height (little endian), then
 * width*height*4 bytes of RGBA8 with the clip's own processing (its gamma, gamut and white balance).
 * With --shm the pixels (BGRA8) go into slot S of the POSIX shared memory object named in the JSON line (slot S
 * starts at S * slot_bytes), and stdout gets only "BRVS", u32 index, u32 (width << 16 | height), u32 S.
 * A frame the SDK can't read or decode goes out as "BRVE", u32 index, u32 HRESULT, u32 0 (no pixels).
 *
 * Uses Blackmagic's (free) Blackmagic RAW SDK, loaded at run time from where Blackmagic's software installs
 * it (the SDK, Blackmagic RAW Player or DaVinci Resolve); nothing of the SDK is shipped with this program.
 * With Metal, the SDK's buffers come from a small resource manager here that makes them shared (unified) memory,
 * so the finished frame can be read by the CPU without another GPU copy.
 * Part of cDNG Raw Convert (fpSup community). LGPL-2.1-or-later.
 */
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include "BlackmagicRawAPI.h"

static CFStringRef mkstr(const char *s) { return CFStringCreateWithCString(nullptr, s, kCFStringEncodingUTF8); }
static std::string cstr(CFStringRef s) {
    if (!s) return "";
    char buf[8192]; if (CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8)) return buf; return "";
}
static std::string jesc(const std::string &s) {
    std::string o; for (unsigned char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += (char)c; } else if (c >= 32) o += (char)c; } return o;
}
static std::string variant_json(Variant &v) {
    char t[64];
    switch (v.vt) {
        case blackmagicRawVariantTypeU8:      snprintf(t, sizeof t, "%u", (unsigned)(v.uiVal & 0xFF)); return t;
        case blackmagicRawVariantTypeS16:     snprintf(t, sizeof t, "%d", (int)v.iVal); return t;
        case blackmagicRawVariantTypeU16:     snprintf(t, sizeof t, "%u", (unsigned)v.uiVal); return t;
        case blackmagicRawVariantTypeS32:     snprintf(t, sizeof t, "%d", (int)v.intVal); return t;
        case blackmagicRawVariantTypeU32:     snprintf(t, sizeof t, "%u", (unsigned)v.uintVal); return t;
        case blackmagicRawVariantTypeFloat32: snprintf(t, sizeof t, "%g", (double)v.fltVal); return t;
        case blackmagicRawVariantTypeString:  return "\"" + jesc(cstr(v.bstrVal)) + "\"";
        case blackmagicRawVariantTypeSafeArray: {
            SafeArray *a = v.parray; void *data = nullptr; BlackmagicRawVariantType at; long lo = 0, hi = -1;
            if (FAILED(SafeArrayAccessData(a, &data)) || FAILED(SafeArrayGetVartype(a, &at)) || FAILED(SafeArrayGetLBound(a, 1, &lo)) || FAILED(SafeArrayGetUBound(a, 1, &hi))) return "null";
            long n = hi - lo + 1; std::string o = "[";
            if (n > 32) { SafeArrayUnaccessData(a); snprintf(t, sizeof t, "\"(%ld values)\"", n); return t; }
            for (long i = 0; i < n; i++) {
                if (i) o += ",";
                switch (at) {
                    case blackmagicRawVariantTypeU8: snprintf(t, sizeof t, "%u", ((uint8_t *)data)[i]); break;
                    case blackmagicRawVariantTypeS16: snprintf(t, sizeof t, "%d", ((int16_t *)data)[i]); break;
                    case blackmagicRawVariantTypeU16: snprintf(t, sizeof t, "%u", ((uint16_t *)data)[i]); break;
                    case blackmagicRawVariantTypeS32: snprintf(t, sizeof t, "%d", ((int32_t *)data)[i]); break;
                    case blackmagicRawVariantTypeU32: snprintf(t, sizeof t, "%u", ((uint32_t *)data)[i]); break;
                    case blackmagicRawVariantTypeFloat32: snprintf(t, sizeof t, "%g", (double)((float *)data)[i]); break;
                    default: snprintf(t, sizeof t, "null");
                }
                o += t;
            }
            SafeArrayUnaccessData(a); return o + "]";
        }
        default: return "null";
    }
}

/* ---- Metal resource manager: every SDK buffer in shared storage, so the CPU can read the processed frame.
   The SDK asks for (and hands back) a few large buffers for every frame. Released Metal buffers weren't being freed
   (something in the SDK's threads keeps them alive: 60 GB after half a minute of 6K playback), so buffers are
   recycled here instead: a released buffer goes on a free list and the next request of that size reuses it. */
struct SharedRM : public IBlackmagicRawResourceManager {
    id<MTLDevice> dev = nil;
    std::mutex lock; NSMutableArray<id<MTLBuffer>> *spare = nil; long live = 0, made = 0;
    HRESULT CreateResource(void *, void *q, uint32_t n, BlackmagicRawResourceType type, BlackmagicRawResourceUsage, void **res) override {
      @autoreleasepool {
        if (type == blackmagicRawResourceTypeBufferMetal) {
            NSUInteger len = n ? n : 16; id<MTLBuffer> b = nil;
            {
                std::lock_guard<std::mutex> l(lock);
                if (!spare) spare = [NSMutableArray array];
                for (NSUInteger i = 0; i < spare.count; i++) if (spare[i].length == len) { b = spare[i]; [spare removeObjectAtIndex:i]; break; }
            }
            if (!b) {
                id<MTLDevice> d = q ? ((__bridge id<MTLCommandQueue>)q).device : dev;
                b = [d newBufferWithLength:len options:MTLResourceStorageModeShared];
                if (!b) return E_OUTOFMEMORY;
                std::lock_guard<std::mutex> l(lock); made++;
            }
            { std::lock_guard<std::mutex> l(lock); live++; }
            if (getenv("BRAW_VIEW_DEBUG")) fprintf(stderr, "create %u: live %ld, allocated %ld, spare %lu\n", n, live, made, (unsigned long)spare.count);
            *res = (__bridge_retained void *)b; return S_OK;
        }
        if (type == blackmagicRawResourceTypeBufferCPU) { void *p = nullptr; if (posix_memalign(&p, 16384, n ? n : 16)) return E_OUTOFMEMORY; *res = p; return S_OK; }
        return E_NOTIMPL;
      }
    }
    HRESULT ReleaseResource(void *, void *, void *res, BlackmagicRawResourceType type) override {
      @autoreleasepool {
        if (type == blackmagicRawResourceTypeBufferMetal) {
            id<MTLBuffer> b = (__bridge_transfer id<MTLBuffer>)res;
            std::lock_guard<std::mutex> l(lock); live--;
            if (!spare) spare = [NSMutableArray array];
            [spare addObject:b];
            // keep the free list to what a few frames in flight need; free the oldest beyond that
            while (spare.count > 24) { made--; [spare removeObjectAtIndex:0]; }
            return S_OK;
        }
        if (type == blackmagicRawResourceTypeBufferCPU) { free(res); return S_OK; }
        return E_NOTIMPL;
      }
    }
    HRESULT CopyResource(void *, void *, void *, BlackmagicRawResourceType, void *, BlackmagicRawResourceType, uint32_t, bool) override { return E_NOTIMPL; }
    HRESULT GetResourceHostPointer(void *, void *, void *res, BlackmagicRawResourceType type, void **host) override {
        if (type == blackmagicRawResourceTypeBufferMetal) { *host = [(__bridge id<MTLBuffer>)res contents]; return *host ? S_OK : E_FAIL; }
        if (type == blackmagicRawResourceTypeBufferCPU) { *host = res; return S_OK; }
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID *) override { return E_NOTIMPL; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};
static SharedRM g_rm;
static bool g_metal = false;

/* ---- output */
static std::mutex g_out;
static IBlackmagicRawClipProcessingAttributes *g_attr = nullptr;   // overrides, or null for the clip's own
static bool g_shm = false; static uint8_t *g_base = nullptr; static size_t g_slot_bytes = 0; static int g_slots = 0;
static std::mutex g_sm; static std::condition_variable g_scv; static std::vector<char> g_free; static std::atomic<bool> g_quit{false};

static void emit(const char tag[4], uint32_t a, uint32_t b, uint32_t c, const void *px = nullptr, size_t n = 0) {
    std::lock_guard<std::mutex> l(g_out);
    uint32_t hd[3] = { a, b, c };
    fwrite(tag, 1, 4, stdout); fwrite(hd, 4, 3, stdout); if (px) fwrite(px, 1, n, stdout); fflush(stdout);
}
static void emit_error(uint32_t index, HRESULT r) { emit("BRVE", index, (uint32_t)r, 0); }
static int take_slot() {
    std::unique_lock<std::mutex> l(g_sm);
    for (;;) {
        if (g_quit) return -1;
        for (int s = 0; s < g_slots; s++) if (g_free[s]) { g_free[s] = 0; return s; }
        g_scv.wait(l);
    }
}

struct Viewer : public IBlackmagicRawCallback {
    void ReadComplete(IBlackmagicRawJob *job, HRESULT result, IBlackmagicRawFrame *frame) override { @autoreleasepool { read_done(job, result, frame); } }
    void ProcessComplete(IBlackmagicRawJob *job, HRESULT result, IBlackmagicRawProcessedImage *img) override { @autoreleasepool { process_done(job, result, img); } }
    void read_done(IBlackmagicRawJob *job, HRESULT result, IBlackmagicRawFrame *frame) {
        void *ud = nullptr; job->GetUserData(&ud); uintptr_t u = (uintptr_t)ud;
        job->Release();
        if (FAILED(result)) { emit_error((uint32_t)(u & 0xFFFFFFFFu), result); return; }
        uint32_t scale = (uint32_t)(u >> 40);
        frame->SetResourceFormat(g_shm ? blackmagicRawResourceFormatBGRAU8 : blackmagicRawResourceFormatRGBAU8);
        frame->SetResolutionScale(scale >= 8 ? blackmagicRawResolutionScaleEighth : scale >= 4 ? blackmagicRawResolutionScaleQuarter
                                  : scale >= 2 ? blackmagicRawResolutionScaleHalf : blackmagicRawResolutionScaleFull);
        IBlackmagicRawJob *dp = nullptr;
        HRESULT hr = frame->CreateJobDecodeAndProcessFrame(g_attr, nullptr, &dp);
        if (FAILED(hr)) { emit_error((uint32_t)(u & 0xFFFFFFFFu), hr); return; }
        dp->SetUserData(ud);
        if (FAILED(hr = dp->Submit())) { dp->Release(); emit_error((uint32_t)(u & 0xFFFFFFFFu), hr); }
    }
    void process_done(IBlackmagicRawJob *job, HRESULT result, IBlackmagicRawProcessedImage *img) {
        void *ud = nullptr; job->GetUserData(&ud); uint32_t index = (uint32_t)((uintptr_t)ud & 0xFFFFFFFFu);
        if (FAILED(result)) emit_error(index, result);
        else {
            uint32_t w = 0, h = 0, n = 0, type = 0; void *res = nullptr, *px = nullptr;
            img->GetWidth(&w); img->GetHeight(&h); img->GetResourceSizeBytes(&n); img->GetResource(&res); img->GetResourceType(&type);
            if (type == blackmagicRawResourceTypeBufferCPU) px = res; else g_rm.GetResourceHostPointer(nullptr, nullptr, res, type, &px);
            size_t need = (size_t)w * h * 4;
            if (!px || n < need) emit_error(index, 0x80004005u);
            else if (!g_shm) emit("BRVF", index, w, h, px, need);
            else if (need > g_slot_bytes || w > 0xFFFF || h > 0xFFFF) emit_error(index, 0x8007000Eu);
            else {
                int s = take_slot();
                if (s >= 0) { memcpy(g_base + (size_t)s * g_slot_bytes, px, need); emit("BRVS", index, (w << 16) | h, (uint32_t)s); }
            }
        }
        job->Release();
    }
    void DecodeComplete(IBlackmagicRawJob *, HRESULT) override {}
    void TrimProgress(IBlackmagicRawJob *, float) override {}
    void TrimComplete(IBlackmagicRawJob *, HRESULT) override {}
    void SidecarMetadataParseWarning(IBlackmagicRawClip *, CFStringRef, uint32_t, CFStringRef) override {}
    void SidecarMetadataParseError(IBlackmagicRawClip *, CFStringRef, uint32_t, CFStringRef) override {}
    void PreparePipelineComplete(void *, HRESULT) override {}
    void ReadAudioComplete(IBlackmagicRawJob *, HRESULT, IBlackmagicRawAudioBuffer *) override {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, LPVOID *) override { return E_NOTIMPL; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
};

int main(int argc, char **argv) {
  @autoreleasepool {
    const char *path = nullptr, *gamma = nullptr, *gamut = nullptr; int gen = 0; bool cpu = getenv("BRAW_VIEW_CPU") != nullptr;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--gen") && i + 1 < argc) gen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--gamma") && i + 1 < argc) gamma = argv[++i];
        else if (!strcmp(argv[i], "--gamut") && i + 1 < argc) gamut = argv[++i];
        else if (!strcmp(argv[i], "--cpu")) cpu = true;
        else if (!strcmp(argv[i], "--shm")) g_shm = true;
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: braw_view [--cpu] [--shm] [--gen N] [--gamma NAME] [--gamut NAME] CLIP.braw\n"); return 2; }
    const char *libs[] = { getenv("BRAW_SDK_LIB"),
        "/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac/Libraries",
        "/Applications/Blackmagic RAW/Blackmagic RAW Player.app/Contents/Frameworks",
        "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries",
        "/Applications/DaVinci Resolve Studio/DaVinci Resolve.app/Contents/Libraries" };
    IBlackmagicRawFactory *factory = nullptr;
    for (const char *l : libs) { if (!l) continue; CFStringRef s = mkstr(l); factory = CreateBlackmagicRawFactoryInstanceFromPath(s); CFRelease(s); if (factory) break; }
    if (!factory) { printf("{\"error\":\"Blackmagic RAW isn't installed (install Blackmagic RAW or DaVinci Resolve)\"}\n"); fflush(stdout); return 1; }
    IBlackmagicRaw *codec = nullptr;
    if (FAILED(factory->CreateCodec(&codec))) { printf("{\"error\":\"CreateCodec failed\"}\n"); fflush(stdout); return 1; }

    // the SDK's Metal pipeline (Resolve and Blackmagic RAW Player decode on the GPU too), with shared buffers
    id<MTLCommandQueue> queue = nil;
    if (!cpu) {
        IBlackmagicRawConfiguration *cfg = nullptr; IBlackmagicRawConfigurationEx *cfx = nullptr; bool ok = false;
        codec->QueryInterface(IID_IBlackmagicRawConfiguration, (void **)&cfg);
        codec->QueryInterface(IID_IBlackmagicRawConfigurationEx, (void **)&cfx);
        if (cfg && cfx && SUCCEEDED(cfg->IsPipelineSupported(blackmagicRawPipelineMetal, &ok)) && ok) {
            g_rm.dev = MTLCreateSystemDefaultDevice();
            if (g_rm.dev) queue = [g_rm.dev newCommandQueue];
            if (queue && SUCCEEDED(cfx->SetResourceManager(&g_rm)) && SUCCEEDED(cfg->SetPipeline(blackmagicRawPipelineMetal, nullptr, (__bridge void *)queue))) g_metal = true;
            else if (cfx) cfx->SetResourceManager(nullptr);
        }
        if (cfg) cfg->Release(); if (cfx) cfx->Release();
    }

    IBlackmagicRawClip *clip = nullptr; CFStringRef file = mkstr(path);
    if (FAILED(codec->OpenClip(file, &clip))) { printf("{\"error\":\"the Blackmagic RAW SDK couldn't open the clip\"}\n"); fflush(stdout); return 1; }
    uint32_t w = 0, h = 0; float fps = 0; uint64_t nf = 0; CFStringRef cam = nullptr, tc = nullptr;
    clip->GetWidth(&w); clip->GetHeight(&h); clip->GetFrameRate(&fps); clip->GetFrameCount(&nf); clip->GetCameraType(&cam);
    clip->GetTimecodeForFrame(0, &tc);
    std::string meta = "{";
    IBlackmagicRawMetadataIterator *it = nullptr; bool first = true;
    if (SUCCEEDED(clip->GetMetadataIterator(&it))) {
        CFStringRef key = nullptr; Variant v;
        while (SUCCEEDED(it->GetKey(&key))) {
            VariantInit(&v);
            if (SUCCEEDED(it->GetData(&v))) { std::string k = cstr(key); if (k.find("post_3dlut_embedded_data") == std::string::npos) { meta += (first ? "\"" : ",\"") + jesc(k) + "\":" + variant_json(v); first = false; } }
            VariantClear(&v); it->Next();
        }
        it->Release();
    }
    meta += "}";

    // shared memory for --shm: slots of one full-size BGRA frame each
    char shm_name[32] = "";
    if (g_shm) {
        g_slots = 10; g_slot_bytes = (((size_t)w * h * 4) + 16383) & ~(size_t)16383;
        snprintf(shm_name, sizeof shm_name, "/cdngbv.%d", (int)getpid());
        shm_unlink(shm_name);
        int fd = shm_open(shm_name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || ftruncate(fd, (off_t)(g_slot_bytes * g_slots)) != 0
            || (g_base = (uint8_t *)mmap(nullptr, g_slot_bytes * g_slots, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
            if (fd >= 0) close(fd); shm_unlink(shm_name); g_shm = false; g_base = nullptr; shm_name[0] = 0;   // fall back to the pipe
        } else { close(fd); g_free.assign(g_slots, 1); }
    }
    char shm_json[160] = "";
    if (g_shm) snprintf(shm_json, sizeof shm_json, ",\"shm\":\"%s\",\"slots\":%d,\"slot_bytes\":%zu", shm_name, g_slots, g_slot_bytes);
    printf("{\"width\":%u,\"height\":%u,\"fps\":%.6f,\"frames\":%llu,\"camera\":\"%s\",\"timecode\":\"%s\",\"pipeline\":\"%s\"%s,\"metadata\":%s}\n",
           w, h, (double)fps, (unsigned long long)nf, jesc(cstr(cam)).c_str(), jesc(cstr(tc)).c_str(), g_metal ? "Metal" : "CPU", shm_json, meta.c_str());
    fflush(stdout);

    if ((gen || gamma || gamut) && SUCCEEDED(clip->CloneClipProcessingAttributes(&g_attr))) {
        Variant v;
        if (gen) { VariantInit(&v); v.vt = blackmagicRawVariantTypeU16; v.uiVal = (uint16_t)gen; g_attr->SetClipAttribute(blackmagicRawClipProcessingAttributeColorScienceGen, &v); }
        if (gamma) { VariantInit(&v); v.vt = blackmagicRawVariantTypeString; v.bstrVal = mkstr(gamma); g_attr->SetClipAttribute(blackmagicRawClipProcessingAttributeGamma, &v); CFRelease(v.bstrVal); }
        if (gamut) { VariantInit(&v); v.vt = blackmagicRawVariantTypeString; v.bstrVal = mkstr(gamut); g_attr->SetClipAttribute(blackmagicRawClipProcessingAttributeGamut, &v); CFRelease(v.bstrVal); }
    }
    Viewer cb; codec->SetCallback(&cb);
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        if (line[0] == 'q') break;
        if (line[0] == 'r') {
            int s = atoi(line + 1);
            if (s >= 0 && s < g_slots) { std::lock_guard<std::mutex> l(g_sm); g_free[s] = 1; g_scv.notify_all(); }
            continue;
        }
        unsigned long long idx = 0; unsigned scale = 2;
        if (line[0] == 'f' && sscanf(line + 1, "%llu %u", &idx, &scale) >= 1) {
            if (idx >= nf) { emit_error((uint32_t)idx, 0x80070057u); continue; }      /* E_INVALIDARG */
            IBlackmagicRawJob *job = nullptr; HRESULT hr = clip->CreateJobReadFrame(idx, &job);
            if (SUCCEEDED(hr)) {
                job->SetUserData((void *)(uintptr_t)(((uint64_t)scale << 40) | (uint64_t)idx));
                if (FAILED(hr = job->Submit())) { job->Release(); emit_error((uint32_t)idx, hr); }
            } else emit_error((uint32_t)idx, hr);
        }
    }
    { std::lock_guard<std::mutex> l(g_sm); g_quit = true; g_scv.notify_all(); }    // frames waiting for a slot are dropped
    codec->FlushJobs();
    if (shm_name[0]) shm_unlink(shm_name);
    clip->Release(); codec->Release(); factory->Release(); CFRelease(file);
  }
  return 0;
}
