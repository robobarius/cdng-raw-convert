# Porting cDNG Raw Convert to Windows and Linux: a handoff guide

This is for whoever picks up cDNG Raw Convert next. Its encoder is called `dng2prr` (the native `dng2prr_cli.c` and the Python `dng2prr.py`); the file names keep that name. It covers how the tool works, what's specific to each platform, what has and hasn't been tested, how to prove a port is correct, and what's still open.

Background reading:
- **README.md:** what the tool does, for users.
- **FINDINGS.md:** how the format and Apple's decoder behave. **Read finding 2 before changing anything in the encoder.**

## 1. Where things stand (0.3, 24 October 2026)

| Platform | Command line | Window / app | Checked in a real decoder |
|---|---|---|---|
| **macOS** (Apple silicon) | ✅ used on real clips | ✅ native app (`build/macos-app/`; SwiftUI + native encoder); older Python droplet in `build/macos/` | ✅ Resolve Studio 21.1.1 |
| **Linux** (x86-64) | ✅ all development and byte tests ran on Linux | ✅ `dng2prr_gui.py` (Tkinter) run under a virtual display: picks clips, encodes, progress bar, Stop | ❌ no Linux Resolve test yet |
| **Windows** | ⚠️ prepared, never run | ⚠️ `dng2prr_gui.py` and PyInstaller script prepared, never run | ❌ |

**What "prepared" means for Windows.** The code has been changed for every Windows difference we know of (section 3), and each change was tested on Linux where possible. For example, we forced Windows-style "spawn" multiprocessing and got output byte-identical to the normal path. Nobody has yet compiled or run it on a Windows PC. **Expect some first-run fixes.**

**Linux and Windows decoding.** A third-party guide says DaVinci Resolve has decoded ProRes RAW on Windows and Linux since version 20.2. We haven't confirmed that against Blackmagic's release notes. If true, the validation in section 5 can run on those platforms directly.

## 2. How it works

Everything that matters is in four source files, plus `source/dng2prr_cli.c` (a native C port of `dng2prr.py` that produces identical files, and the compressed-CinemaDNG writer) `source/dng_jpeg.c` (JPEG codecs for DNG tiles) and `source/fsg2.c` (fpSup Gyro2 data → Gyroflow Protobuf track, GPL-3.0-or-later; FINDINGS §13): `source/dng2prr.py` (~700 lines), `source/prr_core.c` (~300 lines), `source/dng_lj92.c` (~200 lines, compressed DNGs) and the optional `source/dng2prr_gui.py`.

```
 DNG frame ─► DNGFrame            read tags + unpack 12/14-bit Bayer         (dng2prr.py)
          ─► colour_params()      white balance, camera→XYZ matrix, CCT, per the DNG spec
          ─► encode_frame()       sensor values → linear 16-bit → code values u
                                  (u = inverse of the default curve P; black 256 → u 4096)
          ─► prr_encode_tiles()   8×8 DCT, quantise (tile quantiser 4/8/15/30), entropy
                                  code 16-row tiles, 4 Bayer components each   (prr_core.c, via ctypes)
          ─► frame header         'prrf' + 136-byte header + tile size table + tiles + 'eomd'
 all frames ─► write_mov()        QuickTime: 'aprn' video track, LPCM audio from the WAV,
                                  movie-level mdta metadata (makes Resolve's Camera Raw controls work)
```

- **Parallelism:** `main()` encodes frames in a `multiprocessing.Pool`, one frame per task. `write_mov()` streams them to disk in order, interleaving audio per frame.
- **Memory:** about 3–4× one frame's pixels per worker. With 4 workers on a 4K 14-bit clip that's a few GB in total, so lower `--jobs` on small machines.
- **Speed:** about 1–2 s per 4K frame per CPU core (numpy plus the C core), so a 4-core machine does about 2–4 frames per second.

**Things in the code that look odd but are deliberate:**
- **`QUALITY_Q`:** the quality setting `--scale 1/2/4/8` writes tile quantiser 4/8/15/30. 15 is the Ninja V's "HQ" level. **Never write quantiser 1:** Apple decodes it as 2, which caused the 0.1-pre magenta bug.
- **The quant matrix is all 1s.** All compression comes from the tile quantiser.
- **`prr_set_dc_clamp(0)`:** the clamp is a leftover from quantiser-1 experiments. It stays off.
- **Level scaling (`level_scale`, `LEVEL_MODE`):** 12-bit data is scaled ×16 and 14-bit ×4, as a Ninja V does. Black is fixed at 256 in the format. This leaves 16 or 64 DN below black, which is enough because the fp clips its raw data at black. `LEVEL_MODE = 'footroom'` restores 0.1.x behaviour.
- **bycct tables (`calibration_by_cct`):** Resolve reads these for the as-shot picture. They must be shifted by the clip's tint, and must include the clip's exact colour temperature, or the as-shot colour changes (FINDINGS.md finding 10).
- **Header fields with no visible effect:** the curve flag, version and CCT made no difference in Resolve. CCT shows up only as "Captured Temp". We write the same values as an Atomos recorder.
- **Test hooks:** `f.u`, `f.colour` and `f.sensel_override` exist so synthetic test frames can bypass the DNG path. The validation scripts use them.

## 3. What's platform-specific, and what was done about it

| Area | macOS | Linux | Windows | Status |
|---|---|---|---|---|
| **C core** (`prr_core.c` + `dng_lj92.c`) | `libprr.dylib` (universal) | `libprr.so` | `libprr.dll` | `dng2prr.py` picks the name by platform. `PRR_LIB=/path` overrides it. `prr_core.c` exports with `__declspec(dllexport)` on Windows and has an MSVC-safe `clz32` and `M_PI` (not yet compiled with MSVC) |
| **Compiler flags** | `-ffp-contract=off` | `-ffp-contract=off` | MSVC `/fp:precise`; MinGW `-ffp-contract=off` | Keeps output byte-identical to the reference. With fused multiply-add on, bytes differ by under 0.2% in size: harmless, but `selftest.py` then can't confirm "identical" |
| **Multiprocessing** | fork | fork | spawn | Settings reach workers via a Pool initializer, so both behave the same (tested by forcing spawn on Linux: identical output). `freeze_support()` added for a frozen .exe |
| **File names** | case-insensitive (usually) | case-sensitive | case-insensitive | Globbing `*.DNG` and `*.dng` on Windows returns every file twice. Replaced with one case-insensitive `listdir` filter (DNGs and the WAV) |
| **Front end** | AppleScript droplet (`build/macos/`) | `dng2prr_gui.py` (Tkinter), optional `.desktop` entry | `dng2prr_gui.py`, packaged with PyInstaller | The GUI accepts paths as arguments, so dropping folders onto the .exe in Explorer works. With the optional `tkinterdnd2` package, folders can also be dropped onto the window |
| **Stop button** | `pkill` | `terminate()` | `taskkill /T /F` | Kills the encoder and its worker processes and deletes the half-written file |
| **Python + numpy** | Xcode tools Python (the build script installs numpy) | distro packages | python.org or PyInstaller bundle | The .exe bundles both, so end users need nothing installed |
| **Output location** | next to the clip, else `~/Movies/ProRes RAW` | next to the clip, else `~/Videos/ProRes RAW` | same as Linux | Never overwrites; adds `_2`, `_3`, … |

**Windows risks we know of but couldn't test:**
- **Long paths:** paths over 260 characters fail unless long paths are enabled. Cards copied deep into folders could hit this.
- **Security warnings:** antivirus software and SmartScreen sometimes flag unsigned PyInstaller .exes. Code signing, or sharing the source with `build_exe.bat`, avoids this.
- **Windows on Arm:** needs an ARM64 Python and an ARM64 DLL.
- **Writing to the card:** if the card is read-only, the GUI falls back to `~/Videos`.
- **Console output in the windowed .exe:** the GUI starts the encoder as a child process (`--cli` mode). If progress never appears, check that the child's stdout reaches the GUI (there's a fallback in `run_cli`).

## 4. Building

**A native route for Windows and Linux too.** `source/dng2prr_cli.c` needs only a C compiler: no Python and no numpy.
- **Linux:** it builds as is (`build/linux/build.sh` builds it).
- **Windows:** MinGW provides `dirent.h` and pthreads, so it should build there too. MSVC would need small shims for `opendir/readdir`, `realpath` and pthreads.
- **A Windows GUI** could run `dng2prr.exe` exactly the way the macOS app does: one clip at a time, parsing the `frame i/n` progress lines.


| Platform | Steps |
|---|---|
| **Linux** | `build/linux/build.sh` compiles `source/libprr.so`, checks numpy and runs the self-test. Add `--desktop` for a menu entry. The GUI needs Tkinter (`python3-tk` / `python3-tkinter`). |
| **Windows** | 1. Run `build\windows\build.bat` from the "x64 Native Tools Command Prompt for VS", or with MinGW gcc on PATH. It compiles `source\libprr.dll` and runs the self-test.<br>2. Run `build\windows\build_exe.bat` to produce `dist\cDNG Raw Convert Droplet\` with the .exe. |
| **macOS** | Double-click `build/macos/Build cDNG Raw Convert Droplet.command`. |

All three put the compiled core in `source/`, next to `dng2prr.py`, where it's found automatically.

## 5. Proving a port is correct

Do these in order. Don't share a build that hasn't passed step 3.

1. **Self-test:** `python3 tests/selftest.py`, optionally with `--dng <folder of 4K/FHD/OG2K/OG3K.DNG>`, and `python3 tests/test_lj92.py`.
   - It checks frame structure (header, tile quantiser, tile sizes, trailer).
   - It compares output bytes against `tests/reference.json`.
   - "identical" is the goal. "ok-ish" means floating-point contraction differs, which is fine but worth fixing with the flags above.
   - The four DNGs are fpSup's public quality-test frames.
2. **Round trip through FFmpeg (optional, good for debugging).** Decode a frame with FFmpeg and compare it with the DNG. On x86, use an FFmpeg built with `--disable-asm` (FINDINGS.md finding 8). This checks the bitstream, not Apple's interpretation of it.
3. **Real decoder:** follow `tests/validation/README.md`.
   - Make the wedge clips, export linear stills from Resolve (or another Apple-based decoder), and run `measure_still.py`.
   - All four quality levels must pass and match each other.
   - Compare with `tests/validation/reference_results_resolve_21.1.1_macos.txt`.
   - **If Resolve on Windows or Linux gives different numbers from macOS, that's a major finding.** Please write it up in FINDINGS.md.
4. **A real clip, end to end:** convert an fp clip with audio and check in Resolve:
   - it imports as "Apple ProRes RAW";
   - the audio is in sync;
   - the Camera Raw tab shows ISO, Captured Temp and Exposure Bias;
   - the picture matches the macOS conversion of the same clip.

   Bytes should match a macOS conversion of the same clip once both are built with contraction off.

## 6. Things you must not change without re-validating in a real decoder

- The tile quantiser must be **2 or more** (`QUALITY_Q` / `tile_quantiser`).
- The bycct tables must reproduce the frame header at the clip's own colour temperature. Check this with an as-shot still, with and without tables.
- The code-value mapping: `LIN_CURVE`/`LIN_KNOTS`, black 256 at u = 4096, sensel range = white − black after scaling.
- The 136-byte header layout, flags 0x7, `ALIGN = 3`, and the component order G2, G1, B, R in tiles.
- The `mdta` keys in `camera_metadata()`. Resolve hides the Camera Raw controls without them.
- Bayer phase: frame width and height must stay even, and only RGGB is supported (the fp is RGGB).

## 7. Open work, roughly in order of usefulness

1. **Compressed DNGs as input** are done in 0.3. `dng_lj92.c` decodes lossless JPEG and `dng_jpeg.c` lossy JPEG, plus LinearizationTable support. Checked on a real fpSup lossless clip, with 512×512 tiles and mixed compressed and uncompressed frames, and with synthetic data (`tests/test_lj92.py`).
2. **Resolve on Windows and Linux:** run section 5 step 3 and report the results.
3. **Gyroflow check:** the Gyro2 telemetry track (FINDINGS §13) still needs confirming in Gyroflow and its OpenFX plugin. Timecode is done (0.3).
4. **Lens corrections:** the DNG's OpcodeList3 (vignetting and distortion) isn't carried over. ProRes RAW has no equivalent field we know of, so this may mean baking the corrections in, or leaving them out.
5. **Speed:**
   - Move the level mapping (now in numpy) into C.
   - Use a fast integer DCT.
   - Read DNGs with memory mapping.
   - 3–5× faster looks achievable.
6. **Other decoders:** Final Cut Pro, Premiere Pro and Assimilate, which use Apple's decoder on macOS and Windows.
7. **The fp's AsShotNeutral** can be wrong (FINDINGS.md finding 6). This is worth investigating on the firmware side.
8. **Same scene, internal and Ninja V:** this would settle the 0.26 EV gain difference and the `psim` optical-correction record (FINDINGS.md finding 10).
9. **Upstream:** an FFmpeg `prores_raw` encoder based on this code, plus an FFmpeg decoder fix for quantiser 1 and the x86 transpose issue.

## 8. Lessons learned

**Debugging Apple's decoder:**
- **Measure; don't guess.** Every "obvious" model of the decoder we started from turned out wrong somewhere. Synthetic patterns with known values, measured as linear stills, settled every question.
- **Test every quality level.** The quantiser-1 special case stayed hidden for weeks because early calibration ran only at the default level.
- **Resolve's 16-bit TIFF stills carry about 10 bits.** Use mid-tones for precise measurements.
- **Resolve rewrote one of our test `.mov` files after import,** adding a `uuid` atom near the start of the file. Don't rely on a file's checksum staying the same after Resolve has opened it.

**Building and running on macOS:**
- **Terminal and Desktop folders:** macOS can block Terminal from reading Desktop or Documents folders, so build scripts there may see files as missing. The macOS build script now detects this and explains how to fix it.
- **AppleScript:** don't use `running` as a variable name in the droplet; it's a reserved word (error −10006).

## 9. Licence

- **dng2prr is LGPL-2.1-or-later.** `prr_core.c` contains codebook tables and the scan order from FFmpeg (LGPL-2.1-or-later). Keep the attribution in NOTICE.md.
- **Distributing a PyInstaller build:** include LICENSE and NOTICE.md (`build_exe.bat` does) and point to where the source is.
- **Trademarks:** don't use Apple or SIGMA trademarks in a way that suggests endorsement. The README's disclaimer covers this.
