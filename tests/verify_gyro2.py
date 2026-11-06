#!/usr/bin/env python3
"""verify_gyro2.py: read each DNG's fpSup Gyro2 block the way the Gyroflow (fpSup) OpenFX plugin v0.1.10 does
(common/src/cdng.rs read_frame: the SIGMA MakerNote's own tag 0x4653, block version 2, else the MakerNote-tail
block, version 1 with CRC), so a re-written clip can be checked without Resolve.

  verify_gyro2.py CLIP_FOLDER [REFERENCE_FOLDER]
With a reference folder, each frame's payload must equal the reference frame's (same file name).
Test tool for cDNG Raw Convert (fpSup community). LGPL-2.1-or-later.
"""
import os, struct, sys, zlib

def ifd(f, off, e):
    f.seek(off); n = struct.unpack(e + 'H', f.read(2))[0]
    return [struct.unpack(e + 'HHI4s', f.read(12)) for _ in range(n)]

TS = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8}

def value(f, e, ent):
    t, ty, c, raw = ent; n = TS.get(ty, 0) * c
    if n <= 4: return raw[:n]
    f.seek(struct.unpack(e + 'I', raw)[0]); return f.read(n)

def read_frame(path):
    with open(path, 'rb') as f:
        h = f.read(8); e = '<' if h[:2] == b'II' else '>'
        root = ifd(f, struct.unpack(e + 'I', h[4:])[0], e)
        if any(x[0] == 65000 for x in root): return 'tag65000', None
        ex = [x for x in root if x[0] == 0x8769]
        if not ex: raise ValueError('no Exif IFD')
        exif = ifd(f, struct.unpack(e + 'I', ex[0][3])[0], e)
        mn = [x for x in exif if x[0] == 0x927C]
        if not mn: raise ValueError('no MakerNote')
        mn = mn[0]
        if mn[2] >= 16 and mn[1] in (1, 7):
            start = struct.unpack(e + 'I', mn[3])[0]; f.seek(start)
            if f.read(8) == b'SIGMA\0\0\0':
                try: sub = ifd(f, start + 10, e)
                except Exception: sub = None
                if sub is not None:
                    g = [x for x in sub if x[0] == 0x4653]
                    if len(g) > 1: raise ValueError('more than one Gyro2 MakerNote tag')
                    if g:
                        v = value(f, e, g[0]); pad = (-struct.unpack(e + 'I', g[0][3])[0]) % 4; b = v[pad:]
                        if len(b) < 12: raise ValueError('Gyro2 MakerNote tag is too short for a block')
                        if b[:4] != b'FSG2': raise ValueError('no telemetry block in the Gyro2 MakerNote tag')
                        if b[4] != 2: raise ValueError('unsupported block version %d in the Gyro2 MakerNote tag' % b[4])
                        if b[5] != 2: raise ValueError('unsupported block payload kind %d' % b[5])
                        ln = struct.unpack('<I', b[8:12])[0]
                        if not ln or 12 + ln > len(b): raise ValueError('block payload length is invalid or exceeds the tag')
                        return 'tag', b[12:12 + ln]
        data = value(f, e, mn); at = 0; found = None; rej = None
        while True:
            at = data.find(b'FSG2', at)
            if at < 0: break
            b = data[at:]
            try:
                if b[4] != 1: raise ValueError('unsupported block version %d' % b[4])
                ln = struct.unpack('<I', b[8:12])[0]
                if zlib.crc32(b[:12 + ln]) != struct.unpack('<I', b[12 + ln:16 + ln])[0]: raise ValueError('CRC mismatch')
                if found: raise RuntimeError('more than one block')
                found = ('tail', b[12:12 + ln])
            except (ValueError, struct.error, IndexError) as x: rej = str(x)
            at += 1
        if found: return found
        raise ValueError('invalid telemetry block in MakerNote: ' + rej if rej else 'no telemetry block in MakerNote')

def main():
    d = sys.argv[1]; ref = sys.argv[2] if len(sys.argv) > 2 else None
    fs = sorted(x for x in os.listdir(d) if x.lower().endswith('.dng') and not x.startswith('._'))
    ok = bad = diff = 0; carriers = set(); first_err = None; vers = set()
    for x in fs:
        try:
            c, p = read_frame(os.path.join(d, x)); carriers.add(c); vers.add(p[0] if p else None); ok += 1
            if ref:
                c2, p2 = read_frame(os.path.join(ref, x))
                if p != p2: diff += 1
        except Exception as ex:
            bad += 1; first_err = first_err or '%s: %s' % (x, ex)
    print('%s: %d frames, %d with a readable Gyro2 block (%s, payload version %s), %d failed%s%s' % (
        os.path.basename(d.rstrip('/')), len(fs), ok, '/'.join(sorted(carriers)) or '-', '/'.join(str(v) for v in sorted(v for v in vers if v is not None)) or '-', bad,
        ', %d differ from the reference' % diff if ref else '', '; first error: ' + first_err if first_err else ''))
    return 1 if bad or diff else 0

if __name__ == '__main__':
    sys.exit(main())
