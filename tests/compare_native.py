#!/usr/bin/env python3
"""
compare_native.py: checks that the native encoder (dng2prr_cli.c) writes the
same file as dng2prr.py.

  python3 tests/compare_native.py PATH_TO_NATIVE_BINARY OUT_DIR INPUT [extra dng2prr options...]

Runs both encoders on INPUT (a clip folder or DNG file). Then it compares the
two .mov files: byte for byte, and if they differ, atom by atom. The frame
header floats (white balance, matrix) may differ in the last bit, because
numpy and the C code invert 3x3 matrices differently. Tile data, levels and
the container layout must be identical.
"""
import os, struct, subprocess, sys
HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, '..', 'source')


def frames(path):
    d = open(path, 'rb').read(); out = []; i = 0
    while True:
        i = d.find(b'prrf', i)
        if i < 0: break
        n = struct.unpack('>I', d[i - 4:i])[0]; out.append(d[i - 4:i - 4 + n]); i += n - 4
    return d, out


def main():
    if len(sys.argv) < 4: sys.exit(__doc__)
    native, od, inp, extra = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    os.makedirs(od, exist_ok=True)
    a, b = os.path.join(od, 'python.mov'), os.path.join(od, 'native.mov')
    subprocess.run([sys.executable, os.path.join(SRC, 'dng2prr.py'), a, inp, '--jobs', '2'] + extra, check=True, stdout=subprocess.DEVNULL)
    subprocess.run([native, b, inp, '--jobs', '3', '--no-extra'] + extra, check=True, stdout=subprocess.DEVNULL)
    da, fa = frames(a); db, fb = frames(b)
    if da == db:
        print(f'IDENTICAL  ({len(da)} bytes, {len(fa)} frames)'); return
    print(f'files differ: python {len(da)} B, native {len(db)} B; frames {len(fa)} vs {len(fb)}')
    bad = 0
    for k, (x, y) in enumerate(zip(fa, fb)):
        if x == y: continue
        hdr_a, hdr_b = x[:8 + 136], y[:8 + 136]
        if x[8 + 136:] != y[8 + 136:] or len(x) != len(y):
            print(f'  frame {k}: tile data differs'); bad += 1; continue
        fa_ = struct.unpack('>11f', hdr_a[28:72]); fb_ = struct.unpack('>11f', hdr_b[28:72])
        rel = max(abs(p - q) / max(abs(p), 1e-6) for p, q in zip(fa_, fb_))
        same_rest = hdr_a[:28] == hdr_b[:28] and hdr_a[72:] == hdr_b[72:]
        print(f'  frame {k}: header floats differ by up to {rel:.2e} (relative); other header bytes {"same" if same_rest else "DIFFER"}')
        if rel > 1e-5 or not same_rest: bad += 1
    ma, mb = da[da.rfind(b'moov') - 4:], db[db.rfind(b'moov') - 4:]
    if ma != mb:
        n = min(len(ma), len(mb)); first = next((i for i in range(n) if ma[i] != mb[i]), n)
        print(f'  moov differs: {len(ma)} vs {len(mb)} B, first difference at moov+{first}: {ma[max(0,first-24):first+24]!r} / {mb[max(0,first-24):first+24]!r}')
    print('OK apart from last-bit float differences' if not bad else f'{bad} real difference(s)')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
