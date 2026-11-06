// cDNG Raw Convert: a native macOS front end for the fpSup encoders
// (SIGMA fp CinemaDNG -> Apple ProRes RAW, Blackmagic RAW, or compressed CinemaDNG).
// Part of the fpSup community tools. LGPL-2.1-or-later. Experimental; not Apple or Blackmagic Design software.
//
// Layout (0.6 UI mockup "Option A", revised in 0.6.1-0.6.11): one dark, dense page in the style of a colour/edit suite.
//   left    Media Storage (volumes, cards, folders; click a clip to view it) above the Job Queue
//   centre  Viewer: plays DNG clips (dng_view helper), .braw (Blackmagic RAW SDK via braw_view) and .mov
//           (AVFoundation) with sound, through an optional monitoring LUT and de-squeeze; the Log below.
//           The histogram is a floating palette. Space / arrows control playback unless a text field is being edited.
//   right   Output settings or the selected clip's Metadata
// Every panel boundary is a splitter; sizes are remembered.
// The encoding is done by the bundled command-line tools: `dng2prr` (ProRes RAW, CinemaDNG) and
// `dng2braw` (Blackmagic RAW; `--probe` describes a clip for Media Storage and the viewer).
//
// Built by "Build cDNG Raw Convert.command" with swiftc (Xcode command-line tools).
// No @State / @Observable: in recent SDKs those are Swift macros, and the command-line tools' swiftc lacks the
// SwiftUIMacros plugin. @StateObject, @ObservedObject, @Published, @AppStorage and @Binding are plain wrappers.

import SwiftUI
import AppKit
import IOSurface
import CoreImage
import AVFoundation
import ImageIO
import UniformTypeIdentifiers
import CoreVideo

let appVersion = "0.6.11"

// MARK: - Theme

extension Color {
    init(hex: UInt32) { self.init(.sRGB, red: Double((hex >> 16) & 255) / 255, green: Double((hex >> 8) & 255) / 255, blue: Double(hex & 255) / 255, opacity: 1) }
}
enum Th {
    static let bg = Color(hex: 0x141416), win = Color(hex: 0x1c1c1e), panel = Color(hex: 0x232326), panel2 = Color(hex: 0x28282c)
    static let head = Color(hex: 0x2c2c30), line = Color(hex: 0x0f0f11), line2 = Color(hex: 0x38383d), field = Color(hex: 0x1a1a1d)
    static let txt = Color(hex: 0xcfcfd2), dim = Color(hex: 0x8b8b92), dim2 = Color(hex: 0x66666c), sel = Color(hex: 0x3a3a40)
    static let acc = Color(hex: 0xe8862e), acc2 = Color(hex: 0xffb066), red = Color(hex: 0xe5483a), ok = Color(hex: 0x59b36b)
    static let viewerBg = Color(hex: 0x101012)
    static func f(_ size: CGFloat, _ w: Font.Weight = .regular) -> Font { .system(size: size, weight: w) }
    static let mono = Font.system(size: 11, design: .monospaced)
}

struct PanelHeader<Trailing: View>: View {
    let title: String
    var subtitle: String = ""
    @ViewBuilder var trailing: () -> Trailing
    var body: some View {
        HStack(spacing: 8) {
            Text(title).font(Th.f(11, .semibold)).foregroundColor(Color(hex: 0xe3e3e6))
            if !subtitle.isEmpty { Text(subtitle).font(Th.f(11)).foregroundColor(Th.dim).lineLimit(1) }
            Spacer(minLength: 4)
            trailing()
        }
        .padding(.horizontal, 10).frame(height: 28).background(Th.head)
        .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .bottom)
    }
}
extension PanelHeader where Trailing == EmptyView {
    init(title: String, subtitle: String = "") { self.title = title; self.subtitle = subtitle; self.trailing = { EmptyView() } }
}

/// A compact segmented control in the dark style.
struct Seg<T: Hashable>: View {
    let items: [(T, String)]
    @Binding var value: T
    var disabled = false
    var body: some View {
        HStack(spacing: 0) {
            ForEach(Array(items.enumerated()), id: \.offset) { i, it in
                Text(it.1).font(Th.f(10.5)).lineLimit(1)
                    .foregroundColor(it.0 == value ? .white : Th.dim)
                    .padding(.horizontal, 9).padding(.vertical, 4)
                    .background(it.0 == value ? Color(hex: 0x3b3b42) : Color.clear)
                    .contentShape(Rectangle())
                    .onTapGesture { if !disabled { value = it.0 } }
                if i < items.count - 1 { Rectangle().fill(Th.line2).frame(width: 1) }
            }
        }
        .fixedSize()
        .background(Th.field)
        .overlay(RoundedRectangle(cornerRadius: 3).stroke(Th.line2))
        .clipShape(RoundedRectangle(cornerRadius: 3))
        .opacity(disabled ? 0.5 : 1)
    }
}

struct CheckRow: View {
    let label: String
    @Binding var on: Bool
    var disabled = false
    var hint: String = ""
    var body: some View {
        HStack(spacing: 8) {
            ZStack {
                RoundedRectangle(cornerRadius: 2).fill(on ? Th.acc : Th.field)
                RoundedRectangle(cornerRadius: 2).stroke(on ? Th.acc : Color(hex: 0x5a5a60))
                if on { Image(systemName: "checkmark").font(.system(size: 8, weight: .heavy)).foregroundColor(Color(hex: 0x1b1b1b)) }
            }.frame(width: 12, height: 12)
            Text(label).font(Th.f(11.5)).foregroundColor(Th.txt)
            Spacer(minLength: 0)
        }
        .padding(.vertical, 3).contentShape(Rectangle())
        .onTapGesture { if !disabled { on.toggle() } }
        .opacity(disabled ? 0.45 : 1)
        .help(hint)
    }
}

struct KV<V: View>: View {
    let k: String
    @ViewBuilder var v: () -> V
    var body: some View {
        HStack(spacing: 10) { Text(k).font(Th.f(11.5)).foregroundColor(Th.dim).lineLimit(1).fixedSize(); Spacer(minLength: 6); v() }.padding(.vertical, 3)
    }
}

/// A collapsible sidebar section.
struct Section2<Content: View>: View {
    let title: String
    var value: String = ""
    @Binding var open: Bool
    @ViewBuilder var content: () -> Content
    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 7) {
                Image(systemName: open ? "chevron.down" : "chevron.right").font(.system(size: 8, weight: .bold)).foregroundColor(Th.dim).frame(width: 10)
                Text(title).font(Th.f(11.5, .semibold)).foregroundColor(Color(hex: 0xe6e6e8))
                Spacer()
                Text(value).font(Th.f(10.5)).foregroundColor(Th.dim).lineLimit(1)
            }
            .padding(.horizontal, 12).padding(.vertical, 9).contentShape(Rectangle())
            .onTapGesture { withAnimation(.easeOut(duration: 0.12)) { open.toggle() } }
            if open { VStack(alignment: .leading, spacing: 2) { content() }.padding(.horizontal, 12).padding(.bottom, 12) }
        }
        .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .bottom)
    }
}

struct DarkButton: ButtonStyle {
    var primary = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.font(Th.f(11, primary ? .semibold : .regular))
            .foregroundColor(primary ? Color(hex: 0x1c1c1c) : Color(hex: 0xeeeeee))
            .padding(.horizontal, 12).padding(.vertical, 6).frame(maxWidth: .infinity)
            .background(RoundedRectangle(cornerRadius: 3).fill(primary ? Th.acc : Color(hex: 0x3a3a40)).opacity(configuration.isPressed ? 0.8 : 1))
            .overlay(RoundedRectangle(cornerRadius: 3).stroke(primary ? Th.acc : Color(hex: 0x4a4a50)))
    }
}
struct ToolButton: ButtonStyle {
    var on = false
    func makeBody(configuration: Configuration) -> some View {
        configuration.label.font(Th.f(11)).foregroundColor(on ? .white : Th.txt)
            .padding(.horizontal, 10).padding(.vertical, 5)
            .background(RoundedRectangle(cornerRadius: 3).fill(on ? Th.sel : (configuration.isPressed ? Th.panel2 : Color.clear)))
    }
}

struct Badge: View {
    let text: String; let kind: Int   // 0 gyro, 1 audio, 2 warning, 3 neutral
    var body: some View {
        let c: (Color, Color) = [(Color(hex: 0x23402c), Color(hex: 0x7fd492)), (Color(hex: 0x2a3446), Color(hex: 0x8fb4e8)),
                                 (Color(hex: 0x4a2f17), Color(hex: 0xf0a35a)), (Color(hex: 0x3d3d44), Color(hex: 0xcccccc))][kind]
        Text(text).font(.system(size: 9, weight: .bold)).foregroundColor(c.1).lineLimit(1).fixedSize().padding(.horizontal, 4).padding(.vertical, 1)
            .background(RoundedRectangle(cornerRadius: 2).fill(c.0))
    }
}

// MARK: - Clips

struct ProbeInfo: Decodable {
    var frames = 0, width = 0, height = 0, picture_width = 0, picture_height = 0, bits = 0
    var fps_num = 24, fps_den = 1, iso = 0, kelvin = 0, tint = 0
    var gyro = false, braw_supported = false
    var mode = "", timecode = "", wav = "", first_frame = "", lens = ""
    var shutter_angle = 0.0, fnumber = 0.0, focal_mm = 0.0, exposure_s = 0.0, black = 0.0, white = 0.0, baseline_exposure = 0.0
    var neutral: [Double] = [], crop: [Int] = [], braw_coded: [Int] = []
    var imu_hz = 0.0, readout_ms = 0.0
    var error: String? = nil
    var fps: Double { fps_den > 0 ? Double(fps_num) / Double(fps_den) : 24 }
    var fpsLabel: String { fpsString(fps) }
    var modeLabel: String { mode.isEmpty ? "\(String(picture_width))×\(String(picture_height))" : mode }
    enum CodingKeys: String, CodingKey { case frames, width, height, picture_width, picture_height, bits, fps_num, fps_den, iso, kelvin, tint, gyro, braw_supported, mode, timecode, wav, first_frame, lens,
        shutter_angle, fnumber, focal_mm, exposure_s, black, white, baseline_exposure, neutral, crop, braw_coded, imu_hz, readout_ms, error }
    init() {}
    init(from d: Decoder) throws {
        let c = try d.container(keyedBy: CodingKeys.self)
        func i(_ k: CodingKeys, _ def: Int = 0) -> Int { (try? c.decode(Int.self, forKey: k)) ?? def }
        func f(_ k: CodingKeys) -> Double { (try? c.decode(Double.self, forKey: k)) ?? 0 }
        func s(_ k: CodingKeys) -> String { (try? c.decode(String.self, forKey: k)) ?? "" }
        frames = i(.frames); width = i(.width); height = i(.height); picture_width = i(.picture_width); picture_height = i(.picture_height)
        bits = i(.bits); fps_num = i(.fps_num, 24); fps_den = i(.fps_den, 1); iso = i(.iso); kelvin = i(.kelvin); tint = i(.tint)
        gyro = (try? c.decode(Bool.self, forKey: .gyro)) ?? false; braw_supported = (try? c.decode(Bool.self, forKey: .braw_supported)) ?? false
        mode = s(.mode); timecode = s(.timecode); wav = s(.wav); first_frame = s(.first_frame); lens = s(.lens)
        shutter_angle = f(.shutter_angle); fnumber = f(.fnumber); focal_mm = f(.focal_mm); exposure_s = f(.exposure_s)
        black = f(.black); white = f(.white); baseline_exposure = f(.baseline_exposure); imu_hz = f(.imu_hz); readout_ms = f(.readout_ms)
        neutral = (try? c.decode([Double].self, forKey: .neutral)) ?? []; crop = (try? c.decode([Int].self, forKey: .crop)) ?? []
        braw_coded = (try? c.decode([Int].self, forKey: .braw_coded)) ?? []
        error = try? c.decode(String.self, forKey: .error)
    }
}

func fpsString(_ f: Double) -> String { abs(f - f.rounded()) < 0.001 ? String(Int(f.rounded())) : String(format: "%.3f", f) }

func durationLabel(frames: Int, fps: Double) -> String {
    guard fps > 0 else { return "" }
    let s = Double(frames) / fps; let m = Int(s) / 60, sec = Int(s) % 60, fr = Int((s - floor(s)) * fps)
    return m > 0 ? String(format: "%d:%02d:%02d", m, sec, fr) : String(format: "%02d:%02d", sec, fr)
}

/// Adds frames to an HH:MM:SS:FF timecode (non-drop arithmetic at the nominal rate).
func timecode(_ start: String, plus frames: Int, fps: Double) -> String {
    let nom = max(1, Int(fps.rounded()))
    let p = start.split(whereSeparator: { $0 == ":" || $0 == ";" }).compactMap { Int($0) }
    let base = p.count == 4 ? ((p[0] * 60 + p[1]) * 60 + p[2]) * nom + p[3] : 0
    let t = base + frames
    return String(format: "%02d:%02d:%02d:%02d", t / (3600 * nom) % 24, t / (60 * nom) % 60, t / nom % 60, t % nom)
}

enum ClipKind { case dng, braw, mov }

/// A clip: a folder of DNG frames, or a .braw / .mov file.
final class Clip: ObservableObject, Identifiable {
    let id = UUID()
    let url: URL
    let bin: String
    let kind: ClipKind
    var name: String { kind == .dng ? url.lastPathComponent : url.lastPathComponent }
    // the common description (filled by a probe for each kind)
    @Published var info: ProbeInfo?
    @Published var thumb: CGImage?
    /// Every detail for the metadata panel, grouped: [(group, [(key, value)])]
    @Published var details: [(String, [(String, String)])] = []
    @Published var detailsLoaded = false
    var brawMeta: [String: Any] = [:]
    private var _frames: [URL]?
    init(url: URL, bin: String, kind: ClipKind) { self.url = url; self.bin = bin; self.kind = kind }
    var frames: [URL] {
        if kind != .dng { return [] }
        if let f = _frames { return f }
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: url.path)) ?? [])
            .filter { !$0.hasPrefix(".") && $0.lowercased().hasSuffix(".dng") }.sorted()
        let f = names.map { url.appendingPathComponent($0) }
        _frames = f; return f
    }
    var wavURL: URL? { guard kind == .dng, let w = info?.wav, !w.isEmpty else { return nil }; return url.appendingPathComponent(w) }
    var isOpenGate: Bool { info?.mode.hasPrefix("OG") ?? false }
    var convertible: Bool { kind == .dng }
    var kindLabel: String { kind == .dng ? "CinemaDNG" : kind == .braw ? "BRAW" : "MOV" }
}

func bundledTool(_ name: String) -> URL? {
    Bundle.main.url(forAuxiliaryExecutable: name) ?? Bundle.main.url(forResource: name, withExtension: nil)
}

/// Runs a bundled tool to completion and returns its standard output (for --list-clips and --probe).
func runTool(_ name: String, _ args: [String]) -> String {
    guard let exe = bundledTool(name) else { return "" }
    let p = Process(); p.executableURL = exe; p.arguments = args
    let out = Pipe(); p.standardOutput = out; p.standardError = Pipe()
    do { try p.run() } catch { return "" }
    let d = out.fileHandleForReading.readDataToEndOfFile(); p.waitUntilExit()
    return String(decoding: d, as: UTF8.self)
}

/// Opens a .braw through the bundled braw_view helper: returns its info line and frame `thumbIndex` at 1/8 size.
func brawProbe(_ url: URL) -> ([String: Any]?, CGImage?) {
    guard let exe = bundledTool("braw_view") else { return (nil, nil) }
    let p = Process(); p.executableURL = exe; p.arguments = [url.path]
    let inp = Pipe(), out = Pipe(); p.standardInput = inp; p.standardOutput = out; p.standardError = Pipe()
    do { try p.run() } catch { return (nil, nil) }
    let h = out.fileHandleForReading
    var line = Data()
    while true { let b = h.readData(ofLength: 1); if b.isEmpty || b[0] == 10 { break }; line.append(b) }
    let info = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any]
    var img: CGImage?
    if info?["error"] == nil {
        inp.fileHandleForWriting.write(Data("f 0 8\n".utf8))
        img = BrawStream.readFrame(h)?.1
    }
    try? inp.fileHandleForWriting.write(contentsOf: Data("q\n".utf8)); try? inp.fileHandleForWriting.close()
    p.waitUntilExit()
    return (info, img)
}

func cgImageRGBA(_ data: Data, _ w: Int, _ h: Int) -> CGImage? {
    guard let prov = CGDataProvider(data: data as CFData) else { return nil }
    return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                   bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue), provider: prov, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
}

/// BGRA8 pixels (the byte order Core Animation and Metal use natively) as an image.
func cgImageBGRA(_ data: Data, _ w: Int, _ h: Int) -> CGImage? {
    guard let prov = CGDataProvider(data: data as CFData) else { return nil }
    return CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                   bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                   provider: prov, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
}

// ---- frames in IOSurfaces: GPU-shareable memory that Core Animation shows (and Core Image reads and writes) without
// copying or colour-converting it on the main thread, which is what a 6K CGImage costs every frame.
private var surfaceKey: UInt8 = 0
extension CGImage {
    /// The IOSurface this image's pixels live in, if it was made by `surfaceImage`.
    var surface: IOSurface? { objc_getAssociatedObject(self, &surfaceKey) as? IOSurface }
}
/// Recycles frame surfaces: a frame's surface comes back when its image is freed (and the GPU is done with it).
final class SurfacePool {
    static let shared = SurfacePool()
    private let lock = NSLock(); private var free: [IOSurface] = []
    func get(_ w: Int, _ h: Int) -> IOSurface? {
        lock.lock()
        if let i = free.firstIndex(where: { $0.width == w && $0.height == h && !$0.isInUse }) { let s = free.remove(at: i); lock.unlock(); return s }
        lock.unlock()
        return IOSurface(properties: [.width: w, .height: h, .bytesPerElement: 4, .pixelFormat: 0x42475241 /* 'BGRA' */])
    }
    func put(_ s: IOSurface) { lock.lock(); if free.count < 6 { free.append(s) }; lock.unlock() }
    func drain() { lock.lock(); free.removeAll(); lock.unlock() }
}
/// A CGImage over the surface's own memory (no copy), carrying the surface for the viewer and the LUT.
func surfaceImage(_ s: IOSurface) -> CGImage? {
    let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
    if let pl = srgb.copyPropertyList() { s.setAttachment(pl, forKey: kIOSurfaceColorSpace as String) }
    let w = s.width, h = s.height, bpr = s.bytesPerRow
    let info = Unmanaged.passRetained(s).toOpaque()
    guard let prov = CGDataProvider(dataInfo: info, data: s.baseAddress, size: bpr * h,
                                    releaseData: { i, _, _ in if let i = i { SurfacePool.shared.put(Unmanaged<IOSurface>.fromOpaque(i).takeRetainedValue()) } })
    else { Unmanaged<IOSurface>.fromOpaque(info).release(); return nil }
    guard let img = CGImage(width: w, height: h, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: bpr, space: srgb,
                            bitmapInfo: CGBitmapInfo(rawValue: CGBitmapInfo.byteOrder32Little.rawValue | CGImageAlphaInfo.noneSkipFirst.rawValue),
                            provider: prov, decode: nil, shouldInterpolate: true, intent: .defaultIntent) else { return nil }
    objc_setAssociatedObject(img, &surfaceKey, s, .OBJC_ASSOCIATION_RETAIN)
    return img
}

/// The shared memory braw_view (--shm) decodes frames into: `slots` frames of `slotBytes` each. A 6K frame is 77 MB,
/// too much to push through a pipe 24+ times a second, so only a short record goes through the pipe.
/// (shm_open is variadic in C, which Swift can't call; opening an existing object needs only its first two
/// arguments, so it's called through dlsym with that fixed signature.)
final class SharedFrames {
    let base: UnsafeMutableRawPointer, size: Int, slots: Int, slotBytes: Int
    init?(name: String, slots: Int, slotBytes: Int) {
        typealias ShmOpen = @convention(c) (UnsafePointer<CChar>, Int32) -> Int32
        guard slots > 0, slotBytes > 0, let sym = dlsym(UnsafeMutableRawPointer(bitPattern: -2), "shm_open") else { return nil }
        let fd = name.withCString { unsafeBitCast(sym, to: ShmOpen.self)($0, O_RDONLY) }
        guard fd >= 0 else { return nil }
        defer { close(fd) }
        let n = slots * slotBytes
        guard let p = mmap(nil, n, PROT_READ, MAP_SHARED, fd, 0), p != MAP_FAILED else { return nil }
        _ = name.withCString { shm_unlink($0) }        // gone from the system once both sides let go of it
        base = p; size = n; self.slots = slots; self.slotBytes = slotBytes
    }
    deinit { munmap(base, size) }
}

let movieExtensions: Set<String> = ["mov", "mp4", "m4v"]

/// The clips the app knows about: made on demand as Media Storage shows clip folders and files, probed once, and
/// kept by path. There is no media pool to import into: browse, preview in the viewer, add to the Job Queue.
final class MediaPool: ObservableObject {
    @Published var current: Clip?                 // in the viewer
    @Published var selectedClips: [Clip] = []     // selected in Media Storage
    private var cache: [String: Clip] = [:]
    private let probeQueue: OperationQueue = { let q = OperationQueue(); q.maxConcurrentOperationCount = 4; q.qualityOfService = .userInitiated; return q }()

    static func kind(of url: URL, isDir: Bool) -> ClipKind? {
        if isDir { return .dng }
        let ext = url.pathExtension.lowercased()
        return ext == "braw" ? .braw : movieExtensions.contains(ext) ? .mov : nil
    }
    /// The clip at `url` (a DNG clip folder, or a .braw / .mov file), probed in the background the first time.
    func clip(_ url: URL, _ kind: ClipKind) -> Clip {
        let key = url.standardizedFileURL.path
        if let c = cache[key] { return c }
        let c = Clip(url: url, bin: url.deletingLastPathComponent().lastPathComponent, kind: kind)
        cache[key] = c; probe(c)
        return c
    }
    func preview(_ c: Clip?) { if current?.id != c?.id { current = c } }

    /// Every clip in a folder (DNG clip folders at any depth, .braw and .mov files), or the clip a file or clip folder is.
    func clips(in url: URL, _ done: @escaping ([Clip]) -> Void) {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { done([]); return }
        DispatchQueue.global(qos: .userInitiated).async {
            var found: [(URL, ClipKind)] = []
            if isDir.boolValue {
                let out = runTool("dng2prr", ["--list-clips", url.path])
                found += out.split(separator: "\n").map(String.init).filter { $0.hasPrefix("/") }
                    .filter { $0 == url.standardizedFileURL.path || $0 == url.path || !DngMode.isOutputFolder(URL(fileURLWithPath: $0).lastPathComponent) }   // compressed outputs only when picked themselves
                    .map { (URL(fileURLWithPath: $0), .dng) }
                if let en = FileManager.default.enumerator(at: url, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles, .skipsPackageDescendants]) {
                    for case let f as URL in en {
                        if en.level > 6 { en.skipDescendants(); continue }
                        let ext = f.pathExtension.lowercased()
                        if ext == "braw" { found.append((f, .braw)) } else if movieExtensions.contains(ext) { found.append((f, .mov)) }
                        else if ext == "dng" { en.skipDescendants() }
                    }
                }
            } else {
                let ext = url.pathExtension.lowercased()
                if ext == "braw" { found.append((url, .braw)) } else if movieExtensions.contains(ext) { found.append((url, .mov)) }
                else if ext == "dng" { found.append((url.deletingLastPathComponent(), .dng)) }
            }
            DispatchQueue.main.async {
                var seen = Set<String>(); var out: [Clip] = []
                for (u, k) in found where seen.insert(u.standardizedFileURL.path).inserted { out.append(self.clip(u, k)) }
                done(out)
            }
        }
    }

    private func probe(_ c: Clip) {
        probeQueue.addOperation {
            switch c.kind {
            case .dng:
                let out = runTool("dng2braw", ["--probe", c.url.path])
                let info = (try? JSONDecoder().decode(ProbeInfo.self, from: Data(out.utf8))) ?? { var i = ProbeInfo(); i.error = "could not read"; return i }()
                var thumb: CGImage?
                let first = info.first_frame.isEmpty ? c.frames.first : URL(fileURLWithPath: info.first_frame)
                if let f = first, let src = CGImageSourceCreateWithURL(f as CFURL, nil) {
                    thumb = CGImageSourceCreateThumbnailAtIndex(src, 0, [kCGImageSourceCreateThumbnailFromImageAlways: true, kCGImageSourceThumbnailMaxPixelSize: 400,
                                                                           kCGImageSourceCreateThumbnailWithTransform: true] as CFDictionary)
                }
                DispatchQueue.main.async { c.info = info; c.thumb = thumb; self.objectWillChange.send() }
            case .braw:
                let (j, img) = brawProbe(c.url)
                var info = ProbeInfo()
                if let j = j, j["error"] == nil {
                    info.width = j["width"] as? Int ?? 0; info.height = j["height"] as? Int ?? 0
                    info.picture_width = info.width; info.picture_height = info.height
                    let fps = j["fps"] as? Double ?? 24; info.fps_num = Int((fps * 1000).rounded()); info.fps_den = 1000
                    info.frames = j["frames"] as? Int ?? 0; info.timecode = j["timecode"] as? String ?? ""
                    let m = j["metadata"] as? [String: Any] ?? [:]
                    c.brawMeta = m
                    info.mode = j["camera"] as? String ?? ""
                    info.wav = "embedded"
                } else { info.error = (j?["error"] as? String) ?? "Blackmagic RAW couldn't be read" }
                DispatchQueue.main.async { c.info = info; c.thumb = img; self.objectWillChange.send() }
            case .mov:
                let asset = AVURLAsset(url: c.url)
                var info = ProbeInfo()
                if let v = asset.tracks(withMediaType: .video).first {
                    let sz = v.naturalSize; info.width = Int(sz.width); info.height = Int(sz.height); info.picture_width = info.width; info.picture_height = info.height
                    let fps = Double(v.nominalFrameRate > 0 ? v.nominalFrameRate : 24); info.fps_num = Int((fps * 1000).rounded()); info.fps_den = 1000
                    info.frames = Int((asset.duration.seconds * fps).rounded())
                    if let fd = v.formatDescriptions.first { let t = CMFormatDescriptionGetMediaSubType(fd as! CMFormatDescription); info.mode = fourCCName(t) }
                }
                if !asset.tracks(withMediaType: .audio).isEmpty { info.wav = "embedded" }
                info.timecode = movTimecode(asset) ?? ""
                let gen = AVAssetImageGenerator(asset: asset); gen.maximumSize = CGSize(width: 400, height: 400); gen.appliesPreferredTrackTransform = true
                let img = try? gen.copyCGImage(at: .zero, actualTime: nil)
                DispatchQueue.main.async { c.info = info; c.thumb = img; self.objectWillChange.send() }
            }
        }
    }

}

/// "lossless" or "lossy" for a DNG with JPEG tiles: the first JPEG frame marker (SOF3, or SOF0/SOF1).
func jpegKind(_ url: URL) -> String? {
    guard let fh = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? fh.close() }
    let d = fh.readData(ofLength: 4 << 20); var i = d.startIndex
    var inJpeg = false
    while i + 1 < d.endIndex {
        if d[i] == 0xFF {
            let m = d[i + 1]
            if m == 0xD8 { inJpeg = true } else if inJpeg {
                if m == 0xC3 { return "lossless" }
                if m == 0xC0 || m == 0xC1 { return "lossy" }
            }
        }
        i += 1
    }
    return nil
}

func fourCCName(_ t: FourCharCode) -> String {
    let s = String([Character(UnicodeScalar((t >> 24) & 255) ?? " "), Character(UnicodeScalar((t >> 16) & 255) ?? " "), Character(UnicodeScalar((t >> 8) & 255) ?? " "), Character(UnicodeScalar(t & 255) ?? " ")])
    switch s {
    case "aprn": return "ProRes RAW"; case "aprh": return "ProRes RAW HQ"; case "apch": return "ProRes 422 HQ"; case "apcn": return "ProRes 422"
    case "apcs": return "ProRes 422 LT"; case "apco": return "ProRes 422 Proxy"; case "ap4h": return "ProRes 4444"; case "ap4x": return "ProRes 4444 XQ"
    case "avc1": return "H.264"; case "hvc1", "hev1": return "HEVC"
    default: return s.trimmingCharacters(in: .whitespaces)
    }
}

/// The start timecode of a QuickTime movie, read from its 'tmcd' track: the first sample (a frame number) and the
/// sample description's frames-per-second byte. AVFoundation hands timecode samples back empty, so the atoms are walked.
func movTimecode(_ asset: AVAsset) -> String? {
    guard let url = (asset as? AVURLAsset)?.url, let fh = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? fh.close() }
    func u32(_ d: Data, _ o: Int) -> UInt32 { d.count >= o + 4 ? d[d.startIndex + o..<d.startIndex + o + 4].reduce(0) { $0 << 8 | UInt32($1) } : 0 }
    func u64(_ d: Data, _ o: Int) -> UInt64 { UInt64(u32(d, o)) << 32 | UInt64(u32(d, o + 4)) }
    // top level: find moov
    var pos: UInt64 = 0; var moov: Data?
    let size = (try? fh.seekToEnd()) ?? 0
    while pos + 8 <= size {
        try? fh.seek(toOffset: pos); let h = fh.readData(ofLength: 16); if h.count < 8 { break }
        var sz = UInt64(u32(h, 0)); let typ = String(decoding: h[h.startIndex + 4..<h.startIndex + 8], as: UTF8.self)
        if sz == 1 { sz = u64(h, 8) } else if sz == 0 { sz = size - pos }
        if sz < 8 { break }
        if typ == "moov" { try? fh.seek(toOffset: pos); moov = fh.readData(ofLength: Int(min(sz, 64 << 20))); break }
        pos += sz
    }
    guard let m = moov else { return nil }
    func children(_ d: Data, _ from: Int, _ to: Int) -> [(String, Int, Int)] {   // (type, body start, end)
        var out: [(String, Int, Int)] = []; var o = from
        while o + 8 <= to { let sz = Int(u32(d, o)); if sz < 8 || o + sz > to { break }
            out.append((String(decoding: d[d.startIndex + o + 4..<d.startIndex + o + 8], as: UTF8.self), o + 8, o + sz)); o += sz }
        return out
    }
    for (t, a, b) in children(m, 8, m.count) where t == "trak" {
        guard let mdia = children(m, a, b).first(where: { $0.0 == "mdia" }) else { continue }
        let mk = children(m, mdia.1, mdia.2)
        guard let hdlr = mk.first(where: { $0.0 == "hdlr" }), String(decoding: m[m.startIndex + hdlr.1 + 8..<m.startIndex + hdlr.1 + 12], as: UTF8.self) == "tmcd",
              let minf = mk.first(where: { $0.0 == "minf" }), let stbl = children(m, minf.1, minf.2).first(where: { $0.0 == "stbl" }) else { continue }
        let sk = children(m, stbl.1, stbl.2)
        var off: UInt64?
        if let c = sk.first(where: { $0.0 == "co64" }) { off = u64(m, c.1 + 8) } else if let c = sk.first(where: { $0.0 == "stco" }) { off = UInt64(u32(m, c.1 + 8)) }
        var quanta = 0, drop = false
        if let sd = sk.first(where: { $0.0 == "stsd" }) {          // entry: size, 'tmcd', 6 reserved, ref index, 4 reserved, flags, timescale, duration, quanta
            let e = sd.1 + 8
            drop = (u32(m, e + 20) & 1) != 0; quanta = Int(m[m.startIndex + e + 32])
        }
        guard let o = off, quanta > 0 else { return nil }
        try? fh.seek(toOffset: o); let d = fh.readData(ofLength: 4); guard d.count == 4 else { return nil }
        let f = Int(u32(d, 0)), n = quanta
        return String(format: "%02d:%02d:%02d%@%02d", f / (3600 * n) % 24, f / (60 * n) % 60, f / n % 60, drop ? ";" : ":", f % n)
    }
    return nil
}

// MARK: - Clip details (metadata panel)

/// Metadata keys without their reverse-DNS prefixes, for display.
func shortKey(_ k: String) -> String {
    for p in ["io.github.fpsup.dng2prr.", "io.github.fpsup.dng2braw.", "io.github.ijigen.fpsup.", "com.apple.proapps.exif.{Exif}.", "com.apple.proapps.image.{TIFF}.",
              "com.apple.proapps.", "com.apple.proresraw.", "org.smpte.rdd18.camera.", "org.smpte.rdd18.lens.", "org.smpte.rdd18."] where k.hasPrefix(p) {
        return String(k.dropFirst(p.count))
    }
    return k
}

func loadDetails(_ c: Clip) {
    if c.detailsLoaded { return }
    c.detailsLoaded = true
    DispatchQueue.global(qos: .userInitiated).async {
        var groups: [(String, [(String, String)])] = []
        func fmt(_ x: Double, _ d: Int = 2) -> String { String(format: "%.\(d)f", x) }
        let fm = FileManager.default
        switch c.kind {
        case .dng:
            let i = c.info ?? ProbeInfo()
            var general: [(String, String)] = [("Clip", c.name), ("Format", "CinemaDNG sequence"), ("Frames", String(i.frames)),
                                                ("Duration", durationLabel(frames: i.frames, fps: i.fps)), ("Frame rate", i.fpsLabel + " fps"), ("Start timecode", i.timecode)]
            var camera: [(String, String)] = []
            var exposure: [(String, String)] = []
            var image: [(String, String)] = [("Recording mode", i.mode.isEmpty ? "—" : i.mode), ("Picture", "\(String(i.picture_width)) × \(String(i.picture_height))"),
                                             ("DNG frame", "\(String(i.width)) × \(String(i.height))"), ("Bit depth", "\(String(i.bits))-bit")]
            if i.crop.count == 4 { image.append(("Default crop", "L \(i.crop[0])  R \(i.crop[1])  T \(i.crop[2])  B \(i.crop[3])")) }
            image.append(("Black / white level", "\(fmt(i.black, 1)) / \(fmt(i.white, 0))"))
            if let f = c.frames.first, let src = CGImageSourceCreateWithURL(f as CFURL, nil), let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [String: Any] {
                let tiff = props[kCGImagePropertyTIFFDictionary as String] as? [String: Any] ?? [:]
                let exif = props[kCGImagePropertyExifDictionary as String] as? [String: Any] ?? [:]
                let dng = props[kCGImagePropertyDNGDictionary as String] as? [String: Any] ?? [:]
                if let v = tiff[kCGImagePropertyTIFFMake as String] as? String { camera.append(("Make", v)) }
                if let v = tiff[kCGImagePropertyTIFFModel as String] as? String { camera.append(("Model", v)) }
                if let v = exif[kCGImagePropertyExifBodySerialNumber as String] as? String { camera.append(("Serial number", v)) }
                else if let v = dng[kCGImagePropertyDNGCameraSerialNumber as String] as? String { camera.append(("Serial number", v)) }
                if let v = tiff[kCGImagePropertyTIFFSoftware as String] as? String { camera.append(("Firmware / software", v)) }
                if let v = exif[kCGImagePropertyExifLensModel as String] as? String, !v.isEmpty { camera.append(("Lens", v)) }
                if let v = exif[kCGImagePropertyExifDateTimeOriginal as String] as? String { general.append(("Recorded", v)) }
                if let v = dng[kCGImagePropertyDNGVersion as String] as? [Int] { image.append(("DNG version", v.map(String.init).joined(separator: "."))) }
                if let v = tiff[kCGImagePropertyTIFFCompression as String] as? Int {
                    // compression 7 is used for lossless and lossy (DCT) JPEG tiles alike: the JPEG's frame marker says which
                    let probeFrame = c.frames.count > 1 ? c.frames[1] : f
                    let kind = v == 7 ? jpegKind(probeFrame) : nil
                    image.append(("Compression", v == 1 ? "uncompressed" : v == 7 ? (kind == "lossy" ? "lossy JPEG (DCT)" : kind == "lossless" ? "lossless JPEG" : "JPEG") : v == 34892 ? "lossy JPEG" : "code \(v)"))
                }
            }
            if camera.isEmpty { camera.append(("Camera", "SIGMA fp")) }
            if !i.lens.isEmpty && !camera.contains(where: { $0.0 == "Lens" }) { camera.append(("Lens", i.lens)) }
            exposure.append(("ISO", String(i.iso)))
            if i.exposure_s > 0 { exposure.append(("Shutter", i.exposure_s < 1 ? "1/\(Int((1 / i.exposure_s).rounded())) s" : fmt(i.exposure_s) + " s")) }
            if i.shutter_angle > 0 { exposure.append(("Shutter angle", fmt(i.shutter_angle, 1) + "°")) }
            exposure.append(("Aperture", i.fnumber > 0 ? "f/" + fmt(i.fnumber, 1) : "— (manual lens)"))
            exposure.append(("Focal length", i.focal_mm > 0 ? fmt(i.focal_mm, 0) + " mm" : "—"))
            var colour: [(String, String)] = [("White balance (as BRAW writes it)", "\(String(i.kelvin)) K, tint \(i.tint >= 0 ? "+" : "")\(String(i.tint))")]
            if i.neutral.count == 3 { colour.append(("AsShotNeutral", i.neutral.map { fmt($0, 4) }.joined(separator: "  "))) }
            colour.append(("Baseline exposure", (i.baseline_exposure >= 0 ? "+" : "") + fmt(i.baseline_exposure, 2) + " EV"))
            var audio: [(String, String)] = []
            if let w = c.wavURL, let af = try? AVAudioFile(forReading: w) {
                let f = af.fileFormat
                let bits = (f.settings[AVLinearPCMBitDepthKey] as? Int) ?? 0
                audio = [("File", w.lastPathComponent), ("Format", "PCM \(bits)-bit, \(Int(f.sampleRate)) Hz, \(f.channelCount) ch"),
                         ("Length", fmt(Double(af.length) / f.sampleRate) + " s")]
            } else { audio = [("Audio", "no WAV in the clip folder")] }
            var gyro: [(String, String)] = [("Gyro2 data", i.gyro ? "yes (fpSup)" : "no")]
            if i.gyro { gyro += [("IMU rate", fmt(i.imu_hz, 0) + " Hz"), ("Sensor readout", fmt(i.readout_ms, 3) + " ms")] }
            var conv: [(String, String)] = [("Blackmagic RAW", i.braw_supported ? "supported" : "not yet for this mode")]
            if i.braw_coded.count == 2 { conv.append(("BRAW coded frame", "\(i.braw_coded[0]) × \(i.braw_coded[1])")) }
            var total: Int64 = 0
            for f in c.frames { total += Int64((try? f.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
            let file: [(String, String)] = [("Folder", c.url.path), ("Size on disk", ByteCountFormatter.string(fromByteCount: total, countStyle: .file)),
                                            ("Average frame", c.frames.isEmpty ? "—" : ByteCountFormatter.string(fromByteCount: total / Int64(c.frames.count), countStyle: .file))]
            groups = [("General", general), ("Camera", camera), ("Exposure", exposure), ("Colour", colour), ("Image", image), ("Audio", audio), ("Gyro", gyro), ("Conversion", conv), ("File", file)]
        case .braw:
            let i = c.info ?? ProbeInfo()
            let general: [(String, String)] = [("Clip", c.name), ("Format", "Blackmagic RAW"), ("Picture", "\(String(i.width)) × \(String(i.height))"), ("Frames", String(i.frames)),
                                                ("Frame rate", i.fpsLabel + " fps"), ("Duration", durationLabel(frames: i.frames, fps: i.fps)), ("Start timecode", i.timecode), ("Camera", i.mode)]
            let meta = c.brawMeta.keys.sorted().map { k -> (String, String) in
                let v = c.brawMeta[k]; let s: String
                if let a = v as? [Any] { s = a.map { "\($0)" }.joined(separator: ", ") } else { s = v.map { "\($0)" } ?? "" }
                return (k, s)
            }
            let size = (try? c.url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            groups = [("General", general), ("Clip metadata", meta), ("File", [("Path", c.url.path), ("Size", ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file))])]
        case .mov:
            let asset = AVURLAsset(url: c.url)
            var general: [(String, String)] = [("Clip", c.name), ("Duration", fmt(asset.duration.seconds) + " s")]
            if let v = asset.tracks(withMediaType: .video).first {
                general.append(("Video", "\(fourCCName(CMFormatDescriptionGetMediaSubType(v.formatDescriptions.first as! CMFormatDescription))), \(Int(v.naturalSize.width)) × \(Int(v.naturalSize.height)), \(fpsString(Double(v.nominalFrameRate))) fps"))
                general.append(("Data rate", String(format: "%.0f Mb/s", v.estimatedDataRate / 1e6)))
            }
            for a in asset.tracks(withMediaType: .audio) {
                if let fd = a.formatDescriptions.first, let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(fd as! CMAudioFormatDescription)?.pointee {
                    general.append(("Audio", "\(asbd.mChannelsPerFrame) ch, \(Int(asbd.mSampleRate)) Hz, \(asbd.mBitsPerChannel > 0 ? "\(asbd.mBitsPerChannel)-bit" : "")"))
                }
            }
            if let tc = movTimecode(asset) { general.append(("Start timecode", tc)) }
            let tracks = asset.tracks.map { t -> (String, String) in ("Track \(t.trackID)", "\(t.mediaType.rawValue) · \(t.formatDescriptions.first.map { fourCCName(CMFormatDescriptionGetMediaSubType($0 as! CMFormatDescription)) } ?? "")") }
            var meta: [(String, String)] = []
            for item in asset.metadata(forFormat: .quickTimeMetadata) {
                let k = (item.key as? String) ?? item.identifier?.rawValue ?? "?"
                var v: String
                if let sv = item.stringValue { v = sv.hasPrefix("<") && sv.count > 200 ? "XML, \(sv.count) characters" : sv }
                else if let n = item.numberValue { v = n.stringValue }
                else if let d = item.dataValue { v = "binary, \(d.count) bytes" }
                else { v = item.value.map { "\($0)" } ?? "" }
                meta.append((shortKey(k), String(v.replacingOccurrences(of: "\n", with: " ").prefix(160))))
            }
            let size = (try? c.url.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            groups = [("General", general), ("Tracks", tracks), ("Clip metadata", meta), ("File", [("Path", c.url.path), ("Size", ByteCountFormatter.string(fromByteCount: Int64(size), countStyle: .file))])]
        }
        _ = fm
        DispatchQueue.main.async { c.details = groups.filter { !$0.1.isEmpty } }
    }
}

// MARK: - Monitoring LUTs

struct CubeLUT {
    let name: String
    let size: Int
    let data: Data           // RGBA float32, red fastest (CIColorCube layout)
    var domainMin: [Float] = [0, 0, 0], domainMax: [Float] = [1, 1, 1]

    /// Parses an Adobe/Resolve .cube file (3D, or 1D expanded to a 33³ cube). Cubes over 64 are resampled to 64.
    static func load(_ url: URL) -> CubeLUT? {
        guard let text = (try? String(contentsOf: url, encoding: .utf8)) ?? (try? String(contentsOf: url, encoding: .isoLatin1)) else { return nil }
        var size3 = 0, size1 = 0, vals: [Float] = []; var dmin: [Float] = [0, 0, 0], dmax: [Float] = [1, 1, 1]
        vals.reserveCapacity(65 * 65 * 65 * 3)
        for raw in text.split(whereSeparator: \.isNewline) {
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty || line.hasPrefix("#") { continue }
            let parts = line.split(whereSeparator: { $0 == " " || $0 == "\t" })
            guard let head = parts.first else { continue }
            switch head.uppercased() {
            case "LUT_3D_SIZE": size3 = Int(parts.last ?? "") ?? 0
            case "LUT_1D_SIZE": size1 = Int(parts.last ?? "") ?? 0
            case "DOMAIN_MIN": dmin = parts.dropFirst().compactMap { Float($0) }
            case "DOMAIN_MAX": dmax = parts.dropFirst().compactMap { Float($0) }
            case "TITLE", "LUT_3D_INPUT_RANGE", "LUT_1D_INPUT_RANGE": continue
            default:
                if parts.count >= 3, let r = Float(parts[0]), let g = Float(parts[1]), let b = Float(parts[2]) { vals += [r, g, b] }
            }
        }
        if dmin.count != 3 { dmin = [0, 0, 0] }; if dmax.count != 3 { dmax = [1, 1, 1] }
        let name = url.deletingPathExtension().lastPathComponent
        if size3 >= 2, vals.count >= size3 * size3 * size3 * 3 {
            var n = size3, v = vals
            if n > 64 { v = resample(v, n, 64); n = 64 }
            return CubeLUT(name: name, size: n, data: rgba(v), domainMin: dmin, domainMax: dmax)
        }
        if size1 >= 2, vals.count >= size1 * 3 {
            let n = 33; var v = [Float](); v.reserveCapacity(n * n * n * 3)
            func lut1(_ x: Float, _ ch: Int) -> Float {
                let p = max(0, min(1, x)) * Float(size1 - 1); let i = min(Int(p), size1 - 2); let t = p - Float(i)
                return vals[i * 3 + ch] * (1 - t) + vals[(i + 1) * 3 + ch] * t
            }
            for b in 0..<n { for g in 0..<n { for r in 0..<n {
                let s = Float(n - 1); v += [lut1(Float(r) / s, 0), lut1(Float(g) / s, 1), lut1(Float(b) / s, 2)]
            } } }
            return CubeLUT(name: name, size: n, data: rgba(v), domainMin: dmin, domainMax: dmax)
        }
        return nil
    }
    private static func rgba(_ v: [Float]) -> Data {
        var out = [Float](); out.reserveCapacity(v.count / 3 * 4)
        var i = 0; while i + 2 < v.count { out += [v[i], v[i + 1], v[i + 2], 1]; i += 3 }
        return out.withUnsafeBufferPointer { Data(buffer: $0) }
    }
    private static func resample(_ v: [Float], _ n: Int, _ m: Int) -> [Float] {
        func at(_ r: Int, _ g: Int, _ b: Int, _ c: Int) -> Float { v[((b * n + g) * n + r) * 3 + c] }
        var out = [Float](); out.reserveCapacity(m * m * m * 3)
        let s = Float(n - 1) / Float(m - 1)
        for b in 0..<m { for g in 0..<m { for r in 0..<m {
            let fr = Float(r) * s, fg = Float(g) * s, fb = Float(b) * s
            let r0 = min(Int(fr), n - 2), g0 = min(Int(fg), n - 2), b0 = min(Int(fb), n - 2)
            let tr = fr - Float(r0), tg = fg - Float(g0), tb = fb - Float(b0)
            for c in 0..<3 {
                func lerp(_ a: Float, _ b: Float, _ t: Float) -> Float { a + (b - a) * t }
                let c00 = lerp(at(r0, g0, b0, c), at(r0 + 1, g0, b0, c), tr), c10 = lerp(at(r0, g0 + 1, b0, c), at(r0 + 1, g0 + 1, b0, c), tr)
                let c01 = lerp(at(r0, g0, b0 + 1, c), at(r0 + 1, g0, b0 + 1, c), tr), c11 = lerp(at(r0, g0 + 1, b0 + 1, c), at(r0 + 1, g0 + 1, b0 + 1, c), tr)
                out.append(lerp(lerp(c00, c10, tg), lerp(c01, c11, tg), tb))
            }
        } } }
        return out
    }

    /// A built-in exposure check: luma bands in false colour (darkest purple, 18% grey green, near-white yellow, clipped red).
    static func falseColor() -> CubeLUT {
        let n = 33; var v = [Float](); v.reserveCapacity(n * n * n * 3)
        for b in 0..<n { for g in 0..<n { for r in 0..<n {
            let s = Float(n - 1), R = Float(r) / s, G = Float(g) / s, B = Float(b) / s
            let y = 0.2126 * R + 0.7152 * G + 0.0722 * B
            let grey = y * 0.55 + 0.1
            var c: [Float] = [grey, grey, grey]
            if y < 0.03 { c = [0.45, 0.0, 0.6] } else if y < 0.08 { c = [0.0, 0.25, 0.9] }
            else if y > 0.40 && y < 0.47 { c = [0.1, 0.75, 0.2] } else if y > 0.62 && y < 0.68 { c = [0.95, 0.5, 0.65] }
            else if y > 0.97 { c = [0.95, 0.1, 0.05] } else if y > 0.92 { c = [0.95, 0.85, 0.1] }
            v += c
        } } }
        return CubeLUT(name: "False Color (exposure)", size: n, data: rgba(v))
    }

    /// Blackmagic Film Generation 5 (encoded 0..1) to scene-linear, from Blackmagic's published formula.
    static func gen5ToLinear(_ y: Float) -> Float {
        let A: Float = 0.08692876065491224, B: Float = 0.005494072432257808, C: Float = 0.5300133392291939, D: Float = 8.283605932402494, E: Float = 0.09246575342465753
        return y < D * 0.005 + E ? (y - E) / D : expf((y - C) / A) - B
    }
    /// This LUT's output for an input (in its own domain), trilinear.
    func sample(_ r: Float, _ g: Float, _ b: Float) -> (Float, Float, Float) {
        let n = size, s = Float(n - 1)
        func idx(_ x: Float, _ c: Int) -> (Int, Float) {
            let t = max(0, min(1, (x - domainMin[c]) / max(1e-6, domainMax[c] - domainMin[c]))) * s
            let i = min(Int(t), n - 2); return (i, t - Float(i))
        }
        let (r0, tr) = idx(r, 0), (g0, tg) = idx(g, 1), (b0, tb) = idx(b, 2)
        return data.withUnsafeBytes { (p: UnsafeRawBufferPointer) -> (Float, Float, Float) in
            let f = p.bindMemory(to: Float.self)
            func at(_ ri: Int, _ gi: Int, _ bi: Int, _ ch: Int) -> Float { f[((bi * n + gi) * n + ri) * 4 + ch] }
            var o: [Float] = [0, 0, 0]
            for ch in 0..<3 {
                func l(_ a: Float, _ b: Float, _ t: Float) -> Float { a + (b - a) * t }
                let c00 = l(at(r0, g0, b0, ch), at(r0 + 1, g0, b0, ch), tr), c10 = l(at(r0, g0 + 1, b0, ch), at(r0 + 1, g0 + 1, b0, ch), tr)
                let c01 = l(at(r0, g0, b0 + 1, ch), at(r0 + 1, g0, b0 + 1, ch), tr), c11 = l(at(r0, g0 + 1, b0 + 1, ch), at(r0 + 1, g0 + 1, b0 + 1, ch), tr)
                o[ch] = l(l(c00, c10, tg), l(c01, c11, tg), tb)
            }
            return (o[0], o[1], o[2])
        }
    }
    /// For the viewer's Linear decode: the picture arrives as Blackmagic Film Gen 5 (Rec.709 primaries), so it can
    /// carry the whole range in 8 bits; this cube turns it back into scene-linear and then applies `lut` (a LUT that
    /// expects linear input), or shows the linear values as they are when there's no LUT.
    static func linearInput(then lut: CubeLUT?) -> CubeLUT {
        let n = 64; var v = [Float](); v.reserveCapacity(n * n * n * 3)
        var lin = [Float](repeating: 0, count: n); for i in 0..<n { lin[i] = max(0, gen5ToLinear(Float(i) / Float(n - 1))) }
        for b in 0..<n { for g in 0..<n { for r in 0..<n {
            if let l = lut { let o = l.sample(lin[r], lin[g], lin[b]); v += [o.0, o.1, o.2] }
            else { v += [min(1, lin[r]), min(1, lin[g]), min(1, lin[b])] }
        } } }
        return CubeLUT(name: (lut?.name ?? "Linear") + " (linear input)", size: n, data: rgba(v))
    }

    func filter(on image: CIImage) -> CIImage {
        var img = image
        if domainMin != [0, 0, 0] || domainMax != [1, 1, 1] {
            let sc = (0..<3).map { CGFloat(1 / max(1e-6, domainMax[$0] - domainMin[$0])) }
            let bias = (0..<3).map { -CGFloat(domainMin[$0]) * sc[$0] }
            img = img.applyingFilter("CIColorMatrix", parameters: ["inputRVector": CIVector(x: sc[0], y: 0, z: 0, w: 0), "inputGVector": CIVector(x: 0, y: sc[1], z: 0, w: 0),
                                                                   "inputBVector": CIVector(x: 0, y: 0, z: sc[2], w: 0), "inputBiasVector": CIVector(x: bias[0], y: bias[1], z: bias[2], w: 0)])
        }
        let f = CIFilter(name: "CIColorCubeWithColorSpace")!
        f.setValue(img, forKey: kCIInputImageKey)
        f.setValue(size, forKey: "inputCubeDimension"); f.setValue(data, forKey: "inputCubeData")
        f.setValue(CGColorSpace(name: CGColorSpace.sRGB)!, forKey: "inputColorSpace")
        return f.outputImage ?? image
    }
}

final class LUTLibrary: ObservableObject {
    @Published var files: [URL] = []
    @Published var selected: String = UserDefaults.standard.string(forKey: "monitorLUT") ?? "" { didSet { UserDefaults.standard.set(selected, forKey: "monitorLUT"); loadSelected() } }
    @Published var lut: CubeLUT?
    static let falseColorID = "builtin:falsecolor"
    let folder: URL = {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first!
        let u = base.appendingPathComponent("cDNG Raw Convert/LUTs", isDirectory: true)
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }()
    init() { refresh(); loadSelected() }
    func refresh() {
        files = ((try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension.lowercased() == "cube" }.sorted { $0.lastPathComponent.lowercased() < $1.lastPathComponent.lowercased() }
    }
    var label: String {
        if selected.isEmpty { return "None" }
        if selected == Self.falseColorID { return "False Color" }
        return URL(fileURLWithPath: selected).deletingPathExtension().lastPathComponent
    }
    private func loadSelected() {
        if selected.isEmpty { lut = nil }
        else if selected == Self.falseColorID { lut = CubeLUT.falseColor() }
        else {
            lut = CubeLUT.load(URL(fileURLWithPath: selected))
            if lut == nil { DispatchQueue.main.async { self.selected = "" } }
        }
    }
    func add() {
        let p = NSOpenPanel(); p.allowedContentTypes = [UTType(filenameExtension: "cube") ?? .data]; p.allowsMultipleSelection = true
        p.prompt = "Add LUT"
        guard p.runModal() == .OK else { return }
        var last: URL?
        for u in p.urls {
            let dst = folder.appendingPathComponent(u.lastPathComponent)
            if FileManager.default.fileExists(atPath: dst.path) { try? FileManager.default.removeItem(at: dst) }
            if (try? FileManager.default.copyItem(at: u, to: dst)) != nil { last = dst }
        }
        refresh()
        if let l = last { selected = l.path }
    }
}

// MARK: - Viewer playback

/// Reads the playback helpers' records (braw_view, dng_view): "BRVF", u32 index, u32 width, u32 height, then RGBA8,
/// "BRVS", u32 index, u32 width << 16 | height, u32 slot for a BGRA8 frame in shared memory (braw_view --shm; the
/// slot is copied out and handed back with `release`), or "BRVE", u32 index, u32 error code, u32 0 for a frame the
/// helper couldn't read or decode.
enum BrawStream {
    enum Record { case frame(Int, CGImage), failed(Int, UInt32), shared(Int, Int, Int, UInt32) }   // shared: index, width, height, slot
    static func readExactly(_ h: FileHandle, _ n: Int) -> Data? {
        var d = Data(); d.reserveCapacity(n)
        while d.count < n { let c = h.readData(ofLength: n - d.count); if c.isEmpty { return nil }; d.append(c) }
        return d
    }
    static func readRecord(_ h: FileHandle, shm: SharedFrames? = nil, release: ((UInt32) -> Void)? = nil) -> Record? {
        guard let hd = readExactly(h, 16) else { return nil }
        let v = hd.withUnsafeBytes { p in (p.load(fromByteOffset: 4, as: UInt32.self), p.load(fromByteOffset: 8, as: UInt32.self), p.load(fromByteOffset: 12, as: UInt32.self)) }
        if hd.prefix(4) == Data("BRVE".utf8) { return .failed(Int(v.0), v.1) }
        if hd.prefix(4) == Data("BRVS".utf8) {
            guard let m = shm, Int(v.2) < m.slots else { return nil }
            let w = Int(v.1 >> 16), hgt = Int(v.1 & 0xFFFF)
            guard w > 0, hgt > 0, w * hgt * 4 <= m.slotBytes else { release?(v.2); return .failed(Int(v.0), 0x80004005) }
            return .shared(Int(v.0), w, hgt, v.2)            // copied out by `copyOut`, off the reading thread
        }
        guard hd.prefix(4) == Data("BRVF".utf8) else { return nil }
        let w = Int(v.1), hgt = Int(v.2)
        guard w > 0, hgt > 0, let px = readExactly(h, w * hgt * 4), let img = cgImageRGBA(px, w, hgt) else { return nil }
        return .frame(Int(v.0), img)
    }
    /// A shared-memory frame into a frame surface (then the slot can be handed back).
    static func copyOut(_ m: SharedFrames, _ slot: UInt32, _ w: Int, _ hgt: Int) -> CGImage? {
        let n = w * hgt * 4
        do {
            let src = m.base + Int(slot) * m.slotBytes
            guard let sf = SurfacePool.shared.get(w, hgt) else { return cgImageBGRA(Data(bytes: src, count: n), w, hgt) }
            sf.lock(options: [], seed: nil)
            let bpr = sf.bytesPerRow, dst = sf.baseAddress
            if bpr == w * 4 { memcpy(dst, src, n) } else { for y in 0..<hgt { memcpy(dst + y * bpr, src + y * w * 4, w * 4) } }
            sf.unlock(options: [], seed: nil)
            return surfaceImage(sf)
        }
    }
    static func readFrame(_ h: FileHandle) -> (Int, CGImage)? {
        if case .frame(let i, let img)? = readRecord(h) { return (i, img) }
        return nil
    }
}

enum ViewerLook: Int, CaseIterable {
    case rec709 = 0, film = 1, linear = 2, linear2020 = 3
    var label: String { ["Rec.709", "Blackmagic Film Gen 5", "Linear (Rec.709 primaries)", "Linear (Rec.2020 primaries)"][rawValue] }
    var short: String { ["709", "Film", "Lin 709", "Lin 2020"][rawValue] }
    var isLinear: Bool { self == .linear || self == .linear2020 }
}

enum PlaybackRes: Int, CaseIterable { case full = 1, half = 2, quarter = 4
    var label: String { ["", "Full", "Half", "", "Quarter"][rawValue] }
}

/// Plays a clip in the viewer. DNG frames go through the dng_view helper (the fpSup DNG reader with a fast
/// monitoring development, on every core), .braw through braw_view (Blackmagic RAW SDK), .mov through
/// AVFoundation. The clock follows the display's refresh (a display link): each refresh shows the frame that is
/// due, frames are requested for the refreshes ahead only (a 100 fps clip on a 60 Hz screen decodes the frames that
/// will be seen), and a frame counts as dropped only when it isn't ready when due. Frames go straight to the
/// viewer's layer; SwiftUI hears about the frame number a few times a second.
final class Player: ObservableObject {
    @Published var image: CGImage?             // the picture when paused (and the first one), for SwiftUI
    @Published var frame = 0                   // follows `pos`: immediately when paused, ~15 times a second when playing
    @Published var playing = false
    @Published var buffering = false
    @Published var muted = UserDefaults.standard.bool(forKey: "viewerMuted") { didSet { UserDefaults.standard.set(muted, forKey: "viewerMuted"); applyVolume() } }
    @Published var res = PlaybackRes(rawValue: UserDefaults.standard.integer(forKey: "playbackRes")) ?? .half { didSet { UserDefaults.standard.set(res.rawValue, forKey: "playbackRes"); reset(keepFrame: true) } }
    @Published var droppedRecently = false
    @Published var histogram: [[Float]] = []        // Y, R, G, B: 128 bins each, normalised to the largest bin
    @Published var error: String?
    var wantHistogram = false
    weak var sink: LayerImageView.V? { didSet { sink?.display(shown) } }
    private(set) var clip: Clip?
    private(set) var shown: CGImage?
    private(set) var pos = 0
    private var frames: [URL] = []
    private var count = 0
    private var fps: Double = 24
    var lut: CubeLUT? { didSet { updateViewLUT(); reset(keepFrame: true) } }
    /// How raw frames are decoded for the viewer (DNG and BRAW; .mov plays as AVFoundation renders it).
    @Published var look = ViewerLook(rawValue: UserDefaults.standard.integer(forKey: "viewerLook")) ?? .rec709 {
        didSet { UserDefaults.standard.set(look.rawValue, forKey: "viewerLook"); if look != oldValue { updateViewLUT(); reset(keepFrame: true) } }
    }
    private var viewLUT: CubeLUT?             // the LUT as applied: for Linear, with the log-to-linear step in front
    private func updateViewLUT() {
        if look.isLinear && clip?.kind != .mov { viewLUT = CubeLUT.linearInput(then: lut) } else { viewLUT = lut }
    }

    private let ctx = CIContext(options: [.cacheIntermediates: false, .outputColorSpace: CGColorSpace(name: CGColorSpace.sRGB)!])
    private let decodeQ = DispatchQueue(label: "viewer.decode", qos: .userInteractive, attributes: .concurrent)
    private let movQ = DispatchQueue(label: "viewer.mov", qos: .userInteractive)
    private let frameQ: OperationQueue = { let q = OperationQueue(); q.maxConcurrentOperationCount = Int(ProcessInfo.processInfo.environment["CDNG_FRAMEQ"] ?? "") ?? 1; q.qualityOfService = .userInteractive; return q }()
    private let lock = NSLock()
    private var cache: [Int: CGImage] = [:]
    private var cacheBytes = 0, cachePos = 0
    private let cacheCap = Int(min(1_200_000_000, ProcessInfo.processInfo.physicalMemory / 8))
    private var inflight = Set<Int>(), failed = Set<Int>()
    private var generation = 0
    private var link: CVDisplayLink?, timer: DispatchSourceTimer?
    private var tickQueued = false
    private var refresh = 60.0
    private var startTime = 0.0, startFrame = 0, bufferStart = 0.0, lastTarget = -1
    private var lastHisto = 0.0, lastPublish = 0.0, lastDrop = -10.0
    private var audio: AVAudioPlayer?
    private var avAudio: AVPlayer?             // .braw sound (QuickTime audio track)
    private var movPlayer: AVPlayer?, movItem: AVPlayerItem?, movOutput: AVPlayerItemVideoOutput?, movGen: AVAssetImageGenerator?
    private var movBusy = false, movSized = false
    private var helper: Process?, helperIn: FileHandle?, helperCancels = false
    private var maxInflight = 6
    private let cpuInflight = max(3, min(10, ProcessInfo.processInfo.activeProcessorCount - 1))
    private var reportedFailure = -1
    // playback statistics (for the test hook): frames due, frames shown on time
    private(set) var statDue = 0, statShown = 0

    var frameCount: Int { count }
    var currentFps: Double { fps }

    func load(_ c: Clip?) {
        pause(); stopHelper()
        lock.lock(); cache.removeAll(); cacheBytes = 0; inflight.removeAll(); failed.removeAll(); generation += 1; lock.unlock()
        clip = c; updateViewLUT(); pos = 0; frame = 0; image = nil; shown = nil; sink?.display(nil); error = nil; histogram = []; reportedFailure = -1
        fps = c?.info?.fps ?? 24
        audio = nil; avAudio = nil; movPlayer = nil; movItem = nil; movOutput = nil; movGen = nil
        guard let c = c else { frames = []; count = 0; return }
        switch c.kind {
        case .dng:
            frames = c.frames; count = frames.count
            if let w = c.wavURL { audio = try? AVAudioPlayer(contentsOf: w); audio?.prepareToPlay() }
            startHelper(c)
        case .braw:
            frames = []; count = c.info?.frames ?? 0
            if count > 0 { startHelper(c) }
            // the sound is an ordinary QuickTime audio track: AVFoundation plays it through a .mov-named link
            let link = FileManager.default.temporaryDirectory.appendingPathComponent("cdng-audio-\(abs(c.url.path.hashValue)).mov")
            try? FileManager.default.removeItem(at: link); try? FileManager.default.createSymbolicLink(at: link, withDestinationURL: c.url)
            avAudio = AVPlayer(url: link)
        case .mov:
            frames = []; count = c.info?.frames ?? 0
            let asset = AVURLAsset(url: c.url)
            let item = AVPlayerItem(asset: asset); movItem = item
            attachMovOutput()
            movPlayer = AVPlayer(playerItem: item)
            let g = AVAssetImageGenerator(asset: asset); g.requestedTimeToleranceBefore = .zero; g.requestedTimeToleranceAfter = .zero
            g.appliesPreferredTrackTransform = true; movGen = g
        }
        applyVolume()
        if count > 0 { show(0) }
    }
    func refreshInfo() {
        guard let c = clip else { return }
        fps = c.info?.fps ?? fps
        if c.kind != .dng, count == 0, let n = c.info?.frames, n > 0 { count = n; if c.kind == .braw && helper == nil { startHelper(c) }; show(0) }
        if c.kind == .mov, !movSized, (c.info?.width ?? 0) > 0 { attachMovOutput() }
        if c.kind == .dng, audio == nil, let w = c.wavURL { audio = try? AVAudioPlayer(contentsOf: w); applyVolume() }
    }
    private func applyVolume() { let v: Float = muted ? 0 : 1; audio?.volume = v; avAudio?.volume = v; movPlayer?.volume = v }
    private func reset(keepFrame: Bool) {
        let f = pos, was = playing
        if was { pause() }
        lock.lock(); cache.removeAll(); cacheBytes = 0; inflight.removeAll(); failed.removeAll(); generation += 1; lock.unlock()
        if let c = clip {
            if c.kind == .mov { attachMovOutput() }
            else if helper != nil || c.kind == .dng { stopHelper(); startHelper(c) }
        }
        if count > 0 { show(keepFrame ? f : 0) }
        if was { play() }
    }

    // ---- helpers: dng_view (DNG) and braw_view (Blackmagic RAW), same protocol
    private func startHelper(_ c: Clip) {
        let gen = generation
        var args: [String] = []
        let exe: URL?
        if c.kind == .braw {
            exe = bundledTool("braw_view"); maxInflight = 8; helperCancels = false
            args = ["--shm"]                     // frames in shared memory; the SDK decodes on the GPU (Metal)
            switch look {
            case .rec709: args += ["--gamma", "Rec.709", "--gamut", "Rec.709"]
            case .film: args += ["--gen", "5", "--gamma", "Blackmagic Design Film", "--gamut", "Blackmagic Design"]
            case .linear: args += ["--gen", "5", "--gamma", "Blackmagic Design Film", "--gamut", "Rec.709"]
            case .linear2020: args += ["--gen", "5", "--gamma", "Blackmagic Design Film", "--gamut", "Rec.2020"]
            }
            args.append(c.url.path)
            if exe == nil { error = "Blackmagic RAW playback helper missing"; return }
        } else {
            exe = bundledTool("dng_view"); helperCancels = true
            guard exe != nil, !frames.isEmpty else { maxInflight = cpuInflight; return }      // Core Image fallback
            let list = FileManager.default.temporaryDirectory.appendingPathComponent("cdng-view-\(abs(c.url.path.hashValue)).txt")
            do { try (frames.map { $0.path }.joined(separator: "\n") + "\n").write(to: list, atomically: true, encoding: .utf8) } catch { maxInflight = cpuInflight; return }
            args = ["--look", ["display", "film", "film709", "film2020"][look.rawValue], list.path]; maxInflight = max(8, min(24, 2 * ProcessInfo.processInfo.activeProcessorCount))
        }
        let p = Process(); p.executableURL = exe; p.arguments = args
        let inp = Pipe(), out = Pipe(); p.standardInput = inp; p.standardOutput = out; p.standardError = FileHandle.standardError
        do { try p.run() } catch { self.error = "couldn't start the playback helper"; return }
        helper = p; helperIn = inp.fileHandleForWriting
        let h = out.fileHandleForReading, kind = c.kind, toHelper = inp.fileHandleForWriting
        DispatchQueue.global(qos: .userInteractive).async {
            var line = Data()
            while true { let b = h.readData(ofLength: 1); if b.isEmpty || b[0] == 10 { break }; line.append(b) }
            let j = (try? JSONSerialization.jsonObject(with: line)) as? [String: Any]
            if let e = j?["error"] as? String {
                DispatchQueue.main.async { if gen == self.generation { self.error = e } }; return
            }
            var shm: SharedFrames?
            if let name = j?["shm"] as? String, let n = j?["slots"] as? Int, let b = j?["slot_bytes"] as? Int {
                shm = SharedFrames(name: name, slots: n, slotBytes: b)
                if shm == nil { DispatchQueue.main.async { if gen == self.generation { self.error = "couldn't open the Blackmagic RAW helper's shared memory" } }; return }
            }
            let release: (UInt32) -> Void = { s in try? toHelper.write(contentsOf: Data("r \(s)\n".utf8)) }
            while let r = BrawStream.readRecord(h, shm: shm, release: release) {
                switch r {
                case .frame(let i, let img):
                    let pic = self.viewLUT == nil ? img : (img.surface.flatMap { self.applyLUT(surface: $0) } ?? self.applyLUT(CIImage(cgImage: img)))
                    self.store(i, pic, gen)
                case .shared(let i, let w, let hgt, let slot):
                    guard let m = shm else { break }
                    self.frameQ.addOperation {           // copy (and LUT) one frame while the next is read; more at once was slower
                        let img = BrawStream.copyOut(m, slot, w, hgt); release(slot)
                        let pic = img.flatMap { i in self.viewLUT == nil ? i : (i.surface.flatMap { self.applyLUT(surface: $0) } ?? self.applyLUT(CIImage(cgImage: i))) }
                        if pic != nil { self.store(i, pic, gen) } else { self.store(i, nil, gen, failedCode: 0x80004005, kind: kind) }
                    }
                case .failed(let i, let code):
                    self.store(i, nil, gen, failedCode: code, kind: kind)
                }
            }
        }
    }
    private func stopHelper() {
        if let w = helperIn { try? w.write(contentsOf: Data("q\n".utf8)); try? w.close() }
        helper = nil; helperIn = nil
    }
    /// Drop queued requests (dng_view only) when the playhead jumps, so the helper starts on what's needed now.
    private func cancelQueued() {
        guard helperCancels, let w = helperIn else { return }
        try? w.write(contentsOf: Data("c\n".utf8))
        lock.lock(); inflight.removeAll(); lock.unlock()
    }

    // ---- decoding
    /// The LUT on the GPU, from one frame surface into another (no read-back to the CPU).
    private func applyLUT(surface s: IOSurface) -> CGImage? {
        guard let l = viewLUT, let out = SurfacePool.shared.get(s.width, s.height) else { return nil }
        let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
        let img = l.filter(on: CIImage(ioSurface: s, options: [.colorSpace: srgb]))
        let d = CIRenderDestination(ioSurface: out); d.colorSpace = srgb
        guard let task = try? ctx.startTask(toRender: img, to: d), (try? task.waitUntilCompleted()) != nil else { SurfacePool.shared.put(out); return nil }
        return surfaceImage(out)
    }
    private func applyLUT(_ img: CIImage) -> CGImage? {
        var i = img; if let l = viewLUT { i = l.filter(on: i) }
        return ctx.createCGImage(i, from: i.extent)
    }
    private func decodeDNG(_ i: Int) -> CGImage? {
        guard i >= 0, i < frames.count, let f = CIRAWFilter(imageURL: frames[i]) else { return nil }
        f.scaleFactor = Float(1.0 / Double(res.rawValue)); f.isDraftModeEnabled = res != .full
        guard let img = f.outputImage else { return nil }
        return applyLUT(img)
    }
    private func store(_ i: Int, _ cg: CGImage?, _ gen: Int, failedCode: UInt32? = nil, kind: ClipKind = .dng) {
        lock.lock()
        inflight.remove(i)
        if gen == generation {
            if let cg = cg {
                if let old = cache[i] { cacheBytes -= old.bytesPerRow * old.height }
                cache[i] = cg; cacheBytes += cg.bytesPerRow * cg.height
                if cacheBytes > cacheCap { evictLocked() }
            } else { failed.insert(i) }
        }
        lock.unlock()
        DispatchQueue.main.async {
            guard gen == self.generation else { return }
            if let cg = cg { if !self.playing, i == self.pos { self.setImage(cg) } }
            else if let code = failedCode { self.reportFailure(i, code, kind) }
            else if self.clip?.kind == .dng, !self.playing, i == self.pos { self.reportFailure(i, 0, .dng) }
        }
    }
    private func reportFailure(_ i: Int, _ code: UInt32, _ kind: ClipKind) {
        guard reportedFailure < 0 else { return }
        reportedFailure = i
        if kind == .braw, playing { pause() }
        if kind == .braw {
            error = String(format: "Frame %d couldn't be decoded (Blackmagic RAW SDK error 0x%08X).", i + 1, code)
                + (i > 0 ? " BRAW files made by cDNG Raw Convert 0.6.1 or earlier from whole-number frame rates (24, 25, 30, 48, 50…) have this problem: render the clip again with this version." : "")
        } else {
            error = "Frame \(i + 1) couldn't be read" + (i < frames.count ? " (\(frames[i].lastPathComponent))" : "")
        }
    }
    /// Free memory: frames behind the playhead first, then the ones furthest ahead.
    private func evictLocked() {
        let p = cachePos
        let behind = cache.keys.filter { $0 < p - 2 }.sorted()
        for k in behind where cacheBytes > cacheCap * 3 / 4 { if let c = cache.removeValue(forKey: k) { cacheBytes -= c.bytesPerRow * c.height } }
        if cacheBytes > cacheCap * 3 / 4 {
            for k in cache.keys.filter({ $0 > p + 2 }).sorted(by: >) where cacheBytes > cacheCap * 3 / 4 { if let c = cache.removeValue(forKey: k) { cacheBytes -= c.bytesPerRow * c.height } }
        }
    }
    private func request(_ i: Int) {
        guard i >= 0, i < count, let c = clip, c.kind != .mov else { return }
        lock.lock()
        if cache[i] != nil || inflight.contains(i) || failed.contains(i) || inflight.count >= maxInflight { lock.unlock(); return }
        inflight.insert(i); let gen = generation
        lock.unlock()
        if let w = helperIn {
            do { try w.write(contentsOf: Data("f \(i) \(res.rawValue)\n".utf8)) } catch { lock.lock(); inflight.remove(i); lock.unlock() }
        } else if c.kind == .dng {
            decodeQ.async { let cg = self.decodeDNG(i); self.store(i, cg, gen) }
        } else { lock.lock(); inflight.remove(i); lock.unlock() }
    }
    private func cached(_ i: Int) -> CGImage? { lock.lock(); defer { lock.unlock() }; return cache[i] }

    private func setImage(_ cg: CGImage) {
        shown = cg
        sink?.display(cg)
        if !playing || image == nil { image = cg }
        let now = CACurrentMediaTime()
        if wantHistogram, now - lastHisto > (playing ? 0.12 : 0) { lastHisto = now; computeHistogram(cg) }
    }
    private func setPos(_ i: Int, force: Bool = false) {
        pos = i
        lock.lock(); cachePos = i; lock.unlock()
        let now = CACurrentMediaTime()
        if force || !playing || now - lastPublish > 1.0 / 15 { lastPublish = now; if frame != i { frame = i } }
    }
    private func computeHistogram(_ cg: CGImage) {
        DispatchQueue.global(qos: .utility).async {
            let w = 192, h = max(1, Int(192.0 * Double(cg.height) / Double(max(1, cg.width))))
            var px = [UInt8](repeating: 0, count: w * h * 4)
            guard let bc = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                     bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue) else { return }
            bc.interpolationQuality = .low; bc.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
            var b = [[Float]](repeating: [Float](repeating: 0, count: 128), count: 4)
            var k = 0
            while k < px.count {
                let r = Int(px[k]), g = Int(px[k + 1]), bl = Int(px[k + 2])
                let y = (2126 * r + 7152 * g + 722 * bl) / 10000
                b[0][y >> 1] += 1; b[1][r >> 1] += 1; b[2][g >> 1] += 1; b[3][bl >> 1] += 1
                k += 4
            }
            for c in 0..<4 { let m = max(1, b[c].dropFirst().dropLast().max() ?? 1); b[c] = b[c].map { min(1, $0 / m) } }
            DispatchQueue.main.async { self.histogram = b }
        }
    }

    func show(_ i: Int) {
        guard count > 0 else { return }
        let i = max(0, min(count - 1, i))
        let jump = abs(i - pos) > 6
        setPos(i, force: true)
        if clip?.kind == .mov { showMov(i); return }
        if jump { cancelQueued() }
        if let h = cached(i) { setImage(h) } else { request(i) }
        for k in 1...4 { request(i + k) }
    }
    private func showMov(_ i: Int) {
        guard let g = movGen else { return }
        let gen = generation, t = CMTime(seconds: Double(i) / fps, preferredTimescale: 600000)
        movPlayer?.seek(to: t, toleranceBefore: .zero, toleranceAfter: .zero)
        let maxSize = movSize()
        decodeQ.async {
            g.maximumSize = maxSize
            guard let cg = try? g.copyCGImage(at: t, actualTime: nil) else { return }
            let pic = self.viewLUT == nil ? cg : (cg.surface.flatMap { self.applyLUT(surface: $0) } ?? self.applyLUT(CIImage(cgImage: cg)))
            DispatchQueue.main.async { if gen == self.generation, !self.playing, self.pos == i, let s = pic { self.setImage(s) } }
        }
    }
    func step(_ d: Int) { pause(); show(pos + d) }
    func toStart() { pause(); show(0) }
    func toEnd() { pause(); show(count - 1) }
    func toggle() { playing ? pause() : play() }

    // ---- .mov: the decoder scales to the playback resolution, and the LUT runs off the main thread
    private func movSize() -> CGSize {
        guard let i = clip?.info, i.width > 0, i.height > 0 else { return .zero }
        return CGSize(width: i.width / res.rawValue, height: i.height / res.rawValue)
    }
    private func attachMovOutput() {
        guard let item = movItem else { return }
        if let o = movOutput { item.remove(o) }
        var att: [String: Any] = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
        let s = movSize(); movSized = s.width > 0
        if s.width > 0, res != .full { att[kCVPixelBufferWidthKey as String] = Int(s.width); att[kCVPixelBufferHeightKey as String] = Int(s.height) }
        let out = AVPlayerItemVideoOutput(pixelBufferAttributes: att)
        item.add(out); movOutput = out
    }

    func play() {
        guard count > 0, !playing, let c = clip else { return }
        if pos >= count - 1 { setPos(0, force: true) }
        // a frame that couldn't be read: clear the message and try again (it comes back if the frame still fails)
        if reportedFailure >= 0 { error = nil; reportedFailure = -1; lock.lock(); failed.removeAll(); lock.unlock() }
        playing = true; statDue = 0; statShown = 0; lastTarget = -1; lastDrop = -10; if droppedRecently { droppedRecently = false }
        startTicks()
        if c.kind == .mov {
            movPlayer?.seek(to: CMTime(seconds: Double(pos) / fps, preferredTimescale: 600000), toleranceBefore: .zero, toleranceAfter: .zero) { _ in self.movPlayer?.play() }
            return
        }
        // pre-roll: start the clock once the first frames are ready (or after a moment, whatever happens)
        buffering = true; bufferStart = CACurrentMediaTime()
        cancelQueued()
        for f in plan(from: pos, at: bufferStart, startedAt: bufferStart, startFrame: pos) { request(f) }
    }
    /// The frames the next refreshes will show (each once), from time `t` on.
    private func plan(from: Int, at t: Double, startedAt t0: Double, startFrame f0: Int, seconds: Double = 0.75) -> [Int] {
        var out: [Int] = []; var last = -1
        let n = Int(refresh * seconds)
        for j in 0...n {
            let f = f0 + Int(((t + Double(j) / refresh) - t0) * fps)
            if f >= count { break }
            if f != last, f >= from { out.append(f); last = f }
        }
        return out
    }
    private func startTicks() {
        stopTicks()
        var l: CVDisplayLink?
        if CVDisplayLinkCreateWithActiveCGDisplays(&l) == kCVReturnSuccess, let l = l {
            let nominal = CVDisplayLinkGetNominalOutputVideoRefreshPeriod(l)
            if nominal.timeValue > 0 { refresh = max(24, min(240, Double(nominal.timeScale) / Double(nominal.timeValue))) }
            CVDisplayLinkSetOutputCallback(l, { _, _, _, _, _, ctx -> CVReturn in
                if let ctx = ctx { Unmanaged<Player>.fromOpaque(ctx).takeUnretainedValue().vsync() }
                return kCVReturnSuccess
            }, Unmanaged.passUnretained(self).toOpaque())
            CVDisplayLinkStart(l); link = l
        } else {
            refresh = 60
            let t = DispatchSource.makeTimerSource(queue: .main)
            t.schedule(deadline: .now(), repeating: 1.0 / 120, leeway: .milliseconds(1))
            t.setEventHandler { [weak self] in self?.tick() }
            t.resume(); timer = t
        }
    }
    private func vsync() {
        lock.lock(); if tickQueued { lock.unlock(); return }; tickQueued = true; lock.unlock()
        DispatchQueue.main.async { self.lock.lock(); self.tickQueued = false; self.lock.unlock(); self.tick() }
    }
    private func stopTicks() {
        if let l = link { CVDisplayLinkStop(l) }
        link = nil; timer?.cancel(); timer = nil
    }
    private func beginClock(_ now: Double) {
        buffering = false
        startFrame = pos; startTime = now; lastTarget = pos
        if let a = audio { a.currentTime = Double(pos) / fps; a.play() }
        if let a = avAudio { a.seek(to: CMTime(seconds: Double(pos) / fps, preferredTimescale: 48000), toleranceBefore: .zero, toleranceAfter: .zero); a.play() }
    }
    func pause() {
        stopTicks()
        if playing {
            playing = false; buffering = false; audio?.pause(); avAudio?.pause(); movPlayer?.pause()
            setPos(pos, force: true)
            if let s = shown { image = s }
            if droppedRecently { droppedRecently = false }
        }
    }
    private func tick() {
        guard playing, let c = clip else { return }
        if c.kind == .mov { tickMov(); return }
        let now = CACurrentMediaTime()
        if buffering {
            let p = plan(from: pos, at: now, startedAt: now, startFrame: pos)
            for f in p { request(f) }
            let need = min(p.count, max(4, min(maxInflight, Int(fps / 3))))
            lock.lock(); var ready = 0; for f in p.prefix(need) where cache[f] != nil || failed.contains(f) { ready += 1 }; lock.unlock()
            if ready >= need || now - bufferStart > 2.5 { beginClock(now) } else { return }
        }
        let target = startFrame + Int((now - startTime) * fps)
        if target >= count { pause(); show(count - 1); return }
        for f in plan(from: target, at: now, startedAt: startTime, startFrame: startFrame) { request(f) }
        if target != lastTarget {
            lastTarget = target; statDue += 1
            lock.lock()
            var best: Int? = cache[target] != nil ? target : nil
            if best == nil { for k in stride(from: target - 1, through: max(pos + 1, target - 12), by: -1) where cache[k] != nil { best = k; break } }
            let img = best.flatMap { cache[$0] }
            lock.unlock()
            if best == target { statShown += 1 } else { lastDrop = now }
            if let b = best, let img = img, b != pos || shown == nil { setPos(b); setImage(img) } else { setPos(pos) }
        }
        let dropping = now - lastDrop < 0.6
        if dropping != droppedRecently { droppedRecently = dropping }
    }
    private func tickMov() {
        guard let p = movPlayer, let out = movOutput else { return }
        let t = out.itemTime(forHostTime: CACurrentMediaTime())
        let i = max(0, min(count - 1, Int((t.seconds * fps).rounded(.down))))
        if i >= count - 1 && p.rate == 0 { pause(); return }
        if i != lastTarget, p.rate != 0 { lastTarget = i; statDue += 1 }
        guard !movBusy, out.hasNewPixelBuffer(forItemTime: t), let pb = out.copyPixelBuffer(forItemTime: t, itemTimeForDisplay: nil) else { return }
        movBusy = true
        let gen = generation
        movQ.async {
            let cg = self.applyLUT(CIImage(cvPixelBuffer: pb))
            DispatchQueue.main.async {
                self.movBusy = false
                guard gen == self.generation, self.playing, let cg = cg else { return }
                self.statShown += 1
                self.setPos(i); self.setImage(cg)
            }
        }
    }
    /// Clicking or dragging on the scrubber: paused, it shows the frame; playing, playback carries on from there
    /// (a short pre-roll at the new place), for spot-checking a clip.
    func scrub(to i: Int) {
        guard playing, count > 0, let c = clip else { show(i); return }
        let i = max(0, min(count - 1, i))
        if c.kind == .mov {
            setPos(i, force: true); lastTarget = -1
            movPlayer?.seek(to: CMTime(seconds: Double(i) / fps, preferredTimescale: 600000), toleranceBefore: .zero, toleranceAfter: .zero)
            return
        }
        audio?.pause(); avAudio?.pause()
        if abs(i - pos) > 6 { cancelQueued() }
        setPos(i, force: true)
        if let h = cached(i) { setImage(h) }
        buffering = true; bufferStart = CACurrentMediaTime(); lastTarget = -1
        for f in plan(from: i, at: bufferStart, startedAt: bufferStart, startFrame: i) { request(f) }
    }
}

/// The viewer's zoom, shared by the picture (which does the zooming) and the header's zoom menu.
final class ViewerZoom: ObservableObject {
    @Published var label = "Fit"                  // "Fit" or the size against the source frame, e.g. "100%"
    weak var view: LayerImageView.V?
    func fit() { view?.zoomToFit() }
    func setPercent(_ p: Double) { view?.zoom(toPercent: CGFloat(p)) }
}

/// The viewer picture: a layer showing the frame with the given display aspect (de-squeezed when anamorphic).
/// The player feeds it directly at the display's rate, without going through SwiftUI.
/// Zoom as in Resolve's viewer: the scroll wheel (or a pinch) zooms around the pointer, dragging pans,
/// Z goes back to fit; the header's zoom menu has Fit and 25-400 % of the source frame.
struct LayerImageView: NSViewRepresentable {
    let player: Player
    var squeeze: CGFloat = 1
    var sourceWidth: CGFloat = 0          // the clip's picture width in pixels (before de-squeeze)
    var zoomModel: ViewerZoom?
    final class V: NSView {
        let pic = CALayer()
        var aspect: CGFloat = 16 / 9
        var squeeze: CGFloat = 1 { didSet { if squeeze != oldValue { updateAspect(); place() } } }
        var sourceWidth: CGFloat = 0 { didSet { if sourceWidth != oldValue { report() } } }
        weak var zoomModel: ViewerZoom?
        private var imgW = 16, imgH = 9
        private(set) var zoom: CGFloat = 1                // 1 = fit
        private var pan = CGPoint.zero                     // picture centre from the view centre, in points
        private var dragFrom: NSPoint?
        override init(frame: NSRect) {
            super.init(frame: frame); wantsLayer = true; layer?.masksToBounds = true
            pic.contentsGravity = .resize; pic.magnificationFilter = .linear; pic.minificationFilter = .trilinear
            layer?.addSublayer(pic)
        }
        required init?(coder: NSCoder) { fatalError() }
        override var isFlipped: Bool { false }
        override func layout() { super.layout(); place() }
        func display(_ img: CGImage?) {
            CATransaction.begin(); CATransaction.setDisableActions(true)
            pic.contents = img?.surface ?? img          // a frame surface goes to the GPU as it is
            if let i = img, i.width != imgW || i.height != imgH { imgW = i.width; imgH = i.height; updateAspect(); placeLocked() }
            CATransaction.commit()
        }
        private func updateAspect() { aspect = CGFloat(imgW) * squeeze / CGFloat(max(1, imgH)) }
        func place() { CATransaction.begin(); CATransaction.setDisableActions(true); placeLocked(); CATransaction.commit() }
        private func fitSize() -> CGSize {
            let b = bounds; var w = b.width, h = w / aspect
            if h > b.height { h = b.height; w = h * aspect }
            return CGSize(width: w, height: h)
        }
        private func placeLocked() {
            let b = bounds; guard b.width > 0, b.height > 0 else { return }
            let f = fitSize(), w = f.width * zoom, h = f.height * zoom
            // keep part of the picture in view
            let mx = max(0, (w + b.width) / 2 - 60), my = max(0, (h + b.height) / 2 - 60)
            pan.x = max(-mx, min(mx, pan.x)); pan.y = max(-my, min(my, pan.y))
            if zoom <= 1 { pan = .zero }
            pic.frame = CGRect(x: (b.width - w) / 2 + pan.x, y: (b.height - h) / 2 + pan.y, width: w, height: h)
            window?.invalidateCursorRects(for: self)
        }
        /// Percent of the source frame: one source pixel (de-squeezed) per screen pixel is 100 %.
        private var percentPerZoom: CGFloat {
            let src = (sourceWidth > 0 ? sourceWidth : CGFloat(imgW)) * squeeze
            let bs = window?.backingScaleFactor ?? 2
            return src > 0 ? fitSize().width * bs / src * 100 : 100
        }
        private func report() {
            let l = zoom == 1 ? "Fit" : String(format: "%.0f%%", zoom * percentPerZoom)
            if let m = zoomModel, m.label != l { DispatchQueue.main.async { m.label = l } }
        }
        private func setZoom(_ z: CGFloat, around p: NSPoint?) {
            let nz = max(0.1, min(64, z))
            if let p = p {
                let c = NSPoint(x: bounds.midX + pan.x, y: bounds.midY + pan.y), r = nz / zoom
                pan = CGPoint(x: p.x - bounds.midX - (p.x - c.x) * r, y: p.y - bounds.midY - (p.y - c.y) * r)
            }
            zoom = abs(nz - 1) < 0.02 ? 1 : nz
            place(); report()
        }
        func zoomToFit() { pan = .zero; zoom = 1; place(); report() }
        func zoom(toPercent p: CGFloat) { let k = percentPerZoom; guard k > 0 else { return }; pan = .zero; setZoom(p / k, around: nil) }
        override func scrollWheel(with e: NSEvent) {
            let dy = e.hasPreciseScrollingDeltas ? e.scrollingDeltaY * 0.01 : e.scrollingDeltaY * 0.12
            guard dy != 0 else { return }
            setZoom(zoom * exp(dy), around: convert(e.locationInWindow, from: nil))
        }
        override func magnify(with e: NSEvent) { setZoom(zoom * (1 + e.magnification), around: convert(e.locationInWindow, from: nil)) }
        override func mouseDown(with e: NSEvent) { KeyRouter.endEditing(); dragFrom = convert(e.locationInWindow, from: nil) }
        override func otherMouseDown(with e: NSEvent) { dragFrom = convert(e.locationInWindow, from: nil) }
        override func mouseDragged(with e: NSEvent) { drag(e) }
        override func otherMouseDragged(with e: NSEvent) { drag(e) }
        private func drag(_ e: NSEvent) {
            let p = convert(e.locationInWindow, from: nil)
            if let f = dragFrom, zoom > 1 { pan.x += p.x - f.x; pan.y += p.y - f.y; place() }
            dragFrom = p
        }
        override func mouseUp(with e: NSEvent) { dragFrom = nil }
        override func otherMouseUp(with e: NSEvent) { dragFrom = nil }
        override func resetCursorRects() { if zoom > 1 { addCursorRect(bounds, cursor: .openHand) } }
    }
    func makeNSView(context: Context) -> V {
        let v = V(); v.squeeze = squeeze; v.sourceWidth = sourceWidth; v.zoomModel = zoomModel; zoomModel?.view = v; player.sink = v; return v
    }
    func updateNSView(_ v: V, context: Context) {
        v.squeeze = squeeze; v.sourceWidth = sourceWidth
        if v.zoomModel !== zoomModel { v.zoomModel = zoomModel; zoomModel?.view = v }
        if player.sink !== v { player.sink = v }
    }
}

/// The anamorphic de-squeeze factor: one setting for the viewer and the output, kept from clip to clip like the LUT.
/// While it's None, a Blackmagic RAW file that says it was shot anamorphic is shown with its own factor.
enum Squeeze {
    static let factors: [Double] = [1.0, 1.25, 1.33, 1.5, 1.6, 1.66, 1.8, 2.0]
    static let key = "viewerSqueeze"
    static func label(_ f: Double) -> String { f <= 1.0 ? "None" : (abs(f - f.rounded()) < 0.001 ? String(format: "%.1fx", f) : String(format: "%gx", f)) }
    static var current: Double { let v = UserDefaults.standard.double(forKey: key); return v >= 1 ? v : 1 }
    static func get(_ c: Clip?) -> Double {
        if current > 1 { return current }
        if let c = c, c.kind == .braw, (c.brawMeta["anamorphic_enable"] as? Int ?? 0) != 0, let s = c.brawMeta["anamorphic"] as? String, let v = Double(s.replacingOccurrences(of: "x", with: "")), v > 1 { return v }
        return 1
    }
    static func set(_ f: Double) { UserDefaults.standard.set(f, forKey: key) }
}

/// A small histogram in the style of a grading suite's scope: luma and the three channels, each switchable.
struct HistogramView: View {
    let bins: [[Float]]
    @AppStorage("histoY") var showY = true
    @AppStorage("histoR") var showR = true
    @AppStorage("histoG") var showG = true
    @AppStorage("histoB") var showB = true
    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 6) {
                Text("HISTOGRAM").font(.system(size: 9, weight: .semibold)).foregroundColor(Th.dim2)
                Spacer()
                chan("Y", Color(hex: 0xe6e6e6), $showY); chan("R", Color(hex: 0xf0645a), $showR); chan("G", Color(hex: 0x6ad07c), $showG); chan("B", Color(hex: 0x6aa3f0), $showB)
            }
            Canvas { gc, size in
                for x in stride(from: 0.0, through: Double(size.width), by: Double(size.width) / 4) {
                    var p = Path(); p.move(to: CGPoint(x: x, y: 0)); p.addLine(to: CGPoint(x: x, y: size.height)); gc.stroke(p, with: .color(Color.white.opacity(0.07)), lineWidth: 1)
                }
                guard bins.count == 4 else { return }
                let order: [(Int, Color, Bool)] = [(1, Color(hex: 0xf0645a), showR), (2, Color(hex: 0x6ad07c), showG), (3, Color(hex: 0x6aa3f0), showB)]
                for (c, col, on) in order where on {
                    var p = Path(); p.move(to: CGPoint(x: 0, y: size.height))
                    for (i, v) in bins[c].enumerated() { p.addLine(to: CGPoint(x: Double(size.width) * Double(i) / 127, y: Double(size.height) * (1 - Double(v)))) }
                    p.addLine(to: CGPoint(x: size.width, y: size.height)); p.closeSubpath()
                    gc.fill(p, with: .color(col.opacity(0.33))); gc.stroke(p, with: .color(col.opacity(0.8)), lineWidth: 1)
                }
                if showY {
                    var p = Path()
                    for (i, v) in bins[0].enumerated() { let pt = CGPoint(x: Double(size.width) * Double(i) / 127, y: Double(size.height) * (1 - Double(v))); if i == 0 { p.move(to: pt) } else { p.addLine(to: pt) } }
                    gc.stroke(p, with: .color(Color.white.opacity(0.9)), lineWidth: 1.2)
                }
            }
        }
        .padding(8).frame(minWidth: 180, maxWidth: .infinity, minHeight: 90, maxHeight: .infinity)
        .background(Color(hex: 0x0c0c0e))
    }
    func chan(_ t: String, _ c: Color, _ on: Binding<Bool>) -> some View {
        Text(t).font(.system(size: 9, weight: .bold)).foregroundColor(on.wrappedValue ? c : Th.dim2)
            .frame(width: 16, height: 14).background(RoundedRectangle(cornerRadius: 2).fill(on.wrappedValue ? Color.white.opacity(0.08) : Color.clear))
            .onTapGesture { on.wrappedValue.toggle() }
    }
}

/// The histogram as a floating palette: its own small utility window over the main one, resizable, remembered
/// where it was put. It never takes the keyboard, so the space bar keeps controlling playback.
final class HistogramPalette: NSObject, NSWindowDelegate {
    static let shared = HistogramPalette()
    private var panel: NSPanel?
    private struct Content: View {
        @ObservedObject var player: Player
        var body: some View { HistogramView(bins: player.histogram).preferredColorScheme(.dark) }
    }
    func show(_ player: Player) {
        if panel == nil {
            let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 300, height: 170), styleMask: [.titled, .closable, .resizable, .utilityWindow, .nonactivatingPanel],
                            backing: .buffered, defer: false)
            p.title = "Histogram"; p.isFloatingPanel = true; p.becomesKeyOnlyIfNeeded = true; p.hidesOnDeactivate = true
            p.appearance = NSAppearance(named: .darkAqua); p.backgroundColor = NSColor(red: 0.047, green: 0.047, blue: 0.055, alpha: 1)
            p.minSize = NSSize(width: 200, height: 120); p.isReleasedWhenClosed = false; p.delegate = self
            p.contentView = NSHostingView(rootView: Content(player: player))
            if !p.setFrameUsingName("cdngHistogram") {
                if let w = NSApp.mainWindow ?? NSApp.windows.first { let f = w.frame; p.setFrameOrigin(NSPoint(x: f.maxX - 340, y: f.minY + 120)) } else { p.center() }
            }
            p.setFrameAutosaveName("cdngHistogram")
            panel = p
        }
        panel?.orderFront(nil)
    }
    func hide() { panel?.orderOut(nil) }
    func windowWillClose(_ notification: Notification) { UserDefaults.standard.set(false, forKey: "showHistogram") }
}

/// Keyboard and focus for the viewer: space plays/pauses and the arrows step, from anywhere in the window, unless
/// a text field is being edited (the white-balance fields, for example). Nothing starts out in a text field, and a
/// click anywhere outside the field that's being edited (a button, a clip, the viewer) ends the editing.
enum KeyRouter {
    private static var monitors: [Any] = []
    static func editingField(_ w: NSWindow?) -> NSTextView? {
        guard let t = w?.firstResponder as? NSTextView, t.isFieldEditor else { return nil }
        return t
    }
    static func endEditing() {
        for w in NSApp.windows where editingField(w) != nil { w.makeFirstResponder(nil) }
    }
    static func install() {
        guard monitors.isEmpty else { return }
        if let m = NSEvent.addLocalMonitorForEvents(matching: .keyDown, handler: { e in
            let mods = e.modifierFlags.intersection([.command, .option, .control, .shift])
            guard mods.isEmpty, let p = TestHook.player else { return e }
            let w = e.window ?? NSApp.keyWindow
            if editingField(w) != nil || w?.attachedSheet != nil || NSApp.modalWindow != nil { return e }
            if let t = w?.firstResponder as? NSTextView, t.isEditable { return e }
            switch e.keyCode {
            case 49: p.toggle()                  // space
            case 123: p.step(-1)                 // left
            case 124: p.step(1)                  // right
            case 126: p.toStart()                // up
            case 125: p.toEnd()                  // down
            case 6: TestHook.zoom?.fit()         // Z: zoom to fit
            case 51, 117:                        // Delete / forward delete: remove the selected jobs
                guard let s = TestHook.queueSel, !s.ids.isEmpty, let en = TestHook.engine else { return e }
                en.remove(s.ids); s.ids = []
            default: return e
            }
            return nil
        }) { monitors.append(m) }
        if let m = NSEvent.addLocalMonitorForEvents(matching: [.leftMouseDown, .rightMouseDown], handler: { e in
            guard let w = e.window, let ed = editingField(w), let field = ed.delegate as? NSView else { return e }
            let inside = field.bounds.contains(field.convert(e.locationInWindow, from: nil))
            if !inside { DispatchQueue.main.async { if editingField(w) === ed { w.makeFirstResponder(nil) } } }
            return e
        }) { monitors.append(m) }
    }
    /// After the window opens, take the cursor out of whatever text field AppKit put it in.
    static func clearInitialFocus() {
        for t in [0.2, 0.8, 2.0] { DispatchQueue.main.asyncAfter(deadline: .now() + t) { endEditing() } }
    }
}

// MARK: - Output settings

enum OutputFormat: Int, CaseIterable, Identifiable {
    case proresRaw = 0, braw = 2, cdng = 1          // raw values kept from 0.3 (saved settings)
    var id: Int { rawValue }
    var short: String { [0: "ProRes RAW", 2: "BRAW", 1: "CinemaDNG"][rawValue]! }
    var title: String { [1: "CinemaDNG Compress", 2: "Blackmagic RAW", 0: "Apple ProRes RAW"][rawValue]! }
    var sub: String { [1: "image sequence", 2: ".braw file", 0: ".mov file"][rawValue]! }
    var order: Int { [1: 1, 2: 2, 0: 3][rawValue]! }
    var long: String { [0: "Apple ProRes RAW", 2: "Blackmagic RAW", 1: "Compressed CinemaDNG"][rawValue]! }
    var movieExtension: String { self == .braw ? "braw" : "mov" }
    static var ordered: [OutputFormat] { [.cdng, .braw, .proresRaw] }
}

enum Quality: Int, CaseIterable {
    case nearLossless = 1, high = 2, standard = 4, compact = 8
    var label: String { [1: "Near-lossless", 2: "High", 4: "Standard", 8: "Compact"][rawValue]! }
    var note: String {
        switch self {
        case .nearLossless: return "largest files, about half the DNG size"
        case .high: return "about 18% smaller than near-lossless"
        case .standard: return "the compression of a Ninja V \"ProRes RAW HQ\" recording"
        case .compact: return "about 50% smaller than near-lossless"
        }
    }
}

/// Compressed CinemaDNG modes, named as in slimRAW. `arg` is the encoder's --cdng-mode.
struct DngMode: Identifiable, Hashable {
    let arg: String, label: String, note: String, suffix: String
    var id: String { arg }
    static let all: [DngMode] = [
        .init(arg: "lossless", label: "Lossless", note: "bit-exact; typically 2.4-2.7:1 for 12-bit fp footage, 1.4:1 for 14-bit", suffix: "lossless"),
        .init(arg: "log10", label: "Lossless 10-bit log", note: "10-bit curve matched to sensor noise (visually lossless); about 2-3:1", suffix: "lossless-10log"),
        .init(arg: "vbr-hq", label: "Lossy VBR HQ", note: "constant quality, high; typically 3.4-3.9:1 for 12-bit", suffix: "vbr-hq"),
        .init(arg: "vbr-lt", label: "Lossy VBR LT", note: "constant quality, lighter; typically 5-6.7:1 for 12-bit", suffix: "vbr-lt"),
        .init(arg: "3", label: "Lossy 3:1", note: "fixed 3:1 against the uncompressed image", suffix: "lossy3-1"),
        .init(arg: "4", label: "Lossy 4:1", note: "fixed 4:1 against the uncompressed image", suffix: "lossy4-1"),
        .init(arg: "5", label: "Lossy 5:1", note: "fixed 5:1 against the uncompressed image", suffix: "lossy5-1"),
        .init(arg: "7", label: "Lossy 7:1", note: "fixed 7:1 against the uncompressed image", suffix: "lossy7-1"),
    ]
    static let legacySuffixes = ["LOSSLESS", "LOG10", "VBRHQ", "VBRLT", "3TO1", "4TO1", "5TO1", "7TO1"]   /* before 0.6.1 */
    /// folders this app made next to a clip, skipped when searching for clips
    static func isOutputFolder(_ name: String) -> Bool { all.contains { name.hasSuffix("_" + $0.suffix) } || legacySuffixes.contains { name.hasSuffix("_" + $0) } }
}

enum ExistingPolicy: Int, CaseIterable {
    case skip = 0, overwrite = 1, keepBoth = 2
    var label: String { ["Skip", "Overwrite", "Keep both"][rawValue] }
}

/// Every output choice, saved between launches. A job takes a copy when it is queued.
final class OutputModel: ObservableObject {
    private let d = UserDefaults.standard
    @Published var format: OutputFormat { didSet { d.set(format.rawValue, forKey: "format") } }
    @Published var quality: Int { didSet { d.set(quality, forKey: "quality") } }
    @Published var brawByQuality: Bool { didSet { d.set(brawByQuality, forKey: "brawByQuality") } }
    /// How Resolve and Blackmagic's software open the .braw by default: Blackmagic Film Gen 5 (flat) or Rec.709.
    @Published var brawFilm: Bool { didSet { d.set(brawFilm, forKey: "brawFilm") } }
    @Published var brawRatio: Int { didSet { d.set(brawRatio, forKey: "brawRatio") } }
    @Published var brawQ: Int { didSet { d.set(brawQ, forKey: "brawQ") } }
    @Published var dngMode: String { didSet { d.set(dngMode, forKey: "dngMode") } }
    @Published var greyCard: Bool { didSet { d.set(greyCard, forKey: "wbOverride") } }
    @Published var wbR: String { didSet { d.set(wbR, forKey: "wbR") } }
    @Published var wbB: String { didSet { d.set(wbB, forKey: "wbB") } }
    @Published var audio: Bool { didSet { d.set(audio, forKey: "embedAudio") } }
    @Published var gyro: Bool { didSet { d.set(gyro, forKey: "embedGyro") } }
    @Published var sidecars: Bool { didSet { d.set(sidecars, forKey: "copySidecars") } }
    @Published var copyOther: Bool { didSet { d.set(copyOther, forKey: "copyOther") } }
    @Published var verify: Bool { didSet { d.set(verify, forKey: "verify") } }
    @Published var gpu: Bool { didSet { d.set(gpu, forKey: "useGPU") } }
    @Published var nextToClip: Bool { didSet { d.set(nextToClip, forKey: "nextToClip") } }
    @Published var destination: String { didSet { d.set(destination, forKey: "destination") } }
    @Published var policy: Int { didSet { d.set(policy, forKey: "policy") } }
    @Published var openWhenDone: Bool { didSet { d.set(openWhenDone, forKey: "openWhenDone") } }
    @Published var squeeze: Bool { didSet { d.set(squeeze, forKey: "writeSqueeze") } }

    init() {
        let ud = UserDefaults.standard
        let b: (String, Bool) -> Bool = { k, def in ud.object(forKey: k) == nil ? def : ud.bool(forKey: k) }
        let i: (String, Int) -> Int = { k, def in ud.object(forKey: k) == nil ? def : ud.integer(forKey: k) }
        let s: (String, String) -> String = { k, def in ud.string(forKey: k) ?? def }
        format = OutputFormat(rawValue: i("format", 0)) ?? .proresRaw
        quality = i("quality", 1); brawByQuality = b("brawByQuality", false); brawFilm = b("brawFilm", true); brawRatio = i("brawRatio", 5); brawQ = i("brawQ", 0)
        dngMode = s("dngMode", "lossless"); greyCard = b("wbOverride", false); wbR = s("wbR", "0.737"); wbB = s("wbB", "0.367")
        audio = b("embedAudio", true); gyro = b("embedGyro", true); sidecars = b("copySidecars", true)
        copyOther = b("copyOther", true); verify = b("verify", true); gpu = b("useGPU", true)
        nextToClip = b("nextToClip", true); destination = s("destination", ""); policy = i("policy", 0); openWhenDone = b("openWhenDone", false); squeeze = b("writeSqueeze", true)
    }
    var neutral: String? {
        guard greyCard else { return nil }
        guard let r = Double(wbR.trimmingCharacters(in: .whitespaces)), let b = Double(wbB.trimmingCharacters(in: .whitespaces)), r > 0.05, r < 5, b > 0.05, b < 5 else { return "invalid" }
        return "\(r),\(b)"
    }
    var dng: DngMode { DngMode.all.first { $0.arg == dngMode } ?? DngMode.all[0] }
    var brawLabel: String { brawByQuality ? "Q\(brawQ)" : "\(brawRatio):1" }
    var summary: String {
        switch format {
        case .proresRaw: return "ProRes RAW · \(Quality(rawValue: quality)?.label ?? "")"
        case .braw: return "Blackmagic RAW · \(brawLabel)"
        case .cdng: return "CinemaDNG · \(dng.label)"
        }
    }
    func snapshot() -> OutputSnapshot {
        OutputSnapshot(format: format, quality: quality, brawArgs: (brawByQuality ? ["--quality", "Q\(brawQ)"] : ["--ratio", String(brawRatio)]) + ["--viewing", brawFilm ? "film" : "rec709"],
                       dng: dng, neutral: neutral, audio: audio, gyro: gyro, squeeze: squeeze, sidecars: sidecars, copyOther: copyOther, verify: verify, gpu: gpu,
                       destination: nextToClip || destination.isEmpty ? nil : URL(fileURLWithPath: destination),
                       policy: ExistingPolicy(rawValue: policy) ?? .skip, summary: summary)
    }
}

struct OutputSnapshot {
    let format: OutputFormat; let quality: Int; let brawArgs: [String]; let dng: DngMode; let neutral: String?
    let audio, gyro, squeeze, sidecars, copyOther, verify, gpu: Bool
    let destination: URL?; let policy: ExistingPolicy; let summary: String
}

// MARK: - Render queue

enum JobState: Equatable { case queued, rendering, done, skipped, failed, stopped }

final class RenderJob: ObservableObject, Identifiable {
    let id = UUID()
    let clipURL: URL
    let settings: OutputSnapshot
    let squeeze: Double                 // the clip's anamorphic factor when it was queued
    @Published var state: JobState = .queued
    @Published var progress = 0.0
    @Published var detail = "queued"
    @Published var rate = ""
    @Published var output: URL?
    var name: String { clipURL.lastPathComponent }
    init(clipURL: URL, settings: OutputSnapshot, squeeze: Double = 1) { self.clipURL = clipURL; self.settings = settings; self.squeeze = squeeze }
}

final class Engine: ObservableObject {
    @Published var jobs: [RenderJob] = []
    @Published var running = false
    @Published var log: [String] = []
    private var process: Process?
    private var stopRequested = false
    private let maxLogLines = 4000

    var queuedCount: Int { jobs.filter { $0.state == .queued }.count }
    var current: RenderJob? { jobs.first { $0.state == .rendering } }

    func append(_ s: String) {
        DispatchQueue.main.async {
            self.log.append(s)
            if self.log.count > self.maxLogLines { self.log.removeFirst(self.log.count - self.maxLogLines) }
        }
    }
    func add(_ clips: [Clip], _ s: OutputSnapshot) {
        for c in clips where c.convertible { jobs.append(RenderJob(clipURL: c.url, settings: s, squeeze: Squeeze.get(c))) }
    }
    func clearFinished() { jobs.removeAll { $0.state != .queued && $0.state != .rendering } }
    func remove(_ j: RenderJob) { if j.state != .rendering { jobs.removeAll { $0.id == j.id } } }
    func remove(_ ids: Set<UUID>) { jobs.removeAll { ids.contains($0.id) && $0.state != .rendering } }
    /// Moves jobs (in their current order) to just before `target`, or to the end when `target` is nil.
    func move(_ ids: Set<UUID>, before target: UUID?) {
        guard !ids.isEmpty, target.map({ !ids.contains($0) }) ?? true else { return }
        let moving = jobs.filter { ids.contains($0.id) }
        var rest = jobs.filter { !ids.contains($0.id) }
        let at = target.flatMap { t in rest.firstIndex { $0.id == t } } ?? rest.count
        rest.insert(contentsOf: moving, at: at); jobs = rest
    }
    func move(_ ids: Set<UUID>, by d: Int) {
        let idx = jobs.indices.filter { ids.contains(jobs[$0].id) }
        guard let lo = idx.first, let hi = idx.last else { return }
        if d < 0, lo > 0 { move(ids, before: jobs[lo - 1].id) }
        else if d > 0, hi < jobs.count - 1 { move(ids, before: hi + 2 < jobs.count ? jobs[hi + 2].id : nil) }
    }

    @discardableResult
    private func run(_ tool: String, _ args: [String], onLine: @escaping (String) -> Void) -> Int32 {
        guard let exe = bundledTool(tool) else { onLine("ERROR: the \(tool) encoder is missing from the app bundle."); return -1 }
        let p = Process(); p.executableURL = exe; p.arguments = args
        let pipe = Pipe(); p.standardOutput = pipe; p.standardError = pipe
        var pending = Data(); let lock = NSLock()
        pipe.fileHandleForReading.readabilityHandler = { h in
            let d = h.availableData; if d.isEmpty { return }
            lock.lock(); pending.append(d)
            while let nl = pending.firstIndex(of: 10) {
                let line = String(decoding: pending[pending.startIndex..<nl], as: UTF8.self)
                pending = Data(pending[pending.index(after: nl)...]); onLine(line)
            }
            lock.unlock()
        }
        do { try p.run() } catch { onLine("ERROR: could not start the encoder: \(error.localizedDescription)"); return -1 }
        DispatchQueue.main.sync { self.process = p }
        p.waitUntilExit()
        pipe.fileHandleForReading.readabilityHandler = nil
        let rest = pipe.fileHandleForReading.readDataToEndOfFile()
        lock.lock(); pending.append(rest); let tail = String(decoding: pending, as: UTF8.self); lock.unlock()
        for l in tail.split(separator: "\n") where !l.isEmpty { onLine(String(l)) }
        DispatchQueue.main.sync { self.process = nil }
        return p.terminationStatus
    }

    func stop() { stopRequested = true; process?.terminate() }

    func renderAll(openWhenDone: Bool) {
        guard !running, queuedCount > 0 else { return }
        running = true; stopRequested = false
        append("cDNG Raw Convert \(appVersion): rendering \(queuedCount) job\(queuedCount == 1 ? "" : "s")")
        DispatchQueue.global(qos: .userInitiated).async {
            var outputs: [URL] = []
            while !self.stopRequested, let j = DispatchQueue.main.sync(execute: { self.jobs.first { $0.state == .queued } }) {
                DispatchQueue.main.sync { j.state = .rendering; j.detail = "starting"; j.progress = 0 }
                let (state, detail, out) = self.renderOne(j)
                DispatchQueue.main.sync { j.state = state; j.detail = detail; j.output = out; if state == .done { j.progress = 1 } }
                if let o = out, state == .done { outputs.append(o) }
            }
            let done = DispatchQueue.main.sync { self.jobs.filter { $0.state == .done }.count }
            self.append((self.stopRequested ? "Stopped. " : "Finished. ") + "\(done) job\(done == 1 ? "" : "s") done.")
            DispatchQueue.main.async {
                self.running = false
                if openWhenDone, !outputs.isEmpty { NSWorkspace.shared.activateFileViewerSelecting(outputs) }
            }
        }
    }

    /// Progress lines "frame F/T ... (X fps)" from both encoders.
    private func progress(_ j: RenderJob, _ line: String, _ lastLogged: inout Int) {
        guard let r = line.range(of: #"frame (\d+)/(\d+)"#, options: .regularExpression) else { return }
        let parts = line[r].dropFirst(6).split(separator: "/")
        guard parts.count == 2, let f = Int(parts[0]), let t = Int(parts[1]), t > 0 else { return }
        var rate = ""
        if let fr = line.range(of: #"\(([\d.]+) fps\)"#, options: .regularExpression) { rate = String(line[fr].dropFirst().dropLast().replacingOccurrences(of: " fps", with: "")) + " fps" }
        DispatchQueue.main.async { j.progress = Double(f) / Double(t); j.detail = "\(f) / \(t)"; j.rate = rate }
        let step = max(1, t / 10)
        if f == t || f / step != lastLogged { lastLogged = f / step; append("    " + line.trimmingCharacters(in: .whitespaces)) }
    }

    private func renderOne(_ j: RenderJob) -> (JobState, String, URL?) {
        let s = j.settings, fm = FileManager.default, clipURL = j.clipURL, name = j.name
        append(""); append("\(name): \(s.summary)")
        if s.format == .cdng { return compressDNG(j) }
        var outDir = s.destination ?? clipURL.deletingLastPathComponent()
        if s.destination == nil && !fm.isWritableFile(atPath: outDir.path) { outDir = fm.homeDirectoryForCurrentUser.appendingPathComponent("Movies/cDNG Raw Convert") }
        try? fm.createDirectory(at: outDir, withIntermediateDirectories: true)
        let ext = s.format.movieExtension
        var out = outDir.appendingPathComponent(name + "." + ext)
        if fm.fileExists(atPath: out.path) {
            switch s.policy {
            case .skip: append("    \(out.lastPathComponent) already exists, skipped."); return (.skipped, "exists, skipped", out)
            case .overwrite: break
            case .keepBoth: var n = 2; while fm.fileExists(atPath: out.path) { out = outDir.appendingPathComponent("\(name)_\(n).\(ext)"); n += 1 }
            }
        }
        // dng2prr writes the name it is given; dng2braw needs a .braw name and writes <name>.partial itself
        let partial = outDir.appendingPathComponent("." + out.lastPathComponent + (s.format == .braw ? "" : ".partial"))
        let brawTemp = URL(fileURLWithPath: partial.path + ".partial")
        try? fm.removeItem(at: partial); if s.format == .braw { try? fm.removeItem(at: brawTemp) }
        append("    → \(out.path)")
        var args: [String]
        if s.format == .braw {
            args = [partial.path, clipURL.path] + s.brawArgs + ["--progress-every", "1"]
            if !s.gpu { args.append("--cpu") }
        } else {
            args = [partial.path, clipURL.path, "--scale", String(s.quality), "--progress-every", "1"]
            if !s.gpu { args.append("--cpu") }
        }
        if !s.audio { args.append("--no-audio") }
        if !s.gyro { args.append("--no-gyro") }
        if let n = s.neutral, n != "invalid" { args += ["--neutral", n] }
        if s.squeeze && j.squeeze > 1 { args += ["--desqueeze", String(j.squeeze)] }
        var lastLogged = -1
        let rc = run(s.format == .braw ? "dng2braw" : "dng2prr", args) { line in
            if line.contains("frame ") && line.contains("/") { self.progress(j, line, &lastLogged) }
            else if !line.hasPrefix("wrote ") { self.append("    " + line) }
        }
        if s.format == .braw { try? fm.removeItem(at: brawTemp) }
        if stopRequested { try? fm.removeItem(at: partial); append("    stopped; partial file removed."); return (.stopped, "stopped", nil) }
        if rc == 0 {
            do {
                if fm.fileExists(atPath: out.path) { try fm.removeItem(at: out) }
                try fm.moveItem(at: partial, to: out)
                let mb = (try? fm.attributesOfItem(atPath: out.path)[.size] as? NSNumber)?.doubleValue ?? 0
                if s.sidecars { copySidecars(from: clipURL, movie: out) }
                let size = mb > 1e9 ? String(format: "%.2f GB", mb / 1e9) : String(format: "%.0f MB", mb / 1e6)
                append("    done: \(size)")
                return (.done, size, out)
            } catch { append("    ERROR: could not move the finished file into place: \(error.localizedDescription)"); return (.failed, "move failed", nil) }
        }
        try? fm.removeItem(at: partial)
        if s.format == .braw && rc == 3 {
            append("    not converted: this recording mode isn't supported by the BRAW encoder yet (it handles fp UHD and open gate OG3K).")
            return (.skipped, "mode not supported yet", nil)
        }
        append("    FAILED (encoder exit code \(rc))."); return (.failed, "failed (\(rc))", nil)
    }

    /// Gyroflow / gyro files fpSup writes into the clip folder are copied next to the movie, named after it.
    private func copySidecars(from clip: URL, movie: URL) {
        let fm = FileManager.default, exts = ["gcsv", "jsonl", "gyroflow", "json", "gyr"]
        guard let names = try? fm.contentsOfDirectory(atPath: clip.path) else { return }
        var used = Set<String>()
        for n in names.sorted() where !n.hasPrefix(".") {
            let ext = (n as NSString).pathExtension.lowercased(); guard exts.contains(ext) else { continue }
            let base = movie.deletingPathExtension().lastPathComponent
            var dst = movie.deletingLastPathComponent().appendingPathComponent(base + "." + ext)
            if used.contains(ext) { dst = movie.deletingLastPathComponent().appendingPathComponent(base + "_" + n) }
            used.insert(ext)
            if fm.fileExists(atPath: dst.path) { try? fm.removeItem(at: dst) }
            do { try fm.copyItem(at: clip.appendingPathComponent(n), to: dst); append("    copied \(n) → \(dst.lastPathComponent)") }
            catch { append("    could not copy \(n): \(error.localizedDescription)") }
        }
    }

    private func compressDNG(_ j: RenderJob) -> (JobState, String, URL?) {
        let s = j.settings, fm = FileManager.default, clipURL = j.clipURL
        var outClip: URL
        if let dest = s.destination { outClip = dest.appendingPathComponent(clipURL.lastPathComponent) }
        else {
            var parent = clipURL.deletingLastPathComponent()
            if !fm.isWritableFile(atPath: parent.path) { parent = fm.homeDirectoryForCurrentUser.appendingPathComponent("Movies/cDNG Raw Convert") }
            outClip = parent.appendingPathComponent(clipURL.lastPathComponent + "_" + s.dng.suffix)
        }
        if fm.fileExists(atPath: outClip.path), s.policy == .keepBoth {
            var n = 2; let base = outClip; while fm.fileExists(atPath: outClip.path) { outClip = URL(fileURLWithPath: base.path + "_\(n)"); n += 1 }
        }
        append("    → \(outClip.path)")
        var args = ["--cdng", outClip.path, clipURL.path, "--cdng-mode", s.dng.arg, "--progress-every", "1"]
        if s.copyOther { args.append("--copy-other") }
        if s.verify { args.append("--verify") }
        if !s.gpu { args.append("--cpu") }
        if s.policy == .skip { args.append("--skip-existing") }
        var lastLogged = -1
        let rc = run("dng2prr", args) { line in
            if line.contains("frame ") && line.contains("/") { self.progress(j, line, &lastLogged) }
            else if !line.hasPrefix("copied ") && !line.contains("frame(s), CinemaDNG") { self.append("    " + line) }
        }
        if let names = try? fm.contentsOfDirectory(atPath: outClip.path) {
            for n in names where n.hasPrefix(".") && n.hasSuffix(".partial") { try? fm.removeItem(at: outClip.appendingPathComponent(n)) }
        }
        if stopRequested { append("    stopped; frames written so far are complete DNG files."); return (.stopped, "stopped", outClip) }
        if rc != 0 { append("    FAILED (encoder exit code \(rc))."); return (.failed, "failed (\(rc))", nil) }
        return (.done, "done", outClip)
    }
}

// MARK: - Splitters (resizable panels)

/// A draggable divider between panels. `value` is the panel size it controls, `sign` +1 when dragging right/down
/// grows it, -1 when it shrinks it.
struct Splitter: View {
    let vertical: Bool              // true: a vertical line between columns (drag left/right)
    @Binding var value: Double
    let range: ClosedRange<Double>
    var sign: Double = 1
    @State0 private var start: Double? = nil
    var body: some View {
        ZStack {
            Rectangle().fill(Th.line).frame(width: vertical ? 1 : nil, height: vertical ? nil : 1)
            Rectangle().fill(Color.clear).frame(width: vertical ? 7 : nil, height: vertical ? nil : 7).contentShape(Rectangle())
                .onHover { inside in if inside { (vertical ? NSCursor.resizeLeftRight : NSCursor.resizeUpDown).push() } else { NSCursor.pop() } }
                .gesture(DragGesture(minimumDistance: 1, coordinateSpace: .global).onChanged { g in
                    if start == nil { start = value }
                    let d = vertical ? g.translation.width : g.translation.height
                    value = min(range.upperBound, max(range.lowerBound, start! + Double(d) * sign))
                }.onEnded { _ in start = nil })
        }
        .frame(width: vertical ? 1 : nil, height: vertical ? nil : 1)
        .zIndex(10)
    }
}

/// Plain mutable storage for a view (stands in for @State, which needs the SwiftUI macro plugin in recent SDKs).
@propertyWrapper struct State0<T>: DynamicProperty {
    final class Box: ObservableObject { @Published var v: T; init(_ v: T) { self.v = v } }
    @StateObject private var box: Box
    init(wrappedValue: T) { _box = StateObject(wrappedValue: Box(wrappedValue)) }
    var wrappedValue: T { get { box.v } nonmutating set { box.v = newValue } }
}

// MARK: - Media Storage pane

final class FSNode: Identifiable {
    let url: URL; let name: String; let isDir: Bool; let depth: Int
    var icon: String; var isClip = false; var removable = false
    var id: String { url.path }
    var isPlayable: Bool { isClip || (!isDir && (url.pathExtension.lowercased() == "braw" || movieExtensions.contains(url.pathExtension.lowercased()))) }
    private var _children: [FSNode]?
    init(url: URL, name: String? = nil, isDir: Bool, depth: Int, icon: String = "folder") {
        self.url = url; self.name = name ?? url.lastPathComponent; self.isDir = isDir; self.depth = depth; self.icon = icon
    }
    var children: [FSNode] {
        if let c = _children { return c }
        var out: [FSNode] = []
        let items = (try? FileManager.default.contentsOfDirectory(at: url, includingPropertiesForKeys: [.isDirectoryKey, .isPackageKey], options: [.skipsHiddenFiles])) ?? []
        for u in items.sorted(by: { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }) {
            let rv = try? u.resourceValues(forKeys: [.isDirectoryKey, .isPackageKey])
            let dir = (rv?.isDirectory ?? false) && !(rv?.isPackage ?? false)
            let ext = u.pathExtension.lowercased()
            if dir {
                let n = FSNode(url: u, isDir: true, depth: depth + 1)
                // a clip folder: DNG frames inside (checks the first few entries only)
                if let names = try? FileManager.default.contentsOfDirectory(atPath: u.path), names.prefix(40).contains(where: { $0.lowercased().hasSuffix(".dng") }) { n.isClip = true; n.icon = "film" }
                out.append(n)
            } else if ext == "braw" || movieExtensions.contains(ext) {
                out.append(FSNode(url: u, isDir: false, depth: depth + 1, icon: "play.rectangle"))
            }
        }
        _children = out; return out
    }
    func reload() { _children = nil }
}

final class StorageModel: ObservableObject {
    @Published var roots: [FSNode] = []
    @Published var expanded: Set<String> = Set(UserDefaults.standard.stringArray(forKey: "storageExpanded") ?? []) { didSet { UserDefaults.standard.set(Array(expanded), forKey: "storageExpanded") } }
    @Published var selection: Set<String> = []          // selected rows (paths)
    private var anchor: String?
    init() {
        refresh()
        let nc = NSWorkspace.shared.notificationCenter
        for n in [NSWorkspace.didMountNotification, NSWorkspace.didUnmountNotification] { nc.addObserver(forName: n, object: nil, queue: .main) { [weak self] _ in self?.refresh() } }
    }
    func refresh() {
        var r: [FSNode] = []
        let keys: [URLResourceKey] = [.volumeNameKey, .volumeIsRemovableKey, .volumeIsInternalKey, .volumeIsEjectableKey]
        for v in FileManager.default.mountedVolumeURLs(includingResourceValuesForKeys: keys, options: [.skipHiddenVolumes]) ?? [] {
            let rv = try? v.resourceValues(forKeys: Set(keys))
            let removable = (rv?.volumeIsRemovable ?? false) || (rv?.volumeIsEjectable ?? false)
            let n = FSNode(url: v, name: rv?.volumeName ?? v.lastPathComponent, isDir: true, depth: 0, icon: removable ? "sdcard" : (rv?.volumeIsInternal ?? true) ? "internaldrive" : "externaldrive")
            n.removable = removable; r.append(n)
        }
        let home = FileManager.default.homeDirectoryForCurrentUser
        for (sub, icon) in [("Desktop", "menubar.dock.rectangle"), ("Movies", "film.stack"), ("Documents", "doc"), ("Downloads", "arrow.down.circle")] {
            let u = home.appendingPathComponent(sub)
            if FileManager.default.fileExists(atPath: u.path) { r.append(FSNode(url: u, isDir: true, depth: 0, icon: icon)) }
        }
        roots = r
    }
    /// The visible rows: roots and the children of expanded folders, in order.
    var rows: [FSNode] {
        var out: [FSNode] = []
        func walk(_ n: FSNode) { out.append(n); if n.isDir && expanded.contains(n.id) { for c in n.children { walk(c) } } }
        for r in roots { walk(r) }
        return out
    }
    func toggle(_ n: FSNode) { if expanded.contains(n.id) { expanded.remove(n.id) } else { expanded.insert(n.id) } }
    /// Click selection as in Finder: plain click selects one row, ⌘ adds or removes, ⇧ selects a run.
    func click(_ n: FSNode, command: Bool, shift: Bool) {
        let r = rows
        if shift, let a = anchor, let i0 = r.firstIndex(where: { $0.id == a }), let i1 = r.firstIndex(where: { $0.id == n.id }) {
            selection = Set(r[min(i0, i1)...max(i0, i1)].filter { $0.isPlayable }.map { $0.id })
        } else if command {
            if selection.contains(n.id) { selection.remove(n.id) } else { selection.insert(n.id) }
            anchor = n.id
        } else { selection = [n.id]; anchor = n.id }
    }
    var selectedNodes: [FSNode] { rows.filter { selection.contains($0.id) } }
    /// Show a path: expand its parents (under one of the roots) and select it.
    func reveal(_ path: String) {
        let p = URL(fileURLWithPath: path).standardizedFileURL.path
        guard let root = roots.filter({ p == $0.id || p.hasPrefix($0.id.hasSuffix("/") ? $0.id : $0.id + "/") }).max(by: { $0.id.count < $1.id.count }) else { return }
        var e = expanded; var u = URL(fileURLWithPath: p).deletingLastPathComponent()
        while u.path.count >= root.id.count { e.insert(u.path == root.url.path ? root.id : u.path); if u.path == "/" { break }; u = u.deletingLastPathComponent() }
        e.insert(root.id); expanded = e; selection = [p]; anchor = p
    }
}

struct StorageView: View {
    @ObservedObject var storage: StorageModel
    @ObservedObject var pool: MediaPool
    var addToQueue: ([FSNode]) -> Void
    var body: some View {
        VStack(spacing: 0) {
            PanelHeader(title: "Media Storage") {
                Image(systemName: "arrow.clockwise").font(.system(size: 10)).foregroundColor(Th.dim).onTapGesture { storage.refresh() }.help("Refresh volumes")
            }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 0) {
                        ForEach(storage.rows) { n in StorageRow(node: n, storage: storage, pool: pool, addToQueue: addToQueue).id(n.id) }
                    }.padding(.vertical, 4)
                }
                .onAppear { if let s = storage.selection.first { proxy.scrollTo(s, anchor: .center) } }
            }
            HStack(spacing: 8) {
                Text(footer).font(Th.f(10.5)).foregroundColor(Th.dim2).lineLimit(1).truncationMode(.middle)
                Spacer(minLength: 4)
                Button("Add to Queue") { addToQueue(storage.selectedNodes) }.buttonStyle(DarkButton(primary: false)).fixedSize().disabled(storage.selection.isEmpty)
                    .help("Add the selected clips (or every clip in the selected folders) to the Job Queue")
            }
            .padding(.horizontal, 8).padding(.vertical, 6).overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .top)
        }
        .background(Th.panel)
        .onChange(of: storage.selection) { _ in syncSelection() }
    }
    var footer: String {
        let n = storage.selectedNodes
        if n.isEmpty { return "Click a clip to view it; select clips or folders to queue" }
        if n.count == 1 { return n[0].name }
        return "\(n.count) selected"
    }
    /// The selection drives the viewer (the last clip clicked) and the Output panel's clip list.
    func syncSelection() {
        let nodes = storage.selectedNodes.filter { $0.isPlayable }
        pool.selectedClips = nodes.compactMap { n in MediaPool.kind(of: n.url, isDir: n.isDir).map { pool.clip(n.url, $0) } }
    }
}

struct StorageRow: View {
    let node: FSNode
    @ObservedObject var storage: StorageModel
    @ObservedObject var pool: MediaPool
    var addToQueue: ([FSNode]) -> Void
    var body: some View {
        let n = node, sel = storage.selection.contains(n.id)
        let clip: Clip? = n.isPlayable ? MediaPool.kind(of: n.url, isDir: n.isDir).map { pool.clip(n.url, $0) } : nil
        return HStack(spacing: 5) {
            if n.isDir && !n.isClip {
                Image(systemName: storage.expanded.contains(n.id) ? "chevron.down" : "chevron.right").font(.system(size: 8, weight: .bold)).foregroundColor(Th.dim2).frame(width: 10)
                    .contentShape(Rectangle()).onTapGesture { storage.toggle(n) }
            } else { Spacer().frame(width: 10) }
            Image(systemName: n.icon).font(.system(size: 10)).foregroundColor(n.isClip ? Th.acc : n.removable ? Th.acc2 : Th.dim).frame(width: 14)
            Text(n.name).font(Th.f(11)).foregroundColor(pool.current?.url.standardizedFileURL.path == n.url.standardizedFileURL.path ? Th.acc : Th.txt).lineLimit(1).truncationMode(.middle)
                .layoutPriority(1)
            if let c = clip { ClipSummary(clip: c) }
            Spacer(minLength: 0)
        }
        .padding(.leading, CGFloat(8 + n.depth * 14)).padding(.trailing, 8).padding(.vertical, 3)
        .background(sel ? Th.sel : Color.clear)
        .contentShape(Rectangle())
        .onTapGesture(count: 2) { KeyRouter.endEditing(); if n.isPlayable { addToQueue([n]) } else if n.isDir { storage.toggle(n) } }
        .onTapGesture {
            KeyRouter.endEditing()
            let f = NSEvent.modifierFlags
            storage.click(n, command: f.contains(.command), shift: f.contains(.shift))
            if let c = clip { pool.preview(c) }
        }
        .onDrag { NSItemProvider(object: n.url as NSURL) }
        .contextMenu {
            let targets = sel ? storage.selectedNodes : [n]
            Button(targets.count > 1 ? "Add \(targets.count) Items to Queue" : (n.isPlayable ? "Add to Queue" : "Add Every Clip Inside to Queue")) { addToQueue(targets) }
            if let c = clip { Button("View") { pool.preview(c) } }
            Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting(targets.map { $0.url }) }
            if n.isDir { Button("Refresh") { n.reload(); storage.objectWillChange.send() } }
        }
        .help(n.url.path)
    }
}

/// A clip row's short description in Media Storage: mode, rate, length and its badges (once probed).
struct ClipSummary: View {
    @ObservedObject var clip: Clip
    var body: some View {
        HStack(spacing: 4) {
            if let i = clip.info {
                if let e = i.error { Text(e).font(Th.f(9.5)).foregroundColor(Th.acc2).lineLimit(1) }
                else {
                    Text(clip.kind == .dng ? "\(i.modeLabel) · \(i.fpsLabel)" : "\(i.fpsLabel)\(i.mode.isEmpty ? "" : " · " + i.mode)")
                        .font(Th.f(9.5)).foregroundColor(Th.dim2).lineLimit(1)
                    if i.frames > 0 { Text(durationLabel(frames: i.frames, fps: i.fps)).font(Th.f(9.5)).foregroundColor(Th.dim2).lineLimit(1).fixedSize() }
                    if i.gyro { Badge(text: "GYRO", kind: 0) }
                }
            } else { Text("…").font(Th.f(9.5)).foregroundColor(Th.dim2) }
        }
    }
}

func chooseFolder(_ prompt: String) -> URL? {
    let p = NSOpenPanel(); p.canChooseDirectories = true; p.canChooseFiles = false; p.allowsMultipleSelection = false
    p.canCreateDirectories = true; p.prompt = prompt
    return p.runModal() == .OK ? p.url : nil
}

// MARK: - Viewer view

struct Scrubber: View {
    let frame: Int, count: Int
    var onScrub: (Int) -> Void
    var body: some View {
        GeometryReader { g in
            let w = max(1, g.size.width - 24), x = count > 1 ? 12 + w * CGFloat(frame) / CGFloat(count - 1) : 12
            ZStack(alignment: .topLeading) {
                Rectangle().fill(Color(hex: 0x18181b))
                Path { p in
                    let n = max(1, min(80, count / 6)); for i in 0...n { let xx = 12 + w * CGFloat(i) / CGFloat(n); p.move(to: CGPoint(x: xx, y: 3)); p.addLine(to: CGPoint(x: xx, y: 8)) }
                }.stroke(Color(hex: 0x444444), lineWidth: 1)
                Rectangle().fill(Color(hex: 0x333333)).frame(width: w, height: 3).offset(x: 12, y: 12)
                Rectangle().fill(Color(hex: 0x5a5a5a)).frame(width: max(0, x - 12), height: 3).offset(x: 12, y: 12)
                Rectangle().fill(Th.red).frame(width: 2, height: 18).offset(x: x - 1, y: 4)
            }
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { v in
                guard count > 1 else { return }
                let t = (v.location.x - 12) / w; onScrub(Int((max(0, min(1, t)) * CGFloat(count - 1)).rounded()))
            })
        }
        .frame(height: 26)
        .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .top)
    }
}

struct HeaderMenuLabel: View {
    let text: String
    var body: some View {
        Text(text).font(Th.f(11)).padding(.horizontal, 8).padding(.vertical, 3).background(Th.field).overlay(RoundedRectangle(cornerRadius: 3).stroke(Th.line2))
    }
}

struct ViewerView: View {
    @ObservedObject var player: Player
    @ObservedObject var luts: LUTLibrary
    @ObservedObject var pool: MediaPool
    @AppStorage("showHistogram") var showHistogram = false
    @AppStorage("viewerSqueeze") var squeezeSetting = 1.0
    @StateObject var zoom = ViewerZoom()
    @State0 var headerWidth = 900.0
    var squeeze: Double { _ = squeezeSetting; return Squeeze.get(pool.current) }
    var body: some View {
        VStack(spacing: 0) {
            header
            ZStack {
                Th.viewerBg
                if pool.current == nil {
                    VStack(spacing: 8) {
                        Image(systemName: "film").font(.system(size: 34)).foregroundColor(Th.dim2)
                        Text("Click a clip in Media Storage to view it, or drop clips on the window to queue them").font(Th.f(12)).foregroundColor(Th.dim)
                    }
                } else {
                    LayerImageView(player: player, squeeze: CGFloat(squeeze), sourceWidth: CGFloat(pool.current?.info?.picture_width ?? 0), zoomModel: zoom).padding(14)
                        .onAppear { TestHook.zoom = zoom }
                    if let e = player.error { Text(e).font(Th.f(12)).foregroundColor(Th.acc2).padding(8).background(Color.black.opacity(0.6)).cornerRadius(3) }
                    else if player.image == nil || player.buffering { ProgressView().scaleEffect(0.7) }
                }
                VStack {
                    Spacer()
                    HStack(alignment: .bottom) {
                        VStack(alignment: .leading, spacing: 4) {
                            if player.look != .rec709 { tag("Decode: \(player.look.label)") }
                            if luts.lut != nil { tag("LUT: \(luts.label)") }
                            if squeeze > 1 { tag("De-squeeze \(Squeeze.label(squeeze))") }
                            if player.playing && player.droppedRecently && !player.buffering && player.error == nil { tag("dropping frames — try Quarter resolution", warn: true) }
                        }
                        Spacer()
                    }.padding(20)
                }
            }
            Scrubber(frame: player.frame, count: player.frameCount) { player.scrub(to: $0) }
            transport
        }
        .background(Th.viewerBg)
        .onAppear { player.wantHistogram = showHistogram; if showHistogram { HistogramPalette.shared.show(player) } }
        .onChange(of: showHistogram) { v in
            player.wantHistogram = v
            if v { HistogramPalette.shared.show(player); player.show(player.pos) } else { HistogramPalette.shared.hide() }
        }
    }
    func tag(_ s: String, warn: Bool = false) -> some View {
        Text(s).font(Th.f(10)).foregroundColor(warn ? Th.acc2 : Th.txt).padding(.horizontal, 6).padding(.vertical, 2).background(Color.black.opacity(0.55)).cornerRadius(2)
    }
    var header: some View {
        HStack(spacing: 8) {
            if let c = pool.current {
                Text(c.name).font(Th.f(11, .semibold)).lineLimit(1).truncationMode(.middle).layoutPriority(1)
                if !compact, let i = c.info, i.error == nil {
                    Text(c.kind == .dng ? "\(String(i.picture_width))×\(String(i.picture_height)) · \(i.fpsLabel) · ISO \(String(i.iso)) · \(String(i.kelvin)) K"
                                        : "\(c.kindLabel) · \(String(i.width))×\(String(i.height)) · \(i.fpsLabel)").font(Th.f(11)).foregroundColor(Th.dim).lineLimit(1).frame(minWidth: 0)
                }
            } else { Text("Viewer").font(Th.f(11, .semibold)) }
            Menu {
                Section(header: Text("Decode raw frames as (viewer only)")) {
                    ForEach(ViewerLook.allCases, id: \.self) { l in Button(action: { player.look = l }) { Text((player.look == l ? "✓ " : "") + l.label) } }
                }
                Divider()
                Text("Film: flat, for Blackmagic Film Gen 5 / Wide Gamut LUTs. Linear: for LUTs that expect linear input")
                Text("(middle grey at 0.18; a LUT only sees what its input range covers).")
                Text(".mov files play as macOS renders them.")
            } label: { HeaderMenuLabel(text: player.look.short) }
            .menuStyle(BorderlessButtonMenuStyle()).fixedSize().help("How the viewer decodes raw frames: Rec.709, Blackmagic Film Gen 5 (log) or linear")
            Menu {
                Section(header: Text("Monitoring LUT (viewer only)")) {
                    Button(action: { luts.selected = "" }) { Text(luts.selected.isEmpty ? "✓ None" : "None") }
                    Button(action: { luts.selected = LUTLibrary.falseColorID }) { Text(luts.selected == LUTLibrary.falseColorID ? "✓ False Color (exposure)" : "False Color (exposure)") }
                    ForEach(luts.files, id: \.self) { u in
                        Button(action: { luts.selected = u.path }) { Text((luts.selected == u.path ? "✓ " : "") + u.deletingPathExtension().lastPathComponent) }
                    }
                }
                Divider()
                Button("Add LUT (.cube)…") { luts.add() }
                Button("Open LUT Folder") { NSWorkspace.shared.open(luts.folder); luts.refresh() }
            } label: { HeaderMenuLabel(text: compact ? "LUT" : "LUT: \(luts.label)") }
            .menuStyle(BorderlessButtonMenuStyle()).fixedSize().padding(.leading, 4)
            Menu {
                Section(header: Text("Anamorphic de-squeeze (every clip, viewer and output)")) {
                    ForEach(Squeeze.factors, id: \.self) { f in
                        Button(action: { squeezeSetting = f }) { Text((abs(squeeze - f) < 0.001 ? "✓ " : "") + Squeeze.label(f)) }
                    }
                }
            } label: { HeaderMenuLabel(text: squeeze > 1 ? (compact ? Squeeze.label(squeeze) : "Squeeze \(Squeeze.label(squeeze))") : (compact ? "Sq" : "Squeeze")) }
            .menuStyle(BorderlessButtonMenuStyle()).fixedSize()
            Menu {
                Section(header: Text("Playback resolution")) {
                    ForEach(PlaybackRes.allCases, id: \.self) { r in Button(action: { player.res = r }) { Text((player.res == r ? "✓ " : "") + r.label) } }
                }
            } label: { HeaderMenuLabel(text: compact ? ["", "Full", "Half", "", "Qtr"][player.res.rawValue] : player.res.label) }
            .menuStyle(BorderlessButtonMenuStyle()).fixedSize().help("Decode at full, half or quarter resolution (quarter plays most smoothly)")
            Menu {
                Section(header: Text("Zoom (scroll or pinch on the picture; drag to pan; Z to fit)")) {
                    Button("Fit") { zoom.fit() }
                    ForEach([25.0, 50.0, 100.0, 200.0, 400.0], id: \.self) { p in Button(String(format: "%.0f%%", p)) { zoom.setPercent(p) } }
                }
            } label: { HeaderMenuLabel(text: zoom.label) }
            .menuStyle(BorderlessButtonMenuStyle()).fixedSize().help("Zoom the viewer")
            Image(systemName: "chart.bar.xaxis").font(.system(size: 11)).foregroundColor(showHistogram ? Th.acc : Th.dim)
                .onTapGesture { showHistogram.toggle() }.help("Show or hide the histogram")
            Image(systemName: player.muted ? "speaker.slash" : "speaker.wave.2").font(.system(size: 11)).foregroundColor(player.muted ? Th.dim2 : Th.dim)
                .onTapGesture { player.muted.toggle() }.help(player.muted ? "Unmute the clip's audio" : "Mute the clip's audio")
            Spacer(minLength: 6)
            Text(tc).font(.system(size: 13, design: .monospaced)).foregroundColor(Th.acc2).fixedSize().layoutPriority(2)
        }
        .padding(.horizontal, 10).frame(height: 30).frame(minWidth: 0, maxWidth: .infinity).background(Th.head)
        .background(GeometryReader { g in Color.clear.onAppear { headerWidth = Double(g.size.width) }.onChange(of: g.size.width) { w in headerWidth = Double(w) } })
        .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .bottom)
        .clipped()
    }
    var compact: Bool { headerWidth < 720 }
    var tc: String {
        guard let i = pool.current?.info, i.error == nil else { return "--:--:--:--" }
        return timecode(i.timecode.isEmpty ? "00:00:00:00" : i.timecode, plus: player.frame, fps: i.fps)
    }
    var transport: some View {
        HStack(spacing: 22) {
            Spacer()
            tbtn("backward.end.fill", "Go to first frame (↑)") { player.toStart() }
            tbtn("backward.frame.fill", "Previous frame (←)") { player.step(-1) }
            tbtn(player.playing ? "pause.fill" : "play.fill", "Play / pause (space)", size: 17) { player.toggle() }
            tbtn("forward.frame.fill", "Next frame (→)") { player.step(1) }
            tbtn("forward.end.fill", "Go to last frame (↓)") { player.toEnd() }
            Spacer()
        }
        .overlay(HStack {
            Spacer()
            Text(player.frameCount > 0 ? "Frame \(player.frame + 1) / \(player.frameCount)" : "").font(Th.f(10.5)).foregroundColor(Th.dim).padding(.trailing, 12)
        })
        .frame(height: 38).background(Th.panel)
        .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .top)
    }
    func tbtn(_ icon: String, _ help: String, size: CGFloat = 13, _ action: @escaping () -> Void) -> some View {
        Button(action: { KeyRouter.endEditing(); action() }) { Image(systemName: icon).font(.system(size: size)).foregroundColor(Th.txt).frame(width: 26, height: 26).contentShape(Rectangle()) }
            .buttonStyle(.plain).help(help)
    }
}

// MARK: - Metadata panel

struct MetadataPanel: View {
    @ObservedObject var pool: MediaPool
    var body: some View {
        if let c = pool.current { MetadataContent(clip: c) }
        else { Text("Select a clip to see its details").font(Th.f(11)).foregroundColor(Th.dim2).frame(maxWidth: .infinity, maxHeight: .infinity) }
    }
}
struct MetadataContent: View {
    @ObservedObject var clip: Clip
    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 0) {
                if clip.details.isEmpty { ProgressView().scaleEffect(0.6).frame(maxWidth: .infinity).padding(20) }
                ForEach(Array(clip.details.enumerated()), id: \.offset) { _, g in
                    Text(g.0.uppercased()).font(.system(size: 9.5, weight: .semibold)).foregroundColor(Th.dim2).padding(.horizontal, 12).padding(.top, 12).padding(.bottom, 4)
                    ForEach(Array(g.1.enumerated()), id: \.offset) { _, kv in
                        HStack(alignment: .top, spacing: 8) {
                            Text(kv.0).font(Th.f(11)).foregroundColor(Th.dim).frame(width: 132, alignment: .leading).lineLimit(2).truncationMode(.middle).help(kv.0)
                            Text(kv.1.isEmpty ? "—" : kv.1).font(Th.f(11)).foregroundColor(Th.txt).frame(maxWidth: .infinity, alignment: .leading).fixedSize(horizontal: false, vertical: true)
                        }
                        .padding(.horizontal, 12).padding(.vertical, 2)
                    }
                }
            }
            .textSelection(.enabled)
            .padding(.bottom, 12)
        }
        .onAppear { if clip.info != nil { loadDetails(clip) } }
        .onReceive(clip.$info) { i in if i != nil { loadDetails(clip) } }
    }
}

// MARK: - Queue views

struct JobRow: View {
    @ObservedObject var job: RenderJob
    var selected = false
    var remove: () -> Void
    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            Group {
                switch job.state {
                case .rendering: Circle().fill(Th.acc).frame(width: 7, height: 7)
                case .done: Image(systemName: "checkmark").font(.system(size: 9, weight: .bold)).foregroundColor(Th.ok)
                case .failed: Image(systemName: "xmark").font(.system(size: 9, weight: .bold)).foregroundColor(Th.red)
                case .skipped: Image(systemName: "arrow.uturn.right").font(.system(size: 9)).foregroundColor(Th.dim)
                case .stopped: Image(systemName: "stop.fill").font(.system(size: 8)).foregroundColor(Th.dim)
                case .queued: Circle().stroke(Th.dim, lineWidth: 1).frame(width: 7, height: 7)
                }
            }.frame(width: 12, height: 14)
            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text(job.settings.format == .cdng ? "\(job.name) → \(job.name)_\(job.settings.dng.suffix)" : "\(job.name) → .\(job.settings.format.movieExtension)")
                        .font(Th.f(11)).lineLimit(1).truncationMode(.middle)
                    Spacer(minLength: 4)
                    if job.state == .rendering { Text(job.rate).font(Th.f(10.5)).foregroundColor(Th.dim).lineLimit(1).fixedSize() }
                }
                HStack(spacing: 6) {
                    Text(job.settings.summary).font(Th.f(10)).foregroundColor(Th.dim2).lineLimit(1).truncationMode(.tail)
                    Spacer(minLength: 4)
                    Text(job.detail).font(Th.f(10)).foregroundColor(Th.dim).lineLimit(1).truncationMode(.tail)
                }
                if job.state == .rendering || job.progress > 0 {
                    GeometryReader { g in
                        ZStack(alignment: .leading) {
                            RoundedRectangle(cornerRadius: 2).fill(Color(hex: 0x333333))
                            RoundedRectangle(cornerRadius: 2).fill(job.state == .done ? Th.ok : job.state == .failed ? Th.red : Th.acc).frame(width: g.size.width * CGFloat(job.progress))
                        }
                    }.frame(height: 3)
                }
            }
            if job.state != .rendering {
                Image(systemName: "xmark.circle").font(.system(size: 10)).foregroundColor(Th.dim2).frame(height: 14).onTapGesture(perform: remove).help("Remove from the queue")
            } else { Spacer().frame(width: 10) }
        }
        .padding(.horizontal, 10).padding(.vertical, 6)
        .background(selected ? Th.sel : Color.clear)
        .overlay(Rectangle().frame(height: 1).foregroundColor(Color(hex: 0x1d1d20)), alignment: .bottom)
    }
}

/// The Job Queue: click, ⌘-click and ⇧-click select jobs; drag them to reorder (the next queued job is the one
/// highest in the list); Delete or the context menu removes the selected ones.
final class QueueSelection: ObservableObject {
    @Published var ids: Set<UUID> = []
    @Published var dropTarget: UUID?                // where dragged jobs would go (shown as a line)
    var anchor: UUID?
}

struct QueueView: View {
    @ObservedObject var engine: Engine
    @StateObject var sel = QueueSelection()
    var body: some View {
        VStack(spacing: 0) {
            PanelHeader(title: "Job Queue") {
                HStack(spacing: 12) {
                    Text(summary).font(Th.f(11)).foregroundColor(Th.dim).lineLimit(1)
                    if engine.jobs.contains(where: { $0.state != .queued && $0.state != .rendering }) {
                        Text("Clear finished").font(Th.f(11)).foregroundColor(Th.acc).lineLimit(1).fixedSize().onTapGesture { engine.clearFinished() }
                    }
                }
            }
            if engine.jobs.isEmpty {
                Text("Select clips in Media Storage and press Add to Queue (or Render in the Output panel)").font(Th.f(11)).foregroundColor(Th.dim2)
                    .multilineTextAlignment(.center).padding(12).frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView {
                    LazyVStack(spacing: 0) {
                        ForEach(engine.jobs) { j in row(j) }
                        endZone
                    }
                }
                .background(Th.panel.onTapGesture { sel.ids = [] })
            }
        }
        .background(Th.panel)
        .onAppear { TestHook.queueSel = sel }
    }
    var endZone: some View {
        let line = Rectangle().fill(Th.acc).frame(height: 2).opacity(sel.dropTarget == JobDrop.endID ? 1 : 0)
        return Color.clear.frame(height: 30).overlay(line, alignment: .top)
            .onDrop(of: [UTType.plainText], delegate: JobDrop(target: nil, engine: engine, sel: sel))
    }
    func row(_ j: RenderJob) -> some View {
        let line = Rectangle().fill(Th.acc).frame(height: 2).opacity(sel.dropTarget == j.id ? 1 : 0)
        let r = JobRow(job: j, selected: sel.ids.contains(j.id)) { engine.remove(j); sel.ids.remove(j.id) }
        return r.overlay(line, alignment: .top)
            .contentShape(Rectangle())
            .onTapGesture { KeyRouter.endEditing(); click(j) }
            .onDrag { dragItem(j) }
            .onDrop(of: [UTType.plainText], delegate: JobDrop(target: j.id, engine: engine, sel: sel))
            .contextMenu { menu(j) }
    }
    func dragItem(_ j: RenderJob) -> NSItemProvider {
        if !sel.ids.contains(j.id) { sel.ids = [j.id]; sel.anchor = j.id }
        return NSItemProvider(object: ("cdng-jobs:" + j.id.uuidString) as NSString)
    }
    func click(_ j: RenderJob) {
        let f = NSEvent.modifierFlags
        if f.contains(.shift), let a = sel.anchor, let i0 = engine.jobs.firstIndex(where: { $0.id == a }), let i1 = engine.jobs.firstIndex(where: { $0.id == j.id }) {
            sel.ids = Set(engine.jobs[min(i0, i1)...max(i0, i1)].map { $0.id })
        } else if f.contains(.command) {
            if sel.ids.contains(j.id) { sel.ids.remove(j.id) } else { sel.ids.insert(j.id) }; sel.anchor = j.id
        } else { sel.ids = [j.id]; sel.anchor = j.id }
    }
    @ViewBuilder func menu(_ j: RenderJob) -> some View {
        let ids = sel.ids.contains(j.id) ? sel.ids : [j.id]
        let n = ids.count
        Button(n > 1 ? "Remove \(n) Jobs" : "Remove Job") { engine.remove(ids); sel.ids.subtract(ids) }
        Button("Move Up") { engine.move(ids, by: -1) }
        Button("Move Down") { engine.move(ids, by: 1) }
        Button("Move to Top") { engine.move(ids, before: engine.jobs.first?.id) }
        Button("Move to Bottom") { engine.move(ids, before: nil) }
        if let o = j.output { Divider(); Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([o]) } }
    }
    var summary: String {
        let q = engine.queuedCount, r = engine.current != nil
        return "\(engine.jobs.count) job\(engine.jobs.count == 1 ? "" : "s")" + (r ? " · rendering" : "") + (q > 0 ? " · \(q) queued" : "") + (sel.ids.count > 1 ? " · \(sel.ids.count) selected" : "")
    }
}

/// Dropping dragged jobs above another job (or below the last one).
struct JobDrop: DropDelegate {
    static let endID = UUID()
    let target: UUID?
    let engine: Engine
    let sel: QueueSelection
    func validateDrop(info: DropInfo) -> Bool { info.hasItemsConforming(to: [UTType.plainText]) }
    func dropEntered(info: DropInfo) { sel.dropTarget = target ?? JobDrop.endID }
    func dropExited(info: DropInfo) { if sel.dropTarget == (target ?? JobDrop.endID) { sel.dropTarget = nil } }
    func dropUpdated(info: DropInfo) -> DropProposal? { DropProposal(operation: .move) }
    func performDrop(info: DropInfo) -> Bool {
        sel.dropTarget = nil
        guard let p = info.itemProviders(for: [UTType.plainText]).first else { return false }
        let ids = sel.ids, target = target, engine = engine
        _ = p.loadObject(ofClass: NSString.self) { obj, _ in
            guard let s = obj as? String, s.hasPrefix("cdng-jobs:") else { return }
            DispatchQueue.main.async { engine.move(ids, before: target) }
        }
        return true
    }
}

// MARK: - Output sidebar

struct FormatRow: View {
    let f: OutputFormat; let on: Bool; let tap: () -> Void
    var body: some View {
        HStack(spacing: 10) {
            Circle().stroke(on ? Th.acc : Color(hex: 0x5a5a60), lineWidth: on ? 4 : 1).frame(width: 12, height: 12)
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 6) {
                    Text(f.title).font(Th.f(12, .semibold)).foregroundColor(on ? .white : Th.txt)
                    if f == .braw {            // still being validated (gyro in Gyroflow, Resolve's stabilisation)
                        Text("BETA").font(Th.f(8.5, .bold)).foregroundColor(Th.acc)
                            .padding(.horizontal, 4).padding(.vertical, 1)
                            .overlay(RoundedRectangle(cornerRadius: 2).stroke(Th.acc, lineWidth: 1))
                            .help("Blackmagic RAW output is in beta: decoding and colour are checked, gyro stabilisation is still being validated.")
                    }
                }
                Text(f.sub).font(Th.f(10.5)).foregroundColor(on ? Color(hex: 0xe9c49f) : Th.dim)
            }
            Spacer()
            Text("\(f.order)").font(Th.f(10, .semibold)).foregroundColor(on ? Th.acc : Th.dim2)
        }
        .padding(.horizontal, 10).padding(.vertical, 7)
        .background(RoundedRectangle(cornerRadius: 4).fill(on ? Color(hex: 0x2e2419) : Color(hex: 0x1c1c1f)))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(on ? Th.acc : Th.line2))
        .contentShape(Rectangle()).onTapGesture(perform: tap)
    }
}

struct OutputSidebar: View {
    @ObservedObject var o: OutputModel
    @ObservedObject var pool: MediaPool
    @ObservedObject var engine: Engine
    @ObservedObject var storage: StorageModel
    @AppStorage("secCompression") var secCompression = true
    @AppStorage("secWB") var secWB = true
    @AppStorage("secData") var secData = true
    @AppStorage("secDest") var secDest = true
    @AppStorage("secProc") var secProc = true
    var addSelected: () -> Void
    var render: () -> Void

    var selected: [Clip] { pool.selectedClips }
    var targets: [Clip] { selected.filter { $0.convertible } }
    var body: some View {
        VStack(spacing: 0) {
            VStack(spacing: 6) { ForEach(OutputFormat.ordered) { f in FormatRow(f: f, on: o.format == f) { o.format = f } } }
                .padding(.horizontal, 12).padding(.vertical, 10)
                .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .bottom)
            ScrollView {
                VStack(spacing: 0) {
                    compression
                    if o.format != .cdng { whiteBalance; embedded }
                    destination
                    Section2(title: "Processing", value: o.gpu ? "GPU" : "CPU", open: $secProc) { CheckRow(label: "Use the GPU (Metal)", on: $o.gpu, hint: o.format == .braw ? "Runs the colour transform and DCT on the GPU, several times faster." : "Runs the DCT, quantisation and rate-control statistics on the GPU; entropy coding stays on every CPU core. 2-4x faster at the same quality.") }
                    warnings
                }
            }
            VStack(spacing: 8) {
                HStack(spacing: 8) {
                    // the same as Media Storage's Add to Queue: the selected clips, or every clip in the selected folders
                    let nsel = storage.selection.count
                    Button(nsel > 1 ? "Add \(nsel) to Queue" : "Add to Queue", action: addSelected).buttonStyle(DarkButton()).disabled(nsel == 0 || o.neutral == "invalid")
                    if engine.running { Button("Stop") { engine.stop() }.buttonStyle(DarkButton()) }
                    else { Button(engine.queuedCount > 0 ? "Render All" : (nsel == 0 ? "Render" : nsel == 1 && !targets.isEmpty ? "Render Clip" : "Render Selected"), action: render).buttonStyle(DarkButton(primary: true))
                        .disabled((engine.queuedCount == 0 && nsel == 0) || o.neutral == "invalid") }
                }
                CheckRow(label: "Show the files in Finder when done", on: $o.openWhenDone)
            }
            .padding(12).overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .top)
        }
        .background(Th.panel)
    }

    var sizeNote: String {
        guard o.format == .braw, !o.brawByQuality, !targets.isEmpty else { return o.format == .braw ? "size varies with the picture" : "" }
        let ratio = Double(o.brawRatio)
        func frameBytes(_ i: ProbeInfo) -> Double {
            let rows: Int = (i.height + 15) / 16 * 16
            let px: Double = Double(i.width) * Double(rows)
            return px * 1.5 / ratio
        }
        var bytes = 0.0, per = 0.0
        for c in targets { if let i = c.info { let fb = frameBytes(i); bytes += fb * Double(i.frames); if per == 0 { per = fb } } }
        let total: String = bytes > 1e9 ? String(format: "%.1f GB", bytes / 1e9) : String(format: "%.0f MB", bytes / 1e6)
        let perMB: Double = per / 1e6
        return String(format: "≈ %.1f MB per frame · ≈ ", perMB) + total + " in all"
    }
    @ViewBuilder var compression: some View {
        switch o.format {
        case .braw:
            Section2(title: "Compression", value: o.brawLabel, open: $secCompression) {
                KV(k: "Mode") { Seg(items: [(false, "Constant bitrate"), (true, "Constant quality")], value: $o.brawByQuality) }
                if o.brawByQuality { KV(k: "Quality") { Seg(items: [(0, "Q0"), (1, "Q1"), (3, "Q3"), (5, "Q5")], value: $o.brawQ) } }
                else { KV(k: "Ratio") { Seg(items: [(3, "3:1"), (5, "5:1"), (8, "8:1"), (12, "12:1")], value: $o.brawRatio) } }
                Text(o.brawByQuality ? (o.brawQ == 0 ? "Q0: the Video Assist's own quantiser; size varies with the picture" : "constant quality; size varies with the picture") : sizeNote)
                    .font(Th.f(10.5)).foregroundColor(Th.dim2).fixedSize(horizontal: false, vertical: true)
                KV(k: "Opens as") { Seg(items: [(true, "Film Gen 5"), (false, "Rec.709")], value: $o.brawFilm) }
                Text(o.brawFilm ? "Resolve opens it flat (Blackmagic Film Gen 5, Blackmagic Wide Gamut), as Blackmagic cameras shoot it. Every highlight is there to grade." : "Resolve opens it in Rec.709, as the Video Assist's files. The highlights are still in the file: set Gamma to Blackmagic Design Film to see them.")
                    .font(Th.f(10.5)).foregroundColor(Th.dim2).fixedSize(horizontal: false, vertical: true)
            }
        case .proresRaw:
            Section2(title: "Quality", value: Quality(rawValue: o.quality)?.label ?? "", open: $secCompression) {
                Seg(items: Quality.allCases.map { ($0.rawValue, $0.label) }, value: $o.quality).padding(.vertical, 3)
                Text(Quality(rawValue: o.quality)?.note ?? "").font(Th.f(10.5)).foregroundColor(Th.dim2)
            }
        case .cdng:
            Section2(title: "Compression", value: o.dng.label, open: $secCompression) {
                HStack(alignment: .top, spacing: 14) {
                    VStack(alignment: .leading, spacing: 0) { ForEach(DngMode.all.prefix(4)) { m in radio(m) } }
                    VStack(alignment: .leading, spacing: 0) { ForEach(DngMode.all.suffix(4)) { m in radio(m) } }
                }
                Text(o.dng.note).font(Th.f(10.5)).foregroundColor(Th.dim2).fixedSize(horizontal: false, vertical: true).padding(.top, 3)
                CheckRow(label: "Copy non-DNG files (WAV, XMP…)", on: $o.copyOther).padding(.top, 4)
                CheckRow(label: "Run a verification pass", on: $o.verify, hint: "Decodes every tile again after compressing. Lossless modes must match exactly.")
            }
        }
    }
    func radio(_ m: DngMode) -> some View {
        HStack(spacing: 6) {
            Circle().stroke(o.dngMode == m.arg ? Th.acc : Color(hex: 0x5a5a60), lineWidth: o.dngMode == m.arg ? 4 : 1).frame(width: 11, height: 11)
            Text(m.label).font(Th.f(11)).foregroundColor(Th.txt)
        }.padding(.vertical, 3).contentShape(Rectangle()).onTapGesture { o.dngMode = m.arg }
    }
    var whiteBalance: some View {
        Section2(title: "White Balance", value: o.greyCard ? "Grey card" : "As shot", open: $secWB) {
            KV(k: "Source") { Seg(items: [(false, "As shot"), (true, "Grey card")], value: $o.greyCard) }
            if o.greyCard {
                KV(k: "R/G · B/G") {
                    HStack(spacing: 6) {
                        TextField("0.737", text: $o.wbR).textFieldStyle(.plain).font(Th.mono).frame(width: 52).padding(3).background(Th.field).overlay(RoundedRectangle(cornerRadius: 3).stroke(Th.line2))
                        TextField("0.367", text: $o.wbB).textFieldStyle(.plain).font(Th.mono).frame(width: 52).padding(3).background(Th.field).overlay(RoundedRectangle(cornerRadius: 3).stroke(Th.line2))
                    }
                }
                if o.neutral == "invalid" { Text("Enter two numbers, e.g. 0.737 and 0.367").font(Th.f(10.5)).foregroundColor(Th.red) }
            } else if targets.count == 1, let i = targets[0].info, o.format == .braw {
                KV(k: "Writes") { Text("\(String(i.kelvin)) K · tint \(i.tint >= 0 ? "+" : "")\(String(i.tint))").font(Th.f(11.5)) }
            } else {
                Text("from each clip's DNG AsShotNeutral").font(Th.f(10.5)).foregroundColor(Th.dim2)
            }
        }
    }
    @AppStorage("viewerSqueeze") var squeezeSetting = 1.0
    var squeezeNote: String {
        _ = squeezeSetting
        let s = targets.map { Squeeze.get($0) }.filter { $0 > 1 }
        if s.isEmpty { return "none (set it in the viewer's Squeeze menu)" }
        return Set(s).count == 1 ? "\(Squeeze.label(s[0])) from the viewer" : "each clip's factor from the viewer"
    }
    var embedded: some View {
        Section2(title: "Embedded Data", open: $secData) {
            CheckRow(label: "Audio (the clip's WAV\(o.format == .braw ? ", 24-bit" : ""))", on: $o.audio)
            CheckRow(label: o.format == .braw ? "Gyro (Blackmagic motion + Gyroflow)" : "Gyro (Gyroflow track)", on: $o.gyro,
                     hint: "fpSup Gyro2 data in the DNGs, for Gyroflow and its OpenFX plugin.")
            CheckRow(label: "Anamorphic de-squeeze", on: $o.squeeze, hint: "Writes the clip's de-squeeze factor (pixel aspect) so Resolve de-squeezes it.")
            Text(squeezeNote).font(Th.f(10)).foregroundColor(Th.dim2).padding(.leading, 20)
            HStack(spacing: 8) { Image(systemName: "checkmark").font(.system(size: 8, weight: .bold)).foregroundColor(Th.dim).frame(width: 12); Text("Timecode and clip metadata").font(Th.f(11.5)).foregroundColor(Th.dim) }.padding(.vertical, 3)
            CheckRow(label: "Copy Gyroflow sidecar files", on: $o.sidecars)
        }
    }
    var destination: some View {
        Section2(title: "Destination", value: o.nextToClip ? "Next to clip" : "Folder", open: $secDest) {
            KV(k: "Save to") { Seg(items: [(true, "Next to clip"), (false, "Folder…")], value: Binding(get: { o.nextToClip }, set: { v in
                if !v && o.destination.isEmpty { if let u = chooseFolder("Choose") { o.destination = u.path; o.nextToClip = false } } else { o.nextToClip = v } })) }
            if !o.nextToClip {
                HStack(spacing: 6) {
                    Text(o.destination.isEmpty ? "No folder chosen" : o.destination).font(Th.f(10.5)).foregroundColor(Th.dim).lineLimit(1).truncationMode(.middle).help(o.destination)
                    Spacer()
                    Text("Change…").font(Th.f(10.5)).foregroundColor(Th.acc).onTapGesture { if let u = chooseFolder("Choose") { o.destination = u.path } }
                }.padding(.vertical, 2)
            }
            KV(k: "If it exists") { Seg(items: ExistingPolicy.allCases.map { ($0.rawValue, $0.label) }, value: $o.policy) }
            if o.format == .cdng && o.nextToClip { Text("writes <clip>_\(o.dng.suffix) next to each clip folder").font(Th.f(10.5)).foregroundColor(Th.dim2) }
        }
    }
    @ViewBuilder var warnings: some View {
        let unsupported = o.format == .braw ? targets.filter { !($0.info?.braw_supported ?? true) } : []
        let files = selected.filter { !$0.convertible }
        if !unsupported.isEmpty {
            note("\(unsupported.count == 1 ? unsupported[0].name + " is" : "\(unsupported.count) clips are") in a mode the BRAW encoder doesn't support yet (fp UHD and OG3K work); \(unsupported.count == 1 ? "it" : "they") will be skipped.")
        }
        if !files.isEmpty {
            note("\(files.count == 1 ? files[0].name + " is a" : "\(files.count) selected clips are") .braw/.mov file\(files.count == 1 ? "" : "s"): playback only, not converted.")
        }
    }
    func note(_ s: String) -> some View {
        HStack(alignment: .top, spacing: 6) {
            Image(systemName: "exclamationmark.triangle.fill").font(.system(size: 10)).foregroundColor(Th.acc2)
            Text(s).font(Th.f(10.5)).foregroundColor(Th.acc2).fixedSize(horizontal: false, vertical: true)
        }.padding(12)
    }
}

/// The right-hand panel: Output settings or the selected clip's Metadata.
struct RightPanel: View {
    @ObservedObject var output: OutputModel
    @ObservedObject var pool: MediaPool
    @ObservedObject var engine: Engine
    @ObservedObject var storage: StorageModel
    @Binding var tab: Int
    var addSelected: () -> Void
    var render: () -> Void
    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 0) {
                tabButton("Output", 0); Rectangle().fill(Th.line).frame(width: 1); tabButton("Metadata", 1)
            }
            .frame(height: 30).background(Th.head)
            .overlay(Rectangle().frame(height: 1).foregroundColor(Th.line), alignment: .bottom)
            if tab == 0 { OutputSidebar(o: output, pool: pool, engine: engine, storage: storage, addSelected: addSelected, render: render) }
            else { MetadataPanel(pool: pool).background(Th.panel) }
        }
        .background(Th.panel)
    }
    func tabButton(_ t: String, _ i: Int) -> some View {
        Text(t).font(Th.f(11, tab == i ? .semibold : .regular)).foregroundColor(tab == i ? .white : Th.dim)
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(tab == i ? Th.panel : Color.clear)
            .overlay(Rectangle().frame(height: 2).foregroundColor(tab == i ? Th.acc : Color.clear), alignment: .top)
            .contentShape(Rectangle()).onTapGesture { tab = i }
    }
}

// MARK: - Log

struct LogView: View {
    @ObservedObject var engine: Engine
    var body: some View {
        VStack(spacing: 0) {
            PanelHeader(title: "Log") { Text("Copy").font(Th.f(11)).foregroundColor(Th.acc).onTapGesture {
                NSPasteboard.general.clearContents(); NSPasteboard.general.setString(engine.log.joined(separator: "\n"), forType: .string) } }
            ScrollViewReader { proxy in
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 1) {
                        ForEach(Array(engine.log.enumerated()), id: \.offset) { i, l in Text(l.isEmpty ? " " : l).font(Th.mono).foregroundColor(Th.txt).id(i) }
                    }.textSelection(.enabled).padding(10).frame(maxWidth: .infinity, alignment: .leading)
                }
                .background(Color(hex: 0x161618))
                .onChange(of: engine.log.count) { n in if n > 0 { proxy.scrollTo(n - 1, anchor: .bottom) } }
            }
        }
    }
}

// MARK: - Main window

/// One page, four columns' worth of panels, all resizable:
///   left: Media Storage over the Job Queue; centre: the Viewer over the Log; right: Output / Metadata.
/// The stored sizes are what the user dragged to; what's shown is fitted to the window, so nothing is cut off.
struct ContentView: View {
    @StateObject var pool = MediaPool()
    @StateObject var player = Player()
    @StateObject var luts = LUTLibrary()
    @StateObject var output = OutputModel()
    @StateObject var engine = Engine()
    @StateObject var storage = StorageModel()
    @AppStorage("rightTab") var rightTab = 0
    @AppStorage("leftWidth") var leftWidth = 400.0
    @AppStorage("rightWidth") var rightWidth = 330.0
    @AppStorage("logHeight") var logHeight = 150.0
    @AppStorage("storageHeight2") var storageHeight = 430.0

    static let minLeft = 260.0, minRight = 300.0, minCenter = 420.0, minPane = 90.0, minViewer = 260.0
    var body: some View {
        GeometryReader { g in
            let W = Double(g.size.width), H = Double(g.size.height)
            let (lw, rw) = fitSides(W)
            let sh = min(max(Self.minPane, storageHeight), max(Self.minPane, H - Self.minPane - 1))
            let lh = min(max(Self.minPane, logHeight), max(Self.minPane, H - Self.minViewer - 1))
            HStack(spacing: 0) {
                VStack(spacing: 0) {
                    StorageView(storage: storage, pool: pool, addToQueue: addNodes).frame(height: CGFloat(sh))
                    Splitter(vertical: false, value: Binding(get: { sh }, set: { storageHeight = $0 }), range: Self.minPane...max(Self.minPane, H - Self.minPane - 1))
                    QueueView(engine: engine).frame(maxHeight: .infinity)
                }
                .frame(width: CGFloat(lw)).clipped()
                Splitter(vertical: true, value: Binding(get: { lw }, set: { leftWidth = $0 }), range: Self.minLeft...max(Self.minLeft, W - rw - Self.minCenter - 2))
                VStack(spacing: 0) {
                    ViewerView(player: player, luts: luts, pool: pool).frame(maxHeight: .infinity)
                    Splitter(vertical: false, value: Binding(get: { lh }, set: { logHeight = $0 }), range: Self.minPane...max(Self.minPane, H - Self.minViewer - 1), sign: -1)
                    LogView(engine: engine).frame(height: CGFloat(lh))
                }
                .frame(minWidth: 0, maxWidth: .infinity).clipped()
                Splitter(vertical: true, value: Binding(get: { rw }, set: { rightWidth = $0 }), range: Self.minRight...max(Self.minRight, W - lw - Self.minCenter - 2), sign: -1)
                RightPanel(output: output, pool: pool, engine: engine, storage: storage, tab: $rightTab, addSelected: { addNodes(storage.selectedNodes) }, render: render).frame(width: CGFloat(rw)).clipped()
            }
            .frame(width: g.size.width, height: g.size.height)
        }
        .background(Th.win)
        .foregroundColor(Th.txt)
        .preferredColorScheme(.dark)
        .frame(minWidth: CGFloat(Self.minLeft + Self.minRight + Self.minCenter + 2), minHeight: 600)
        .onDrop(of: [UTType.fileURL], isTargeted: nil) { providers in
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    guard let url = url else { return }
                    DispatchQueue.main.async { dropped(url) }
                }
            }
            return true
        }
        .onAppear { TestHook.pool = pool; TestHook.luts = luts; TestHook.player = player; TestHook.output = output; TestHook.engine = engine; TestHook.storage = storage }
        .onReceive(pool.$current) { c in if c?.id != player.clip?.id { player.load(c) } }
        .onReceive(pool.objectWillChange) { _ in DispatchQueue.main.async { player.refreshInfo() } }
        .onReceive(luts.$lut) { l in player.lut = l }
        .onReceive(NotificationCenter.default.publisher(for: .importFolder)) { _ in for u in chooseMedia() { addURL(u) } }
        .onReceive(NotificationCenter.default.publisher(for: .addSelected)) { _ in addNodes(storage.selectedNodes) }
    }
    /// The side panels' widths: as dragged, shrunk in proportion when the window is too narrow for both plus the viewer.
    func fitSides(_ W: Double) -> (Double, Double) {
        let a = max(Self.minLeft, leftWidth), b = max(Self.minRight, rightWidth)
        let spare = max(Self.minLeft + Self.minRight, W - Self.minCenter - 2)
        if a + b <= spare { return (a, b) }
        let over = a + b - spare, ra = a - Self.minLeft, rb = b - Self.minRight
        guard ra + rb > 0 else { return (Self.minLeft, Self.minRight) }
        return (max(Self.minLeft, a - over * ra / (ra + rb)), max(Self.minRight, b - over * rb / (ra + rb)))
    }

    func add(_ clips: [Clip]) { let c = clips.filter { $0.convertible }; guard !c.isEmpty else { return }; engine.add(c, output.snapshot()) }
    /// Media Storage rows: clips go straight in; a folder adds every clip found inside it.
    func addNodes(_ nodes: [FSNode]) { addNodes(nodes, then: nil) }
    func addNodes(_ nodes: [FSNode], then done: (() -> Void)?) {
        guard !nodes.isEmpty else { done?(); return }
        var left = nodes.count, found: [[Clip]] = Array(repeating: [], count: nodes.count)
        for (k, n) in nodes.enumerated() {
            pool.clips(in: n.url) { cs in
                found[k] = cs; left -= 1
                if left == 0 {                       // keep the order the rows are in, and don't queue a clip twice
                    var seen = Set<String>(); add(found.flatMap { $0 }.filter { seen.insert($0.url.standardizedFileURL.path).inserted }); done?()
                }
            }
        }
    }
    func addURL(_ u: URL) { pool.clips(in: u) { add($0) } }
    func dropped(_ u: URL) {
        storage.reveal(u.path)
        pool.clips(in: u) { cs in
            add(cs)
            if cs.count == 1 { pool.selectedClips = cs; pool.preview(cs[0]) }
        }
    }
    func render() {
        if engine.queuedCount == 0 { addNodes(storage.selectedNodes) { engine.renderAll(openWhenDone: output.openWhenDone) } }
        else { engine.renderAll(openWhenDone: output.openWhenDone) }
    }
}

func chooseMedia() -> [URL] {
    let p = NSOpenPanel(); p.canChooseDirectories = true; p.canChooseFiles = true; p.allowsMultipleSelection = true; p.prompt = "Add to Queue"
    p.allowedContentTypes = [.folder, UTType(filenameExtension: "dng") ?? .image]
    return p.runModal() == .OK ? p.urls : []
}

enum TestHook { static weak var queueSel: QueueSelection?; static weak var zoom: ViewerZoom?; static weak var pool: MediaPool?; static weak var luts: LUTLibrary?; static weak var player: Player?; static weak var output: OutputModel?; static weak var engine: Engine?; static weak var storage: StorageModel? }
extension Notification.Name { static let importFolder = Notification.Name("cdng.importFolder"); static let addSelected = Notification.Name("cdng.addSelected") }

@main
struct CDNGRawConvertApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    var body: some Scene {
        WindowGroup("cDNG Raw Convert") { ContentView() }
            .commands {
                CommandGroup(replacing: .newItem) {
                    Button("Add Clips to Queue…") { NotificationCenter.default.post(name: .importFolder, object: nil) }.keyboardShortcut("i", modifiers: .command)
                }
            }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.appearance = NSAppearance(named: .darkAqua)
        signal(SIGPIPE, SIG_IGN)                 // a playback helper that quits mustn't take the app with it
        KeyRouter.install(); KeyRouter.clearInitialFocus()
        if let w = NSApp.windows.first { w.setContentSize(NSSize(width: 1600, height: 960)); w.center(); w.titlebarAppearsTransparent = true; w.backgroundColor = NSColor(red: 0.094, green: 0.094, blue: 0.102, alpha: 1) }
        // Test hooks for builds checked without screen access (environment variables only):
        // CDNG_SELECT=clip path (shown in Media Storage and the viewer), CDNG_LUT=falsecolor, CDNG_PLAY=1, CDNG_RES=1|2|4, CDNG_HISTO=1,
        // CDNG_SQUEEZE=2, CDNG_TAB=1 (metadata), CDNG_WINDOW=WxH, CDNG_QUEUE=1 (queue the selected clip), CDNG_ZOOM=percent, CDNG_LOOK=0|1|2, CDNG_SCRUB=frame (at 9 s), CDNG_EXPAND=path|path (media storage), CDNG_RENDER_TO=folder,
        // CDNG_SNAPSHOT=out.png (after CDNG_SNAPSHOT_DELAY s, then quit; playback statistics go to stderr), CDNG_FRAME_PNG=frame.png (the frame shown, full size).
        let env = ProcessInfo.processInfo.environment
        func log(_ s: String) { FileHandle.standardError.write(Data((s + "\n").utf8)) }
        func after(_ t: Double, _ f: @escaping () -> Void) { DispatchQueue.main.asyncAfter(deadline: .now() + t, execute: f) }
        if let ps = env["CDNG_EXPAND"] { after(0.5) { for p in ps.split(separator: "|") { TestHook.storage?.expanded.insert(String(p)) } } }
        if let r = env["CDNG_RES"], let v = Int(r), let pr = PlaybackRes(rawValue: v) { after(1) { TestHook.player?.res = pr } }
        if env["CDNG_HISTO"] != nil { after(0.5) { UserDefaults.standard.set(true, forKey: "showHistogram") } }
        if let t = env["CDNG_TAB"], let v = Int(t) { after(0.5) { UserDefaults.standard.set(v, forKey: "rightTab"); UserDefaults.standard.set(true, forKey: "showOutput") } }
        if let c = env["CDNG_SELECT"] { after(2) {
            guard let pool = TestHook.pool else { return }
            let u = URL(fileURLWithPath: c); var d: ObjCBool = false; FileManager.default.fileExists(atPath: c, isDirectory: &d)
            guard let k = MediaPool.kind(of: u, isDir: d.boolValue) else { log("select: not a clip: \(c)"); return }
            TestHook.storage?.reveal(c)
            let clip = pool.clip(u, k); pool.selectedClips = [clip]; pool.preview(clip)
            if let s = env["CDNG_SQUEEZE"], let f = Double(s) { Squeeze.set(f) } } }
        if let l = env["CDNG_LUT"] { after(3) { TestHook.luts?.selected = l == "falsecolor" ? LUTLibrary.falseColorID : l } }
        if let dir = env["CDNG_RENDER_TO"] { after(5) {
            guard let o = TestHook.output, let e = TestHook.engine, let pool = TestHook.pool else { return }
            o.format = .braw; o.brawByQuality = false; o.brawRatio = 5; o.destination = dir; o.nextToClip = false
            e.add(pool.selectedClips.filter { $0.convertible }, o.snapshot()); e.renderAll(openWhenDone: false) } }
        if let ws = env["CDNG_WINDOW"] { let p = ws.split(separator: "x").compactMap { Double($0) }
            if p.count == 2 { after(0.3) { if let w = NSApp.windows.first { w.setContentSize(NSSize(width: p[0], height: p[1])) } } } }
        if env["CDNG_QUEUE"] != nil { after(3) { if let o = TestHook.output, let e = TestHook.engine, let pool = TestHook.pool { e.add(pool.selectedClips.filter { $0.convertible }, o.snapshot()) } } }
        // CDNG_ADDSEL=1: the Output panel's Add to Queue (at 3 s); CDNG_MOVETOP=1: move the last job to the top (at 6 s)
        if env["CDNG_ADDSEL"] != nil { after(3) { NotificationCenter.default.post(name: .addSelected, object: nil) } }
        if env["CDNG_MOVETOP"] != nil { after(6) { if let e = TestHook.engine, let l = e.jobs.last { e.move([l.id], before: e.jobs.first?.id); TestHook.queueSel?.ids = [l.id] } } }
        if let l = env["CDNG_LOOK"], let v = Int(l), let lk = ViewerLook(rawValue: v) { after(1) { TestHook.player?.look = lk } }
        if let z = env["CDNG_ZOOM"], let p = Double(z) { after(5) { TestHook.zoom?.setPercent(p) } }
        if let f = env["CDNG_SCRUB"], let i = Int(f) { after(9) { TestHook.player?.scrub(to: i) } }
        if env["CDNG_PLAY"] != nil { after(6) { TestHook.player?.play() } }
        if let out = env["CDNG_SNAPSHOT"] {
            after(Double(env["CDNG_SNAPSHOT_DELAY"] ?? "") ?? 8) {
                if let p = TestHook.player { log("playback: frames due \(p.statDue), shown on time \(p.statShown), at frame \(p.frame + 1)/\(p.frameCount), res 1/\(p.res.rawValue)") }
                if let f = env["CDNG_FRAME_PNG"], let cg = TestHook.player?.shown, let d = CGImageDestinationCreateWithURL(URL(fileURLWithPath: f) as CFURL, "public.png" as CFString, 1, nil) { CGImageDestinationAddImage(d, cg, nil); CGImageDestinationFinalize(d); log("frame: \(cg.width)x\(cg.height), surface \(cg.surface != nil)") }
                if let p = TestHook.player { log("state: playing \(p.playing), zoom \(TestHook.zoom?.label ?? "-"), squeeze \(Squeeze.current)") }
                log("windows: " + NSApp.windows.filter { $0.isVisible }.map { "\($0.title.isEmpty ? "main" : $0.title) \(Int($0.frame.width))x\(Int($0.frame.height))" }.joined(separator: ", "))
                if let w = NSApp.keyWindow { log("first responder: \(w.firstResponder.map { String(describing: type(of: $0)) } ?? "none")") }
                if let v = NSApp.windows.first?.contentView, let rep = v.bitmapImageRepForCachingDisplay(in: v.bounds) {
                    v.cacheDisplay(in: v.bounds, to: rep)
                    try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: out))
                }
                NSApp.terminate(nil)
            }
        }
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
