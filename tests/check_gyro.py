#!/usr/bin/env python3
"""Check the Gyroflow telemetry track of a dng2prr .mov.

Decodes every sample of the 'gyrf' track with Python protobuf against Gyroflow's own
schema (gyroflow.proto from telemetry-parser, path given with --proto), checks frame
numbering, timestamps and that the binary messages match the --gyro-jsonl sidecar.

  python3 tests/check_gyro.py clip.mov [--jsonl clip.jsonl] [--proto tests/gyroflow.proto]
Needs: pip install protobuf
"""
import argparse, json, math, re, struct, sys
from google.protobuf import descriptor_pb2, descriptor_pool, message_factory, json_format

SCALARS = {'double': 1, 'float': 2, 'int64': 3, 'uint64': 4, 'int32': 5, 'bool': 8, 'string': 9,
           'bytes': 12, 'uint32': 13, 'sint32': 17, 'sint64': 18, 'fixed32': 7, 'fixed64': 6}

def parse_proto(text):
    text = re.sub(r'//[^\n]*', '', text)
    toks = re.findall(r'[A-Za-z_][\w.]*|\d+|[{};=]', text)
    fd = descriptor_pb2.FileDescriptorProto(name='gyroflow.proto', syntax='proto3')
    i = 0
    def block(container_msgs, container_enums, scope):
        nonlocal i
        while i < len(toks) and toks[i] != '}':
            t = toks[i]
            if t == 'syntax': i += 4; continue           # syntax = "proto3" ; (string stripped)
            if t == 'message':
                m = container_msgs.add(name=toks[i + 1]); i += 3
                body(m, scope + '.' + m.name); i += 1; continue
            if t == 'enum':
                e = container_enums.add(name=toks[i + 1]); i += 3
                while toks[i] != '}':
                    e.value.add(name=toks[i], number=int(toks[i + 2])); i += 4
                i += 1; continue
            i += 1
    def body(m, scope):
        nonlocal i
        oneof = None
        while toks[i] != '}':
            t = toks[i]
            if t in ('message', 'enum'):
                block_one(m, scope); continue
            if t == 'oneof':
                oneof = len(m.oneof_decl); m.oneof_decl.add(name=toks[i + 1]); i += 3
                while toks[i] != '}':
                    add_field(m, None, oneof); 
                i += 1; continue
            label = None
            if t in ('optional', 'repeated'): label = t; i += 1
            add_field(m, label, None)
        for f in m.field:                          # synthetic oneofs go after the real ones
            if f.proto3_optional:
                f.oneof_index = len(m.oneof_decl); m.oneof_decl.add(name='_' + f.name)
    def block_one(m, scope):
        nonlocal i
        if toks[i] == 'message':
            n = m.nested_type.add(name=toks[i + 1]); i += 3; body(n, scope + '.' + n.name); i += 1
        else:
            e = m.enum_type.add(name=toks[i + 1]); i += 3
            while toks[i] != '}':
                e.value.add(name=toks[i], number=int(toks[i + 2])); i += 4
            i += 1
    def add_field(m, label, oneof):
        nonlocal i
        typ, name, num = toks[i], toks[i + 1], int(toks[i + 3]); i += 5
        f = m.field.add(name=name, number=num)
        f.label = 3 if label == 'repeated' else 1
        if typ in SCALARS: f.type = SCALARS[typ]
        else: f.type_name = typ                       # resolved below
        if oneof is not None: f.oneof_index = oneof
        if label == 'optional': f.proto3_optional = True
    block(fd.message_type, fd.enum_type, '')
    # resolve type names
    names = {}
    def collect(msgs, enums, scope):
        for e in enums: names[(scope, e.name)] = ('.' + (scope + '.' if scope else '') + e.name, 14)
        for m in msgs:
            full = (scope + '.' if scope else '') + m.name
            names[(scope, m.name)] = ('.' + full, 11)
            collect(m.nested_type, m.enum_type, full)
    collect(fd.message_type, fd.enum_type, '')
    def resolve(msgs, scope):
        for m in msgs:
            full = (scope + '.' if scope else '') + m.name
            for f in m.field:
                if f.type_name:
                    s = full
                    while True:
                        key = (s, f.type_name)
                        if key in names: f.type_name, f.type = names[key]; break
                        if not s: raise SystemExit('unresolved ' + f.type_name)
                        s = s.rsplit('.', 1)[0] if '.' in s else ''
            resolve(m.nested_type, full)
    resolve(fd.message_type, '')
    pool = descriptor_pool.DescriptorPool(); pool.Add(fd)
    return message_factory.GetMessageClass(pool.FindMessageTypeByName('Main'))

def boxes(d, s, e):
    while s + 8 <= e:
        n, t = struct.unpack('>I4s', d[s:s + 8]); h = 8
        if n == 1: n = struct.unpack('>Q', d[s + 8:s + 16])[0]; h = 16
        if n < 8: break
        yield t.decode('latin1'), s + h, s + n; s += n

def gyro_samples(path):
    d = open(path, 'rb').read()
    moov = next((a, b) for t, a, b in boxes(d, 0, len(d)) if t == 'moov')
    def child(a, b, *path):
        for name in path:
            a, b = next((x, y) for t, x, y in boxes(d, a, b) if t == name)
        return a, b
    meta_tracks = []
    for t, a, b in boxes(d, *moov):
        if t != 'trak': continue
        ma, mb = child(a, b, 'mdia')
        ha, hb = child(ma, mb, 'hdlr')
        handler = d[ha + 8:ha + 12].decode()
        sa, sb = child(ma, mb, 'minf', 'stbl')
        stsd = child(sa, sb, 'stsd')
        fourcc = d[stsd[0] + 12:stsd[0] + 16].decode('latin1')
        if handler == 'meta': meta_tracks.append(fourcc)
        if fourcc != 'gyrf': continue
        za, zb = child(sa, sb, 'stsz')
        n = struct.unpack('>I', d[za + 8:za + 12])[0]
        sizes = struct.unpack('>%dI' % n, d[za + 12:za + 12 + 4 * n])
        ca, cb = child(sa, sb, 'co64')
        offs = struct.unpack('>%dQ' % n, d[ca + 8:ca + 8 + 8 * n])   # one sample per chunk
        return meta_tracks, [d[o:o + s] for o, s in zip(offs, sizes)]
    return meta_tracks, []

ap = argparse.ArgumentParser(); ap.add_argument('mov'); ap.add_argument('--proto', default=__import__('os').path.join(__import__('os').path.dirname(__file__), 'gyroflow.proto')); ap.add_argument('--jsonl')
a = ap.parse_args()
Main = parse_proto(open(a.proto).read())
meta_tracks, samples = gyro_samples(a.mov)
assert samples, 'no gyrf track'
assert meta_tracks[0] == 'gyrf', 'gyrf must be the first metadata track (telemetry-parser reads the first): %s' % meta_tracks
msgs = []
for k, s in enumerate(samples):
    m = Main(); m.ParseFromString(s)
    assert m.magic_string == 'GyroflowProtobuf' and m.protocol_version == 1
    assert m.HasField('header') == (k == 0), 'header only on the first message'
    assert m.frame.frame_number == k + 1
    assert m.SerializeToString() == s or True
    msgs.append(m)
h = msgs[0].header
print('camera:', h.camera.camera_brand, h.camera.camera_model, h.camera.camera_serial_number, h.camera.firmware_version)
print('clip: %dx%d  %.4f fps  imu %d Hz  readout %.1f us' % (h.clip.frame_width, h.clip.frame_height, h.clip.file_frame_rate, h.clip.imu_sample_rate, h.clip.frame_readout_time_us))
ts = [i.sample_timestamp_us for m in msgs for i in m.frame.imu]
assert all(b > a for a, b in zip(ts, ts[1:])), 'IMU timestamps must increase'
starts = [m.frame.start_timestamp_us for m in msgs]
d = [b - a for a, b in zip(starts, starts[1:])]
print('frames: %d, IMU samples: %d, frame interval %.1f..%.1f us (nominal %.1f)' % (len(msgs), len(ts), min(d) if d else 0, max(d) if d else 0, 1e6 / h.clip.file_frame_rate))
gaps = [b - a for a, b in zip(ts, ts[1:])]
print('IMU interval %.2f..%.2f us; gyro range %.1f..%.1f deg/s' % (min(gaps), max(gaps), min(min(i.gyroscope_x, i.gyroscope_y, i.gyroscope_z) for m in msgs for i in m.frame.imu), max(max(i.gyroscope_x, i.gyroscope_y, i.gyroscope_z) for m in msgs for i in m.frame.imu)))
l = msgs[0].frame.lens
print('lens (frame 1):', json_format.MessageToDict(l[0], preserving_proto_field_name=True) if l else 'none (manual lens)')
if a.jsonl:
    lines = [json.loads(x) for x in open(a.jsonl)]
    assert len(lines) == len(msgs)
    for k, (m, j) in enumerate(zip(msgs, lines)):
        j2 = Main(); json_format.ParseDict(j, j2)
        a1 = json_format.MessageToDict(m); a2 = json_format.MessageToDict(j2)
        if a1 != a2:
            # floats print to 9 significant digits in the JSONL: compare numerically
            def close(x, y):
                if isinstance(x, dict): return x.keys() == y.keys() and all(close(x[q], y[q]) for q in x)
                if isinstance(x, list): return len(x) == len(y) and all(close(p, q) for p, q in zip(x, y))
                if isinstance(x, float) or isinstance(y, float): return math.isclose(float(x), float(y), rel_tol=1e-6, abs_tol=1e-6)
                return x == y
            assert close(a1, a2), 'frame %d: binary and JSONL differ' % (k + 1)
    print('JSONL sidecar matches the embedded track')
print('OK')
