#!/usr/bin/env python3
"""
measure_still.py: measures a still exported from a ProRes RAW decoder showing
one of the wedge_q*.mov clips, and says whether the decode is linear and neutral.

  python3 measure_still.py STILL.tif [STILL2.tif ...]

The still must be a LINEAR export (see README.md here: in Resolve, a colour-
managed project with Rec.709 Linear timeline and output gamma). 8- or 16-bit
uncompressed TIFF is read directly; other formats need the tifffile or Pillow
package. If the still isn't 1920x1080 but has the same 16:9 framing (e.g. a
3840x2160 timeline), patch positions are scaled.

Pass criteria (patches from 1% to 90% of white), each with an allowance of
1.5 code steps of a 10-bit export, because Resolve's 16-bit TIFF stills carry
only about 10 bits (values come in steps of 1/1024):
  linearity   G / level constant within +-3%
  neutrality  R and B within +-2% of G

A correct decode in Resolve 21.1.1 measured: scale about 1.11, worst linearity
about 2.6% (all from 10-bit steps in the darkest judged patches), R/G and B/G
exactly 1.000 from 5% up, and the same numbers at all four quality levels.
The 0.1-pre magenta bug showed up as R/G and B/G of 1.5 to 2.9.
"""
import os, struct, sys
import numpy as np

N, LEVELS = 32, (np.logspace(-10, 0, 32, base=2) * 0.9)


def read_tiff(path):
    with open(path, 'rb') as fh: d = fh.read()
    e = {b'II': '<', b'MM': '>'}.get(d[:2])
    if e is None or struct.unpack(e + 'H', d[2:4])[0] != 42: return None
    off, = struct.unpack(e + 'I', d[4:8]); n, = struct.unpack(e + 'H', d[off:off + 2])
    tags = {}
    for i in range(n):
        code, typ, cnt, val = struct.unpack(e + 'HHI4s', d[off + 2 + 12 * i: off + 14 + 12 * i])
        sz = {3: 2, 4: 4}.get(typ)
        if sz is None: continue
        fmt = e + ('H' if sz == 2 else 'I') * cnt
        raw = val[:sz * cnt] if sz * cnt <= 4 else d[struct.unpack(e + 'I', val)[0]:][:sz * cnt]
        tags[code] = struct.unpack(fmt, raw)
    w, h = tags[256][0], tags[257][0]
    bps = tags.get(258, (8,))[0]; spp = tags.get(277, (1,))[0]
    if tags.get(259, (1,))[0] != 1 or tags.get(284, (1,))[0] != 1 or bps not in (8, 16): return None
    buf = b''.join(d[o:o + c] for o, c in zip(tags[273], tags[279]))
    a = np.frombuffer(buf, (e + 'u2') if bps == 16 else np.uint8)[:w * h * spp].reshape(h, w, spp)
    return a[..., :3].astype(np.float64) / (65535.0 if bps == 16 else 255.0)


def load(path):
    img = read_tiff(path) if path.lower().endswith(('.tif', '.tiff')) else None
    if img is not None: return img
    try:
        import tifffile; a = tifffile.imread(path)
    except Exception:
        from PIL import Image; a = np.array(Image.open(path))
    a = a[..., :3].astype(np.float64)
    return a / (65535.0 if a.max() > 255 else 255.0)


def measure(path):
    img = load(path); h, w = img.shape[:2]
    if abs(w / h - 16 / 9) > 0.01:
        print(f'{os.path.basename(path)}: {w}x{h} is not 16:9; export the still at the clip\'s framing'); return False
    sx = w / 1920.0
    y0, y1 = int(h * 340 / 1080), int(h * 740 / 1080)
    rgb = np.array([img[y0:y1, int((60 * i + 15) * sx):int((60 * i + 45) * sx)].reshape(-1, 3).mean(0) for i in range(N)])
    good = LEVELS >= 0.01
    ratio = rgb[:, 1] / LEVELS
    k = np.median(ratio[good])
    lin_dev = (ratio / k - 1) * 100
    rg, bg = rgb[:, 0] / rgb[:, 1], rgb[:, 2] / rgb[:, 1]
    print(f'\n{os.path.basename(path)}  ({w}x{h})')
    print('  patch  level     R        G        B       G/level (norm)  R/G    B/G')
    for i in range(N):
        mark = '' if good[i] else '   (below 1%, not judged)'
        print(f'  {i:2d}  {LEVELS[i]:7.4f}  {rgb[i,0]:.5f}  {rgb[i,1]:.5f}  {rgb[i,2]:.5f}  {ratio[i]/k:8.3f}       '
              f'{rg[i]:.3f}  {bg[i]:.3f}{mark}')
    lsb = 1.5 / 1023.0
    exp_g = LEVELS * k
    lin_ok = np.all(np.abs(rgb[good, 1] - exp_g[good]) <= 0.03 * exp_g[good] + lsb)
    tol = 0.02 * rgb[good, 1] + lsb
    neu_ok = np.all(np.abs(rgb[good, 0] - rgb[good, 1]) <= tol) and np.all(np.abs(rgb[good, 2] - rgb[good, 1]) <= tol)
    print(f'  scale (output per unit level): {k:.4f}')
    print(f'  linearity: worst {np.abs(lin_dev[good]).max():.2f}% (10-bit steps allowed)  -> {"PASS" if lin_ok else "FAIL"}')
    print(f'  neutrality: R/G {rg[good].min():.3f}..{rg[good].max():.3f}, B/G {bg[good].min():.3f}..{bg[good].max():.3f}'
          f' (10-bit steps allowed)  -> {"PASS" if neu_ok else "FAIL"}')
    if rgb[:, 1].max() > 0.995:
        print('  note: brightest patch is near 1.0; if it reads exactly 1.0 the export is clipping')
    return lin_ok and neu_ok, k


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    res = [(p, measure(p)) for p in sys.argv[1:]]
    ks = [r[1][1] for r in res if isinstance(r[1], tuple)]
    print()
    for p, r in res:
        print(f'{"PASS" if isinstance(r, tuple) and r[0] else "FAIL"}  {os.path.basename(p)}')
    if len(ks) > 1:
        spread = (max(ks) / min(ks) - 1) * 100
        print(f'brightness scale across stills differs by {spread:.2f}% '
              f'({"PASS" if spread <= 2 else "FAIL"}: all quality levels should decode alike)')


if __name__ == '__main__':
    main()
