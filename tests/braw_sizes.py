#!/usr/bin/env python3
"""braw_sizes.py: put dng2braw's output at many frame sizes through Blackmagic's own decoder.

  braw_sizes.py DNG2BRAW BRAW_VIEW CLIP_FOLDER [WxH ...]

A size is WxH, or WxH+L+T to also set the left and top crop margins.
For each size, dng2braw's test hook (DNG2BRAW_TEST_SIZE) crops or repeats the clip's frames to that size, the
result is decoded by the Blackmagic RAW SDK (through cDNG Raw Convert's braw_view helper), and the picture is
compared with the decode of the same clip at its own size: where both show the same photosites they must agree
(apart from compression). A layout the decoder reads differently shows up as a large difference or an error.
Needs macOS with Blackmagic RAW installed. Test tool for cDNG Raw Convert (fpSup community). LGPL-2.1-or-later.
"""
import json, os, re, struct, subprocess, sys, tempfile

def decode(view, path, frame=0):
    p = subprocess.Popen([view, '--cpu', '--gen', '5', '--gamma', 'Blackmagic Design Film', '--gamut', 'Blackmagic Design', path],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    info = json.loads(p.stdout.readline())
    if 'error' in info: p.kill(); return info, None, 0, 0
    p.stdin.write(b'f %d 1\n' % frame); p.stdin.flush()
    hd = p.stdout.read(16)
    if len(hd) < 16: p.wait(); return {'error': 'the decoder process ended (exit %d)' % p.returncode}, None, 0, 0
    if hd[:4] != b'BRVF':
        p.stdin.write(b'q\n'); p.stdin.flush(); p.wait(); return {'error': 'decode failed 0x%08X' % struct.unpack('<I', hd[8:12])[0]}, None, 0, 0
    _, w, h = struct.unpack('<III', hd[4:]); px = p.stdout.read(w * h * 4)
    p.stdin.write(b'q\n'); p.stdin.flush(); p.wait()
    return info, px, w, h

def main():
    enc, view, clip = sys.argv[1:4]
    sizes = sys.argv[4:] or ['1936x1090', '2016x1340', '2048x1080', '3000x2000', '3032x2020', '3856x2170', '4112x2170',
                             '4496x3000', '6000x4000', '6016x4010', '1000x562', '640x480', '258x146', '64x64']
    tmp = tempfile.mkdtemp(prefix='brawsizes')
    def encode(size, out):
        env = dict(os.environ)
        if size: env['DNG2BRAW_TEST_SIZE'] = size
        r = subprocess.run([enc, out, clip, '--limit', '2', '--quality', 'Q0', '--no-audio', '--no-gyro'], env=env, capture_output=True, text=True)
        return r.returncode, (r.stdout + r.stderr)
    def origin(log):                                  # "picture WxH from DNG photosite (x,y)" in dng2braw's log
        m = re.search(r'from DNG photosite \((\d+),(\d+)\)', log); return (int(m.group(1)), int(m.group(2))) if m else (0, 0)
    ref = os.path.join(tmp, 'ref.braw'); rc, log = encode(None, ref)
    if rc: print('reference encode failed:', log[-400:]); return 1
    ri, rpx, rw, rh = decode(view, ref); r0 = origin(log)
    if rpx is None: print('reference decode failed:', ri); return 1
    print('reference: picture %dx%d from DNG photosite (%d,%d)' % (rw, rh, r0[0], r0[1])); bad = 0
    for s in sizes:
        out = os.path.join(tmp, s + '.braw'); rc, log = encode(s, out)
        if rc: print('%-10s ENCODE FAILED (%d): %s' % (s, rc, log.strip().split('\n')[-1][:160])); bad += 1; continue
        lay = [l for l in log.split('\n') if l.startswith('coded ')]
        i, px, w, h = decode(view, out)
        if px is None: print('%-10s DECODER REFUSED: %s   [%s]' % (s, i.get('error'), lay[0][:110] if lay else '')); bad += 1; continue
        # frame 1 as well: must decode
        i1, px1, _, _ = decode(view, out, 1)
        # where does the decoder's picture start? Find the shift (ox, oy) with test(x, y) = reference(x + ox, y + oy)
        def diff(ox, oy, sx, sy):
            n = tot = mx = 0
            for y in range(40, min(h, rh - oy) - 40, sy):
                if y + oy < 0: continue
                ro = (y + oy) * rw * 4; to = y * w * 4
                for x in range(40, min(w, rw - ox) - 40, sx):
                    for c in range(3):
                        d = abs(px[to + 4 * x + c] - rpx[ro + 4 * (x + ox) + c]); tot += d; n += 1
                        if d > mx: mx = d
            return (tot / n if n else 99), mx, n
        # the shift the encoder says the picture has against the reference, and the one the decoder really shows
        t0 = origin(log); want = (t0[0] - r0[0], t0[1] - r0[1])
        best = min(((diff(a, b, 61, 47)[0], abs(a - want[0]) + abs(b - want[1]), a, b) for a in range(-16, 26, 2) for b in range(-16, 26, 2)
                    if (a - want[0]) % 2 == 0 and (b - want[1]) % 2 == 0))
        ox, oy = best[2], best[3]
        if want[0] % 2 or want[1] % 2: ox, oy = want       # an odd shift changes the CFA colours under each pixel: compare as told
        mean, mx, n = diff(ox, oy, 7, 5)
        shift = ' from (%d,%d)' % t0 + ('' if (ox, oy) == want else ' BUT THE DECODER SHOWS (%d,%d)' % (r0[0] + ox, r0[1] + oy))
        ok = px1 is not None and (ox, oy) == want and (n == 0 or (mean < 1.5 and mx < 48))
        print('%-10s %s picture %dx%d,%s vs reference: mean %.3f max %d of 255 over %d samples%s   [%s]' % (
            s, 'ok  ' if ok else 'BAD ', w, h, shift, mean, mx, n, '' if px1 is not None else ', FRAME 1 FAILED', lay[0][:120] if lay else ''))
        if not ok: bad += 1
        os.remove(out)
    print('%d size(s) failed' % bad if bad else 'all sizes decode correctly')
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main())
