# Technical findings: writing ProRes RAW that Apple's decoder accepts

These findings come from building cDNG Raw Convert (its `dng2prr` encoder) and testing it against **DaVinci Resolve Studio 21.1.1 (macOS)**, which decodes ProRes RAW with Apple's own decoder.

**Method:**
- Encode synthetic patterns with known values (flat fields, ramps, step wedges, code-value sweeps), plus real SIGMA fp frames.
- Import them into Resolve and export 16-bit TIFF stills from a colour-managed project set to **linear** output (Rec.709 primaries).
- Compare the measured values with the known inputs.

Everything below is measured behaviour, not documentation from Apple.

_Updated 2026-10-24: finding 2 rewritten after tests at higher compression revealed Apple's handling of tile quantiser 1. Finding 10 added (0.3): what a real SIGMA fp → Atomos Ninja V recording contains, and what we changed because of it._

## 1. Bitstream and container (what we write)

**Frame header:** the layout FFmpeg documents in `libavcodec/prores_raw.c`.
- `prrf`, header length 136, version 0, vendor tag `fpsp`.
- Width and height, recommended crop (taken from the DNG's DefaultCrop), Bayer pattern 0 (RGGB).
- Sensel range, white-balance red and blue factors, a 3×3 camera→XYZ (D65) matrix, gain, and colour temperature.
- Flags 0x7: quant matrix present, tile alignment 3 (tiles of up to 8 blocks), no curve.
- A flat quant matrix of 1s, and a tile quantiser of 4, 8, 15 or 30 for the four quality levels (0.1.x: 2, 4, 8, 16; never 1, see finding 2). 15 is what a Ninja V uses for the fp in "ProRes RAW HQ".

**Levels (0.3):** sensor values above black are scaled by the largest power of two that keeps white at or below 61440 + 256: ×16 for 12-bit (sensel range 61424, exactly what the Ninja V writes for the fp), ×4 for 14-bit. 0.1.x used ×4 and ×1 to keep 64–256 DN of footroom below black, which turned out to be unnecessary (finding 10).

**Tiles:**
- Each tile is 16 rows tall and up to 128 px wide, holding four Bayer components (G2, G1, B, R in coded order).
- Each component is coded as 8×8 blocks: forward DCT (orthonormal ÷ 8, matching FFmpeg's 16-bit inverse DCT), then quantisation.
- DC values are delta-coded with FFmpeg's sign-tracking scheme. AC values use interleaved run/level coding with the adaptive codebooks from FFmpeg's decoder tables.
- A component ends at end-of-data; trailing zeros are implicit.

**Frame trailer:** an `eomd` record, padded to 16 bytes.

**Container:** QuickTime (`ftyp qt`).
- Video track: `aprn` sample description (+ `fiel`, `pasp`), one sample per chunk, `co64` chunk offsets.
- Audio track: LPCM (SoundDescription v2), interleaved per video frame and trimmed to the picture length.
- Movie-level `mdta` metadata (finding 4).

## 2. How Apple's decoder interprets code values and the tile quantiser (the key finding)

Apple's decoder follows FFmpeg's documented semantics (`linear = P(u)`, where `u` is the inverse DCT output + 32768 and `P` is the default piecewise "powers of two" curve with knots every 8192 codes), **with one exception: a tile quantiser of 1 is treated as 2.**

**Measurement:**
- We encoded flat patches of known code values at tile quantisers 1, 2, 3, 4, 8, 16 and 45 (flat quant matrix of 1), then measured them in Resolve's linear output.
- At every quantiser from 2 to 45, the decoded residual matched FFmpeg exactly.
- At quantiser 1 it was exactly twice FFmpeg's.

**Consequences:**
- **This encoder always writes quantiser ≥ 2,** using the standard mapping: black (linear 256) at u = 4096, white = sensel range + 256. The quality levels Near-lossless, High, Standard and Compact are quantisers 4, 8, 15 and 30 (0.1: 2, 4, 8, 16).
- **Real recorders avoid quantiser 1.** The Atomos Ninja V frame we examined uses quantiser 45 with a flat matrix, and its data sits at u ≈ 4,000–34,000, black at 4096, exactly as FFmpeg's semantics predict.
- **Early quantiser-1 encodes** decode in Apple as `linear = P(2 × (u − 16384))`. That's where our earlier "×2 inverse DCT" model came from. It also explains the apparent limit on the first DC value (−16384…16383), since doubling must keep the value within 16 bits.
- **Mixing up the two cases causes a tint.** In dng2prr 0.1-pre, files at quantiser 2, 4 and 8 were encoded with the quantiser-1 assumption, so Resolve decoded them at half the intended contrast in code space. After white balance that showed up as a strong magenta tint (R ×2.9, B ×2.3 vs G ×1.8). Fixed in 0.1.
- **Neutrality and linearity check:** with the standard mapping, the measured output is linear (a constant scale from 0.8% to 72% of white). It's also neutral (R = G = B within 0.3%) under tungsten white-balance gains (1.36 / 2.72), identically at all four quality levels.
- **White balance and gain:** the white-balance factors apply exactly as written, and the gain field scales exposure.

**Superseded models (for the record):** we first fitted `linear = P(u)²/4096` with black 408 and white balance ×0.89, then `P(2(u − 16384))`. Both described quantiser-1 behaviour only.

## 3. Fields that made no difference

In all our tests, Resolve ignored the linearisation curve (flag bit 4, 8 control points) and the version byte (0 or 1). It also ignored the colour-temperature field for rendering; Resolve shows that field only as "Captured Temp".

## 4. Camera Raw controls need movie-level metadata

Resolve shows ISO, Captured Temp, Captured ISO and Exposure Bias only when the QuickTime `mdta` metadata block (`moov/meta` with `hdlr` mdta, `keys` and `ilst`) is present. Without it, only Decode Quality and Plugin appear.

We copy the layout of a real Atomos Ninja V recording:

| Key | Type | Example |
|---|---|---|
| `com.apple.proapps.manufacturer` / `.modelname` | UTF-8 | Sigma / SIGMA fp (as the Ninja V writes them; 0.1.x wrote SIGMA / fp) |
| `org.smpte.rdd18.camera.isosensitivity` | UTF-8 | "800" |
| `com.apple.proapps.exif.{Exif}.ExposureIndex` | float32 BE (data type 23) | 800.0 |
| `org.smpte.rdd18.camera.whitebalance` | UTF-8 | "3305K" |
| `com.apple.proapps.exif.{Exif}.FNumber`, `org.smpte.rdd18.lens.irisfnumber`, `org.smpte.rdd18.lens.lensmodel` | float / UTF-8 | 11.0 / "F11" |
| `com.apple.proapps.exif.{Exif}.ShutterSpeedValue`, `org.smpte.rdd18.camera.shutterspeed_angle` | float (APEX) / UTF-8 | 5.58 / "180.0deg" (0.3) |
| `com.apple.proapps.exif.{ExifAux}.LensModel`, `org.smpte.rdd18.lens.lensattributes` | UTF-8 | lens name (0.3) |
| `com.apple.proresraw.whitebalance.bycct.whitebalancefactors` / `.colormatrices` | binary (data type 0) | per-temperature calibration tables (0.3; see finding 10) |
| `com.apple.proapps.image.{TIFF}.Make` / `.Model` / `.Software` | UTF-8 | SIGMA / fp / dng2prr |

ISO, F-number and lens come from each DNG's EXIF. The fp's DNGs don't record a colour temperature, so we compute one (see finding 5).

## 5. Colour

- **Matrix:** we follow the DNG spec. ColorMatrix1 (Standard A) and ColorMatrix2 (D65) are interpolated by the white's colour temperature, found iteratively from AsShotNeutral. The resulting white is then Bradford-adapted to D65, and the matrix is normalised so white → Y = 1.
- **Colour-temperature check:** a 3200K tungsten bulb with grey-card white balance came out at 3305K.
- **ColorChecker (Calibrite Passport Video 2) under 3200K tungsten, fp FHD 12-bit, f/11, ISO 800, grey-card white balance:** mean ΔE2000 **4.5**, neutral patches 1.0–1.8, worst patch cyan at 11. That was measured on Resolve's linear output, exposure-normalised on the neutrals, against the published sRGB values.
- **For comparison,** separate earlier tests on the same footage scored:

  | Rendering | Mean ΔE |
  |---|---|
  | fp Color Mode OFF, straight out | 10.1 |
  | Sigma's own colour | 6.1 |
  | A chart-fitted plugin | 4.3 |
  | A tungsten matrix fitted to the chart | 3.0 |
  | Blackmagic Cinema Camera 6K FF | 3.0 |

  Those used a different method, so treat them as indicative only.

## 6. The fp's AsShotNeutral can be wrong

In the 2026-10-05 tungsten tests (custom white balance set in camera from the chart's white-balance page; firmware 5.02):

- **Every DNG recorded** AsShotNeutral = R/G **0.911**, B/G **0.719**.
- **The light actually measured** R/G **0.737**, B/G **0.367**, consistently across the grey target, all six neutral chart patches and a sheet of white paper.
- **Result:** "as shot" renders come out strongly green in both DNG and ProRes RAW. That accounts for most of the "straight out" error.
- **Open question:** we couldn't find the correct value stored elsewhere in the DNG. This is worth investigating on the firmware side.
- **Workaround:** `--neutral 0.737,0.367`.

## 7. A real Atomos Ninja V frame (Sony FX6), for comparison

- **The same 136-byte header layout,** with vendor `atm0` and flags 0x7.
- **White balance:** factors 1.0 / 1.0, colour temperature 5600, and a matrix that maps white to D65.
- **Other fields:** gain 11.8 and sensel range 65023.
- **Tile quantiser 45** (resolved by finding 2): its data range is exactly what FFmpeg's standard semantics predict for a quantiser ≥ 2. Our earlier attempts to write data in that range failed only because they used quantiser 1.

## 8. FFmpeg notes

- **Quantiser 1:** FFmpeg's decoder matches Apple's for tile quantiser ≥ 2. For quantiser 1, Apple behaves as if the quantiser were 2 and FFmpeg doesn't. It's worth handling in FFmpeg if Apple's encoders ever emit quantiser 1. We haven't seen a real camera do so.
- **Possible bug:** on x86 builds with assembly enabled, `ff_proresdsp_init_x86` sets `FF_IDCT_PERM_TRANSPOSE`, and the ProRes RAW decoder applies it to its scan table and quant matrix. It then runs the plain-C 16-bit Bayer inverse DCT, which expects natural order, so coefficients end up transposed. The C-only build (`--disable-asm`) and the Vulkan path agree with each other and with Apple.

## 9. Data rates (per frame, cDNG Raw Convert 0.3)

| Mode | DNG | Near-lossless | High | Standard (= Ninja HQ quantiser) | Compact |
|---|---|---|---|---|---|
| 4K UHD 12-bit | 12.6 MB | 6.4 MB | 5.2 MB | 4.3 MB | 3.2 MB |
| 4K UHD 14-bit | 14.7 MB | 7.9 MB | 6.7 MB | 5.6 MB | 4.5 MB |
| OG3K 12-bit | 9.2 MB | 4.6 MB | 3.8 MB | 3.1 MB | 2.4 MB |
| OG2K 12-bit | 4.1 MB | 2.1 MB | 1.7 MB | 1.4 MB | 1.0 MB |
| FHD 12-bit | 3.2 MB | 1.5 MB | 1.3 MB | 1.0 MB | 0.7 MB |

For comparison, the Ninja V's fp recording (4096×2160, "HQ") averages 4.4 MB per frame.

**Round-trip error** (FFmpeg decode vs the DNG, 4K 12-bit frame, RMS in sensor DN, by signal level as a fraction of white):

| Quality | below 1% | 1–10% | above 10% |
|---|---|---|---|
| Near-lossless (q4) | 0.04 | 0.13 | 0.62 |
| High (q8) | 0.08 | 0.25 | 1.2 |
| Standard (q15) | 0.14 | 0.47 | 2.3 |
| Compact (q30) | 0.27 | 0.91 | 4.5 |

Compared with 0.1.x at the same file size, shadow error is about halved and highlight error is about 1.7× larger. That trade suits the image, because sensor noise grows with signal level: at 10% of white the shot noise alone is far above 4 DN. For 14-bit footage, 0.3 is better everywhere and about 10% smaller.

## 10. A SIGMA fp recorded on an Atomos Ninja V (ProRes RAW HQ), and what it changed

Source: a clip shared by a SIGMA fp owner who records to a Ninja V (`NINJAV_S001_S001_T043.mov`, Ninja V firmware 10.92, 2023), 4096×2160, 24 fps, 280 frames.

**Frame header (identical in every frame we checked):**
- Vendor `atm0`, version 0.
- Sensel range **61424**, which is (4095 − 256) × 16: the fp's 12-bit HDMI raw shifted left by 4 bits, with black at 256.
- White balance 1.898 / 1.727, colour temperature 5419 K (the camera's white-balance setting).
- Matrix: maps white to 0.98 × D65.
- Gain **9.59** at ISO 800. The fp's own DNGs use BaselineExposure = log2(ISO/100), which gives 8 at ISO 800. So a Ninja recording renders about 0.26 EV brighter than our conversion of an internal clip at the same ISO. **This is unexplained.** HDMI raw may be scaled differently, or Atomos may simply prefer brighter. Resolving it needs the same scene recorded both ways; we left our gain unchanged.
- **Flags 0x27:** quant matrix and tile alignment 3 as usual, plus **bit 5 (0x20), whose meaning is unknown.** The Sony FX6 Ninja file doesn't set it.
- Flat quant matrix, **tile quantiser 15** in every tile.
- Columns 4094–4095 are always 0, so the fp probably sends 4094 active pixels per line.

**Per-frame trailer:**
- Besides `eomd`, there's a `psim` record named `com.sigma.fp.optical_correction`: two 16-bit values (128, 128), then 12 floats in three groups of four. These are probably lens-shading parameters per colour channel.
- We don't know whether Resolve uses it. If the format is decoded, it could carry the fp's lens corrections, which DNG stores in OpcodeList3.

**Movie metadata, beyond what we already wrote:**
- Manufacturer is "Sigma" and model "SIGMA fp". We now write exactly these.
- Shutter speed (APEX) and shutter angle, a lens model under three keys, `com.atomos.raw.intermediate_oetf/gamut` = VLog/VGamut, and recorder Make/Model/Software. We now write all of these except the Atomos-specific keys.
- **`com.apple.proresraw.whitebalance.bycct.whitebalancefactors`** and **`...bycct.colormatrices`**: binary tables, big-endian.
  - Layout: uint16 count (14), uint16 version (1), then per colour temperature (2500–10000 K) either uint32 K + float R + float B factors, or uint32 K + 9 floats (white-balanced camera → XYZ D65).
  - This is the fp's calibration as Atomos ships it.
- Video sample description `aprh` ("Apple ProRes RAW HQ"). There's also a `tmcd` timecode track and 24-bit 3-channel PCM audio.

**What we measured with it:**
1. **Resolve uses the bycct tables, even for the as-shot picture.** Adding tables built on the blackbody locus to one of our clips changed Resolve's as-shot render (blue −15%, red −3%): Resolve looks up the clip's colour temperature in the tables. The locus has no tint, though, and real white balances (a grey-card balance under tungsten, for example) sit off it.
   - **The fix:** dng2prr 0.3 shifts the whole table by the clip's own tint and adds the clip's exact temperature as an entry. With that, Resolve's as-shot render matches the no-table render exactly.
   - **What the tables should add:** white-balance changes in Resolve's Camera Raw panel now follow the fp's calibration from its DNG colour matrices. **We haven't confirmed this in the UI yet.**
2. **The Atomos white-balance locus agrees with the fp's DNG calibration** to within 1–3% from 2700 K to 5000 K. That confirms the HDMI raw has the same channel balance as the internal raw. They diverge by up to about 11% at 6000–7500 K, mostly in tint.
3. **The Atomos colour matrices are less accurate than the fp's own DNG matrices** on our ColorChecker under tungsten (grey-card white balance, same measurement for both): mean ΔE2000 **7.7** with Atomos's matrices at 2800–3600 K, against **4.7** with the DNG-spec interpolation. So we kept the DNG-derived matrices and build our bycct tables from them.
4. **The fp clips its raw data at black.** Across all our sample DNGs, the lowest value is 2 DN below black. The footroom 0.1.x reserved was wasted, so 0.3 uses Atomos-style levels (finding 1) for finer shadow precision (finding 9).

**About the matching "cDNG" folder:** it isn't the camera's own recording. It's a conversion of the Ninja file by a tool that identifies itself as "RAW convertor". The DNGs are:
- 4096×2160, 16-bit tiles (256×272);
- compressed with **lossy** 12-bit JPEG (SOF1, two components, Compression = 7);
- linearised with a square-law table (value = code² / 256);
- tagged with the Ninja header's white balance (AsShotNeutral = 1/factors) and gain (BaselineExposure = log2 9.59).

So they're a lossy derivative of the same data and can't serve as an independent reference. The other files in that folder (three internal fp DNGs from January 2026, and a BRAW clip) come from different shoots.

**Still wanted:** the same scene recorded on the fp internally (cDNG) and on a Ninja V. That would settle the 0.26 EV gain difference and show whether Resolve applies the `psim` optical correction.

## 11. Compressed CinemaDNG: what the writer does and what Resolve accepts (0.3)

**Layout we write:**
- **Tiles:** 256×256, Compression = 7, every other tag copied verbatim.
- **Tags dropped:** strip tags, and RawImageDigest/NewRawImageDigest, which would no longer match the image data.
- **Lossless:** lossless JPEG with predictor 1, 2 components, JPEG width = tile width / 2. Each component holds one CFA column phase, so prediction is always from the same colour. Same bit depth as the source.
- **10-bit log:**
  - The curve is v = black + span·((x+e)² − e²)/((1+e)² − e²), with x = code/1023 and e chosen for 1 DN per code at black.
  - It's stored as a 1024-entry LinearizationTable, with BitsPerSample = 10. Resolve reads it correctly.
- **Lossy:**
  - Uses the layout of existing lossy CinemaDNG files (the Ninja-derived ones in finding 10): SOF1 12-bit DCT, 2 components, JPEG width = tile width and height = tile height / 2.
  - With that layout, every 8×8 block contains a single CFA colour.
  - Codes follow a square-root curve with a linear toe and a pedestal 128 codes below black. That is a 4096-entry LinearizationTable with BitsPerSample = 12. Resolve also accepts BitsPerSample = 16, with identical results.
  - Each tile gets its own optimised Huffman tables.

**Lessons:**
- **Inverting a table with flat runs.** Near black, several codes map to the same integer value. Picking the last code of such a run made DCT errors decode asymmetrically: +0.4 DN on average, which was +2–4% red and blue in a dark tungsten frame. Picking the middle of the run brought the bias to under 0.01 DN.
- **Clipping at code 0.** Without a pedestal below black, DCT ringing clips at code 0 and biases the shadows upwards.
- **Resolve and lossless files.** Lossless output renders identically in Resolve except at the outermost picture rows. The tile size made no difference there (256×256 and a single 1936×1104 tile gave the same result), so it's how Resolve handles the image edge.
- **Our decoder on third-party lossy DNGs.** It decodes the lossy DNGs another tool made from the Ninja V clip. They correlate at 0.999997 with FFmpeg's decode of the original ProRes RAW. That cross-checks both the layout and the decoder.

## 12. Timecode and per-frame metadata in the .mov (0.3)

**Timecode:**
- **Source:** DNG TimeCodes (tag 51043) holds SMPTE 12M BCD bytes: frames, seconds, minutes, hours, with bit 6 of the frames byte as the drop-frame flag.
- **What we write:** a standard QuickTime `tmcd` track.
  - Timescale = fps numerator, frame duration = denominator, nominal frames = round(fps).
  - Flags: 24-hour max, plus drop-frame when set.
  - One 4-byte sample holding the start frame number.
  - The video track references it with `tref/tmcd`.
- **Checked in Resolve 21.1.1:** Start TC 20:33:12:19 for a B001_013 frame, matching the DNG.

**Per-frame metadata:**
- **Where:** a QuickTime timed-metadata track. Handler `meta`, `nmhd`, sample entry `mebx` with one key: `mdta` `io.github.ijigen.fpsup.dng.frame-tags`, well-known type 0 (binary). It's linked to the video track with `tref/cdsc`.
- **What's in each sample:** one item (size, local key 1, value). The value is a TIFF made from that frame's IFD0 and sub-IFDs (EXIF with MakerNote, GPS), with only the image-data tags removed.
- **Size:** about 79 KB per frame for the fp, mostly Sigma's 54 KB MakerNote. That's about 15 Mbit/s at 24 fps, around 2% of a ProRes RAW HQ stream.
- **Checked:** Resolve imports and decodes the file normally, and FFmpeg lists the track. Extracted tags are byte-identical to the source DNG's.

**fpSup gyro telemetry:** the raw blocks arrive inside the per-frame metadata unchanged. Since 0.3 they're also converted into a Gyroflow track: see section 13.

## 13. fpSup Gyro2 data → a Gyroflow telemetry track (0.3)

**Where the data is in the DNGs.** Checked on two clips from 2026-10-25 (B001_003 uncompressed, B001_002 camera lossless, fp firmware 5.02 with fpSup Gyro2):
- **The block:** each frame has one "FSG2" block in the zero-filled tail of the EXIF MakerNote. In these files it's at MakerNote offset 43328; the MakerNote is 54338 bytes.
- **Block layout** (little-endian): magic, version 1, kind, 2 reserved bytes, u32 length, payload, IEEE CRC-32 over everything before it. These clips are all **kind 2**, the camera's raw gyro record. Kind 1 (a ready-made Gyroflow message) and IFD0 tag 65000 don't appear.
- **Kind-2 payload:**
  - A 48-byte header: flags, clip id, frame sequence number, sample period in ps, gyro scale (rad/s per count), accelerometer scale (g per count), readout time in ns, sensor mode, readout crop, frame mark, base sample index, sample and event counts.
  - Then the raw gyro samples (3 × i16).
  - Then events: accelerometer "level" readings and the sensor's Vd frame sync.
  - With flag 64, the lens's distortion table follows.
- **In these clips:** the sample rate is 2499 Hz (period 400.0854 µs) and the readout 12.435 ms. The crop is 6000×4000, a full-sensor readout. The first block carries about 100 ms of samples from before the first frame, and later ones about 85–105 samples each.

**The conversion** follows ijigen/gyroflow-plugins `common/src/fsg2.rs` (`to_messages_with`) and `lensfit.rs` at commit 4b3dbac, translated to C (`source/fsg2.c`):
- **Units:** gyro counts × gscale × 180/π gives deg/s; accelerometer counts × ascale × 9.80665 gives m/s².
- **Accelerometer:** turned into the gyro's axes as (y, −x, z), and held between level events. Until the first event, the first reading of the take is used.
- **Samples:** de-duplicated by global index, so guard samples shared by two blocks appear once. A lost block leaves a gap rather than shifting later samples.
- **Sample time:** (base_idx + position) × period.
- **Frame timing:**
  1. Fit a line through all Vd events.
  2. Assign each frame its Vd as frame_seq plus a ±4-frame median offset. This survives late hooks and skipped sensor frames.
  3. start = Vd × period + 900 µs; end = start + readout.
- **Lens:**
  - For frames with a focal length: focal length, f-number and focus distance.
  - Plus a camera matrix and OpenCV-fisheye coefficients, fitted from the camera's distortion table (or the frame's WarpRectilinear opcode) at that frame's focus.
  - A lens without contacts gets only the f-number, like the plugin's default.
- **Header:** brand/model/serial/firmware from the DNG; pixel pitch 5983 nm; sensor 6000×4000; IMU orientation "xyz"; frame rate from the DNG FrameRate; readout top to bottom.
- **Frame size:** the DNG's DefaultCropSize, 3008×2000 here. That's the picture Resolve decodes from our ProRes RAW, which applies the same crop.
- **Tests:** `tests/test_fsg2.c` reproduces the plugin's golden values: distortion interpolation and fit to 1e-8, and Vd assignment through a late hook and a skipped frame.

**How it's stored:**
- One serialized Gyroflow `Main` message (protocol 1) per video frame, in a QuickTime metadata track: handler `meta` named "GyroflowTelemetry", `nmhd`, sample entry `gyrf`, `tref/cdsc` → video. The samples share the video track's timeline and are interleaved with the frames.
- The first message carries the Header.
- **Reading it:** telemetry-parser recognises the file by the "GyroflowProtobuf" string in the first or last 5 MB, then reads the **first** metadata track. So the gyro track is written before the `mebx` per-frame tag track.
- **Size:** about 2–3 KB per frame.

**Checked:**
- `tests/check_gyro.py` decodes every sample with Python protobuf against telemetry-parser's `gyroflow.proto`. Frame numbers run 1…N, the header is on the first message only, IMU timestamps increase, and the embedded track matches the `--gyro-jsonl` output.
- **B001_003:** 157 frames, 16494 IMU samples, frame interval 41708.0 µs (nominal 41708.3).
- **B001_002:** 152 frames, 15972 IMU samples.
- Resolve 21.1.1 imports and decodes both files (3008×2000, audio, Start TC). FFmpeg lists the `gyrf` stream.
- Re-compressed CinemaDNG output (lossless and VBR HQ) still carries valid blocks, and converts to the same messages.
- **Not checked yet:** Gyroflow itself (no Rust registry access here to build telemetry-parser), and the per-frame lens fit on a real clip, because the test clips used a manual lens.

## 14. fpSup Gyro2 v0.2.0: the gyro block moved to its own MakerNote entry (0.6.10)

**What changed in the camera's DNGs.** Clips from 2026-11-04 (fpsup-v4-gyro2, fp firmware 5.02, plugin v0.1.10), checked against ijigen/gyroflow-plugins at tag fpsup-v0.1.10 (commit f0ad113, 2026-10-15):
- **The carrier.** The block is no longer in the MakerNote's zero tail. The SIGMA MakerNote (it starts "SIGMA\0\0\0", its IFD follows at +10) gets an entry of its own, **tag 0x4653** (UNDEFINED, sized for the take: 2871 bytes here). Its value starts with the block on the file's first 4-byte boundary at or after the entry's offset.
- **Block version 2:** magic "FSG2", version 2, kind 2, two bytes holding the writing sup's build (diagnostics only), u32 length, payload, and **no CRC**. The camera clears the magic when it has no block for a frame; the bytes after the payload are left from earlier frames.
- **Payload version 2** differs from version 1 only at bytes 24–38: sensor mode id, hmax (72 MHz clocks per line), columns read, lines read, horizontal and vertical binning, and the calibrated focal length in tenths of a mm. Readout = hmax × lines / 72 MHz (24.982 ms in these UHD 3:2 clips); the readout window is centred on the 6000×4000 sensor. Version 2 has **no lens table**: each DNG's WarpRectilinear opcode is the distortion for its focus, with the calibrated focal length from the payload. A version 2 block may also end before its frame mark (the next block catches up).
- **Zoomed takes** (plugin v0.1.10, both payload versions): when the mount's focal length changes during a take, frames away from the starting focal length use their own focal length and opcode instead of the table or calibrated focal length.

**Why 0.6.9's outputs lost the gyro data:**
- `fsg2.c` (ported at 4b3dbac) only looked for version 1 blocks in the MakerNote tail. It reported "unsupported block version 2", so the ProRes RAW .mov got no Gyroflow track (the plugin: "Not synced. Open in Gyroflow") and the .braw got no motion tracks or Gyroflow track ("OK", but nothing to stabilise with). The app's Metadata panel, which asks the same reader, said "Gyro2 data: no".
- **Compressed CinemaDNG**: the SIGMA MakerNote's IFD gives its values' positions **from the start of the file**, not the MakerNote. The writer copied the MakerNote as an opaque block to a new place (offset ~2900 instead of 18988), so tag 0x4653 still pointed at 73338, now inside the image data, and the plugin found "no telemetry block in the Gyro2 MakerNote tag" ("Failed to load CinemaDNG telemetry"). The .mov's per-frame tag track had the same problem.

**The fix (0.6.10):**
- `fsg2.c` now reads both carriers and both payload versions as the plugin does (`cdng.rs` gyro2_tag / parse_tag_block, `fsg2.rs` SensorMode, zoom_start, lens_data); `tests/test_fsg2.c` adds the plugin's version 2 test values.
- The DNG writer moves a SIGMA MakerNote's internal offsets with it (those pointing inside it), and keeps its position modulo 4, so the block is found at the same word boundary.
- **Checked** on RITZ_SD B001_001 (296 frames; the camera's JEYI_SSD clips read the same way): `tests/verify_gyro2.py` (a re-implementation of the plugin's reader) reads every frame's block from lossless, VBR-HQ and 7:1 output, byte-identical to the camera's; every other MakerNote and EXIF value is identical too. The .mov from the camera's DNGs and the .mov from re-compressed DNGs both get the full track (30997 IMU samples at 2499 Hz); `tests/check_gyro.py` decodes it (frame interval 41708.8 µs, lens matrix from the 24-70 at 35 mm). The .braw gets the Gyroflow track and the Blackmagic motion tracks (30851 readings).
- **Clips re-compressed with 0.6.9 or earlier from Gyro2 v0.2.0 footage** have the wrong offset and need converting again from the camera's DNGs.

## 15. Gyro in .braw: the plugin's session cache, and the readout its parser computes (0.6.11)

**Symptom (2026-11-04).** After 0.6.10, the Gyroflow (fpSup) plugin v0.1.10 in Resolve said "Status: OK" for re-rendered .braw files but didn't stabilise them. The .mov and DNG routes worked.

**Cause 1: Gyroflow's in-memory telemetry cache.** gyroflow-core (ijigen fork, rev f1f09194, the plugin's) keeps every file's parsed telemetry for the life of the host process. The key is the path, the load options, the frame size and the frame rate (`GyroSource::parse_telemetry_file`, feature `cache-gyro-metadata`), not the file's contents. The plugin's log showed the .braw telemetry parsed only at 17:38, from the 0.6.9 files, which had no motion data. Those parsed successfully (a Blackmagic file with no readings), so they were cached. The 0.6.10 files written over them at 18:52 were never read, so the plugin kept stabilising with no readings. "OK" means only that the file's timestamps are reliable, which is true for every Blackmagic file; it says nothing about whether there are readings. The 0.6.9 .mov didn't parse at all, so nothing was cached for it, and the new .mov loaded. **Quit and reopen Resolve after replacing a file the plugin has already loaded** (or give the new file another name).

**Cause 2 (a real error, fixed in dng2braw 0.15): the readout the plugin computes.** telemetry-parser at d45ebf2 (the plugin's) computes a .braw's readout as `sensor_area_captured` rows × `sensor_line_time`. At 19bc11f (Gyroflow 1.6.3) it's the picture's rows (`crop_size`) × `sensor_line_time`. dng2braw wrote the line time per picture row (the readout ÷ 2160 rows) and, as the Video Assist does, a sensor area of 6000×3376 for fp UHD. So the plugin took the readout as 3376 × 9.763 µs = 33.0 ms instead of 21.1 ms: rolling-shutter correction 56 % too strong, and the readings shifted 6 ms (the parser moves them back by half the readout). dng2braw 0.15 writes `sensor_area_captured` = the picture's size when there's gyro data, so both versions get 21.088 ms. The Blackmagic RAW SDK still opens and decodes the file.

**Checked:** the motion tracks in the user's B001_003.braw (147 frames) are complete: 15 321 gyroscope and 15 321 accelerometer readings at 2499 Hz, spanning the clip; gyro RMS 1–3 °/s; gravity +9.6 m/s² on Y. The plugin's telemetry-parser reads tracks the same way as 19bc11f, with the same mp4parse revision (ff910c6), which FINDINGS 41 of dng2braw checked against the .mov route. **Not yet seen working in Resolve:** a fresh load of a 0.6.11 .braw in the plugin. Until then the Blackmagic RAW output is labelled BETA in the app.

