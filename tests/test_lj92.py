#!/usr/bin/env python3
"""Round-trip test for dng_lj92.c: encodes images with a tiny lossless-JPEG
encoder written here from the spec (all 7 predictors, 1-4 components, point
transform, restart intervals) and checks the C decoder returns them exactly."""
import ctypes, os, sys, struct
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'source')); sys.argv = sys.argv[:1]
import dng2prr as D
lib = D._lib

def predict(img, y, x, c, nc, pred, initial, first_line):
    if first_line: return initial if x == 0 else int(img[y, (x - 1) * nc + c])
    if x == 0: return int(img[y - 1, c])
    ra, rb, rc = int(img[y, (x - 1) * nc + c]), int(img[y - 1, x * nc + c]), int(img[y - 1, (x - 1) * nc + c])
    return {1: ra, 2: rb, 3: rc, 4: ra + rb - rc, 5: ra + ((rb - rc) >> 1), 6: rb + ((ra - rc) >> 1), 7: (ra + rb) >> 1}[pred]

def encode(img, nc, prec=16, pred=1, pt=0, ri=0):
    H, rowlen = img.shape; W = rowlen // nc
    v = img >> pt
    # Huffman: all 17 categories at length 5 (valid canonical code)
    counts = [0] * 16; counts[4] = 17
    codes = {s: s for s in range(17)}              # canonical: length-5 codes 0..16
    bits = []
    def put(val, n):
        for i in range(n - 1, -1, -1): bits.append((val >> i) & 1)
    out = bytearray()
    def flush_bytes():
        while len(bits) % 8: bits.append(1)
        for i in range(0, len(bits), 8):
            b = int(''.join(map(str, bits[i:i + 8])), 2); out.append(b)
            if b == 0xFF: out.append(0)
        bits.clear()
    initial = 1 << (prec - pt - 1); mcu = 0; rst = 0; first_line = True
    for y in range(H):
        for x in range(W):
            if ri and mcu == ri:
                flush_bytes(); out += bytes([0xFF, 0xD0 + (rst & 7)]); rst += 1; mcu = 0; first_line = True
            for c in range(nc):
                p = predict(v, y, x, c, nc, pred, initial, first_line)
                d = (int(v[y, x * nc + c]) - p) & 0xFFFF
                if d >= 32768: d -= 65536
                if d == -32768: put(codes[16], 5); continue
                s = 0 if d == 0 else int(abs(d)).bit_length()
                put(codes[s], 5)
                if s: put(d if d > 0 else d + (1 << s) - 1, s)
            mcu += 1
        first_line = False
    flush_bytes()
    hdr = b'\xff\xd8'
    sof = struct.pack('>BHHB', prec, H, W, nc) + b''.join(bytes([i, 0x11, 0]) for i in range(nc))
    hdr += b'\xff\xc3' + struct.pack('>H', 2 + len(sof)) + sof
    dht = bytes([0]) + bytes(counts) + bytes(range(17))
    hdr += b'\xff\xc4' + struct.pack('>H', 2 + len(dht)) + dht
    if ri: hdr += b'\xff\xdd' + struct.pack('>HH', 4, ri)
    sos = bytes([nc]) + b''.join(bytes([i, 0]) for i in range(nc)) + bytes([pred, 0, pt])
    hdr += b'\xff\xda' + struct.pack('>H', 2 + len(sos)) + sos
    return hdr + bytes(out) + b'\xff\xd9'

def main():
    rng = np.random.default_rng(3); fails = 0; n = 0
    for nc in (1, 2, 4):
        for pred in range(1, 8):
            for pt, ri in ((0, 0), (2, 0), (0, 24)):
                H, W = 16, 24
                base = np.cumsum(np.cumsum(rng.integers(-40, 41, (H, W * nc)), 0), 1) + 30000
                img = (np.clip(base, 0, 65535).astype(np.uint16) >> pt << pt)
                if pt == 0 and pred == 1 and nc == 1: img[3, 5] = 0; img[4, 6] = 65535   # extreme jumps (category 16)
                data = encode(img, nc, 16, pred, pt, ri)
                out = np.zeros((H, W * nc), np.uint16)
                rc = lib.dng_lj92_decode(data, len(data), out.ctypes.data, W * nc, H, W * nc)
                ok = rc == 0 and np.array_equal(out, img); n += 1
                if not ok: fails += 1; print(f'FAIL nc={nc} pred={pred} pt={pt} ri={ri} rc={rc} mismatches={(out != img).sum()}')
    print(f'{n - fails}/{n} lossless-JPEG round trips exact')
    sys.exit(1 if fails else 0)

if __name__ == '__main__':
    main()
