#!/usr/bin/env python3
"""
make_validation_clips.py: writes test clips for checking a build against a real
ProRes RAW decoder (DaVinci Resolve, Final Cut Pro, ...). See README.md here.

  python3 make_validation_clips.py OUT_FOLDER

Writes wedge_q1.mov, wedge_q2.mov, wedge_q4.mov, wedge_q8.mov (one frame each,
1920x1080, the four quality levels) and wedge.json (what each patch should be).

The picture is 32 vertical grey patches from 0.09% to 90% of white, in log steps.
The raw data is written as if shot under tungsten light: red and blue are
scaled down by the white-balance gains stored in the header (1.36 and 2.72),
so a correct decoder shows neutral grey patches whose brightness is
proportional to their level. A decoder that reads our code values wrongly
shows tinted patches (that's how the 0.1-pre magenta bug appeared) and/or a
non-constant brightness ratio.
"""
import json, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'source'))
_argv = sys.argv; sys.argv = sys.argv[:1]
import numpy as np
import dng2prr as D
sys.argv = _argv

W, H, N = 1920, 1080, 32
PATCH = W // N                                    # 60 px: even, so every patch has whole Bayer quads
WB_R, WB_B = 1.36, 2.72
SRGB2XYZ = np.array([[0.4124, 0.3576, 0.1805], [0.2126, 0.7152, 0.0722], [0.0193, 0.1192, 0.9505]])
LEVELS = (np.logspace(-10, 0, N, base=2) * 0.9).tolist()


class Frame:
    def __init__(self, raw):
        self.raw = raw; self.h, self.w = raw.shape; self.bits = 12
        self.black, self.white = 256.0, 4095.0
        self.baseline_exposure = 0.0; self.fps = (24000, 1001); self.crop = (0, 0, 0, 0)
        self.colour = (WB_R, WB_B, SRGB2XYZ, 3200)


def wedge_raw():
    lev = np.repeat(np.array(LEVELS), PATCH)[None, :].repeat(H, 0)
    span = 4095.0 - 256.0
    raw = 256 + lev * span                         # green sites
    raw[0::2, 0::2] = 256 + lev[0::2, 0::2] * span / WB_R   # red sites (RGGB)
    raw[1::2, 1::2] = 256 + lev[1::2, 1::2] * span / WB_B   # blue sites
    return np.round(raw).astype(np.uint16)


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    od = sys.argv[1]; os.makedirs(od, exist_ok=True)
    for scale in (1, 2, 4, 8):
        fr, _ = D.encode_frame(Frame(wedge_raw()), scale, gain=1.0)
        p = os.path.join(od, f'wedge_q{scale}.mov')
        D.write_mov(p, iter([fr]), 1, W, H, (24000, 1001), None, log=None)
        print('wrote', p)
    json.dump({'width': W, 'height': H, 'patches': N, 'patch_width': PATCH, 'levels': LEVELS,
               'wb_gains': [WB_R, WB_B], 'note': 'level = fraction of white; R,G,B should be equal'},
              open(os.path.join(od, 'wedge.json'), 'w'), indent=1)


if __name__ == '__main__':
    main()
