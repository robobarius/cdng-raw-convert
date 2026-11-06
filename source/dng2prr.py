#!/usr/bin/env python3
"""
dng2prr.py: the Python encoder of cDNG Raw Convert. SIGMA fp CinemaDNG -> Apple ProRes RAW .mov (proof of concept)

Experimental, community-built, NOT made or endorsed by Apple. The bitstream is
derived from FFmpeg's open-source ProRes RAW decoder. LGPL-2.1-or-later (see
NOTICE.md). Version 0.3. See README.md, FINDINGS.md and PORTING.md.

Usage:
  dng2prr.py OUT.mov CLIP_FOLDER                   (all *.DNG + the WAV in the folder)
  dng2prr.py OUT.mov IN1.DNG [IN2.DNG ...] [--wav AUDIO.WAV]
Options: --scale N (1 near-lossless, 2 high, 4 standard = Ninja V HQ, 8 compact), --jobs N,
         --neutral R/G,B/G, --cache DIR (resumable), --gain G, --fps 24000/1001, --limit N

Needs the compiled C core (prr_core.c + dng_lj92.c) next to this file (see build/):
  macOS libprr.dylib, Linux libprr.so, Windows libprr.dll
Tested against DaVinci Resolve Studio 21.1.1 on macOS only.
"""
import argparse, ctypes, glob, os, struct, sys, time, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
_LIBNAME = {'darwin': 'libprr.dylib', 'win32': 'libprr.dll'}.get(sys.platform, 'libprr.so')
def _find_lib():
    cands = [os.environ.get('PRR_LIB', ''), os.path.join(HERE, _LIBNAME)]
    if getattr(sys, 'frozen', False):                 # PyInstaller bundle
        cands.append(os.path.join(getattr(sys, '_MEIPASS', HERE), _LIBNAME))
    for c in cands:
        if c and os.path.isfile(c): return c
    sys.exit(f'dng2prr: {_LIBNAME} not found next to dng2prr.py. Build it first (see build/ and PORTING.md).')
_lib = ctypes.CDLL(_find_lib())
_lib.prr_encode_tiles.restype = ctypes.c_int
HAVE_LJ92 = hasattr(_lib, 'dng_lj92_decode')     # lossless-JPEG DNGs (built from dng_lj92.c)
if HAVE_LJ92:
    _lib.dng_lj92_decode.restype = ctypes.c_int
    _lib.dng_lj92_decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p,
                                     ctypes.c_int, ctypes.c_int, ctypes.c_int]
HAVE_JPEG = hasattr(_lib, 'dng_jpeg_decode')       # + lossy JPEG DNGs (built from dng_jpeg.c)
if HAVE_JPEG:
    _lib.dng_jpeg_decode.restype = ctypes.c_int
    _lib.dng_jpeg_decode.argtypes = _lib.dng_lj92_decode.argtypes
_lib.prr_set_dc_clamp.argtypes = [ctypes.c_int]
_lib.prr_encode_tiles.argtypes = [
    ctypes.c_void_p, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int,
    ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
    ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t), ctypes.POINTER(ctypes.c_long)]

# Default linearization curve used by the format when none is signalled
LIN_CURVE = np.array([0, 512, 1024, 2048, 4096, 8192, 16384, 32768, 65536], dtype=np.float64)
LIN_KNOTS = np.arange(9, dtype=np.float64) * 8192.0
BLACK16 = 256.0          # black level is fixed at 0x100 in the format
VENDOR = b'fpsp'         # our own vendor tag (fpSup), not a camera maker's
# How Apple's decoder reads code values (measured in Resolve 21.1.1, FINDINGS.md finding 2):
#   linear = P(u), P = the default curve above, black (256) at u = 4096,
#   white = sensel range + 256, EXCEPT that a tile quantiser of 1 is decoded as if
#   it were 2. So we never write tile quantiser 1.
#
# Quality ladder (0.3): with the Atomos-style levels below, these tile quantisers give
# about the same file sizes as 0.1.x's 2/4/8/16; 15 is what a Ninja V writes for
# "ProRes RAW HQ" from the fp.
QUALITY_Q = {1: 4, 2: 8, 4: 15, 8: 30}
def tile_quantiser(scale):
    return QUALITY_Q.get(int(scale), max(2, min(255, int(round(3.75 * scale)))))
ALIGN = 3                # tiles of up to 8 blocks (128 px), as Atomos writes

def rat(v):
    if isinstance(v, tuple): return v[0] / v[1]
    return float(v)

def rats(seq):
    return [seq[i] / seq[i + 1] for i in range(0, len(seq), 2)]

# ---------------------------------------------------------------- DNG reading

# Minimal TIFF/DNG tag reader (first IFD only, which holds the raw image in fp DNGs)
TAGNAMES = {256: 'ImageWidth', 257: 'ImageLength', 258: 'BitsPerSample', 259: 'Compression',
            262: 'PhotometricInterpretation', 273: 'StripOffsets', 279: 'StripByteCounts',
            33422: 'CFAPattern', 50714: 'BlackLevel', 50717: 'WhiteLevel', 50719: 'DefaultCropOrigin',
            50720: 'DefaultCropSize', 50722: 'ColorMatrix2', 50721: 'ColorMatrix1', 50728: 'AsShotNeutral',
            50730: 'BaselineExposure', 51043: 'TimeCodes', 51044: 'FrameRate', 34665: 'ExifIFD',
            50778: 'CalibrationIlluminant1', 50779: 'CalibrationIlluminant2',
            322: 'TileWidth', 323: 'TileLength', 324: 'TileOffsets', 325: 'TileByteCounts',
            50712: 'LinearizationTable', 278: 'RowsPerStrip'}
EXIFNAMES = {33434: 'ExposureTime', 33437: 'FNumber', 34855: 'ISO', 37386: 'FocalLength',
             42035: 'LensMake', 42036: 'LensModel'}
_TFMT = {1: ('B', 1), 2: ('s', 1), 3: ('H', 2), 4: ('I', 4), 5: ('I', 8), 7: ('B', 1), 8: ('h', 2),
         9: ('i', 4), 10: ('i', 8), 11: ('f', 4), 12: ('d', 8)}

def read_tiff_tags(fh, ifd=None, names=None):
    names = names or TAGNAMES
    fh.seek(0); bo = fh.read(2)
    e = '<' if bo == b'II' else '>'
    magic, first = struct.unpack(e + 'HI', fh.read(6))
    if magic != 42: raise ValueError('not a TIFF/DNG file')
    fh.seek(first if ifd is None else ifd); n, = struct.unpack(e + 'H', fh.read(2))
    tags = {}
    for _ in range(n):
        code, typ, cnt, val = struct.unpack(e + 'HHI4s', fh.read(12))
        if code not in names or typ not in _TFMT: continue
        f, size = _TFMT[typ]
        nbytes = size * cnt
        here = fh.tell()
        if nbytes > 4:
            off, = struct.unpack(e + 'I', val); fh.seek(off); raw = fh.read(nbytes); fh.seek(here)
        else:
            raw = val[:nbytes]
        name = names[code]
        if typ in (1, 7) and name in ('CFAPattern', 'TimeCodes'):
            v = raw
        elif typ == 2:
            v = raw.rstrip(b'\0').decode('latin-1')
        else:
            k = cnt * 2 if typ in (5, 10) else cnt
            v = struct.unpack(e + f * k, raw)
            if k == 1: v = v[0]
        tags[name] = v
    if ifd is None and 'ExifIFD' in tags:
        try: tags['exif'] = read_tiff_tags(fh, tags['ExifIFD'], EXIFNAMES)
        except Exception: tags['exif'] = {}
    return tags
def unpack_bits(b, w, h, bps):
    """Unpack MSB-first packed samples (TIFF rows start on byte boundaries)."""
    if bps == 16:
        return b[:w * h * 2].view('>u2').reshape(h, w).astype(np.uint16)
    rowbytes = (w * bps + 7) // 8
    rows = b[:rowbytes * h].reshape(h, rowbytes)
    if bps == 12 and w % 2 == 0:
        g = rows.reshape(h, w // 2, 3).astype(np.uint16)
        out = np.empty((h, w), np.uint16)
        out[:, 0::2] = (g[..., 0] << 4) | (g[..., 1] >> 4)
        out[:, 1::2] = ((g[..., 1] & 0xF) << 8) | g[..., 2]
        return out
    if bps == 14 and w % 4 == 0:
        g = rows.reshape(h, w // 4, 7).astype(np.uint64)
        v = np.zeros(g.shape[:2], np.uint64)
        for i in range(7): v = (v << np.uint64(8)) | g[..., i]
        out = np.empty((h, w // 4, 4), np.uint16)
        for i in range(4):
            out[..., i] = ((v >> np.uint64(14 * (3 - i))) & np.uint64(0x3FFF)).astype(np.uint16)
        return out.reshape(h, w)
    bits = np.unpackbits(rows, axis=1)[:, :w * bps].reshape(h, w, bps).astype(np.uint16)
    return (bits << np.arange(bps - 1, -1, -1, dtype=np.uint16)).sum(axis=2).astype(np.uint16)

class DNGFrame:
    def __init__(self, path):
        with open(path, 'rb') as fh:
            tags = read_tiff_tags(fh)
            if int(tags.get('PhotometricInterpretation', 0)) != 32803:
                raise ValueError(f'{path}: not a CFA DNG')
            if tags.get('CFAPattern') != b'\x00\x01\x01\x02':
                raise ValueError(f'{path}: only RGGB is supported (got {tags.get("CFAPattern")!r})')
            self.h, self.w = int(tags['ImageLength']), int(tags['ImageWidth'])
            bps = tags['BitsPerSample']; bps = int(bps[0] if isinstance(bps, tuple) else bps)
            comp = int(tags.get('Compression', 1))
            tup = lambda v: v if isinstance(v, tuple) else (v,)
            if comp == 1:
                offs, cnts = tup(tags['StripOffsets']), tup(tags['StripByteCounts'])
                buf = bytearray()
                for off, cnt in zip(offs, cnts):
                    fh.seek(off); buf += fh.read(cnt)
                self.raw = unpack_bits(np.frombuffer(bytes(buf), np.uint8), self.w, self.h, bps)
            elif comp == 7:                              # lossless JPEG, tiles or strips
                if not HAVE_LJ92: raise ValueError(f'{path}: compressed DNG; rebuild the C core with dng_lj92.c')
                if 'TileOffsets' in tags:
                    tw, tl = int(tags['TileWidth']), int(tags['TileLength'])
                    offs, cnts = tup(tags['TileOffsets']), tup(tags['TileByteCounts'])
                else:
                    tw, tl = self.w, int(tags.get('RowsPerStrip', self.h))
                    offs, cnts = tup(tags['StripOffsets']), tup(tags['StripByteCounts'])
                across = (self.w + tw - 1) // tw
                down = (self.h + tl - 1) // tl
                full = np.zeros((down * tl, across * tw), np.uint16)
                for i, (off, cnt) in enumerate(zip(offs, cnts)):
                    fh.seek(off); data = fh.read(cnt)
                    ty, tx = divmod(i, across)
                    view = full[ty * tl:(ty + 1) * tl, tx * tw:(tx + 1) * tw]
                    dec = _lib.dng_jpeg_decode if HAVE_JPEG else _lib.dng_lj92_decode
                    rc = dec(data, len(data), view.ctypes.data, tw, tl, full.shape[1])
                    if rc != 0: raise ValueError(f'{path}: JPEG decode failed ({rc}) in tile {i}')
                self.raw = np.ascontiguousarray(full[:self.h, :self.w])
            else:
                raise ValueError(f'{path}: DNG compression {comp} not supported')
            lt = tags.get('LinearizationTable')
            if lt is not None:
                lt = np.array(tup(lt), np.uint16)
                self.raw = lt[np.minimum(self.raw, len(lt) - 1)]
        self.bits = bps
        bl = tags.get('BlackLevel', (0, 1))
        bl = rats(bl) if isinstance(bl, tuple) and len(bl) % 2 == 0 and len(bl) >= 2 else [float(bl)]
        self.black = float(np.mean(bl))
        self.white = float(tags.get('WhiteLevel', 4095))
        self.cm2 = np.array(rats(tags['ColorMatrix2']), dtype=np.float64).reshape(3, 3)
        self.cm1 = np.array(rats(tags['ColorMatrix1']), dtype=np.float64).reshape(3, 3) if 'ColorMatrix1' in tags else None
        self.illum1, self.illum2 = tags.get('CalibrationIlluminant1', 17), tags.get('CalibrationIlluminant2', 21)
        self.neutral = np.array(rats(tags['AsShotNeutral']), dtype=np.float64)
        be = tags.get('BaselineExposure', (0, 1))
        self.baseline_exposure = be[0] / be[1] if isinstance(be, tuple) else float(be)
        fr = tags.get('FrameRate', (24000, 1001))
        self.fps = (fr[0], fr[1])
        co = tags.get('DefaultCropOrigin', (0, 0)); cs = tags.get('DefaultCropSize', (self.w, self.h))
        cx, cy = int(rat(co[0]) if isinstance(co[0], tuple) else co[0]), int(rat(co[1]) if isinstance(co[1], tuple) else co[1])
        cw, ch = int(cs[0]), int(cs[1])
        self.crop = (cx, self.w - cx - cw, cy, self.h - cy - ch)  # l, r, t, b
        ex = tags.get('exif', {})
        r2 = lambda v: v[0] / v[1] if isinstance(v, tuple) and len(v) == 2 and v[1] else None
        self.iso = ex.get('ISO') if isinstance(ex.get('ISO'), int) else None
        self.fnumber = r2(ex.get('FNumber')); self.exposure_time = ex.get('ExposureTime')
        self.focal = r2(ex.get('FocalLength'))
        self.lens = ' '.join(x.strip() for x in (ex.get('LensMake', ''), ex.get('LensModel', '')) if isinstance(x, str) and x.strip())

# ---------------------------------------------------------------- colour metadata
XYZ_D65 = np.array([0.95047, 1.0, 1.08883])

ILLUM_CCT = {17: 2856.0, 21: 6504.0, 20: 5503.0, 23: 5003.0, 22: 7504.0, 10: 6774.0}
BRADFORD = np.array([[0.8951, 0.2664, -0.1614], [-0.7502, 1.7135, 0.0367], [0.0389, -0.0685, 1.0296]])

def _cct_from_xy(x, y):
    n = (x - 0.3320) / (0.1858 - y)
    return float(np.clip(449 * n**3 + 3525 * n**2 + 6823.3 * n + 5520.33, 1500, 20000))

def colour_params(f):
    """WB factors + camera(WB'd)->XYZ(D65) matrix + CCT, following the DNG spec:
    the colour matrix is interpolated between the two calibration illuminants
    by the white's colour temperature (found iteratively), then the white is
    Bradford-adapted to D65."""
    n = f.neutral / f.neutral[1]
    wb_r, wb_b = 1.0 / n[0], 1.0 / n[2]
    cm1 = f.cm1 if getattr(f, 'cm1', None) is not None else f.cm2
    t1 = ILLUM_CCT.get(int(getattr(f, 'illum1', 17)), 2856.0)
    t2 = ILLUM_CCT.get(int(getattr(f, 'illum2', 21)), 6504.0)
    if t1 > t2: cm1, f_cm2, t1, t2 = f.cm2, cm1, t2, t1
    else: f_cm2 = f.cm2
    T = 5000.0
    for _ in range(20):
        g = float(np.clip((1 / T - 1 / t2) / (1 / t1 - 1 / t2), 0, 1))
        cm = g * cm1 + (1 - g) * f_cm2
        xyz = np.linalg.inv(cm) @ n
        x, y = xyz[0] / xyz.sum(), xyz[1] / xyz.sum()
        Tn = _cct_from_xy(x, y)
        if abs(Tn - T) < 1: T = Tn; break
        T = Tn
    w = xyz / xyz[1]                                   # scene white (XYZ)
    cat = np.linalg.inv(BRADFORD) @ np.diag((BRADFORD @ XYZ_D65) / (BRADFORD @ w)) @ BRADFORD
    M = cat @ np.linalg.inv(cm) @ np.diag(n)           # WB'd camera -> XYZ(D65)
    M /= (M @ np.ones(3))[1]
    return wb_r, wb_b, M, int(round(T))

def planck_xy(T):
    """CIE xy of a blackbody at T kelvin (Kim et al. cubic approximation, 1667-25000 K)."""
    T = float(np.clip(T, 1667.0, 25000.0))
    if T <= 4000: x = -0.2661239e9 / T**3 - 0.2343589e6 / T**2 + 0.8776956e3 / T + 0.179910
    else:         x = -3.0258469e9 / T**3 + 2.1070379e6 / T**2 + 0.2226347e3 / T + 0.240390
    if T <= 2222:   y = -1.1063814 * x**3 - 1.34811020 * x**2 + 2.18555832 * x - 0.20219683
    elif T <= 4000: y = -0.9549476 * x**3 - 1.37418593 * x**2 + 2.09137015 * x - 0.16748867
    else:           y = 3.0817580 * x**3 - 5.87338670 * x**2 + 3.75112997 * x - 0.37001483
    return x, y

def _cm_at(f, T):
    cm1 = f.cm1 if getattr(f, 'cm1', None) is not None else f.cm2
    t1 = ILLUM_CCT.get(int(getattr(f, 'illum1', 17)), 2856.0)
    t2 = ILLUM_CCT.get(int(getattr(f, 'illum2', 21)), 6504.0)
    cm2 = f.cm2
    if t1 > t2: cm1, cm2, t1, t2 = cm2, cm1, t2, t1
    g = float(np.clip((1 / T - 1 / t2) / (1 / t1 - 1 / t2), 0, 1))
    return g * cm1 + (1 - g) * cm2

# CCTs of the per-temperature calibration tables (the same set an Atomos Ninja V writes for the fp)
BYCCT_TEMPS = (2500, 2727, 2800, 2856, 3000, 3333, 3750, 4000, 4286, 5000, 6000, 6500, 7500, 10000)

def calibration_by_cct(f, temps=BYCCT_TEMPS):
    """For each colour temperature: white-balance factors (R, B) and the WB'd
    camera -> XYZ(D65) matrix, from the DNG's two colour matrices (DNG-spec
    interpolation, Bradford adaptation). This is what an Atomos recorder stores in
    com.apple.proresraw.whitebalance.bycct.*.

    Resolve uses these tables for the as-shot picture too (measured: it looks up
    the clip's colour temperature in them), and a table built on the blackbody
    locus has no tint. So the whole table is shifted by the clip's own tint
    (the ratio between its as-shot neutral and the blackbody neutral at its
    temperature), and the clip's temperature is added as an entry: at the
    as-shot temperature the table then reproduces the frame header exactly,
    and moving the temperature keeps the tint."""
    wb_r0, wb_b0, M0, T0 = colour_params(f)
    def locus_neutral(T):
        x, y = planck_xy(T)
        w = np.array([x / y, 1.0, (1 - x - y) / y])
        n = _cm_at(f, T) @ w
        return n / n[1]
    n0 = np.array([1.0 / wb_r0, 1.0, 1.0 / wb_b0])
    tint = n0 / locus_neutral(T0)                       # per-channel; 1 for a white on the locus
    out = []
    for T in sorted(set(int(t) for t in temps) | {int(T0)}):
        n = locus_neutral(T) * tint
        cm = _cm_at(f, T)
        w = np.linalg.inv(cm) @ n; w = w / w[1]          # the white this neutral corresponds to
        cat = np.linalg.inv(BRADFORD) @ np.diag((BRADFORD @ XYZ_D65) / (BRADFORD @ w)) @ BRADFORD
        M = cat @ np.linalg.inv(cm) @ np.diag(n)
        M /= (M @ np.ones(3))[1]
        if T == int(T0): n, M = n0, M0                    # exact match with the frame header
        out.append((int(T), 1.0 / n[0], 1.0 / n[2], M))
    return out

def bycct_blobs(cal):
    """Binary layout used by Atomos (uint16 count, uint16 version 1, then entries;
    big-endian): factors = [uint32 K, float R, float B], matrices = [uint32 K, 9 floats]."""
    hdr = struct.pack('>HH', len(cal), 1)
    wb = hdr + b''.join(struct.pack('>Iff', T, r, b) for T, r, b, _ in cal)
    mx = hdr + b''.join(struct.pack('>I9f', T, *M.flatten()) for T, _, _, M in cal)
    return wb, mx

# ---------------------------------------------------------------- frame encode
LEVEL_MODE = 'atomos'     # 'atomos' (0.3+) or 'footroom' (0.1.x)
def level_scale(f):
    """Factor from sensor DN (above black) to the format's 16-bit linear range.
    'atomos': the largest power of two that puts white at or below 61440 (+256),
      as an Atomos Ninja V does for the SIGMA fp over HDMI (12-bit x16, sensel
      range 61424). Uses the whole code range, which gives the finest shadow
      precision; leaves 256/k DN below black (16 DN at 12-bit, 64 DN at 14-bit),
      ample because the fp clips its raw data at black.
    'footroom': 0.1.x behaviour, at least 64 DN below black (12-bit x4, 14-bit x1)."""
    span = f.white - f.black
    if LEVEL_MODE == 'atomos':
        k = 1.0
        while span * k * 2 <= 61440.0: k *= 2
        return k
    footroom = max(64.0, span / 60.0)
    return min((65535.0 - BLACK16) / span, BLACK16 / footroom)

def encode_frame(f, scale=1, align=None, gain=None):
    """Encode one frame. f is a DNGFrame (or any object with the same attributes).
    Returns (frame bytes, number of DC clamps). Test hooks on f: f.u (code values
    given directly), f.colour = (wb_r, wb_b, M, cct), f.sensel_override."""
    if align is None: align = ALIGN
    w, h = f.w, f.h
    W, H = (w + 15) // 16 * 16, (h + 15) // 16 * 16
    # Scale sensor values into the 16-bit linear range. Black is fixed at 256,
    # so the scale sets the footroom kept for noise below black: at least
    # 64 DN, or 1/60 of the black-to-white range (64 DN for 12-bit, 256 DN for 14-bit).
    k = level_scale(f)
    _lib.prr_set_dc_clamp(0)                       # only needed for quantiser 1, which we never write
    tile_scale = tile_quantiser(scale)
    if getattr(f, 'u', None) is not None:          # test hook: code values given directly
        u = f.u.astype(np.float64)
    else:
        lin = BLACK16 + (f.raw.astype(np.float32) - np.float32(f.black)) * np.float32(k)
        f.raw = None                               # free memory early
        lin = np.clip(lin, 0, 65535)
        u = np.interp(lin, LIN_CURVE, LIN_KNOTS)   # inverse of linear = P(u)
        del lin
    u = np.clip(np.rint(u), 0, 65535).astype(np.uint16)
    if W != w or H != h:   # pad by repeating the last Bayer row/column pair
        pad = np.empty((H, W), np.uint16)
        pad[:h, :w] = u
        for x in range(w, W): pad[:h, x] = u[:, w - 2 + ((x - w) & 1)]
        for y in range(h, H): pad[y, :] = pad[h - 2 + ((y - h) & 1), :]
        u = pad
    u = np.ascontiguousarray(u)

    qmat = np.ones(64, np.uint8)
    cap = W * H * 4 + 65536
    out = np.empty(cap, np.uint8)
    max_tiles = ((W // 16) + 8) * (H // 16)
    tsz = np.zeros(max_tiles, np.uint16)
    olen = ctypes.c_size_t(0); clamped = ctypes.c_long(0)
    nt = _lib.prr_encode_tiles(u.ctypes.data, W, w, h, align, tile_scale, qmat.ctypes.data,
                               out.ctypes.data, cap, tsz.ctypes.data,
                               ctypes.byref(olen), ctypes.byref(clamped))
    if nt < 0:
        raise RuntimeError(f'tile encoder failed ({nt}); try a larger --scale')

    wb_r, wb_b, M, cct = getattr(f, 'colour', None) or colour_params(f)
    if gain is None:
        gain = 2.0 ** f.baseline_exposure
    flags = 1 | (align << 1)                      # quant matrix present, tile alignment, no curve
    hdr = bytearray()
    hdr += bytes([0, 0])                          # reserved, version
    hdr += VENDOR
    hdr += struct.pack('>HH', w, h)
    l, r, t, b = [max(0, min(255, v)) for v in f.crop]
    hdr += bytes([l, r, t, b])
    hdr += struct.pack('>H', 0)                   # Bayer pattern RGGB
    sensel = getattr(f, 'sensel_override', None) or int(round((f.white - f.black) * k))
    hdr += struct.pack('>H', sensel)  # sensel value range (white - black)
    hdr += struct.pack('>ff', wb_r, wb_b)
    hdr += struct.pack('>9f', *M.flatten())
    hdr += struct.pack('>f', gain)
    hdr += struct.pack('>H', cct)
    hdr += struct.pack('>H', flags)
    hdr += qmat.tobytes()
    header = struct.pack('>H', len(hdr) + 2) + hdr

    body = header + tsz[:nt].astype('>u2').tobytes() + out[:olen.value].tobytes()
    body += struct.pack('>I', 8) + b'eomd'
    total = 8 + len(body)
    total = (total + 15) // 16 * 16
    frame = struct.pack('>I', total) + b'prrf' + body
    frame += b'\0' * (total - len(frame))
    return frame, clamped.value

# ---------------------------------------------------------------- MOV writer
def atom(typ, *payload):
    data = b''.join(payload)
    return struct.pack('>I', 8 + len(data)) + typ + data

def fixed16(v): return struct.pack('>I', int(round(v * 65536)) & 0xFFFFFFFF)
MATRIX = struct.pack('>9I', 0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000)

def hdlr(comp_type, sub, name):
    nm = name.encode()
    return atom(b'hdlr', b'\0\0\0\0', comp_type, sub, b'appl' if comp_type == b'mhlr' else b'\0\0\0\0',
                b'\0' * 8, bytes([len(nm)]) + nm)

def dinf():
    return atom(b'dinf', atom(b'dref', struct.pack('>II', 0, 1), atom(b'alis', struct.pack('>I', 1))))

def stco(offsets):
    return atom(b'co64', struct.pack('>II', 0, len(offsets)), b''.join(struct.pack('>Q', o) for o in offsets))

def stsc(per_chunk):
    entries = []
    for i, n in enumerate(per_chunk):
        if not entries or entries[-1][1] != n:
            entries.append((i + 1, n))
    return atom(b'stsc', struct.pack('>II', 0, len(entries)), b''.join(struct.pack('>III', c, n, 1) for c, n in entries))

def meta_atom(items):
    """QuickTime 'mdta' metadata (keys + ilst), the layout Atomos recorders use.
    items: list of (key, value); str -> UTF-8, float -> big-endian float32."""
    items = [(k, v) for k, v in items if v not in (None, '')]
    keys = b''.join(struct.pack('>I', 8 + len(k.encode())) + b'mdta' + k.encode() for k, _ in items)
    ilst = b''
    for i, (_, v) in enumerate(items, 1):
        if isinstance(v, (bytes, bytearray)): typ, data = 0, bytes(v)     # binary (e.g. bycct tables)
        elif isinstance(v, float): typ, data = 23, struct.pack('>f', v)
        else: typ, data = 1, str(v).encode()
        d = atom(b'data', struct.pack('>II', typ, 0), data)
        ilst += struct.pack('>I', 8 + len(d)) + struct.pack('>I', i) + d
    hd = atom(b'hdlr', b'\0' * 8, b'mdta', b'\0' * 12)
    return atom(b'meta', hd, atom(b'keys', struct.pack('>II', 0, len(items)), keys), atom(b'ilst', ilst))

def camera_metadata(f, cct, fps=None):
    # manufacturer/model strings exactly as a Ninja V writes them for the fp
    md = [('com.apple.proapps.manufacturer', 'Sigma'), ('com.apple.proapps.modelname', 'SIGMA fp')]
    if getattr(f, 'iso', None):
        md += [('com.apple.proapps.exif.{Exif}.ExposureIndex', float(f.iso)),
               ('org.smpte.rdd18.camera.isosensitivity', str(f.iso))]
    if cct: md.append(('org.smpte.rdd18.camera.whitebalance', f'{int(cct)}K'))
    if getattr(f, 'fnumber', None):
        md += [('com.apple.proapps.exif.{Exif}.FNumber', float(f.fnumber)),
               ('org.smpte.rdd18.lens.irisfnumber', f'F{f.fnumber:g}')]
    et = getattr(f, 'exposure_time', None)
    t = et[0] / et[1] if isinstance(et, tuple) and len(et) == 2 and et[1] and et[0] else None
    if t:
        md.append(('com.apple.proapps.exif.{Exif}.ShutterSpeedValue', float(-np.log2(t))))   # APEX
        if fps: md.append(('org.smpte.rdd18.camera.shutterspeed_angle', f'{min(360.0, 360.0 * t * fps[0] / fps[1]):.1f}deg'))
    if getattr(f, 'cm2', None) is not None:
        wbt, mxt = bycct_blobs(calibration_by_cct(f))
        md += [('com.apple.proresraw.whitebalance.bycct.whitebalancefactors', wbt),
               ('com.apple.proresraw.whitebalance.bycct.colormatrices', mxt)]
    if getattr(f, 'lens', None):
        md += [('org.smpte.rdd18.lens.lensmodel', f.lens), ('com.apple.proapps.exif.{ExifAux}.LensModel', f.lens),
               ('org.smpte.rdd18.lens.lensattributes', f.lens)]
    md += [('com.apple.proapps.image.{TIFF}.Make', 'SIGMA'), ('com.apple.proapps.image.{TIFF}.Model', 'fp'),
           ('com.apple.proapps.image.{TIFF}.Software', 'dng2prr (fpSup PoC)')]
    return md

PROGRESS_EVERY = 25
def write_mov(path, frames_iter, nframes, w, h, fps, wav=None, log=print, metadata=None, hq=False):
    """hq: label the track 'aprh' (Apple ProRes RAW HQ) instead of 'aprn' (ProRes RAW)."""
    num, den = fps
    vts, vdur = num, den                   # timescale = numerator, each frame = den
    audio = None
    if wav:
        wf = wave.open(wav, 'rb')
        ach, asw, asr = wf.getnchannels(), wf.getsampwidth(), wf.getframerate()
        abpf = ach * asw
        atotal = wf.getnframes()
        audio = (wf, ach, asw, asr, abpf, atotal)

    with open(path, 'wb') as fh:
        fh.write(atom(b'ftyp', b'qt  ', struct.pack('>I', 0x200), b'qt  '))
        mdat_start = fh.tell()
        fh.write(struct.pack('>I', 1) + b'mdat' + struct.pack('>Q', 0))  # 64-bit size, patched later
        v_off, v_sz, a_off, a_cnt = [], [], [], []
        a_done = 0
        t0 = time.time()
        for i, fr in enumerate(frames_iter):
            v_off.append(fh.tell()); v_sz.append(len(fr)); fh.write(fr)
            if audio:
                wf, ach, asw, asr, abpf, atotal = audio
                end = min(atotal, round((i + 1) * den * asr / num))   # audio trimmed to picture length
                n = end - a_done
                if n > 0:
                    a_off.append(fh.tell()); a_cnt.append(n)
                    fh.write(wf.readframes(n)); a_done = end
            if log and (i % PROGRESS_EVERY == 0 or i == nframes - 1):
                el = time.time() - t0
                log(f'  frame {i + 1}/{nframes}  {len(fr) / 1e6:.2f} MB  ({(i + 1) / el:.2f} fps)')
        mdat_end = fh.tell()
        fh.seek(mdat_start + 8); fh.write(struct.pack('>Q', mdat_end - mdat_start)); fh.seek(mdat_end)

        nf = len(v_sz)
        mv_dur = nf * vdur
        mvhd = atom(b'mvhd', struct.pack('>IIIII', 0, 0, 0, vts, mv_dur), fixed16(1.0),
                    struct.pack('>H', 0x100), b'\0' * 10, MATRIX, b'\0' * 24, struct.pack('>I', 3 if audio else 2))

        # video track
        compname = b'Apple ProRes RAW HQ' if hq else b'Apple ProRes RAW'
        stsd_entry = (b'\0' * 6 + struct.pack('>H', 1) + struct.pack('>HH', 0, 0) + VENDOR +
                      struct.pack('>II', 0, 0x200) + struct.pack('>HH', w, h) +
                      fixed16(72) + fixed16(72) + struct.pack('>IH', 0, 1) +
                      bytes([len(compname)]) + compname.ljust(31, b'\0') + struct.pack('>Hh', 24, -1) +
                      atom(b'fiel', bytes([1, 0])) + atom(b'pasp', struct.pack('>II', 1, 1)))
        vstsd = atom(b'stsd', struct.pack('>II', 0, 1), struct.pack('>I', 8 + len(stsd_entry)) + (b'aprh' if hq else b'aprn') + stsd_entry)
        vstbl = atom(b'stbl', vstsd,
                     atom(b'stts', struct.pack('>III', 0, 1, nf) + struct.pack('>I', vdur)),
                     stsc([1] * nf),
                     atom(b'stsz', struct.pack('>III', 0, 0, nf), b''.join(struct.pack('>I', s) for s in v_sz)),
                     stco(v_off))
        vtrak = atom(b'trak',
                     atom(b'tkhd', struct.pack('>IIIIII', 0xF, 0, 0, 1, 0, mv_dur), b'\0' * 8,
                          struct.pack('>HHH', 0, 0, 0), b'\0\0', MATRIX, fixed16(w), fixed16(h)),
                     atom(b'mdia',
                          atom(b'mdhd', struct.pack('>IIIII', 0, 0, 0, vts, mv_dur), struct.pack('>HH', 0, 0)),
                          hdlr(b'mhlr', b'vide', 'Video'),
                          atom(b'minf', atom(b'vmhd', struct.pack('>I', 1), struct.pack('>HHHH', 0x40, 0x8000, 0x8000, 0x8000)),
                               hdlr(b'dhlr', b'alis', 'Data'), dinf(), vstbl)))
        traks = [vtrak]

        if audio:
            wf, ach, asw, asr, abpf, atotal = audio
            ad = sum(a_cnt)
            # SoundDescription v2, 'lpcm', little-endian signed packed integer
            sd = (b'\0' * 6 + struct.pack('>H', 1) + struct.pack('>HH', 2, 0) + b'\0' * 4 +
                  struct.pack('>HHhHI', 3, 16, -2, 0, 65536) + struct.pack('>I', 72) +
                  struct.pack('>d', float(asr)) + struct.pack('>I', ach) + struct.pack('>I', 0x7F000000) +
                  struct.pack('>I', asw * 8) + struct.pack('>I', 0x4 | 0x8) +
                  struct.pack('>I', abpf) + struct.pack('>I', 1))
            astsd = atom(b'stsd', struct.pack('>II', 0, 1), struct.pack('>I', 8 + len(sd)) + b'lpcm' + sd)
            astbl = atom(b'stbl', astsd,
                         atom(b'stts', struct.pack('>III', 0, 1, ad) + struct.pack('>I', 1)),
                         stsc(a_cnt),
                         atom(b'stsz', struct.pack('>III', 0, abpf, ad)),
                         stco(a_off))
            a_mvdur = round(ad * vts / asr)
            atrak = atom(b'trak',
                         atom(b'tkhd', struct.pack('>IIIIII', 0xF, 0, 0, 2, 0, a_mvdur), b'\0' * 8,
                              struct.pack('>HHH', 0, 1, 0x100), b'\0\0', MATRIX, fixed16(0), fixed16(0)),
                         atom(b'mdia',
                              atom(b'mdhd', struct.pack('>IIIII', 0, 0, 0, asr, ad), struct.pack('>HH', 0, 0)),
                              hdlr(b'mhlr', b'soun', 'Sound'),
                              atom(b'minf', atom(b'smhd', struct.pack('>IHH', 0, 0, 0)),
                                   hdlr(b'dhlr', b'alis', 'Data'), dinf(), astbl)))
            traks.append(atrak)
        extra = [meta_atom(metadata)] if metadata else []
        fh.write(atom(b'moov', mvhd, *extra, *traks))
    return nf

# ---------------------------------------------------------------- CLI
def _has_dng(d):
    try: return any(n.lower().endswith('.dng') for n in os.listdir(d))
    except OSError: return False

def find_clips(paths, depth=3):
    """Clip folders (folders that directly contain DNG frames) inside the given
    files/folders, searching a few levels down (e.g. a whole card)."""
    found = []
    def walk(d, lvl):
        if _has_dng(d): found.append(d); return
        if lvl <= 0: return
        try: subs = sorted(os.path.join(d, n) for n in os.listdir(d) if not n.startswith('.'))
        except OSError: return
        for sd in subs:
            if os.path.isdir(sd): walk(sd, lvl - 1)
    for p in paths:
        p = os.path.abspath(p.rstrip('/')) if p not in ('/',) else p
        if os.path.isfile(p) and p.lower().endswith('.dng'): p = os.path.dirname(p)
        if os.path.isdir(p): walk(p, depth)
    out = []
    for c in found:
        if c not in out: out.append(c)
    return out

CACHE = None
NEUTRAL = None
def _load(fp):
    f = DNGFrame(fp)
    if NEUTRAL is not None: f.neutral = np.array([NEUTRAL[0], 1.0, NEUTRAL[1]])
    return f
def _init_worker(cache, neutral):
    global CACHE, NEUTRAL                           # for 'spawn' (Windows, and macOS by default)
    CACHE, NEUTRAL = cache, neutral
def _encode_path(args):
    fp, scale, gain = args
    if CACHE:
        cp = os.path.join(CACHE, os.path.basename(fp) + f'.s{scale}.prr')
        if os.path.exists(cp):
            with open(cp, 'rb') as fh: data = fh.read()
            if os.environ.get('PRR_CONSUME_CACHE') == '1': os.remove(cp)
            return data, 0
        fr, cl = encode_frame(_load(fp), scale, gain=gain)
        with open(cp + '.tmp', 'wb') as fh: fh.write(fr)
        os.replace(cp + '.tmp', cp)
        return fr, cl
    return encode_frame(_load(fp), scale, gain=gain)

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('out')
    ap.add_argument('inputs', nargs='*')
    ap.add_argument('--wav')
    ap.add_argument('--scale', type=int, default=1, help='quality: 1 = near-lossless (default), 2 = high, 4 = standard (same tile quantiser as a Ninja V "HQ" recording), 8 = compact')
    ap.add_argument('--gain', type=float, default=None, help='override GainFactor (default 2^BaselineExposure)')
    ap.add_argument('--fps', default=None, help='override frame rate, e.g. 24000/1001')
    ap.add_argument('--limit', type=int, default=0, help='only encode the first N frames')
    ap.add_argument('--neutral', help='override the camera white balance: R/G,B/G of a grey card '
                    '(e.g. 0.737,0.367). Use when the fp\'s AsShotNeutral is wrong')
    ap.add_argument('--cache', help='folder for per-frame results (makes long encodes resumable)')
    ap.add_argument('--cache-only', action='store_true', help='only fill the cache, do not write the .mov')
    ap.add_argument('--progress-every', type=int, default=25, help='print progress every N frames')
    ap.add_argument('--list-clips', action='store_true', help='just print the clip folders found in the inputs')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 1, help='parallel encoder processes')
    a = ap.parse_args()
    global PROGRESS_EVERY
    PROGRESS_EVERY = max(1, a.progress_every)
    if a.list_clips:
        for c in find_clips([a.out] + a.inputs): print(c)
        return
    if not a.inputs:
        ap.error('give an output .mov and at least one clip folder or DNG file')
    global CACHE, NEUTRAL
    if a.neutral:
        NEUTRAL = tuple(float(x) for x in a.neutral.split(','))
    if a.cache:
        os.makedirs(a.cache, exist_ok=True); CACHE = a.cache

    files = []
    for p in a.inputs:
        if os.path.isdir(p):
            files += sorted(os.path.join(p, n) for n in os.listdir(p)
                            if n.lower().endswith('.dng') and not n.startswith('.'))
        else:
            files.append(p)
    if a.limit: files = files[:a.limit]
    if not files: sys.exit('no DNG files')
    sizes = set()
    for fp in files:
        with open(fp, 'rb') as fh:
            t = read_tiff_tags(fh)
        sizes.add((t.get('ImageWidth'), t.get('ImageLength')))
    if len(sizes) > 1:
        sys.exit(f'error: the DNGs in this folder have different sizes {sorted(sizes)} - not a single clip')
    first = DNGFrame(files[0])
    if a.neutral: first.neutral = np.array([float(a.neutral.split(',')[0]), 1.0, float(a.neutral.split(',')[1])])
    fps = tuple(int(x) for x in a.fps.split('/')) if a.fps else first.fps
    print(f'{len(files)} frame(s), {first.w}x{first.h}, white {first.white:g}, black {first.black:g}, '
          f'fps {fps[0]}/{fps[1]}, scale {a.scale}')
    wav = a.wav
    if wav is None and os.path.isdir(a.inputs[0]):
        w = sorted(n for n in os.listdir(a.inputs[0]) if n.lower().endswith('.wav') and not n.startswith('.'))
        wav = os.path.join(a.inputs[0], w[0]) if w else None
    if wav: print('audio:', wav)

    clamped_total = [0]
    def gen():
        if a.jobs > 1:
            import multiprocessing as mpc
            # fork where available (Linux, macOS); spawn on Windows. Workers get the
            # settings through the initializer so both behave the same.
            ctx = mpc.get_context('fork' if 'fork' in mpc.get_all_start_methods() else 'spawn')
            with ctx.Pool(a.jobs, initializer=_init_worker, initargs=(CACHE, NEUTRAL)) as pool:
                for fr, cl in pool.imap(_encode_path, [(fp, a.scale, a.gain) for fp in files], chunksize=1):
                    clamped_total[0] += cl
                    yield fr
        else:
            for fp in files:
                fr, cl = _encode_path((fp, a.scale, a.gain))
                clamped_total[0] += cl
                yield fr
    if a.cache_only:
        t0 = time.time()
        for i, _ in enumerate(gen()):
            if i % 50 == 0: print(f'  cached {i + 1}/{len(files)}  ({(i + 1) / (time.time() - t0):.2f} fps)', flush=True)
        print('cache complete'); return
    md = camera_metadata(first, (getattr(first, 'colour', None) or colour_params(first))[3], fps)
    write_mov(a.out, gen(), len(files), first.w, first.h, fps, wav, metadata=md,
              hq=tile_quantiser(a.scale) <= 15)
    print(f'wrote {a.out}  ({os.path.getsize(a.out) / 1e6:.1f} MB)  dc-clamps: {clamped_total[0]}')

if __name__ == '__main__':
    import multiprocessing
    multiprocessing.freeze_support()               # needed for a frozen Windows .exe
    main()
