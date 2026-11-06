# Changelog

## 0.6.11 (2026-11-04): Blackmagic RAW marked beta; gyro readout fix

- The Output panel marks **Blackmagic RAW** as **BETA** while gyro stabilisation through it is being validated.
- **Why BRAW gyro didn't stabilise in Resolve after 0.6.10:** the Gyroflow plugin (its Gyroflow core) caches each file's gyro data, by file name, until Resolve quits. The .braw files had first been loaded as their 0.6.9 versions, which had no gyro data, and the plugin kept using that. **After re-rendering a clip the plugin has already loaded, quit and reopen Resolve.**
- **Fix (dng2braw 0.15):** the plugin's version of Gyroflow's file reader worked out a 56 % too-long sensor readout from our .braw metadata (33 ms instead of 21 ms for fp UHD), which over-corrects rolling shutter and shifts the gyro by 6 ms. The .braw now says the sensor area is the picture when it carries gyro data, so both reader versions get 21.1 ms.

## 0.6.10 (2026-11-04): gyro data from fpSup Gyro2 v0.2.0 (fpsup-v4-gyro2)

- **Fix: gyro data from the current fpSup Gyro2** (v0.2.0 and later, which needs the Gyroflow (fpSup) plugin v0.1.10) is read again. That Gyro2 moved each frame's gyro block into its own SIGMA MakerNote entry, in a new block version and payload version, and 0.6.9 didn't know either. So:
  - **ProRes RAW** `.mov` files now get the Gyroflow track again (0.6.9: none, so the plugin said "Not synced. Open in Gyroflow").
  - **Blackmagic RAW** files now get their Blackmagic motion tracks and Gyroflow track again (0.6.9: none, so the plugin said "OK" but had nothing to stabilise with).
  - The **Metadata panel** shows Gyro2 data for these clips again.
- **Fix: compressed CinemaDNG keeps the gyro data.** That MakerNote entry gives its position from the start of the file, and the writer moved the MakerNote without moving the position, so the plugin found image data there ("Failed to load CinemaDNG telemetry"). The writer now moves the MakerNote's internal positions with it. Re-compressed frames carry byte-identical gyro blocks (checked on 296-frame clips at lossless, VBR-HQ and 7:1).
- Also from plugin v0.1.10: zoomed takes use each frame's own focal length and lens correction away from the starting focal length.
- **Convert again** any Gyro2 v0.2.0 clips you converted with 0.6.9 or earlier (CinemaDNG, ProRes RAW or Blackmagic RAW), from the camera's DNGs.
- New `tests/verify_gyro2.py`: checks that every DNG of a clip has a Gyro2 block the plugin can read (and, given the original folder, that it's unchanged). dng2prr 0.6.10, dng2braw 0.14.

## 0.6.9 (2026-11-04): fix for BRAW playback running out of memory

- **Fix:** in 0.6.8 the Blackmagic RAW playback helper's GPU memory grew by about 1.5 GB per second of full-resolution 6K playback (60 GB after half a minute). The Mac then swapped and frames were dropped again. The SDK asks for new large buffers for every frame and hands them back, but the Metal buffers it handed back were never actually freed. The helper now recycles them, and its memory stays at about 1.4 GB (2.6 GB in all).
- Measured on an M1 Max, Cinema Camera 6K clip (6048×4032, 24 fps) from the Samsung T9 at Full resolution with a monitoring LUT: 962 of 962 frames on time over 40 s of playback, and 566 of 566 with the histogram open. Memory stayed flat in the helper and in the app.

## 0.6.8 (2026-11-04): smooth Blackmagic RAW playback at full resolution

- **BRAW playback on the GPU:** the viewer's Blackmagic RAW helper now uses the SDK's Metal pipeline, like DaVinci Resolve and Blackmagic RAW Player. Full-resolution 6K now decodes at about 100 fps including the copy out of GPU memory (about 220 fps for the decode alone), against 24–28 fps on the CPU, which wasn't enough for 24 fps clips. The result matches the CPU decode to within 1 of 255.
- **Faster hand-off to the viewer:** frames come through shared memory instead of the pipe, which topped out at about 36 fps for 6K, and go into IOSurfaces that the viewer shows directly. That removes a 77–98 MB copy and colour conversion on the main thread for every frame. The monitoring LUT is applied on the GPU, surface to surface.
- Measured on an M1 Max at Full resolution with a monitoring LUT: Pocket 6K Pro and Cinema Camera 6K clips (6048×3200 and 6048×4032 at 23.976/24 fps, the 6K FF clip played straight from a Samsung T9) now show every frame on time. 0.6.7 showed 10 of the first 78 on time and then stalled. Video Assist clips, scrubbing while playing, DNG clips and the Linear decode all checked.

## 0.6.7 (2026-11-04): GPU for every output

- **GPU processing for ProRes RAW and CinemaDNG**, as Blackmagic RAW already had. The Output panel's **Processing → Use the GPU (Metal)** now shows for all three formats (on by default).
  - The GPU does the transform stages: the ProRes RAW curve, DCT and quantisation; the lossy CinemaDNG DCT (once per frame), quantisation, and the symbol statistics the rate control uses to hit 3:1–7:1; the lossless CinemaDNG predictor. Entropy coding stays on every CPU core.
  - M1 Max, 129 UHD frames: CinemaDNG 7:1 17 → 67 fps, VBR-HQ 42 → 85 fps, lossless 35 → 103 fps; ProRes RAW about 35 → 54 fps.
  - Lossless stays bit-exact (verified). Lossy CinemaDNG hits the same ratios (7:1 mode: 6.79:1 against 6.81:1) with the same error. GPU ProRes RAW differs from the CPU path by DCT rounding only (mean 0.009 of an 8-bit level).
- Faster CPU paths too, with byte-identical output: a 64-bit bit writer for the JPEG encoders, a single-pass Huffman coder, a faster ProRes RAW bit writer. CPU-only (`--cpu`) is now 1.3–2.7× faster for CinemaDNG than 0.6.6.
- `dng2prr --cpu` (or `DNG2PRR_CPU=1`) turns the GPU off.

## 0.6.6 (2026-11-02): highlights, Blackmagic Film Gen 5, linear LUTs

- **Viewer Decode menu:** Rec.709, **Blackmagic Film Gen 5** (log, Blackmagic Wide Gamut, from Blackmagic's published formulas), and **Linear** with Rec.709 or Rec.2020 primaries. Works for DNG and BRAW clips.
  - Linear LUTs now get scene-linear input: the frame comes from the decoder as Gen 5 log (so 8 bits hold the whole range) and is turned back into linear in front of the LUT. Before, a "Linear to Rec.709" LUT was applied on top of the finished, already-clipped picture.
  - Checked: the Film decode of an fp DNG matches Blackmagic's own decode of the same clip as BRAW (same colour, exposure matched to 0.34 stop by design).
- **No more cut-off highlights in Rec.709:** the highlight roll-off now reaches white exactly at the sensor's clip point (it used to cut off a stop below it and squeeze the top 2–3 stops into the last few percent). Where one colour channel clips first, highlights go to white instead of magenta.
- **Blackmagic RAW opens flat in Resolve** by default (Blackmagic Film Gen 5 / Blackmagic Wide Gamut). The Output panel's **Opens as** switches back to Rec.709 (dng2braw 0.13, `--viewing`).

## 0.6.5 (2026-11-02): Job Queue editing, Add to Queue fix

- **Fix:** the Output panel's **Add to Queue** (and **Render** with an empty queue) now does exactly what Media Storage's button does: the selected clips, or every clip in the selected folders, with the current Output settings. Before, it only worked for clips and did nothing for a selected folder.
- **Job Queue:** select several jobs (⌘-click, ⇧-click), drag to reorder, remove them with Delete or the right-click menu (which also has Move Up / Down / to Top / to Bottom).
- **Viewer:** a "Frame … couldn't be read" message goes away when playback starts, and the frame is tried again. The DNG playback helper also retries a read that fails, as a busy RAID or USB drive sometimes does.

## 0.6.4 (2026-11-02): SlimRAW lossy DNGs, viewer zoom, spot-checking

- **Fix: lossy DNGs from SlimRAW (e.g. VBR-HQ)** came out as coloured stripes in the viewer, and in the BRAW, ProRes RAW and CinemaDNG outputs. Their JPEG tiles carry each of the two components in its own scan, and the DNG reader only handled one scan with both. It now reads either layout. Checked on two SlimRAW VBR-HQ UHD clips (viewer, Blackmagic RAW through Blackmagic's SDK, ProRes RAW through macOS), and the app's own lossy DNGs still decode the same. **Clips like these converted with an earlier version need converting again.**
- The Metadata panel says whether JPEG-compressed DNGs are lossless or lossy.
- **Viewer zoom** as in Resolve: scroll or pinch to zoom around the pointer, drag to pan, Z to fit, and a zoom menu (Fit, 25–400 %).
- **Spot-checking:** clicking or dragging on the scrubber during playback carries on playing from there.
- **De-squeeze** is one setting kept from clip to clip, like the LUT (it was per clip).
- **Icon** text is now "cDNG" over "RAW CONVERT".

## 0.6.3 (2026-11-02): one page, no media pool; new icon

- **No media pool:** browse in **Media Storage**, click a clip to view it, and **Add to Queue** (or double-click). A selected folder, card or drive adds every clip inside it. Each clip row shows its mode, rate, length and gyro.
- **One page:** Media Storage above the **Job Queue** on the left, the viewer above the **Log** in the centre, and Output / Metadata on the right. The Status page, the page bar and the toolbar's panel buttons are gone. ⌘I ("Add Clips to Queue…") and dropping clips on the window add them to the queue.
- **Nothing is cut off when the window is resized:** the side panels shrink in proportion to keep the viewer at least 420 points wide, the window stops at a size where everything fits, and the viewer's header uses shorter labels when it's narrow.
- **New icon:** the colour-wheel iris over "DNG" and "CONVERT" (option 7, simplified).

## 0.6.2 (2026-11-01): Blackmagic RAW frame-rate fix, faster playback, floating histogram, Status page

- **Fix: Blackmagic RAW from whole-number frame rates.** Clips at 24, 25, 30, 48, 50, 60 or 100 fps (stored by the fp as e.g. 48000/1000) became `.braw` files that only showed their first frame, in the viewer and in Blackmagic's software. The bundled `dng2braw` 0.12 writes the rate the way Blackmagic cameras do. **Render such clips to BRAW again**; 23.976 and 29.97 fps files were fine. The viewer now says what's wrong with a `.braw` it can't decode instead of waiting.
- **Smooth DNG playback at the clip's own rate:**
  - DNG frames are now read and developed by the new `dng_view` helper (the fpSup DNG reader, on every core) instead of macOS's RAW engine, which managed only about 25–35 fps on fp frames. Compressed (lossless and lossy) DNGs play too.
  - Playback follows the display's refresh and only decodes the frames that will be shown. Frames go straight to the screen.
  - `.mov` (ProRes RAW) is decoded at the playback resolution, and its LUT runs off the main thread.
  - On an M1 Max, from the external SSD with the fp's 48 fps OG3K clips: every frame on time for uncompressed DNG at full, half and quarter resolution, lossy 7:1 DNG at half, Blackmagic RAW at half, and ProRes RAW at full and half (5–10 second tests).
- **Histogram** is a floating, resizable palette, remembered where you put it.
- **Status page** (was Deliver): its toolbar has **Queue** and **Log** buttons to show or hide each pane, and the border between them can be dragged.
- **Space bar** plays and pauses from anywhere unless you're typing in a text field (the search). Nothing starts in the search field, and clicking anywhere else (including the transport buttons) leaves it.
- A compressed CinemaDNG folder (e.g. `_lossy7-1`) can be added to the media pool by picking it directly. It's still skipped when a parent folder is scanned.

## 0.6.1 (2026-11-01): resizable workspace, Media Storage, metadata, BRAW/MOV playback, histogram, de-squeeze

- **New app icon:** a stack of DNG frames turning into one clip, in the app's dark-and-orange theme.
- **Smooth playback:**
  - The viewer buffers before it starts the clock, decodes further ahead on more threads, and has a **Full / Half / Quarter** resolution menu.
  - On an M1 Max, every frame was shown on time in 5-second tests: DNG at half and at quarter resolution, and Blackmagic RAW at half.
- **Resizable panels:** every panel border can be dragged (media storage/pool, pool bins, viewer/queue, right panel). Sizes are remembered.
- **Media Storage pane:** volumes and cards (refreshed when mounted), and home folders. Clip folders are marked. Add folders, cards, drives or files by double-click, drag, or **Add to Media Pool**.
- **Playback of `.braw` and `.mov` files** in the viewer, with sound and timecode:
  - `.braw` via the new `braw_view` helper. It uses Blackmagic's SDK, loaded at run time from Blackmagic's own installs, and decodes at 160–220 fps at half/quarter resolution.
  - `.mov` (ProRes RAW, ProRes, H.264) via AVFoundation.
  - These files can be viewed but aren't converted.
- **Metadata panel** (right panel tab, or **Metadata** in the toolbar): the selected clip's full details, grouped.
- **Histogram:** a toggleable luma + R/G/B scope in the viewer.
- **Anamorphic de-squeeze:**
  - Per-clip factor in the viewer (1.25–2.0x, including 1.6x), shown de-squeezed.
  - Written into Blackmagic RAW (`pasp` + Blackmagic's `anamorphic_enable`/`anamorphic` keys) and ProRes RAW (`pasp`). Resolve reads them as the clip's PAR.
  - New `--desqueeze F` in both encoders.
- **Output formats** reordered and renamed: 1. CinemaDNG Compress (image sequence), 2. Blackmagic RAW (.braw file), 3. Apple ProRes RAW (.mov file).
- **CinemaDNG folder suffixes** are now `_lossless`, `_lossless-10log`, `_vbr-hq`, `_vbr-lt`, `_lossy3-1`, `_lossy4-1`, `_lossy5-1`, `_lossy7-1`. The old upper-case ones are still recognised.
- `dng2braw --probe` reports more: shutter, aperture, focal length, levels, neutral, crop, gyro IMU rate and readout.

## 0.6 (2026-11-01): new workspace (media pool, viewer with LUTs, output sidebar)

- **The app is redesigned** after UI mockup "Option A": a dark workspace in the style of a colour-grading suite.
  - **Media Pool:** imported folders and cards as bins; smart bins (Has gyro, Open gate, BRAW-ready); thumbnails with GYRO/WAV badges and length, or a list view with each clip's metadata; search; multiple selection. Imported folders are remembered.
  - **Viewer:** real-time playback of the DNG frames (Core Image RAW decode, half resolution, several frames at a time) with the clip's WAV and timecode; frame stepping; scrubber.
  - **Monitoring LUTs** (viewer only): your own `.cube` files (3D, or 1D expanded; cubes over 64 points resampled), and a built-in False Color exposure check.
  - **Output sidebar:** format cards and folding sections. It shows the BRAW size estimate and the kelvin/tint that will be written, and warns about clips BRAW can't take yet.
  - **Job queue:** each job keeps its own settings; progress, speed and result per job; a Deliver page with the full log.
- **Audio and gyro switches** for ProRes RAW and BRAW output. New `dng2prr --no-audio`.
- New **`dng2braw --probe CLIP`**: one JSON line describing a clip from its first frame (size, mode, rate, ISO, white balance, timecode, WAV, gyro, BRAW support), for the media pool.

## 0.5 (2026-10-31): Blackmagic RAW: open gate OG3K and gyro data

- **Blackmagic RAW output** (bundled `dng2braw` 0.11):
  - **Open gate OG3K** clips are converted (3008×2000). They were skipped in 0.4.
  - **fpSup Gyro2 data goes into the `.braw`** as Blackmagic motion data, which Gyroflow and its OpenFX plugin read, plus a Gyroflow Protobuf track. The `.braw` also states the fp's sensor readout time, for rolling-shutter correction.
  - The skipped-clip message names the supported modes.
- `dng2prr` is unchanged apart from its version number (0.5). Its `fsg2.c` gains a function that dng2braw uses.

## 0.4 (2026-10-31): Blackmagic RAW as the third output

- **New output: Blackmagic RAW**, one `.braw` per clip, made by the bundled `dng2braw` encoder (0.10, from the fpSup dng2braw project; sources in `source/braw/`).
  - Compression: 3:1, 5:1 (default), 8:1 or 12:1 constant bitrate, or Q0, Q1, Q3 or Q5 constant quality.
  - Encodes on the GPU (Metal) by default; the app has a switch to use the CPU only.
  - Carries the clip's audio (24-bit PCM, as a Video Assist writes it), timecode and metadata. The as-shot white balance becomes Blackmagic kelvin and tint, and the app's grey-card white balance applies too.
  - fp UHD clips only for now. Other modes are reported as "not supported yet" and counted as skipped, not failed.
  - Gyroflow sidecars are copied next to the `.braw`, as for `.mov` files.
- The build script now also builds `dng2braw` (universal, tuned for Apple silicon, with Metal) and bundles it in `Contents/MacOS`.
- `dng2prr` itself is unchanged apart from its version number (0.4).

## 0.3 (2026-10-25): cDNG Raw Convert: native macOS app, tuned against a real fp → Atomos Ninja V recording

(Test builds of this release were labelled 0.2.)

**New name:** the project is now **cDNG Raw Convert** (previously "fp DNG to ProRes RAW" / dng2prr; the app was briefly "DNG Raw Converter", then "DNG Raw Convert"). The "c" (for CinemaDNG) sets it apart from DNG converters for still photos. The encoder and its files keep the name `dng2prr`.


**More carried into ProRes RAW** (native encoder):
- **Gyroflow telemetry track** from fpSup Gyro2 data: each DNG's gyro block is converted into Gyroflow Protobuf (as the Gyroflow (fpSup) plugin does for DNG sequences) and stored as a `gyrf` track, which Gyroflow reads from a .mov directly. `--gyro-jsonl` also writes a `.jsonl` copy; `--no-gyro` turns it off. New `source/fsg2.c`, `tests/test_fsg2.c`, `tests/check_gyro.py`.
- **Timecode track** from the DNG TimeCodes tag. Resolve shows the same Start TC as the DNGs.
- **Every frame's DNG tags** in a timed-metadata track, including the MakerNote and any fpSup telemetry. Retrieve them with `--extract-meta`.
- **SPP_metadata.xmp** stored as movie metadata.
- **Gyroflow sidecars:** the app copies them next to the .mov, renamed to match it.
- **`--no-extra`** leaves these out. `compare_native.py` uses it for the byte-identity check against the Python version.

**Compressed CinemaDNG output** (app and `dng2prr --cdng`):
- Eight modes named after slimRAW's: Lossless, Lossless 10-bit log, Lossy VBR HQ, Lossy VBR LT, and Lossy 3:1, 4:1, 5:1 and 7:1.
- Every DNG tag is kept, and non-DNG files can be copied along.
- An optional verification pass decodes every tile again; lossless output is bit-exact.
- New `source/dng_jpeg.c`: a lossless-JPEG encoder, plus a 12-bit DCT JPEG encoder and decoder.
- Lossy DNGs can now be read as input too, and the fp's own lossless mode is confirmed on a real clip (mixed uncompressed and 512×512-tile lossless frames).
- Checked in Resolve 21.1.1: all modes open, lossless matches the original, and lossy keeps the average brightness within 0.7%.

**Native macOS app** (`build/macos-app`), **cDNG Raw Convert**:
- CinemaDNG output can also go next to each clip, as `<clip>_<MODE>` (for example `B001_013_VBRHQ`).
- One window that stays open, in the style of slimRAW: source and destination, quality, white balance, options, Start/Stop, progress bars and a log.
- No Python needed. It's built with Apple's command-line tools.

**Native encoder** (`source/dng2prr_cli.c`):
- A C port of `dng2prr.py` with multithreaded encoding.
- It writes byte-identical files to the Python version: checked on 12- and 14-bit clips, with audio, white-balance override, gain and fps options (`tests/compare_native.py`).
- It's about 2–3× faster, and builds on macOS and Linux.

The file sizes at each quality setting are almost unchanged. The bytes are different, but they render identically in Resolve: as-shot stills from 0.1.1 and 0.3 conversions of the same frame match, apart from compression noise.

**Levels like the Ninja V:**
- 12-bit sensor data is now scaled ×16 (sensel range 61424, exactly what a Ninja V writes for the fp), and 14-bit ×4.
- 0.1.x used ×4 and ×1 to keep footroom the fp never uses.
- The result is finer shadow precision. Shadow error is about halved at the same file size.

**Quality ladder:**
- The tile quantisers are now 4, 8, 15 and 30 (were 2, 4, 8 and 16), keeping sizes the same.
- "Standard" uses the Ninja V's "ProRes RAW HQ" quantiser (15).
- Near-lossless, High and Standard are labelled `aprh` (Apple ProRes RAW HQ), as a Ninja file is.

**Metadata like the Ninja V:**
- Manufacturer and model are now "Sigma" / "SIGMA fp".
- Shutter speed and angle are added, and the lens model is written under the same keys.
- The **per-temperature white-balance and colour-matrix tables** (`com.apple.proresraw.whitebalance.bycct.*`) are now written.
  - They're built from the fp's own DNG calibration, and shifted by the clip's tint so the as-shot picture is unchanged. Resolve does read these tables (FINDINGS.md finding 10).
  - They should make white-balance changes in Resolve follow the fp's calibration.

**Lossless-compressed DNGs:**
- New `source/dng_lj92.c`: a lossless-JPEG decoder for tiled or stripped DNGs (Compression 7), plus LinearizationTable support.
- This is a first step towards fpSup's lossless mode. It passes 63 synthetic round-trip tests (`tests/test_lj92.py`) but **hasn't been tested on real fpSup lossless clips yet.**
- Lossy JPEG DNGs are still rejected.

**Tests:**
- New reference hashes.
- The Resolve wedge validation was re-run: all four levels pass, with the same numbers as 0.1.1.

## 0.1.1 (2026-10-24): porting kit

The encoded output is identical to 0.1: every frame tested is byte-for-byte the same.

**Cross-platform encoder:**
- **C core loading:** finds `libprr.dll` on Windows, and accepts a `PRR_LIB` override.
- **Multiprocessing:** works with "spawn" (Windows), not only "fork". Includes `freeze_support()` for a packaged .exe.
- **File discovery:** case-insensitive, so DNGs aren't listed twice on Windows.
- **`prr_core.c`:** builds with MSVC (explicit exports, portable bit counting, `M_PI`).

**Cleanup:**
- Removed the superseded calibration experiments from `encode_frame`.
- The tile quantiser rule is now a named constant (`TILE_Q_MULT`).

**New files:**
- **`source/dng2prr_gui.py`:** a cross-platform window for Windows and Linux.
- **`build/linux/build.sh`, `build/windows/build.bat` and `build_exe.bat`:** build scripts. The macOS build script now looks in `../../source`, turns off FMA contraction, and runs the self-test.
- **`tests/selftest.py` + `reference.json`:** byte-level and structural checks.
- **`tests/validation/`:**
  - wedge clips, a Resolve still-export script and a measuring script
  - reference results from Resolve 21.1.1 on macOS (all four quality levels pass).
- **`PORTING.md`:** a handoff guide.

## 0.1 (2026-10-24)

- **Fixed a magenta tint at High, Standard and Compact quality.** Apple's decoder treats tile quantiser 1 as 2. The encoder now always writes quantiser 2 or more with FFmpeg's standard code-value mapping.
- **Near-lossless files from earlier builds were not affected.**

## 0.1-pre (2026-10-23)

- First community build: macOS drag-and-drop app, command-line tool, findings.
