#!/usr/bin/env python3
"""
selftest.py: checks that a build of dng2prr produces the same bytes as the
reference build, without needing Resolve or any camera files.

  python3 tests/selftest.py                 synthetic frames only
  python3 tests/selftest.py --dng DIR       also the fpSup public test frames
                                            (4K.DNG, FHD.DNG, OG2K.DNG, OG3K.DNG)
  python3 tests/selftest.py --update        rewrite reference.json (maintainers only)

What it checks
  1. The C core loads (libprr.dylib / .so / .dll next to dng2prr.py).
  2. Frame structure: 'prrf' header fields, tile quantiser per the quality ladder (never 1),
     tile sizes add up, 'eomd' trailer.
  3. Byte-exact output against reference.json (made on Linux x86-64, gcc -O2
     -ffp-contract=off, numpy 2.x). A mismatch is not necessarily wrong:
     different floating-point contraction (FMA) can flip a rare rounding.
     (We measured this: a clang build with FMA enabled differs in bytes from the
     reference on every frame, by under 0.2% in size.) Build with
     -ffp-contract=off (the build scripts do) to get identical bytes.
     If sizes agree to within 0.2% the build is almost certainly fine; confirm
     with tests/validation (Resolve) before sharing a new build.
"""
import argparse, hashlib, json, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'source'))
sys.argv, _argv = sys.argv[:1], sys.argv
import numpy as np
import dng2prr as D
sys.argv = _argv

REF = os.path.join(HERE, 'reference.json')
SRGB2XYZ = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])


class Synth:
    """Minimal stand-in for DNGFrame (12-bit, black 256, white 4095)."""
    def __init__(self, raw):
        self.raw = raw.astype(np.uint16); self.h, self.w = raw.shape; self.bits = 12
        self.black, self.white = 256.0, 4095.0
        self.baseline_exposure = 0.0; self.fps = (24000, 1001); self.crop = (0, 0, 0, 0)
        self.colour = (1.36, 2.72, SRGB2XYZ, 3200)      # fixed, so no colour maths is involved


def lcg_noise(h, w, seed):
    """Platform-independent pseudo-random integers 0..65535 (no numpy RNG involved)."""
    x = (np.arange(h * w, dtype=np.uint64) * np.uint64(2654435761) + np.uint64(seed)) & np.uint64(0xFFFFFFFF)
    for _ in range(3):
        x = (x * np.uint64(1103515245) + np.uint64(12345)) & np.uint64(0xFFFFFFFF)
    return ((x >> np.uint64(8)) & np.uint64(0xFFFF)).astype(np.int64).reshape(h, w)


def synth_frames():
    H, W = 272, 1000                                   # not multiples of 16: exercises padding
    yy, xx = np.mgrid[0:H, 0:W]
    out = {}
    out['ramp'] = 256 + (xx * (4095 - 256)) // (W - 1)
    out['noise'] = 256 + 800 + (lcg_noise(H, W, 1) % 241) - 120
    out['edges'] = np.where(((yy // 24) + (xx // 40)) % 2 == 1, 3900, 300)
    wedge = 256 + ((2 ** (xx * 11 // W)) * (4095 - 256)) // 2048
    out['wedge'] = np.clip(wedge + (lcg_noise(H, W, 7) % 17) - 8, 0, 4095)
    return {k: np.clip(v, 0, 4095) for k, v in out.items()}


def check_structure(fr, scale, w, h):
    errs = []
    size, tag = struct.unpack('>I4s', fr[:8])
    if tag != b'prrf' or size != len(fr) or size % 16: errs.append('bad frame size/tag')
    hl, = struct.unpack('>H', fr[8:10])
    if hl != 136: errs.append(f'header length {hl}')
    if fr[12:16] != D.VENDOR: errs.append('vendor')
    fw, fh = struct.unpack('>HH', fr[16:20])
    if (fw, fh) != (w, h): errs.append('dimensions')
    flags, = struct.unpack('>H', fr[8 + 70:8 + 72])
    if flags != 0x7: errs.append(f'flags {flags:#x}')
    W16, H16 = (w + 15) // 16, (h + 15) // 16
    per_row = 0; rem = W16
    for e in range(D.ALIGN, -1, -1):
        per_row += rem >> e; rem &= (1 << e) - 1
    nt = per_row * H16
    p = 8 + hl
    sizes = struct.unpack(f'>{nt}H', fr[p:p + 2 * nt]); p += 2 * nt
    for t, s in enumerate(sizes):
        if fr[p + 1] != D.tile_quantiser(scale) or fr[p + 1] < 2:
            errs.append(f'tile {t}: quantiser {fr[p + 1]}, expected {D.tile_quantiser(scale)}'); break
        p += s
    if fr[p + 4:p + 8] != b'eomd': errs.append('eomd trailer missing / tile sizes do not add up')
    return errs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dng', help='folder with the fpSup public test frames (4K/FHD/OG2K/OG3K.DNG)')
    ap.add_argument('--update', action='store_true')
    a = ap.parse_args()
    ref = json.load(open(REF)) if os.path.exists(REF) and not a.update else {}
    got, fails, near = {}, 0, 0
    cases = [(n, (lambda r=r: Synth(r))) for n, r in synth_frames().items()]
    if a.dng:
        for n in ('4K', 'FHD', 'OG2K', 'OG3K'):
            p = os.path.join(a.dng, n + '.DNG')
            if os.path.exists(p): cases.append((n + '.DNG', (lambda p=p: D.DNGFrame(p))))
            else: print(f'  (skipping {n}.DNG: not found)')
    for name, mk in cases:
        for scale in (1, 2, 4, 8):
            f = mk(); w, h = f.w, f.h
            fr, _ = D.encode_frame(f, scale, gain=1.0 if isinstance(f, Synth) else None)
            key = f'{name}@{scale}'
            got[key] = {'size': len(fr), 'sha256': hashlib.sha256(fr).hexdigest()}
            errs = check_structure(fr, scale, w, h)
            r = ref.get(key)
            if errs:
                status = 'FAIL ' + '; '.join(errs); fails += 1
            elif r is None:
                status = 'ok (no reference)'
            elif r['sha256'] == got[key]['sha256']:
                status = 'ok (identical)'
            elif abs(r['size'] - len(fr)) <= max(128, 0.002 * r['size']):
                status = f'ok-ish (bytes differ, size {len(fr)} vs {r["size"]})'; near += 1
            else:
                status = f'FAIL (size {len(fr)} vs reference {r["size"]})'; fails += 1
            print(f'{key:16s} {len(fr):9d} B  {status}')
    if a.update:
        json.dump(got, open(REF, 'w'), indent=1, sort_keys=True); print('reference.json written')
    print()
    if fails: print(f'{fails} FAILED'); sys.exit(1)
    print('All checks passed' + (f' ({near} not byte-identical; see the note at the top of this file)' if near else '') + '.')


if __name__ == '__main__':
    main()
