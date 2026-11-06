# BRAW findings (dng2braw 0.1)

What two real camera files show about the Blackmagic RAW layout. Everything here comes from the
files themselves, read with `tools/braw_explore.py`. Nothing comes from Blackmagic documentation
or code. "Confirmed" means it holds in both files and has an obvious meaning. Anything else is
a hypothesis to test against the Tier A clips (Video Assist 12G + fp) and the Tier D camera clips.

Samples:

- **A:** `A001_09262057_C001.braw`: Blackmagic Cinema Camera 6K, firmware 10.2.2, Q0, 6064×4048, 24 fps, 293 frames.
- **B:** `A001_01101733_C031.braw`: Blackmagic PYXIS 6K, firmware 9.6.2, Q0, 6064×3424, 24 fps.


## Status summary (31 Oct 2026, dng2braw 0.2)

Sections 1–6 come from camera files; 7–16 from Video Assist 12G + SIGMA fp recordings, which are the target format. Where they differ, 7–16 win.

| Area | Status | Where |
|---|---|---|
| Container, clip metadata, timecode | **Done**: dng2braw writes it like the Video Assist | 1, 2, 7 |
| Per-frame `bmdf` metadata (256 bytes) | **Done** | 7 |
| Frame header, slice table, 5 strips × 72-line slices, column-major | **Done, decoder-verified** (splice test; some words copied, meaning unknown; table at 0x1080 added) | 7, 10, 20 |
| Values: BRAW decode = DNG raw × 0.00203, no offset | **Confirmed** | 8 |
| DNG-derived planes through the SDK's own processing reproduce the Video Assist image (±1–2 %) | **Confirmed** | 23 |
| Forward transform: T(v)=55.94√v+71.2; Y = 0.29T(R)+0.57T(G)+0.29T(B)−12.7 (smooth, full res); Cb,Cr ≈ 0.57·(T(B/R)−T(G)) (half res) | **Fitted**, rms ≈ 2–3 code values | 22 |
| Colour: Blackmagic's fp colour = DNG calibration (0.8 ΔE) | **Confirmed** | 9 |
| ISO is applied in the camera; white balance is metadata only | **Confirmed** (ISO gain detail open) | 10 |
| Slice internals: Y + 4:2:0 colour difference, log domain, 8×8 DCT | **Model**, well supported (quantiser-scale reading from section 16 retracted) | 12–14 |
| Slice = 8-line block rows coded in turn; per row [Y Y Cb Cr 4b 4b] × 48 | **Confirmed** (corrects 'two passes') | 24 |
| Decoder oracle: SDK decode with two different preceding frames; invalid slices show the previous frame | **Done** (`braw_crop`, `braw_oracle.py`) | 19 |
| Exact oracle: SDK manual decoder, in-memory edits, sentinel, decoded Y (u16) + Cb/Cr (i16, 4:2:0) | **Done** (`braw_lab`) | 21 |
| All-zero bits are not a valid empty code; slices can be copied to other positions | **Confirmed** | 19 |
| Two streams (Y, colour) interleaved in 32-bit units | **Supported**; scheduling rule unknown | 15 |
| Entropy codes (DC, AC, end of block, signs) | **Found: JPEG Annex K luminance tables; DC cat 6/7 = 11100/11101, cat 9–11 = 12-bit codes; AC 0/9, 0/A at 12 bits and 1/5, 2/4, 3/3, 5/2, 7/2 moved; zigzag (luma), 8×4 scan (chroma); DC DPCM from 0 (Y 2048) per slice; EOB 1010** | 24–29 |
| Quantiser (5:1) and decoder filter | **Found: flat, luma 99/8, chroma 99/16; 3-tap luma block-edge filter (3,10,3)/16, inverted exactly in the encoder** | 30 |
| Encoder prototype: whole frames the SDK decodes | **Real DNG chart scene: 155/155 slices, Y rms 2.79, Cb/Cr 1.56** (quantisation-limited); rate control (4-bit fields) open | 28–30 |
| Per-pair fields; header table; rate control | **Found: edge-filter strengths (2 bits per edge); 0x1080 table = quantisation matrix (luma ×3/8, chroma ×3/16); dng2braw 0.4 rate control hits 3:1–12:1** | 32 |
| Calibration and app | **Raw gains fitted on 4 paired takes (−4…+2 EV): our files render within ≈1 % of the Video Assist; cDNG to BRAW 0.1 app** | 37–38 |
| **dng2braw writes real BRAW (C)** | **Done for fp UHD at the 5:1 quantiser: bit-exact with the prototype; SDK and Resolve decode it; matches VA_0007 within 1–2 % through the same processing** | 31 |

Approach rule: we use Blackmagic's decoders (Resolve, SDK) only as black boxes on our own recordings. No disassembly or decompilation of Blackmagic binaries, and no code or tables taken from them (SDK licence §4.3).

## 1. Container: QuickTime (confirmed)

- The file is `wide` + `mdat` (32-bit size) + `moov` at the end.
- Tracks, in order:
  - video, codec `brvl`
  - timecode `tmcd`
  - audio: A has `lpcm` (3 channels, 16-bit); B has `in24` (2 channels)
  - two timed-metadata tracks (`mebx`) at about 1140 Hz, with 20-byte samples, keyed `com.blackmagicdesign.motiondata.gyroscope` and (by the second stsd's length) presumably the accelerometer. That's IMU data Gyroflow could use.
- The timecode sample (4 bytes) sits at file offset 4096, and frame 0 at 4100.
- Video timescale = fps, one sample per chunk, `co64` chunk offsets.
- The `brvl` sample entry is 134 bytes: a standard 86-byte visual entry with an empty compressor name and uninitialised bytes in the reserved fields. Then four extensions, identical in both files: `bfdn`=1, `ctrn`=1, `vsrc`=0, `bver`=1.

## 2. Clip metadata: `moov/meta` keys + `ilst` (confirmed)

- About 55 keys, named in plain words. For example:
  - `camera_type`, `firmware_version`
  - `crop_origin` / `crop_size` / `sensor_area_captured` (type 71: two float32)
  - `braw_compression_ratio` ("Q0"), `braw_codec_bitrate`
  - `viewing_gamma` ("Blackmagic Design Film"), `viewing_gamut`
  - `sensor_photosite_pitch_in_micrometres`
  - `lens_shading_enable`, `analog_gain`, `anamorphic_enable`
  - slate fields (`reel_name`, `scene`, `take`, `good_take`…)
- The camera's embedded 3D LUT is stored as `post_3dlut_embedded_data`: 431 KB, a 33³ LUT.
- There is **no colour matrix or sensor profile key**: colour comes from the SDK's built-in profile for `camera_type`. That is why the fp profile has to come from Video Assist clips.

## 3. Per-frame metadata: a 1024-byte `bmdf` box at the start of every frame (confirmed)

It contains these boxes, in this order:

| Box | Value in A | Meaning |
|---|---|---|
| `srte` | 24, 1 | frame rate |
| `shtv` | "1/60" | shutter, as text |
| `innd` | 0 | unknown |
| `agpf` | 1.0 | analog gain |
| `asct` | 0 | unknown |
| `asti` | 0 | unknown |
| `shpo`, `shdi` | 112 bytes each, all zero | unknown |
| `dpou`, `dpog`, `dpor`, `dpob` | zero | unknown |
| `aptr` | zero | probably aperture |
| `expo` | zero | probably exposure |
| `fcln` | zero | probably focal length |
| `dsnc` | zero | probably focus distance |
| `isoe` | 400 | ISO |
| `wkel` | 5600 | white balance, kelvin |
| `wtin` | 10 | tint |
| `skip` | | padding to 1024 bytes |

## 4. Frame header (image payload, right after `bmdf`)

| Offset | A | B | Status |
|---|---|---|---|
| 0x00 | `braw` | `braw` | magic (confirmed) |
| 0x04 | u32 | u32 | payload size = sample size − 1024 (confirmed) |
| 0x08 | 05 00 | 05 00 | constant; version? |
| 0x0a | 0x0320 | 0x041f | **unknown**; changes with the camera, not the frame |
| 0x0c | 6064, 4048 | 6064, 3424 | width, height (confirmed) |
| 0x10 | u16 128 | u16 112 | slice bands per strip? `ceil(h/32)` rounded up to 16 fits both |
| 0x12 | 08 04 | 08 04 | constant |
| 0x14 | 16, 16, 16 | same | constant |
| 0x1a | 270, 489, 439 | same | constant; unknown |
| 0x20 | 0x0500, 0x0100 | same | constant |
| 0x40 | 2016, 2016, 2032 | 1520, 1504, 1520, 1520 | strip widths, summing to the width (confirmed) |
| 0x80 | u32 table | u32 table | slice offsets from payload start; first = 4608 in both (confirmed) |

Frames are always a multiple of 512 bytes.

## 5. Slices

- Every slice starts with `FE D1`. The rest is entropy-coded, and **this is the unknown part**.
- A: 384 slices (3 strips × 128), average 22 KB.
- B: 496 by the monotone-offset count, which doesn't match 4 × 112. Re-check with more frames.

## 6. Slice order: column-major (likely)

Neighbouring slice sizes correlate most strongly at lag 1 (0.89 in A, 0.96 in B). That means consecutive slices run down a strip. dng2braw writes them in that order.

## Open questions, and the test clips that answer them

| Question | How to answer it |
|---|---|
| The slice bitstream | Tier A A1/A2/A3/A5/A6: start from black and flat frames, where a slice is mostly headers |
| Field 0x0a, the 0x1a constants, the slice band height | Tier A A13 (all fp HDMI modes) and Tier D D3 |
| How the Video Assist fills `camera_type` and the colour keys for the fp | any Tier A clip |
| What `wtin` means against DNG tint | Tier A A11 |
| The `bmdf` lens boxes, and the gyro `mebx` sample format | Tier D D1/D2 |

---

# Video Assist 12G + SIGMA fp: minimum-set results (29 Oct 2026)

Ten Video Assist BRAW clips and four paired fp internal DNG clips (BRAW_Test_Matrix_v5, MVP rows), plus the
flicker test. Recorded on a Video Assist 5" 12G HDR (firmware 3.19) from the fp over HDMI: UHD, 23.976, Q0 unless noted.
The lens was the 85mm f/1.4 EX DG HSM on an MC-21, under a 1K redhead. Settings in every file matched the tracker.
Analysis: `tools/braw_explore.py`, raw DNG measurements with the dng2braw front end, and Resolve 21.1.1 renders in
DaVinci Wide Gamut / Linear (project 2026-10-29_SigmaFP_BlackmagicVA_tests).

## 7. The Video Assist writes a different BRAW variant from the cameras (confirmed)

What the converter must copy for fp files:

| | Camera files (6K, PYXIS) | Video Assist + fp |
|---|---|---|
| `brvl` sample entry | 134 bytes: `bfdn`=1, `ctrn`=1, `vsrc`=0, `bver`=1 | 122 bytes: `bver`=1, `ctrn`=1, `bfdn`=0x3e9; no `vsrc` |
| Per-frame `bmdf` box | 1024 bytes | **256 bytes** |
| Frame header 0x08 | 05 00 | **02 00**; 0x09 = **08 at 5:1** (quality code?) |
| Frame header 0x0a | 0x0320 / 0x041f | **0x051f** |
| Coded size (0x0c) | = image size | **3856 × 2176** (the fp's full HDMI raw); the container says 3840×2160 with `crop_origin` 8,8 |
| Strips (0x40) | 3 or 4 | **5: 768, 768, 768, 768, 784** |
| 0x10 | 128 / 112 | **72 = slice height in lines**. This fits all three: 6K = 12 columns × 32 bands of 128 lines, PYXIS = 16 × 31 of 112, VA = 5 × 31 of 72 (last band 16 lines) |
| Slices | 384 / 496 | **155 = 5 strips × 31 bands, column-major** (confirmed from the slice-size map of the scene) |
| First slice offset | 4608 | 4608 |
| Slice start bytes | always FE D1 | first byte **0xEC–0xFF** (top 3 bits 1s): slice header fields, not a marker (section 12) |
| Frame size | multiple of 512 | multiple of 256 |
| Gyro (`mebx`) tracks | yes | **none** |

This corrects findings 4–6 and dng2braw 0.1's layout guesses: 32-line bands, strips of up to 2048, a fixed FE D1 marker. dng2braw 0.2 writes this layout; `tests/test_layout.py` passes all 39 checks against Video Assist_0007.braw.

**Clip metadata keys:**
- `camera_type` "SIGMA fp L" (the fp itself: `sensor_area_captured` is 6000×3376), `manufacturer` "SIGMA"
- `firmware_version` "5000002", `camera_id`
- `encoder_device_*`: name, serial, version
- `viewing_gamma` / `viewing_gamut` "Rec.709"
- `analog_gain`: 1.26 at ISO 800, 5.04 at ISO 3200
- `sensor_line_time` 0
- slate fields
- no LUT

**Per-frame `bmdf` (256 bytes):**

| Box | Content |
|---|---|
| `srte` | 24000/1001 |
| `shtv` | **shutter angle as text** ("287.7°", "143.9°") |
| `innd` | 0 |
| `agpf` | analog gain (float) |
| `asct` / `asti` | as-shot kelvin / tint |
| `aptr` | "F2.8" |
| `expo` | 0 |
| `fcln` | "85mm" |
| `dsnc` | **focus distance as text** ("1610mm-1730mm") |
| `isoe` | ISO |
| `wkel` / `wtin` | kelvin / tint |
| `skip` | padding |

## 8. Values: BRAW carries the same linear sensor data as the internal DNG (confirmed)

Paired takes −4, −2, 0 and +2 EV, measured on the six ColorChecker greys:
- **BRAW (Resolve, linear) = DNG (G photosites, above black) × 0.00203.** The ratio is constant to within 0.04–0.36% across patches and exposures, with **no offset**. Resolve's BRAW decode is linear, and the HDMI raw has no gain offset against the internal recording.
- **Both clip at the same point:** BRAW clips at the equivalent of 3900 DN, against the DNG's 3839 (0.02 stops apart).
- **Step sizes agree:** BRAW +2.03, +2.02, +1.66 stops; DNG raw +2.03, +2.03, +1.65. The last step is short because f/1.4 transmits less than nominal.
- **Resolve's own DNG decode is not linear** in a linear timeline: its steps are +1.71 and +1.77, and it clips at 1.0 (Neutral 5 at 0 EV is already about 1.0). Compare against raw DNG values, not Resolve DNG renders. This may also explain dng2prr's 0.26 EV question.
- **Exposure:** Neutral 5 at 0 EV = 480 DN above black, 3.0 stops under clipping, as planned.

## 9. Colour: Blackmagic's fp colour is the DNG's own calibration, within 0.8 ΔE2000 (confirmed)

ColorChecker Classic at 0 EV (and −2 EV, which agrees). Veiling flare is about 2.4% of white and is removed using the
grey row. Compared with the published post-2014 values, Bradford-adapted to D65:
- **BRAW, Blackmagic decode in Resolve:** ΔE2000 mean 7.4 (colours 9.2, greys 2.0); worst are yellow green and yellow, about 15.
- **DNG through its own ColorMatrix (dng2prr's maths):** ΔE2000 mean 7.3 (colours 9.0, greys 2.2).
- **BRAW against DNG, patch by patch:** mean **0.8**, max 1.7 (white).

Blackmagic's profile for "SIGMA fp L" reproduces the DNG calibration almost exactly, so the converter's generic DNG colour
already matches Resolve's BRAW colour for the fp. The about 7 ΔE against the reference is common to both. It comes from
the tungsten light and a plain matrix rendering, and should not be compared with the earlier ΔE 4.5, which was measured
another way.

## 10. ISO and white balance

- **White balance is metadata only (confirmed).** 5600 K on the fp recorded as 5337 K, tint 18, in `wkel`/`wtin`/`asct`/`asti`. The frame header is identical and slice sizes correlate at r = 0.9998 with the 3200 K clip. Resolve applies it on decode.
- **ISO is applied in the camera (confirmed), with header parameters that follow it.** At the same light, ISO 3200 frames are 46% larger than ISO 800 (more noise in the data). The frame-header words at 0x1c/0x1e change from 266/280 to 461/420 (camera files at Q0: 489/439), and `agpf` goes 1.26 → 5.04. Words 0x14–0x1f also change at 5:1 (16, 270, 524 / 778, 505, 323), so they are quantiser or gain parameters.
- **Open:** Resolve renders the ISO 3200 clip 0.65 stops brighter than a nominal 2-stop gain predicts. A paired internal DNG at ISO 3200 (and 100, 6400) will show the fp's real gain at its dual-base switch.

## 11. Compression and data rates (Video Assist, UHD 23.976)

| Clip | Q0 | 5:1 |
|---|---|---|
| Lens cap | 6.57 MB (black noise is expensive at Q0) | 2.54 MB (≈ 12.6 MB uncompressed / 5) |
| Flat field | 1.25 MB | 1.25 MB (already under the 5:1 target) |
| Reference scene | 3.2–4.0 MB (about 3.4:1) | — |

Lens-cap slices are about 42 KB at Q0 against about 8 KB for the flat field, which points to a quantiser that coarsens with
signal level (log-like).

## Flicker test (29 Oct)

The redhead at 1/60 is clean in both formats. Row-by-row variation over time was 0.05% in the DNG and 0.03% in the BRAW,
both at the noise floor. The 1/500 control showed 0.76% and 0.42%.

## 12. Inside a slice: first results from bit-flip experiments (29 Oct)

Method: `tools/braw_mutate.py` and `tools/braw_mutate_seq.py` copy one frame of a recording, flip chosen bits,
and write the variants into one small .braw (frame 0 = original). Resolve decodes it with Blackmagic's own
decoder and renders linear EXR, and the damaged pixels are mapped against the original. All on the flat field
(Video Assist_0003, Q0), slice 40 = strip 1, band 9.

- **Slices are written in 64-bit words.** Every slice is a multiple of 8 bytes. Byte entropy is 7.7–7.95 bits, so the content is fully entropy-coded.
- **Blocks are 8×8 photosites, coded in raster order: 96 across × 9 down per 768×72 slice.** A flip at 25% of the bytes corrupts from block 212 (row 2, column 20); at 75% from block 648 (row 6, column 72); at 50% only one block around 430. On the flat field the bytes spread evenly over the 864 blocks (about 76 bits per block).
- **Variable-length codes, no resynchronisation inside a slice.** Most flips corrupt everything from that block to the end of the slice. Some change one block only (fixed-length suffix or sign bits). Some change nothing.
- **Slice header, about 24 bits.** Bits 0–13 are header fields: flips give large gain and colour changes, and bits 6–11 all decode to the same fallback.
- **Bits 14–23: see section 16 (a quantiser scale, not a base value).** Earlier reading: Flipping them scales the whole slice evenly by ×2.09, ×0.61, ×1.24, ×1.11, ×0.95, ×0.97, ×0.986, ×1.007, ×1.003, ×1.002. That is roughly halving weights in stops, so a ~10-bit value on a log scale. Most likely the first block's base value, which later blocks are coded relative to.
- **The data is log-encoded before compression.** That fits this and the cost of black-level noise (6.3 bits per photosite at the lens cap against 1.2 for mid grey).
- **Block 0's data starts at about bit 24.** Bits 25–35 change block 0 only, probably a fixed-length field. Block 1's data begins around bit 94–96.
- **The last byte of a slice is padding** (no effect).

## 13. The block transform is an 8×8 DCT-like transform (29 Oct, bits 24–279 of slice 40)

Method: `braw_mutate_seq.py` made 256 variants, flipping each bit 24–279 of flat-field slice 40. Resolve rendered them
with the timeline clip zoomed ×4 (pan +2800, tilt −760), so a 960×540 render is a 1:1 window with slice 40 at
pixels (20, 20). Each change was compared with the 64 patterns of an 8×8 DCT.

Results for the 256 bits:

| Effect | Count |
|---|---|
| Local: one 8×8 block | 123 |
| Decoder loses sync from that block on | 129 |
| No visible effect | 4 |

- **Local changes are DCT basis patterns.** Many put 90–98% of their energy into a single 8×8 DCT function: bit 227 → (v3, u0) 98%, 269 → (0, 2) 98%, 44 → (4, 0) 96%, 223 → (4, 0) 96%, 266 → (0, 3) 96%. So each 8×8 photosite block is coded with an 8×8 DCT (or an integer approximation of one).
- **Coefficients go from low to high frequency within a block.** Block 0's bits 25–38 are the lowest frequencies ((0,1), (1,0), (2,0), (0,2)); bits 70–88 reach (3,4), (3,5), (5,2).
- **Block data lengths in this flat slice:** block 0 ≈ bits 25–92, block 1 ≈ 97–200, block 2 ≈ 205–255, block 3 from ≈ 262. That fits the 76-bit average.
- **Changes are neutral.** The colour ratio of the change (R 0.82, G 0.79, B 1.00) is the same for every local bit, so these coefficients act on a luminance-like or full-mosaic signal, not on one CFA colour.
- **A separate colour part.** Bits 171–172 make tiny, strongly blue changes (R 0.24, G 0.11, B 1.0), which looks like a colour-difference component coded after the luminance coefficients. Unconfirmed.
- **Repeated refinement: bit-plane coding?** One coefficient recurs every few bits with falling weight: (1,1) at bits 206, 209, 212, 215 in block 2, and at 262, 264, 268, 270 in block 3; (0,1) at 205 and 207; (1,0) at 208 and 210. That looks like coefficients sent as bit planes (most significant planes first, interleaved across the significant coefficients), not one codeword per coefficient. To test.

## 14. Model from Blackmagic's patent, and what the flips show about it (29 Oct)

**Patent:** Blackmagic Design's application US20180367775A1, "Raw image processing system and method" (inventor
Carsten Buettner). Before compression, the CFA mosaic is turned into:
- a **full-resolution luminance image**, Y = (R + 2G + B)/4, computed directly from the mosaic with a 3×3 (or 9×9) kernel
- **two half-resolution colour-difference images**, (B − G) and (R − G), scaled (k = 2 in the example) and computed with 5×5 kernels

That is a YCbCr 4:2:0 layout, then compressed with a transform codec such as a DCT. The patent gives no details of
block size, quantisation or entropy coding. (This summary is in our own words.)

**Fit with our measurements:**
- **Neutral 8×8 DCT changes = the luminance blocks.** Full resolution, one 8×8 DCT per 8×8 photosites (sections 12–13).
- **Small blue changes in the flat field (bits 171–172) = a colour-difference coefficient.** A flat grey field has almost no colour difference, so its changes are tiny.
- **Log domain:** sections 8 and 12 point to the planes being log-encoded before the transform.

**ColorChecker slice 141 (0 EV clip, strip 4, band 17), bits 24–279:**
- 129 local, 87 desync, 40 no effect.
- **All local bits fall in block 0.** This detailed block takes more than 230 bits, against 76 per block in the flat field, so no colour-difference bits appeared yet.
- **Error concealment:** bits 61, 78, 107, 127, 209, 226 and 234 all give the identical change (dlog ≈ +0.36, footprint 22×22). The decoder hits an invalid code and substitutes the same fallback. Flat-field bits 6–11 behave the same way.
  - **Caution:** equal-sized changes from different bits (the (1,1) "triples" in section 13) may be concealment, not coefficient weights.
  - Flip results must be read as "valid code changed" against "decoder gave up", for example by checking whether the change repeats exactly across bits.

## 15. A slice holds a luminance and a colour-difference bitstream, interleaved in 32-bit units (29 Oct, ColorChecker slice 141)

**Locating (80 flips spread over bytes 4040–14814 of the 26936-byte slice):** the block where damage starts does not
advance steadily through the slice. Instead there are two interleaved sequences:
- **Flips that corrupt to the end of the slice:** damage starts at blocks 24, 50, 63, 82, 98, 118, 138, 154, 178, 215, 250, 269. The damage counts equal 882 minus the start block, which is raster order over the 98 × 9 block grid (16×16 macroblock order would leave 144 more blocks intact; it doesn't).
- **Flips that change only one or two blocks, or only colour:** at the same byte positions these sit about one block-row (≈ 98 blocks) ahead.

**Word map (one flip per 8-byte word, bytes 8000–8511):** each word's flip falls into one of four classes.

| Class | Effect | Count | Where |
|---|---|---|---|
| L | luminance, corrupts to the end of the slice | 16 | block-row 0, columns 72 → 83 |
| l | luminance, exactly one block | 27 | block-row 1, columns 70 → 82 (same columns, one row ahead) |
| C/c | colour-difference only (R−G, B−G change, Y almost unchanged) | 10 | around columns 70–80 of rows 0–1 |
| . | no effect | 11 | — |

Pattern: `l.....ccL.lllmll.cLLll.l..cLLlL..LLLCLLllL.lllCcLLll.ll.mccll.ll`

**Revised after sampling 8 bits per word (bits 0, 9, 18, …, 63 of words 8–23):** bits of the *same* word fall
into different classes. For example, word 14 bytes 0–3 are local luminance (block-row 1) and bytes 4–7 are
colour-only; word 11 switches at byte 3. So it is **not** one stream per 64-bit word.

What holds up:
- **The colour-difference data is a separate bitstream.** Some flips corrupt colour over hundreds of blocks with luminance untouched (for example C at word 14 bits 36/45/63, word 16 bits 36/45). That is only possible if the colour decoder keeps its own read position.
- **The streams interleave at a granularity finer than 64 bits:** 32-bit, byte or bit level, not yet resolved.
- **A luminance desync ("L") damages about 800 blocks:** everything after the current position, in raster order.

**All 64 bits of 4 consecutive words (bytes 8104–8135), classified by per-block energy:**

| Word at byte | Bits 0–31 | Bits 32–63 |
|---|---|---|
| 8104 | luminance | luminance |
| 8112 | luminance | **colour-difference, switching exactly at bit 32** |
| 8120 | colour (a few luminance) | colour |
| 8128 | colour | colour |

Across the 256 bits: 87 no effect, 62 colour desync, 37 colour local, 20 luminance desync, 44 luminance local, 6 mixed.

**The luminance and colour streams interleave in 32-bit units.** Each stream has its own read position and both
kinds of bits: desync (variable-length codes) and local (fixed bits). The few luminance-classified bits inside colour
units are probably colour damage leaking into G through the YCbCr→RGB reconstruction.

**Open: two block-rows at once.** Within the luminance units, desync bits start damage at block (0,72) while local
bits change block (1,73), so the same stretch of data concerns two vertically adjacent blocks. That suggests a coding
unit spanning 16 lines (two block-rows), or two block-row streams. Raster damage counts (about 800 = 882 − 72 − spill)
argue against 16×16 macroblocks, so this is unresolved.

Uncertain:
- "Local" luminance changes are mostly assigned to block-row 1 at the same column as the desync front in row 0, which suggests a second luminance data path about one block-row ahead (separate block-row streams, or a separate fixed-bit stream).
- But changes spill 4–8 pixels across block edges in these renders, so the row assignment of a single local change is not reliable.

**Open:**
- the exact scheduling rule (which stream gets the next word)
- whether there are more streams (for example colour-difference codes against colour-difference fixed bits)
- the stream order at the start of a slice (bits 0–23 header, then word-level interleaving?)

## 16. Hand-built slices (29 Oct, `tools/braw_craft.py`)

> **Retracted (30 Oct, section 19).** These renders were made in Resolve, where an invalid slice shows leftover pixels
> from an earlier decode. Zero-filled slices are invalid, so the "fixed flat value", the header-bit readings and the
> single-1-bit results below describe leftover buffers, not decodes. Only "slices are self-contained" (copying a slice)
> has been re-confirmed.

`braw_craft.py` rebuilds a frame from its slices (the rebuild is byte-identical) and applies edits: fill, set bits,
copy, truncate. Results, flat field (Video Assist_0003), slice 40:

- **Slices are self-contained.** Putting slice 41's bytes in slice 40's place decodes cleanly.
- **Zeroed slices decode to a fixed flat value.** Keeping bits 0–23 and zeroing the rest gives a uniform slice with neutral camera colour at the same level (linear 2.34 in DWG, ≈ 1170 DN in DNG terms) every time. The same happens if the whole frame is zeroed, or if only the first 4, 8 or 12 bytes of real data are kept: blocks after the cut go to that fixed level, not to the last real block's brightness.
- **So DC is not predicted from the previous block.** Either each block codes its DC absolutely (a zero code giving log 0, which maps to that level), or zeros are invalid and this is the decoder's fallback. Not yet settled.
- **Header bits 14–23 have no effect on a zeroed slice** (values 0, 256, 512, 768); in real data they scale the whole slice. So they are a **quantiser scale**: scaling zero coefficients changes nothing. Value 1023 behaves differently, possibly as an escape value.
- **A single 1 bit in a zeroed slice** (each of bits 24–279 in turn): every flip that has an effect changes the slice from block 0 onwards, in a few fixed patterns that repeat with a period of 21 bits. That is not what independent block codes would do. Most likely the all-zero stream is invalid and these are fallback or resync behaviours. **Inconclusive.**

## 17. Smallest slice: flat field 5:1, bottom band (30 Oct)

**Target:** slice 61 (strip 1, band 30) of Video Assist_0004 (flat field, 5:1), frame 0. It is 664 bytes for 96×2
luminance blocks, about 27 bits per column of blocks. Only the first block-row is visible in renders (band 30 covers
coded lines 2160–2175 and the picture ends at 2167). Render window: Zoom 4, Pan 2800, Tilt 3240 puts the slice at
window pixel (20, 532).

**All bits 0–511 flipped one at a time** (512 variants):
- **Effect:** 37 no visible effect, 82 local (one or two blocks), the rest change many blocks.
- **Not concealment:** 470 of 475 outputs are distinct, so the decoder is not replacing the slice with a fixed fallback.
- **Local bits advance about one block column per 26 bits** (y = luminance, m = mixed): 27/32/46/47 → col 0; 102–111 → 2; 140–147 → 3–4; 191–201 → 6–7; 248–273 → 8; 286–302 → 9–10; 334–344 → 11; 379–386 → 13; 418–437 → 14–15; 481–498 → 17–19. Bits often come in adjacent pairs (46/47, 110/111, 286/287, 379/380, 430/431, 481/482).
- **Sync-loss flips are of three kinds:**
  - **Clean fronts:** nothing changes before column k, everything from k on does. Examples: bit 310 → col 10, 400 → col 14. These are true Y desyncs, about 30 bits per column.
  - **Colour fronts with a small uniform change everywhere:** bits 100/150/200 → colour desync from col 2/4/6, plus a weak change over the whole slice (≈ 0.2 log-AC, chroma energy ≈ 7 per block) including blocks before the front.
  - **Uniform whole-slice changes:** bits 300, 450, 500 change every block by the same amount (equal ΔDC and AC energy in every column), even blocks decoded long before those bits. An ordinary forward stream can't do this. Either the decoder reads part of the slice from a fixed position (a second stream starting at the end of the slice or at an offset), or a desync is detected and the slice is filtered or re-scaled as a whole.
- **Consequence for section 15:** "Y" or "C" labels on flips that change the whole slice say nothing about which stream the bit belongs to. Only local flips and clean fronts locate bits.

## 18. A slice is two passes over the same columns (30 Oct, slice 61 of the flat field at 5:1)

> **Under re-check (section 19).** Made with Resolve renders. The SDK oracle shows many of these flips push the decoder
> out of sync so that it runs out of data near the end of the slice. Also, zeroing the second half is *invalid*, not
> "nearly unchanged". The two-pass reading may survive but must be re-derived from oracle data.

**Spread flips** (256 flips evenly over bits 24–5219 of the 5240 data bits) give a clean picture. The last 9 bytes are
padding: no effect, and removing 8 bytes changes nothing.

| | Bits | Rate | Local flips | Desync flips |
|---|---|---|---|---|
| **Pass 1** | 24 – about 2340 | ≈ 24 bits per column | sweep columns 3 → 94 | damage **every** visible column: large from the flipped column on, small (refinement-sized) before it |
| **Pass 2** | about 2350 – 5230 | ≈ 30 bits per column | — | **clean fronts** sweeping columns 2 → 95: bit 2367→2, 2611→10, 3000→22, 3500→38, 4000→55, 4500→71, 5000→87, 5199→95; nothing changes before the front |

**Interpretation, consistent with every observation so far:**
- **Pass 1 is a base layer and pass 2 a refinement layer**, for all 96 columns of the band, each in column order.
- **There is no pointer to pass 2: it starts where pass 1 ends.**
  - A pass-1 desync moves where pass 1 ends, so every column's refinement is read misaligned. That is the small uniform damage before the front, and the "uniform whole-slice" flips in section 17.
  - A pass-2 desync only spoils the refinement from that column on.
  - Cutting the slice to 600 bytes loses the refinement of columns 81+. Cutting it in half breaks the slice. Zeroing the second half barely changes a flat grey field, whose refinement is nearly zero.
- **A column covers both block rows of the 16-line band** (and probably the colour difference for that 16×16 area). The coding unit is a 16-line macroblock column. That explains section 15's paired (0, c) / (1, c+1) damage, and it revises the earlier raster reading.
- The "32-bit interleave" of section 15 probably reflects this structure in a 72-line slice (4.5 macroblock rows). To re-check.
- It also fits section 13's repeated refinement of one coefficient: coarse value in pass 1, refinement in pass 2.

**For the encoder:** a valid slice should be header + pass 1 (all macroblock columns) + pass 2 (all columns) +
padding. A first milestone could be a pass-1-only slice with an empty pass 2, if the decoder accepts that. Test next by
zeroing pass 2 exactly.

## 19. A reliable decoder oracle, and what it overturns (30 Oct)

**The problem.** In Resolve, an invalid slice leaves leftover pixels in its place. Two variants can therefore agree while
both are wrong. The leftovers can come from:
- an earlier frame of the same file;
- an earlier render job, even from another file: a flat-field file showed the exact lens-cap slice decoded in an earlier
  job.

The A/B neighbour test (`braw_validity.py`) catches some of these but not all. Rendering a "poison" job first didn't
help either. Neither the SDK sample apps nor `ProcessClipManualFlow2` report errors for invalid slices; every decode
"succeeds".

**The oracle.** `tools/braw_crop.cpp`, built against the installed SDK (`build/macos/braw_crop`, compiled with Apple
clang through Resolve's scripting Python):
- It decodes every frame of a clip in order, in one fresh process, to float RGB.
- It writes a band of rows per frame.

`tools/braw_oracle.py` packs variants as `[pre, v1, pre, v2, ...]` twice, with different pre frames (lens cap 0002 and
+2 EV 0008). `tools/oracle_classify.py` then marks each block column:
- **invalid** if it differs between the two pre frames;
- **changed** if it is valid and differs from orig.

The test is fast: 258 frames decode in about 4 s.

**Re-test of slice 61 (flat field, 5:1):**

| Variant | Oracle result |
|---|---|
| copy slice 92 → 61, copy lens-cap slice 61 → 61 | **valid** (slices self-contained, confirmed) |
| zero from byte 640, cut to 648 bytes | **valid**, tiny changes |
| zero from byte 3, 20, 30, 40, 50 | **invalid** from about the cut column on (previous frame shows through) |
| zero all slices from byte 3 | invalid, whole frame |
| cut to 600 bytes | valid up to block column 82, **invalid from 83**: the decoder respects the table's slice length and stops when data runs out |

**Consequences:**
- **An all-zero bit run is not a valid "nothing" code.** There is no free empty pass 2, and no free empty slice.
- An invalid slice is detectable: its columns depend on the previous frame.
- **Every Resolve-only conclusion that relied on rewritten or broken slices is suspect:** section 16 (retracted), the
  length tests in sections 17 and 18, and the "uniform whole-slice" flips.

**First oracle flip sweep** (bits 24–5240 of slice 61, every 41st, `tests/mutations/oracle/flipsA/class.csv`):
- **About 30 % of flips are invalid near the end of the slice** (first invalid column 73–94). These are desyncs: the
  decoder consumes the rest of the stream differently and runs out of data before the last column. This happens all
  along the slice, in both "passes".
- **Local flips** change 1–5 block columns, and the changed column advances steadily with bit position:
  147→2–4, 229→6–7, 1131→43, 1418→56, 1582→64, 1746→72, 1992→82, 2197→91, 2443→4, 2771→14, 3017→21, 3304→32,
  4001→52, 4329→65, 4698→78, 4944→84. So there are **two sweeps over the 96 columns**:
  - the first ends at about bit 2200;
  - the second starts at about bit 2400 and ends at about bit 5000.

  The two-pass layout of section 18 is **confirmed** by local flips. Column positions here are SDK output columns
  (x 768–1535); the offset against coded columns is still to be checked.
- **Many flips make a small change over the whole slice** (max 0.04–0.05, a recurring size, or about 0.005). These are
  real decodes (not stale), so something slice-wide depends on these bits: perhaps a length or end-of-pass marker read
  from the stream, or decoder post-filtering. To investigate.


## 20. dng2braw's container passes the real decoder (30 Oct, splice test)

**Test.** New developer mode `dng2braw OUT.braw DNG_FOLDER --splice REF.payload` (`tools/braw_payload.py` extracts a
frame payload). It writes a complete dng2braw file with:
- the DNG clip's metadata, timecode and per-frame `bmdf`;
- dng2braw's own frame header and slice table;
- each slice's bytes (and header words 0x08–0x3f) copied from a Video Assist frame.

The SDK's `braw_probe` and `braw_crop` were then run on the result.

**Results** (DNG B001_003 with VA_0007 frame 0, Q0):
- **The SDK opens and decodes every frame.** Clip and frame metadata read back as written (`tests/splice/*.probe.txt`).
- **One header field was missing: 0x1080–0x113F holds 96 big-endian 16-bit values, all 33,** in every Video Assist + fp
  frame (Q0 and 5:1, every ISO and WB clip). Left at zero, the decode came out about 2× too bright, with per-channel
  differences. dng2braw now writes it. The meaning is unknown; perhaps a flat table such as quantiser weights, to test
  later by changing values.
- **0x11F0–0x11FF holds 16 bytes that change every frame.** They are not an MD5 of the slices, the header or the
  `bmdf`. **The decoder doesn't check them:** with zeros there, the payload differs from the reference only in these
  16 bytes, and the decode matches the reference except for white balance. Green ratio is 1.000; red is 0.975 and
  blue 0.946, from the DNG's as-shot 3244 K / 0 against the Video Assist's 3282 K / +9, which the decoder applies from
  metadata. Possibly an ID or a checksum that other software (Resolve, the Video Assist) uses; to check in Resolve.
- **So everything except the slice coder is now confirmed against Blackmagic's decoder.**


## 21. Exact oracle: the SDK's manual decoder, and what the decoded buffer is (30 Oct)

**`tools/braw_lab.cpp`** (built as `build/macos/braw_lab`) uses the SDK's CPU manual decoder (Flow 1):
- It reads a frame into our own buffer once.
- For each variant (`S:flip:BIT`, `S:fill:OFF:HH`, `S:hex:...`, `P:OFF:HH`) it restores the frame, applies the edits
  in memory, fills the decoded buffer with a sentinel byte (0xA5) and decodes.
- `--slice S` writes only that slice's area, and the sentinel count shows exactly what the decoder didn't write
  (invalid).

There are no test files and no stale-buffer ambiguity, and it runs at about 300 variants per second. The 5,312 flips of
slice 61 took under 20 s. `tools/lab_analyze.py` and `tools/lab_sweep.py` do the analysis.

**What the SDK's bitstream buffer shows (only by comparing it with the file, no code read):**
- At 0x0 the SDK keeps its own parsed copy of the frame header, little-endian: magic `warb`, size, `05` at 0x08, and
  16- and 32-bit fields byte-swapped. This **gives the field widths:**
  - 0x04 u32; 0x0c/0x0e u16; 0x10/0x12 u16;
  - **0x14–0x1b eight single bytes** (pairs: Q0 `00 10 00 10 00 10 01 0e`, 5:1 `00 10 01 0e 02 0c 03 0a`);
  - 0x1c/0x1e u16; 0x20/0x22 u16; 0x40 strip widths u16;
  - slice table u32 (rebased by +0x2100);
  - 0x1080 table u16;
  - **0x11f0 is two u64s** (swapped as 64-bit values).
- At 0x2000 it holds the frame as read (`bmdf`, then the payload at 0x2100). Slice bytes are not transformed.

**The decoded buffer** (25,247,744 bytes for UHD) is exactly what the codec reconstructs before debayering:
- **Y:** 3856 × 2176 uint16, at full photosite resolution and smooth on a flat field (≈ 519–717 for the 5:1 flat
  field);
- **then two colour-difference planes**, 1928 × 1088 int16 each (means ≈ −115 and −48 on the flat field).

This confirms the Y + 4:2:0 model with exact integers. To work out later: how Y/Cb/Cr relate to the DNG photosites
(the forward transform an encoder needs), and whether Y is log-encoded.

**Slice 61 (flat field 5:1, 16 lines, 768 wide), all 5,312 bits flipped:**
- 1,983 flips invalid (sentinel left). Zeroing from byte 20 leaves 17,106 of 18,432 samples unwritten; from byte 640,
  176.
- **Recurring 8-bit groups**: four consecutive bits that make tiny changes (±1–10) in block column *c*, then four in
  column *c*+1. Groups advance one macroblock (2 block columns) every ~45 bits:
  - sweep 1: columns 2–3 at bit 128 … 94–95 at bit 2263;
  - **sweep 2:** columns 0–1 at bit 2341 … 94–95 at bit 5239.

  These look like fixed-length low-order (refinement) fields, 2 bits per block row?
- **Colour-difference changes advance only in sweep 2** (Cb/Cr column *k* with Y column 2*k*). In sweep 1 the
  non-local flips change everything from column 0, as expected if pass 1 is only parsed and the output is written
  during pass 2.
- Between the groups are variable-length codes: flips there change wide areas but stay valid.

**The 4-bit fields, all 16 values set** (slice 61; `S:set:BIT:LEN:VALUE`; all 48 variants valid):
- **In the flat field every field reads `1111`, and 15 is "no change".** Lower values change the picture a little
  more, roughly in proportion to how far below 15 they are.
- **Pass-2 field at bit 2341 (block columns 0–1):**
  - the top two bits act on block row 0 (values 8–11 change row 0 only slightly; 12–14 change row 1 only);
  - **the low two bits act on block row 1;**
  - so it is **2 bits per 8×8 block row**.
- **Pass-1 fields at bits 128 and 132 (columns 2 and 3)** change block row 0 only.
- The changes are small and low-frequency (sums of 1–25 over an 8×8 block) and **spread into the next block
  column**. So these fields are not plain DCT coefficients. Perhaps a per-block offset or rounding/refinement field,
  or the reconstruction includes a filter across block edges. To test on textured content, where coefficients are
  larger.


## 22. The forward transform: DNG photosites → decoded Y, Cb, Cr (30 Oct)

**Data.** 24 ColorChecker patches × 4 exposures (−4, −2, 0, +2 EV). DNG side: black-subtracted R, G, B photosite means
(B001_001–004, 8 frames, `tools/analysis/dng_patch_means`). BRAW side: `braw_lab` decoded planes of Video Assist
0005–0008, 2 frames, at the same positions (coded row = DNG row + 3). 92 unclipped pairs. Files are in `tests/xform/`
(`pairs.txt`, `phase.txt`, `model.npy`).

**Results:**
- **The transfer curve is a square root, not a log.** On the six grey patches across all four exposures:
  **Y = 55.94 · G^0.502 + 71.2** (rms 2.9 code values over a range of 190–3060). Call T(v) = 55.94·√v + 71.2, with v
  the black-subtracted DNG value. The same G gives the same Y in every exposure, so the curve is fixed (at least at
  ISO 800, Q0).
- **Y is a smooth luminance, identical at all four Bayer sites** (R, G1, G2, B site means differ by 0.13–0.20). It is
  not a per-photosite value. Fit over all patches:
  **Y = 0.290·T(R) + 0.571·T(G) + 0.293·T(B) − 12.7** (rms 2.7). The weights give exactly T(G) on a raw grey, whose
  R/G ≈ 0.75 and B/G ≈ 0.38.
- **Colour differences at half resolution:**
  **Cr ≈ 0.574·(T(R) − T(G))** (rms 2.1) and **Cb ≈ 0.563·(T(B) − T(G))** (rms 3.5). Cross terms are under 0.011.
- **Consequence:** per 2×2 quad, (Y, Cb, Cr) determine T(R), T(G), T(B), so the decoder can rebuild R, G, B. Exactly how
  it rebuilds four photosites (two greens) from one smooth Y per site plus Cb/Cr per quad is still to measure. The fit is
  of patch means, so it says nothing about detail at photosite scale.
- **Earlier statements corrected:** section 14's "log domain" should read "square-root domain"; section 8's linear
  BRAW = DNG × 0.00203 still holds (it is about Resolve's linear output).

**Next, an exact measurement of the decoder's inverse.** The manual decoder takes a decoded buffer we supply
(`CreateJobProcess(frameState, decodedBuffer, ...)`). So synthetic Y/Cb/Cr planes, including flat fields and single
photosite-scale patterns, can be processed into RGB to read the inverse transform directly. That tells the encoder
exactly which Y/Cb/Cr to write for given photosites.


## 23. End to end: DNG → planes → Blackmagic's own processing matches the Video Assist (30 Oct)

**Test.**
1. `tools/dng_to_planes.py` builds a full decoded buffer from DNG photosites with the section 22 model, per site:
   Y = T(site) − T̄_own + L, using 5×5 same-colour averages; Cb, Cr per 2×2 quad. Input was the average of 16
   frames of B001_003, 0 EV.
2. `braw_lab --process 0 2160 --inject FILE` puts that buffer through the SDK's processing step (`CreateJobProcess`,
   clip defaults, float RGB).
3. The output was compared with the SDK's decode and processing of Video Assist_0007 frame 0, the same scene.

**Results:**
- **Whole frame, 20 × 20 blocks: inj/real = 0.981 / 0.992 / 0.992 (R/G/B medians), IQR under 0.9 %.** The ColorChecker
  region agrees to the same level. The small red offset is within the difference between two separate takes.
- **Visually indistinguishable** (`tests/xform/cmp_full.png`, `cmp_cc.png`; left Video Assist, right DNG through the
  model): star chart, text, ColorChecker colours, highlights.
- **Alignment and CFA phase settled: coded row = DNG row + 2,** not + 3. A per-pixel search gave +2, and an even
  shift is the only one that keeps the decoder's Bayer phase (colours come out right). `BRAW_PAD_TOP` is now 2 in
  `braw_format.h` (2 lines above, 4 below: 2170 → 2176).
- **So everything up to the entropy coder is now known well enough for an encoder:** container, header, slice
  layout, the transfer curve and colour transform, and the decoder's acceptance of our planes. What remains is to
  turn the planes into slice bitstreams.

Notes: `braw_lab` can't find the payload for some frames of 0007 (2, 3 and 5) and exits with code 7; to check (the
SDK may put larger frames elsewhere in its buffer). Edge detail between separate takes could not be compared pixel
for pixel (sub-pixel registration). The flat-area residual is 2.75 code values.


## 24. The slice bitstream is baseline-JPEG Huffman coding, row by row (30 Oct)

Found with `braw_lab` (exact oracle) on slice 61 and slice 40 of Video Assist_0004 (flat field, 5:1). Parser:
`tools/braw_slice_parse.py`.

**Rows, not passes (this corrects sections 18 and 21):**
- **A slice is coded one 8-line block row after another**, each row left to right over the 96 block columns. Slice 40
  (72 lines) has 9 sweeps of about 2,200 bits; slice 61 (16 lines) has 2.
- In the full flip sweep of slice 61, 345 local flips in the first 2,271 bits change block row 0 only and none row 1
  only. After that, row 1.
- **Every local change has a fixed "plus" footprint:** the block, its left and right neighours, and the block above or
  below. So the decoder applies an overlap/smoothing step across block edges (a lapped transform or a deblocking
  filter). Still to characterise; an encoder can ignore it at first.

**Order within a row** (row 0 of slice 61): **[Y block 2k] [Y block 2k+1] [Cb block k] [Cr block k] [4 bits] [4 bits]**,
repeated for k = 0…47. Verified: the parser reads all 48 pairs of row 0 with no error and ends exactly at bit 2271,
where row 1 starts.

**Each block is coded exactly like baseline JPEG** (ITU-T T.81), with the **standard Annex K luminance tables, for
chroma too:**
- **DC:** a size-category code from the standard luminance DC table (`00`=0, `010`=1, `011`=2, `100`=3, `101`=4,
  `110`=5, `1110`=6, …), then that many bits, where a leading 0 means negative (−(2^s−1)+v). **The DC is predicted from
  the previous block of the same component** (DPCM): one DC-code flip shifts every later block, in both rows, by a
  constant. Set experiment, bits 279–286: `101`+4 bits gives ±8…±15 and `100`+3 bits ±4…±7, in steps of 1.515 Y units.
- **AC:** run/size symbols from the standard luminance AC table, then the amplitude bits, in the **standard zigzag
  order**. Each symbol was checked by writing it into block 42 and reading the exact decode (`tests/lab/ac42*`):
  - 0/1 … 0/5 put ±1, ±2, ±4, ±8, ±16 steps on zigzag position 1, about 10.5 coefficient units per step, doubling
    with size;
  - runs 1–6 land on zigzag positions 2–7.
- **End of block = `1010`**, the standard EOB. A flat block is `00 1010`.
- **4-bit fields:** two after each pair, all `1111` in the flat field. Per section 21 they make small additive
  low-frequency changes. Meaning open.

**Still open:**
1. **The slice start (bits 0–93 of slice 61).** It includes the 24-bit header (`11111111 0101000101010111`) and a
   first pair that doesn't parse with plain JPEG codes; probably absolute starting DCs or predictor values.
2. **Row 1, pair 12** (bits 3025–3077) doesn't parse with any combination of JPEG luminance and chroma tables, though
   its fields are where the flip map puts them. An extra symbol, an escape, or a table difference in the long codes.
3. Chroma in 4:2:0: a 16-line band has only one 8-line chroma block row, yet both rows carry Cb/Cr blocks. Which
   chroma rows each set codes (e.g. chroma blocks of 8 × 4?) is to be checked by writing an AC coefficient into a
   row-1 chroma block.
4. Quantiser: DC step 1.515 Y units and AC (zigzag 1) about 10.5 here, at 5:1. Relation to header bytes 0x14–0x1b,
   the 96 × 33 table at 0x1080 and the slice header to find.
5. The overlap/smoothing filter, for an exact encoder.

**So the encoder is in sight:** forward transform (section 22) → 8×8 DCT → quantise → JPEG Huffman with the standard
tables in this row and block order → slices. The first target is a slice of flat blocks (`00 1010`) with correct
starts, then real content.


## 25. Slice start, chroma blocks, scan orders, and the first synthetic slices (30 Oct)

**Slice header (24 bits, slice 61 `ff 51 57`):**
- 8 bits `11111111`;
- bits 8–12 `01010`, the same in every slice seen;
- **bits 13–21: the 9-bit Y DC base**, in DC steps: flip weights 386.7, 193, 96.7 … 1.5 Y, with 1.5 = one DC step;
- bits 22–23: two bits whose meaning is open. `00`, `10` and `11` decode, `01` does not. They may relate to whether
  block column 0 is coded (see below).

**First pair of row 0:**

`[Y blocks] [Cb: 12-bit absolute DC, then AC + EOB] [Cr: 11-bit absolute DC, then AC + EOB] [4 bits] [4 bits]`

- Cb/Cr absolute DC: 1.09 chroma units per step. Found by ±1/±2 set experiments: every window ending at bit 63 (Cb)
  or bit 78 (Cr) shifts only that plane, uniformly.
- **Slice 61 codes two Y blocks here (columns 0 and 1); slice 34 codes only one, which is column 1.** Confirmed by
  adding an AC coefficient and reading the plus-shaped footprint. When column 0 is coded or skipped is still open.
- After the first pair, every row is regular. **The DC predictor runs on in raster order across rows** (the first
  block of row 1 is predicted from the last block of row 0, for Y, Cb and Cr).

**Chroma blocks are 8 wide × 4 tall:** each 8-line Y row carries the Cb/Cr blocks for its 4 chroma lines. They are not
smoothed across edges; Y is.

**Scan orders (each position written as a single coefficient and read back exactly):**
- Luma, 8×8: the **standard JPEG zigzag** at every position.
- Chroma, 8×4, 32 positions (v,u):
  (0,0) (0,1) (0,2) (0,3) (1,0) (1,1) (2,0) (2,1) (1,2) (1,3) (0,4) (0,5) (0,6) (0,7) (1,4) (1,5) (2,2) (2,3) (3,0)
  (3,1) (3,2) (3,3) (2,4) (2,5) (1,6) (1,7) (2,6) (2,7) (3,4) (3,5) (3,6) (3,7).
  Positions 32–63 wrap to 0–31.

**Quantiser steps at 5:1** (VA_0004 header), orthonormal DCT units:
- Y DC 12.1 (1.515 Y per step);
- Y AC 8.8–12 per step, rising mildly with frequency;
- chroma DC and AC flat, about 6.1–6.2 per step.

**AC table:** the standard JPEG luminance codes work for all run/size symbols up to size 8, except **1/5, 2/4, 3/3,
5/2 and 7/2**. Their JPEG codes (`11111110110`, `11111110111`, `111111110100`, `111111110101`, `111111110111`) are
prefixes of longer BRAW codes, valid only after 9–11 more bits. Size 9–10 symbols are also rejected (perhaps a range
limit). So BRAW uses its own AC table, identical to JPEG's for the common codes. **An encoder can avoid these
symbols** by moving such a coefficient to the next size, until the real codes are mapped. The row-1 anomaly of
section 24 is probably one of them.

**First synthetic slices: valid.** Slice 61 rebuilt as original header and first pair plus 95 flat pairs
(`00 1010`×4 + `11111111`) decodes fully (no undecoded samples):
- Y is uniform at 598.4 in both rows; Cb and Cr are constant.
- A small fixed 8×8 pattern of ±1.5 remains (a dither or filter effect).
- Variants with only row 0 or only row 1 flat confirm the DC predictor carrying over between rows.

**Next:** a prototype encoder (Python) that writes whole slices from Y/Cb/Cr planes. Then a whole frame from a DNG,
decoded by the SDK and compared with the planes.


## 26. First real BRAW from dng2braw's own encoder; DC table; per-slice quantiser (30 Oct)

> The per-position luma steps and the 1.475 DC slope here are superseded by section 30.

**Prototype encoder** `tools/braw_encode_proto.py`. From Y/Cb/Cr planes it writes all 155 slices of a frame:
- header: `11111111`, the 5-bit quantiser field, the 9-bit base, then `0` (mode C);
- per row: [Y Y Cb Cr `1111` `1111`] × pairs. In the first pair, the Cb and Cr DCs are absolute 12/11-bit values.
- DC: DPCM, raster order across rows;
- AC: orthonormal DCT, quantised with the measured steps, standard zigzag (luma) or the 8×4 scan (chroma);
- Huffman codes: JPEG standard with the BRAW differences below, avoiding unconfirmed symbols.

The frame header and quantiser bytes come from a Video Assist frame (5:1, VA_0004).

**Closed-loop test on the flat field (VA_0004's own decoded planes → encode → SDK decode): all 155 slices valid.**
Y error 1.24 code values rms (image noise 21), Cb/Cr 0.22 rms, bias 0.45. This is genuine Blackmagic RAW written by
dng2braw code and decoded by Blackmagic's SDK.

Fixes found along the way:
- **Slice start:** after the 9-bit base, write a single `0` and start the first Y block at bit 23 (mode C). With
  that, two Y blocks (columns 0 and 1) always follow. The camera also uses the codes `10` (column 0 not coded) and
  `11` (meaning open).
- **DC table: BRAW differs from JPEG Annex K for categories 6 and 7: `11100` and `11101`** (JPEG: `1110`, `11110`).
  Categories 0–5 and 8 (`111110`) are as JPEG. Found by writing ±v DC steps into synthetic slices and decoding.
- Y offset: Y mean = 468.2 + 1.515·DC at the flat field's quantiser setting.

**Textured content (the DNG chart scene) still fails.** Every slice desyncs or saturates because of the next finding.

**Header bits 8–12 are a per-slice quantiser setting** (the camera's rate control varies it from slice to slice):
- the flat field at 5:1 uses `01010` everywhere;
- the lens cap and −4 EV use `11010`;
- +2 EV mixes `01110`, `01111`, `01000`, `11011`, `11010`, `01010`.

The DC scale depends on it:
- `01010`: Y = 468 + 1.515·base;
- `11010`: Y = 2.045·base − 60 (fits 8 slices from 60 to 806 Y).

So DC and presumably AC steps are per slice. The base (9 bits) covers only part of the Y range at a given setting; the
rest is reached through the first DC difference (needs categories 9–11) or a coarser setting.

**Next:**
1. Map quantiser setting → DC/AC steps and offset, with lab experiments on one slice.
2. Find the BRAW codes for DC categories 9–11 and AC sizes 9–11 and the five AC symbols.
3. Then choose the setting per slice as the camera does, and encode the DNG chart scene.


## 27. Slice start values, chroma start, DC range (30 Oct, evening)

> **Superseded by sections 28–30.** The "selector + mantissa" below is block 0's DC code (categories 9–11), and the step values are replaced by the flat quantiser.

**Corrections to section 26:**
- **The chroma starting values are not fixed-length fields.** The first Cb and Cr DC of a slice are ordinary DC codes
  (BRAW DC table) measured from 0, at 1.09 chroma units per step. In the flat field, `11101 0011100` is category 7,
  value −99 (≈ −108 Cb), and `11100 010011` is category 6, value −44 (≈ −48 Cr). That is why they looked like 12- and
  11-bit fields.
- **The first slice byte is not a fixed marker:** `FF` in the 5:1 clip, `FE` in the Q0 clip. The start-value field
  probably begins at bit 7.

**Y start value (header bits 8–21, 5:1 frame):** a 5-bit range selector, then a mantissa, then the 1-bit flag (`0`
= columns 0 and 1 both coded):

| Bits 8–12 | Mantissa | Starting DC | Status |
|---|---|---|---|
| `01010` | 9 bits, step 1.0 | 0 + m | confirmed; used by the encoder |
| `01011` | 9 bits, step 1.0 | 1535.2 + m | confirmed; used by the encoder |
| `01000` | 9 bits, step 0.5 | 512 + m/2 | DC step OK, but category-8 differences rejected |
| `01001` | 9 bits, step 0.5 | 1279.2 + m/2 | as `01000` |
| `01111` | 10 bits, step 1.0 | 2047.3 + m | to verify |

The camera uses many more values (`11010` with first byte `FE`, etc.); not mapped.

**DC → Y:** Y = 471.4 + 1.475·DC, linear from about DC −300 to +2400. Beyond that it clips, then wraps (apparently
12-bit arithmetic).

**DC categories 9+:** not standard JPEG. `11110` + 13 bits gives very large values in a pattern not yet understood,
and `1111110`/`11111110` + bits are rejected. The encoder limits DC differences to ±255, and chroma start values to
±255 units (the next block catches up).

**Status of the encoder on the DNG chart scene:** still wrong in most slices:
- the bright grey-card region (slices 0–9) fails from the first block;
- slices 10–24 fail where the grey card starts;
- a +23 Y offset at Y ≈ 850.

Next suspects: the Y start/offset model at higher levels, chroma beyond ±255 units at the slice start (the grey card
has Cb ≈ −393), and large AC values (sizes 9+ are clamped to 8). To isolate them: encode a synthetic frame of flat
patches at several Y/Cb levels, then a single sharp edge, and compare.


## 28. There is no slice header: block 0's DC is an ordinary DC code (31 Oct)

Synthetic test `syn1` (flat patches at 14 Y levels × 6 Cb × 4 Cr) failed in 153/155 slices, with errors that depended
only on Y. Probes (`tools/dc_ramp_test.py`, `start_map_test.py`, `start_cat_test.py`, `head_byte_test.py`,
`row_level_test.py`) showed why:

- **The bits we called the slice header are block 0's DC code.** The leading `0xFF` is the start of the long DC codes:

  | Category | Code | Extra bits |
  |---|---|---|
  | 9 | `111111110100` | 9 |
  | 10 | `111111110101` | 10 |
  | 11 | `111111110111` | 11 |

  Every first byte decodes as the start of a normal DC code (`C0`–`DF` are category 5, `E1`–`EF` categories 6 and 7, and
  so on). The "range selector" and "mantissa" of section 27 were the category code and its extra bits. The `0` "flag"
  after them was the start of block 0's AC data: `0`+`00`+`1010` reads as AC 0/1 with value −1, then EOB.
- **The predictor for block 0 is 0, which is Y 2048** (mid-scale), and the differences continue across rows (section 25).
- **DC → Y is exact: Y = 2047.92 + d · 99/64** (fit rms 0.06 over 155 slices × 9 rows of flat levels). The earlier
  1.475 / 471.4 came from the misparsed first block.
- **Chroma DC: 35/32 units per step** (was 1.09), again from predictor 0.

With this, `syn1` decodes correctly in every slice. The Y error inside the patches is 3.3 rms, which is the input
noise (σ 3). Chroma bias is below 0.5 %. The errors left at patch edges are the decoder's block-edge filter
(section 30).

## 29. AC codes: JPEG Annex K plus 0/9 and 0/A at 12 bits (31 Oct)

`tools/ac_code_search.py` builds a base frame and per-candidate variants for `braw_lab`:

- **The variant:** block 0 of slice 40 carries one candidate code plus K extra bits. Blocks 1 and 2 then step the DC by
  +200 and −200, so that a misparse almost never lands on the right levels by chance.
- **`braw_lab --crop`:** new option. It writes only the top-left W×H of the slice, but still counts invalid samples
  over the whole slice.
- **Bug fixed in `braw_lab`:** for larger frames the SDK's buffer holds its little-endian header copy with the slices
  in place, and there is no `bmdf` at 0x2000. The old search then found a false 'braw' inside the slice data, so edits
  went nowhere. It now detects `warb` at offset 0 and reads the slice table little-endian.

**Result:** the BRAW AC table is JPEG Annex K (luminance), including the 16-bit codes for sizes 9 and 10. The only
change is a block of the long codes:

| Symbol | JPEG | BRAW |
|---|---|---|
| 0/9 | `1111111110000010` | `111111110100` (as DC category 9) |
| 0/A | `1111111110000011` | `111111110101` (as DC category 10) |
| 1/5 | `11111110110` | `111111101100` |
| 2/4 | `111111110100` | `1111111011100` |
| 3/3 | `111111110101` | `1111111011101` |
| 5/2 | `11111110111` | `1111111011110` |
| 7/2 | `111111110111` | `1111111011111` |

- 6/2 (`111111110110`) and 8/2 are as in JPEG.
- JPEG's own 16-bit codes for 0/9 and 0/A are invalid in BRAW.
- An EOB is required even after a coefficient at position 63.
- `111111101101` and `111111110111` as AC symbols (perhaps 0/B) are not mapped yet. The encoder clamps AC values to
  ±1023.
- The camera's own slices now parse further, but they still hit codes outside this table (see Next).

## 30. Flat quantiser and the decoder's block-edge filter (31 Oct)

Impulse responses: in slice 40, one coefficient at value 100 in block (row 4, col 10), for every luma and chroma
position (`tools/basis_probe.py`).

- **The decoder is linear in every coefficient up to ±1023** (checked at positions 1 and 2 from 100 to 1023; DC from 2
  to 1000).
- **Quantiser at 5:1 is flat:**
  - luma step 99/8 = 12.375 at every AC position (12.33–12.45 measured), with DC 99/64 per code value of the block
    mean, which is the same step in orthonormal units;
  - chroma step 99/16 at every position, with DC 35/32 per code value, again the same.
  - The per-position table in sections 26–27 came from fits that ignored the edge filter.
- **Luma post-filter:** at every 8-pixel block edge, horizontally and vertically (across slice and strip edges too), the
  two pixels at the edge become `(3·prev + 10·p + 3·next) / 16`, using the unfiltered neighbours along that direction.
  - A least-squares fit over all 64 impulse responses gives taps 0.1875 / 0.625 / 0.1874, with residual 0.46 (rounding).
  - Chroma is not filtered.
- **The filter is not adaptive:** the same result holds from a DC step of 2 up to 1000.
- **Encoder pre-filter:** the exact inverse. For each edge pair, L and R are solved from the 2×2 system
  [[10, 3], [3, 10]] / 16 given the untouched pixels 6 and 9 (`unpost()` in `braw_encode_proto.py`).

**DNG chart scene (`dng3_planes` → `dng7.braw`), all 155 slices valid:**

| Encoder state | Y rms | Worst slice |
|---|---|---|
| Before sections 28–30 | 37.8 | 121 |
| Plus start fix | 25.3 | |
| Plus true steps (2-tap model) | 7.5 | |
| **Plus 3-tap pre-filter** | **2.79** | 3.3 |

- The worst slice is now 3.3 rms. Only 10 pixels in the frame are off by more than 60 (isolated hot-pixel-like
  points).
- Cb and Cr are 1.56 rms, bias −0.01.
- The quantisation-only model predicts 2.3, so what remains is ordinary quantisation error.
- The frame is 2.95 MB, about 4.3:1. The camera's rate control, which is probably the per-pair 4-bit fields that we
  always write as `1111 1111`, is not reproduced.

**Status:** dng2braw's prototype encoder now writes real Blackmagic RAW for a real scene, with a near-exact decode.

## 31. The C encoder: dng2braw writes real Blackmagic RAW (31 Oct)

`source/braw_encode.c` ports `tools/braw_encode_proto.py`:

- the forward transform from the coded CFA frame (section 22; 5×5 same-colour averages with edge replication);
- the luma pre-filter (section 30);
- the 8×8 luma and 4×8 chroma DCT with the flat quantiser;
- the DC/AC Huffman coding (JPEG Annex K tables built from T.81's BITS/HUFFVAL, plus the BRAW changes in sections 26,
  28 and 29);
- the slice layout, with `1111 1111` as the per-pair fields.

`braw_build_frame()` uses it for `BRAW_CODEC_BRAW` and writes the 5:1 frame-header words that a Video Assist writes
(0x08–0x23, from VA_0004).

**Checks:**
- **Bit for bit against the Python:** `tools/braw_enc_planes.c` encodes the same planes (`dng3_planes.bin`). 155/155
  slices are identical (2 954 320 bytes).
- **End to end:** `./dng2braw out.braw B001_003 --limit 8` takes 6 s for 8 frames (≈1.3 fps in the Linux VM), at
  4.3 MB per frame. Then:
  - **SDK decode:** every Y, Cb and Cr sample is written (no sentinels), on frames 0 and 7.
  - **SDK processing with our file's own metadata**, against VA_0007 frame 0 (64-px tiles): G 0.992, R 0.956,
    B 0.938. The R/B gap is colour metadata: ours is generic 3244 K / tint 0, the Video Assist's is 3282 K / tint +9.
  - **Our decoded planes injected into VA_0007's processing state:** R 0.981, G 0.992, B 0.991 (p5–p95 within ±3.5 %),
    which is the accuracy of the forward-transform fit.
  - **DaVinci Resolve 21:** imports it as Blackmagic RAW, 3840×2160, 8 frames, 23.976, and renders a clean picture
    (`tests/enc/resolve/c8_frame0_small.png`).

**Not done yet:**
- **Rate control:** single noisy frames come out at 4.3 MB (≈2.9:1), against the 2.95 MB of the 16-frame average, and
  the metadata says "5:1".
- **Only the fp UHD layout (3856×2176) is supported.** The encoder refuses other sizes.
- **Colour metadata is generic.** Matching the Video Assist's kelvin/tint calculation would close the R/B gap.

## 32. Per-pair fields = edge-filter strengths; header table = quantisation matrix; rate control (31 Oct)

**The two 4-bit fields after each pair** (`tools/field_probe.py`) are four 2-bit block-edge filter strengths:
- group 0: top edge of block A;
- group 1: the A|B edge;
- group 2: top edge of block B;
- group 3: the edge after B (the left edge of the next pair's block A).

A block's bottom edge belongs to the pair below, and its left edge to the pair before. A value v filters the two edge
pixels with weights (v, 16−2v, v)/16:
- 3 is the (3,10,3)/16 filter of section 30;
- 0 turns it off.

They change no coefficients, so they are deblocking control, not rate control. dng2braw writes 3 everywhere.

**Frame header 0x14–0x1b** are byte pairs:
- 5:1: (0,16), (1,14), (2,12), (3,10), which is the filter table for strengths 0–3;
- Q0: (0,16), (0,16), (0,16), (1,14), a gentler filter.

The words at 0x1c and 0x1e (5:1: 505, 323; Q0: 266, 280) don't change the decode when poked in memory. The decoder
probably reads them while parsing the frame header, before in-memory edits apply.

**Header table 0x1080–0x113f is the quantisation matrix** (`tools/qmat_probe.py`: separate files, each doubling one
entry):
- 96 × u16 in raster order: luma 8×8, then chroma 4×8;
- luma AC step = entry × 3/8, chroma step = entry × 3/16;
- entry 0 does not affect luma DC (fixed at 99/64 per code value);
- the Video Assist writes a flat 33 at Q0 and at 5:1, so its own encoder reaches 5:1 by choosing coefficients (an
  encoder-side decision).

**Rate control in dng2braw 0.4:** each frame carries its own matrix, 33 × scale on the AC entries.
- The DCT runs once per frame. `choose_scale()` then searches the scale (log-domain bracketing and interpolation, 0.5 %
  tolerance, starting from the previous frame's) for the finest matrix whose slices fit the budget.
- The budget is the 12-bit coded size ÷ N, minus header and `bmdf`.
- `--ratio N` (default 5); `--quality Q0…Q5` for constant quality (scale 1, 1.4, 2, 2.8, 4, 5.6); `--qscale S`.

B001_003 frame 0, SDK decode against the encoder's input planes:

| Target | Frame | Matrix scale | Y rms | PSNR | Cb / Cr rms |
|---|---|---|---|---|---|
| 3:1 | 4.18 MB | 1.02 | 3.38 | 61.7 dB | 2.02 / 2.02 |
| 5:1 | 2.51 MB | 2.05 | 5.42 | 57.6 dB | 3.20 / 3.20 |
| 8:1 | 1.57 MB | 3.75 | 7.51 | 54.7 dB | 4.57 / 4.53 |
| 12:1 | 1.05 MB | 7.78 | 11.6 | 51.0 dB | 7.17 / 7.11 |

- Every sample is decoded.
- The only errors above 100 were 10 hot pixels above 4095, where the decoder clips at 12 bits. The front end now clamps
  luma to 4095.
- Resolve imports the 12:1 file as Blackmagic RAW, and a 4K render is clean at 1:1 (Siemens star, ColorChecker:
  `tests/enc/resolve/r12_crops.png`).
- Speed with rate control: about 0.8 fps in the VM.

## 33. Speed (31 Oct)

**Threads:** a small pthread parallel-for (`braw_par_for`, `DNG2BRAW_THREADS` to override) over
- forward-transform rows,
- pre-filter rows and column groups,
- DCT block rows,
- and slices (each encode pass, and the final frame).

The output is byte-identical at any thread count, and still bit-identical to the Python prototype.

**Forward transform:** one 25-tap pass computes all three same-colour 5×5 averages, with a branch-free interior path
(sums by row/column parity). It is identical to the six edge-replicated box filters it replaces, at 0.04 s instead of
0.41 s per frame in the VM.

**Rate search:** the first step assumes bits ∝ scale^−0.75, then a log-domain secant inside the bracket. It stops
within 0.8 % under budget, usually after 2–4 passes, starting from the previous frame's scale.

Per frame in the 4-thread VM: transform 0.04 s, pre-filter 0.02 s, DCT 0.03 s, one encode pass 0.04 s.

**Throughput on the M1 Max** (10 cores; 96 frames of B001_003 read from disk):

| Mode | fps |
|---|---|
| `--ratio 5` | 11.5 |
| `--ratio 12` | 20.2 |
| `--quality Q0` (no search) | 15.8 |

The SDK decodes frame 95 of the 5:1 file with every sample written.

## 34. Real time at 5:1 (31 Oct)

| Change | Effect |
|---|---|
| Bit writer: 64-bit accumulator, flushes 32 bits at a time; `b == NULL` counts bits only | Rate-control passes use `braw_count_slices()` and write nothing. Final encode pass 15 → 10 ms |
| Read-ahead: the next DNG loads on a thread while the current frame encodes | |
| Buffers kept across frames (coded frame, planes, pre-filter, coefficients) | |
| Luma forward transform: separable parity sums (H3 = T[x−2]+T[x]+T[x+2], H2 = T[x−1]+T[x+1]) and per-case reciprocals instead of three divisions per pixel | 14 → 7.5 ms. Planes are identical to before |
| Quantiser: multiply by reciprocal steps | Still bit-identical to `tools/braw_encode_proto.py` (155/155 slices on `dng3_planes`) |
| Rate search: accept any scale within 2 % under budget | Usually 1–3 count passes |

Per frame in steady state on the M1 Max (10 threads, `tools/braw_prof.c`):

| Stage | Time |
|---|---|
| DNG load | 3.7 ms (overlapped) |
| Planes | 11.6 ms |
| Pre-filter | 3.4 ms |
| DCT | 3.9 ms |
| Count pass | 6.2 ms |
| Final encode | 10.3 ms |

`DNG2BRAW_PROF=1` prints per-frame planes timing and the number of rate passes.

**Throughput on the M1 Max** (120 frames of B001_003 from disk):

| Mode | fps |
|---|---|
| `--ratio 5` | **25.6** (real time for 23.976) |
| `--ratio 8` | 30.7 |
| `--ratio 12` | 34.1 |
| `--quality Q0` | 26.1 |

Checks on a 24-frame 5:1 clip:
- the SDK decodes frames 0, 11 and 23 with every sample written;
- the largest frame is 2 515 968 bytes against the 2 517 196 target.

## 35. Apple-silicon native: GCD, M1 tuning, Metal GPU, unified memory, pipelining (31 Oct)

**Target machine:** M1 Max (8 P-cores + 2 E-cores, 24-core GPU, 64 GB unified memory). Only the Command Line Tools
are installed, so the Metal kernels compile at run time from source (`newLibraryWithSource`); Xcode is not needed.

**CPU:**
- `braw_par_for` uses Grand Central Dispatch (`dispatch_apply_f`, user-initiated QoS) on macOS instead of creating
  threads per call.
- Universal binary: `-O3 -Xarch_arm64 -mcpu=apple-m1`, plus Intel.
- The quantiser is float `rintf(c × 1/step)`, which vectorises to NEON and matches the GPU counter exactly.
- Category sizes use count-leading-zeros (`__builtin_clz`).
- DNG read-ahead and write-behind threads.

**GPU** (`source/braw_metal.m`; `--cpu` turns it off):
- Kernels: T(v), the same-colour 5×5 averages to Y, per-quad Cb/Cr, the inverse edge filter (horizontal then
  vertical), and the 8×8 luma and 4×8 chroma DCT.
- **Unified memory:** the CFA frame is written straight into a shared `MTLBuffer`, and the CPU's entropy coder reads
  the coefficient buffers in place, with no copies. Intermediates (T, Y, Cb, Cr) stay in GPU-private memory.
- Arithmetic is float32 with `MTLMathModeSafe`, against double on the CPU. Over one frame, 0.0076 % of quantised luma
  coefficients differ by one step, and frame quality is identical (Y rms 5.42 / 57.6 dB at 5:1, the same as the CPU
  path).
- **Pipeline:** two slots. The read-ahead thread loads DNG N+1 and submits its GPU work while the main thread
  entropy-codes frame N (`braw_prepare_frame()` → `braw_build_frame()`).
- **Rate passes on the GPU:**
  - DC codes and the per-pair fields don't depend on the matrix, so the CPU counts them once per frame
    (`braw_slice_fixed_bits`);
  - each search pass is one GPU dispatch that counts AC bits per block (`k_by`, `k_bc`), and the CPU sums them per
    slice;
  - the final CPU encode is checked against the budget (with a 0.2 % coarsening loop as a safeguard; it never
    triggered).
- The GPU does transform + DCT of a frame in about 3.9 ms (the CPU took 34 ms on a cold start).

**Throughput, B001_003, all 129 frames from disk, M1 Max:**

| Mode | 0.5 (CPU) | 0.6 `--cpu` | **0.6 GPU** |
|---|---|---|---|
| 3:1 | – | – | 52.9 fps |
| 5:1 | 25.6 | 33.4 | **75.1 fps** (3.1× real time) |
| 8:1 | 30.7 | – | 94.6 fps |
| 12:1 | 34.1 | – | 105.9 fps |
| Q0 | 26.1 | – | 57.2 fps |
| Q3 | – | – | 91.8 fps |

The main thread never waits for DNG reads or file writes. What's left is the CPU Huffman coder: about 13 ms per frame
at 5:1 and 17 ms at Q0.

**Checks:**
- the full clip at 5:1: 129 frames, between 2 473 216 and 2 515 968 bytes (target 2 517 196);
- the SDK decodes frames 0, 64 and 128 with every sample written;
- Resolve imports it as Blackmagic RAW, 129 frames, 23.976;
- the CPU coder vs the Python prototype: 154/155 slices identical (one float rounding tie).

## 36. Quality per byte: deadzone, edge-filter strength, luma/chroma balance (31 Oct)

B001_003 frame 0. Rate control holds the size constant, so each change is judged by decoded quality (SDK decode
against the encoder's input planes).

**Deadzone quantisation** (`--rounding R`): AC values become sign(x)·⌊|x| + R⌋. The GPU counter applies the same rule;
R is passed in the unused scan slot 0 of the reciprocal-step table.

| R | 5:1 Y PSNR | 12:1 Y PSNR |
|---|---|---|
| 0.5 (round to nearest) | 57.56 dB | 50.91 dB |
| 0.42 | 58.15 dB | 51.51 dB |
| 0.35 | 58.41 dB | 51.67 dB |
| 0.28 | 58.42 dB | 51.67 dB |
| 0.2 | 58.35 dB | 51.53 dB |

- Chroma improves too (3.20 → 3.02 rms at 5:1).
- The noise on the grey card is kept at 99–100 % of its input level, so there is no visible smoothing of grain.
- **Default R = 0.33.**

**Edge-filter strength** (`--edge-filter v`, the per-pair fields of section 32):
- The pre-filter is generalised to the exact inverse for any v: with m = 16 − 2v, l0 = (mA − vB)/(m² − v²), on the CPU
  and the GPU.
- Blockiness = mean |error step| across block edges ÷ the same inside blocks (1 = no blocking).

| v | 5:1 Y PSNR | 5:1 blockiness | 12:1 Y PSNR | 12:1 blockiness |
|---|---|---|---|---|
| 0 | 58.44 dB | 1.13 | 51.24 dB | 1.29–1.32 |
| 1 | 58.70 dB | 0.98 | 51.57 dB | 1.13–1.15 |
| 2 | **58.74 dB** | 0.84 | **51.77 dB** | 0.98–0.99 |
| 3 (the Video Assist's table) | 58.39 dB | 0.68 | 51.69 dB | 0.84 |

Strength 3 over-smooths block edges; 0 leaves steps. **Default v = 2.**

**Luma/chroma balance** (`--chroma-factor f`, chroma AC matrix = luma × f): judged on the SDK's processed RGB (gamma
2.4) against processing of the exact input planes, at 5:1:

| f | RGB PSNR |
|---|---|
| 0.6 | 46.21 dB |
| 0.8 | 46.45 dB |
| 1.0 | 46.44 dB |
| 1.3 | 46.38 dB |
| 1.7 | 46.11 dB |

The optimum is flat between 0.8 and 1.0, so the flat matrix (f = 1) stays.

**Net, at equal size:** 5:1 goes from 57.56 to **58.74 dB** (+1.2 dB; Cb/Cr 3.20 → 2.81); 12:1 goes from 50.91 to
51.77 dB.

**Rate budget fix:** the slice budget now also reserves the frame's 256-byte alignment, so no frame can exceed N:1.
Full clip, all 129 frames under target:

| Ratio | Speed | Largest frame | Target |
|---|---|---|---|
| 3:1 | 49.5 fps | 4 194 048 | 4 195 328 |
| 5:1 | 70.3 fps | 2 516 480 | 2 517 196 |
| 8:1 | 95.5 fps | 1 573 120 | 1 573 248 |
| 12:1 | 102.5 fps | 1 048 576 | 1 048 832 |

The SDK decodes frames 0 and 128 with every sample written.

**Build note:** the Linux build now writes `build/linux/dng2braw`, so it no longer overwrites the macOS binary in the
project root.

## 37. White-balance metadata the way Blackmagic reads the fp (31 Oct)

**What the Video Assist writes** (per-frame `asct`/`asti`, the same in `wkel`/`wtin`):
- 3282 K / +9 on every clip shot at camera WB 3200 K (VA_0001–0009);
- 5337 K / +18 on VA_0010 (camera WB 5600 K).

dng2braw wrote the DNG calibration's CCT (3244 K) with tint 0. Processed with its own metadata, our file was 4–6 % low
in R and B against VA_0007, although the decoded planes match within 1–2 % (section 31).

**Method:** black-box, through the SDK's public API.
- `braw_lab --wb K T` sets the frame processing attributes White Balance Kelvin/Tint (`CloneFrameProcessingAttributes`,
  then `PopulateFrameStateBuffer`). Forcing 3282/+9 on VA_0007 reproduces its own render exactly.
- Flat synthetic planes are built with dng2braw's forward transform (section 22) from a camera-raw neutral (R/G, B/G
  photosite ratios), injected into VA_0007's processing state, and the kelvin/tint that renders them neutral is
  searched for.
- For the 3200 K setting's AsShotNeutral (0.7619, 1, 0.3951), Blackmagic's processing is neutral at **3286 K / +8.2**,
  against the Video Assist's 3282 K / +9. So the Video Assist's value is the Blackmagic-space white point of the
  camera's neutral.

**Table** (`tools/wb_table_probe.py`, resumable, about 50 s on the M1 Max):
- the camera-raw neutral (ln R/G, ln B/G) that renders neutral, on a grid of 12 temperatures (2500–10000 K) × 6 tints
  (−40 to +60);
- each point by Newton steps on SDK output (patch at green raw 300 to avoid clipping);
- the table is smooth; the 4 extreme warm-and-green corners don't converge and are left out.

**`source/braw_wb.c`** embeds the table and inverts it (bilinear in mired/tint, fine search, then refine):
- B001_003 → 3286 K / +8;
- an OG3K clip → 5330 K / +23. The Video Assist wrote 5337 K / +18 for camera WB 5600 K, so that clip was probably
  shot at 5600 K too.

Neutrals outside the table fall back to the generic CCT.

**Result:** processed with its own metadata, our file is within about 1–2 % of VA_0007: R 0.980, G 0.992, B 0.983
(64-px tile medians; p5–p95 within ±3.5 %). Before, R was 0.956 and B 0.938. The rest is the forward-transform fit.

**Analog gain:** the Video Assist writes exactly ISO/800 × 2^(1/3) (1.259921 at 800, 5.039684 at 3200).
dng2braw used ISO/635 (1.259843); it now writes the exact form.

## 38. Calibration on the four paired takes; the first app build (31 Oct)

**Pairs:** B001_001…004 (internal DNG) with VA_0005…0008 (Video Assist, Q0), the same scene at −4, −2, 0 and +2 EV.
- Each DNG clip's frame 0 was encoded with dng2braw and processed by the SDK.
- It was compared with the Video Assist frame per 64-px tile (medians, excluding clipped tiles).

**Findings:**
- The 0.8 output was low by the same amount at every exposure: R 0.977, G 0.989, B 0.980. So the transform's shape is
  right, and what's off is a small constant gain per channel.
- Per-channel gains on (photosite − black) before T(v), on the CPU and the GPU (`braw_raw_gain`, `--raw-gains`), were
  fitted at the Video Assist's own white balance (`--wb 3282 9`) in two iterations.
- **Result: R 1.0300, G 1.0153, B 1.0200.** Ours/VA is then within ±0.3 % at −4, −2 and 0 EV and 0.7 % low at +2 EV
  (mean 0.998 / 0.998 / 1.001; p5–p95 about ±2–3 %).
- The gains are presumably the HDMI raw's channel scaling against the DNG, plus a ~1.5 % level offset.

**White-balance metadata:**
- The table of section 37 was re-measured with the gains applied. It then gives 3249 K / +6 for the 3200 K neutral,
  further from the Video Assist's 3282 / +9 than the gain-free table's 3286 / +8.
- The gain-free table also matches the second Video Assist point better (5330 K / +23 against 5337 / +18 at camera WB
  5600 K).
- So the Video Assist's kelvin/tint corresponds to the DNG-scaled neutral. dng2braw keeps the gain-free table for
  metadata and the gains for the picture data. `tools/wb_table_to_c.py` regenerates the table.

With each file's own metadata, ours/VA is R 0.997, G 0.998, B 0.991 at all four exposures (was 0.980 / 0.992 /
0.983). The 0.9 % on B is the 4 K / 1-tint metadata difference.

**cDNG to BRAW 0.1** (`build/macos-app/`):
- a native SwiftUI app, modelled on cDNG Raw Convert's window, bundling the universal Metal `dng2braw` (engine 0.9);
- `dng2braw --list-clips` finds clip folders for card and folder-of-clips sources;
- clips in unsupported modes exit with code 3, and the app reports them as "not supported yet";
- the app is Apple-silicon only: the command-line tools' Swift compatibility libraries have no x86_64 slice
  (cDNG Raw Convert's x86_64 link fails the same way);
- smoke test: discovery of 4 clips plus an OG3K clip, conversion with the bundled engine (4 × 24 frames in under 2 s;
  OG3K refused cleanly, no partial file left), and all four imported into Resolve as Blackmagic RAW 3840×2160.

## 39. Audio: the Video Assist's sound track, reproduced (31 Oct)

Read from `Video Assist_0007.braw` (track 3) and reproduced byte for byte in the sample description:

- `hdlr` `soun` "Apple Sound Media Handler", `smhd`, `tref` `tmcd` → track 2, one `elst` edit over the track.
- `mdhd` timescale = the sample rate (48000); duration in samples.
- `stsd` entry `in24`, QuickTime sound description **version 1**: 2 channels, sample-size field 16,
  compression id −1, 48000 Hz; v1 fields samples/packet 1, bytes/packet 3, bytes/frame 6, bytes/sample 2; a `wave`
  atom holding `frma in24` and `enda 0001`.
- **Byte order:** `enda 1` means little-endian, and the data agrees: in 48 000 sample words of a chunk, every one has a
  zero *first* byte (the 16-bit camera audio sits in the top two bytes, LE: `00 lo hi`).
- `stts` (N, 1); `stsz` uniform size 1, count N; `stsc` one-second chunks (48000 samples), the last one shorter;
  `co64` with the chunks **interleaved in `mdat` after the frame during which that second ends**.

dng2braw 0.10 writes the same:
- the WAV is `--wav FILE`, or else the first `.wav` in the clip folder (the fp records one per clip); `--no-audio` leaves
  it out;
- integer PCM of 8 to 32 bits becomes in24 (16-bit: `00 lo hi`);
- it is cut to the picture's length (the fp's WAV runs about 0.1 s longer);
- each chunk starts and ends on a 4096-byte boundary in `mdat`. The frames' own offsets and sizes are unaffected.

**Checked:**
- the `stsd` is byte-identical to the Video Assist's;
- Resolve 21 shows the clip as Blackmagic RAW with 2 ch, 24-bit, 48 kHz Linear PCM;
- AVFoundation decodes the track (`AVAssetReader` to 16-bit PCM), and the result is **bit-identical** to the WAV's
  first 2.5 s (120 120 samples);
- the SDK decodes the frames as before, and `tests/run_tests.sh` passes.

## 40. Other frame sizes: the layout is in the frame header; OG3K works (31 Oct)

The frame header describes its own geometry:
- **0x0a = strip count, 0x0b = band count.** The Video Assist's 0x051f is 5 strips × 31 bands, and the two camera
  files fit too (6K: 0x0320 = 3 × 32; PYXIS: 0x041f = 4 × 31). Section 4 had this field as unknown.
- 0x0c/0x0e the coded width and height, 0x10 the band height, 0x40 the strip widths (sections 4 and 7).

dng2braw 0.11 writes these from each clip's layout instead of copying the UHD values: 5 strips (multiples of 16,
remainder in the last), 72-line bands, the coded height rounded up to 16. The GPU path was already size-independent.

**fp OG3K (DNG 3024×2010 → coded 3024×2016, 5 strips 592/592/592/592/656 × 28 bands)**, checked on the 25 Oct clip:
- SDK decode of frame 0 against the encoder's input planes: Y 60.4 dB, Cb/Cr 65.3 dB (4095 peak), even across slices
  (worst slice RMS 4.06 against 3.91 overall). Same on the CPU and GPU (Metal) paths.
- SDK processing: a clean 3008×2000 picture (DefaultCrop applied from `crop_origin`/`crop_size`).
- Resolve 21: Blackmagic RAW, 3008×2000, 157 frames, audio, Start TC.
- Speed: 58 fps for the whole clip on the M1 Max GPU, at 5:1.

`braw_size_tested()` lists the sizes checked this way (UHD and OG3K). Others exit with code 3 ("not supported yet"),
or can be tried with `DNG2BRAW_ANY_SIZE=1`. The 17 Sep 3K clip is 3032 wide, not a multiple of 16; it needs
right-hand padding first.

## 41. Gyro: Gyroflow reads .braw only as Blackmagic motion data, so dng2braw writes that (31 Oct)

**Why not the Gyroflow Protobuf track alone.** Gyroflow 1.6.3 and its plugins use telemetry-parser at rev 19bc11f
(read from its open-source code). Its detectors are filtered by file extension. The Gyroflow Protobuf detector accepts
mp4, mov, jsonl and dng only, so in a `.braw` only the Blackmagic RAW detector runs. That one needs the text
"Blackmagic Design" plus a BRAW key (`braw_codec_bitrate`) in the file's first or last 5 MB. It then reads every
timed-metadata track whose samples are tagged `mogy` (gyroscope, three LE float32, rad/s) or `moac` (accelerometer,
m/s², negated on reading). The axes are taken as "yxz", and readings are moved back by half the readout time
(`sensor_line_time` × picture rows). The same build reads our ProRes RAW `.mov` gyro track correctly.

**Camera layout** (the Cinema Camera 6K FF clip, A001_09262057_C001.braw):
- two `mebx` tracks, keys `com.blackmagicdesign.motiondata.gyroscope` (local key `mogy`) and
  `...accelerometer` (`moac`), each with `keyd`, `dtyp` (1) and `flps` (3);
- `hdlr` meta "Timed Metadata Media Handler", `gmhd`/`gmin`;
- timescale = sample rate (1143 Hz), one tick per sample; 20-byte samples; chunks of 1024 samples, interleaved.

**dng2braw 0.11** writes the fp's Gyro2 readings the same way:
- **Timing:** each reading is placed on the video timeline so that frame k's first row, at mid-exposure, is at
  k / fps, which is the Blackmagic convention. The camera clock is mapped by the frame spacing from the Gyro2 frame
  sync. The readings are resampled to a uniform grid at the IMU's own rate (2499 Hz).
- **Axes and units:** gyro (Ry, Rx, Rz)·π/180 and accelerometer (−Ay, −Ax, −Az), from the Protobuf message's values.
- `sensor_line_time` = Gyro2 readout ÷ picture rows (12.435 ms over 2000 rows for OG3K).
- A clip-metadata entry `io.github.fpsup.dng2braw.format` = "Blackmagic RAW (a Blackmagic Design format), written by
  dng2braw, not by Blackmagic Design software". This also lets the Blackmagic detector find the file.
- The Gyroflow Protobuf track is still written, after the motion tracks: it carries the per-frame lens data, for any
  later reader that accepts it in `.braw`.

**Checked:**
- **telemetry-parser 19bc11f**, built from source: it detects the `.braw` as Blackmagic, model "SIGMA fp L",
  readout 12.435 ms. Its normalised gyro matches the `.mov` path's within 0.011 °/s (RMS signal 1.2–1.6 °/s), with
  zero lag (cross-correlation over ±2 ms); the accelerometer matches too.
- **Blackmagic RAW SDK:** `IBlackmagicRawClipGyroscopeMotion` and `...AccelerometerMotion` both report 2499 Hz,
  3 floats per sample, and return our values. Gravity reads as +Y, as on the 6K FF clip.
- Not checked yet: Resolve's own gyro stabilisation (it may be limited to Blackmagic camera models), and the Gyroflow
  plugin in Resolve. In this path Gyroflow doesn't get the fpSup per-frame lens fit, so load a lens profile in the
  plugin.

## 42. Frame rate: the sample entry's `bfdn` is the rate's denominator; whole rates must be N/1 (1 Nov)

**Symptom.** cDNG Raw Convert 0.6.1 made `.braw` files from 48 fps fp clips that the SDK opened but would only decode
frame 0; every later frame failed with 0x8000FFFF (and the SDK reported 47.952 fps). The fp's DNGs store FrameRate as
48000/1000, and dng2braw wrote that rate as given: `srte` 48000/1000, movie and media timescale 48000, `stts` delta
1000. Forcing `--fps 48/1` was worse: the SDK refused to open the file.

**Camera files** (the Cinema Camera 6K FF clip and a second Blackmagic camera clip, both 24 fps; the SDK's own sample.braw):
`srte` 24/1, timescales 24, `stts` delta 1. Video Assist files at 23.976: `srte` 24000/1001, timescale 24000, delta 1001.

**Cause, found by transplanting boxes** between a failing file and a 6K FF file re-timed to 48/1 (which opened fine):
the `brvl` sample entry's `bfdn` box. dng2braw had copied the Video Assist's value, 1001 (0x3E9), into every file.
It is the rate's denominator: 1 in the camera files, 1001 in the Video Assist's. With `bfdn` 1001 the SDK reads a
whole-number timescale as an NTSC rate (48000 → 47.952) and the sample times stop matching; with timescale 48 it
can't make a rate at all. Moving only the 6K FF's sample entry into the failing 48/1 file made it decode.

**dng2braw 0.12:**
- normalises the clip rate first: a whole rate becomes N/1 (48000/1000 → 48/1), an NTSC rate N×1000/1001
  (59940/1000 → 60000/1001), anything else is reduced;
- writes `bfdn` = that denominator, and the same rate in `srte`, the timescales, `stts` and the timecode track.

**Checked** through the SDK (braw_lab) on an OG3K 48 fps clip: the clip's own rate, `--fps` 48/1, 25/1, 24000/1001
and 59940/1000 all decode on every frame tested (0, 3, 19). Files made earlier from whole-number rates need
re-encoding (24000/1001 and other NTSC-rate clips were fine).

## 43. Default decode: Blackmagic Film Gen 5, and how it lines up with the DNG (2 Nov)

**What the SDK reports** (new `tools/braw_attrs.cpp`, through `IBlackmagicRawClipProcessingAttributes`):
- Video Assist 12G files from the fp, and dng2braw 0.12's: colour science generation 4, gamma "Rec.709", gamut
  "Rec.709", read from the clip keys `viewing_bmdgen`, `viewing_gamma` and `viewing_gamut`.
- The Cinema Camera 6K FF clip (shot in Film): generation 5, "Blackmagic Design Film", "Blackmagic Design".
- Generations 4 and 5 are both offered for our files (6 too for the 6K FF's), with the same 17 gammas and 16 gamuts.

**dng2braw 0.13** writes the 6K FF's three values by default, so Resolve and Blackmagic RAW Player open the file
flat. `--viewing rec709` writes the Video Assist's.

**Checked against the DNG:** an OG3K frame decoded by the SDK as generation 5 / Film / Blackmagic Design, and the
same DNG developed by cDNG Raw Convert's `dng_view` with Blackmagic's published Gen 5 curve and Blackmagic Wide
Gamut primaries (DNG colour matrices, as-shot white balance, BaselineExposure), agree channel for channel to within
the noise (linear ratios 0.784–0.789 in R, G and B, i.e. the same colour) apart from a uniform 0.34 stop: the SDK
exposes our files 0.34 stop under the DNG's BaselineExposure scale. dng_view's film looks apply that offset, so a
clip looks the same as DNG and as BRAW. Decoded as generation 4 / Film, the same frame comes out darker (about 9 of 255 code values lower on average)
with a different curve; generation 5 is the one that matches.


## 44. Playback through the SDK: Metal pipeline, readable buffers, and a buffer leak (4 Nov, cDNG Raw Convert 0.6.8–0.6.9)

**Symptom.** cDNG Raw Convert's viewer dropped frames on Blackmagic 6K clips at Full resolution.

**Measured on an M1 Max** (Pocket 6K Pro clip, 6048×3200, 23.976 fps; Cinema Camera 6K, 6048×4032, 24 fps):
- The SDK's default (CPU) pipeline decodes a full-resolution 6K frame at 24–28 fps, barely real time. The Metal pipeline (`IBlackmagicRawConfiguration::SetPipeline(blackmagicRawPipelineMetal, nil, queue)`, or a device from `CreatePipelineDeviceIterator`) decodes about 220 fps with 8 frames in flight. Its output matches the CPU's to within 1 of 255 (mean 0.5) in RGBA8.
- With Metal, the processed image is an `MTLBuffer` in private storage: `GetResourceHostPointer` on the default resource manager fails. A resource manager of our own (`IBlackmagicRawConfigurationEx::SetResourceManager`) that makes every buffer `MTLResourceStorageModeShared` lets the CPU read it directly: about 100 fps for 6K including the copy out. The SDK asks it for three buffers per frame (bitstream "rgwc", intermediate and output "rgwg"; 59, 59 and 77 MB for 6K).
- **Leak:** the Metal buffers the SDK hands back through `ReleaseResource` weren't freed (released with ARC on the SDK's threads; something there keeps them alive). The helper's "IOAccelerator (graphics)" memory grew about 1.5 GB/s, to 44–62 GB in under a minute, and the Mac swapped. Recycling them (a free list by size, at most 24) keeps it at 1.4 GB. The resource manager's own count of live buffers stayed at 17 throughout, so the leak doesn't show there; use `vmmap --summary`, row "IOAccelerator (graphics)".
- **Hand-off:** a 6K BGRA8 frame is 77–98 MB, and a pipe managed about 36 fps. POSIX shared memory (10 slots, a 16-byte record per frame on the pipe) gets 104 fps. Showing the frames as IOSurfaces (Core Animation takes them without a main-thread copy or colour conversion), and running the monitoring LUT on the GPU surface to surface, gave every frame on time at Full resolution: 962/962 over 40 s on the 6K FF clip from a Samsung T9, LUT on. Doing the copy and LUT for several frames at once was slower than one at a time.

## 45. Gyro through the Gyroflow (fpSup) plugin v0.1.10: its session cache and its readout (4 Nov, dng2braw 0.15)

- **The plugin's versions:** gyroflow-core rev f1f09194 (ijigen fork), telemetry-parser **d45ebf2**, mp4parse ff910c6. Gyroflow 1.6.3 (finding 41's check) uses telemetry-parser 19bc11f, which isn't an ancestor of d45ebf2; their Blackmagic readers differ.
- **Readout:** d45ebf2 takes a .braw's readout as `sensor_area_captured` rows × `sensor_line_time`; 19bc11f as the picture's (`crop_size`) rows × `sensor_line_time`. With the Video Assist's sensor area (6000×3376 for fp UHD) and our line time per picture row, the plugin got 33.0 ms instead of 21.1 ms: rolling-shutter correction 56 % too strong, and the readings moved 6 ms (it shifts them back by half the readout). **dng2braw 0.15** writes `sensor_area_captured` = the picture's size when there's gyro data (otherwise still the Video Assist's), so both versions get 21.088 ms. The SDK opens the file as before.
- **Session cache:** gyroflow-core keeps each file's parsed telemetry until the host quits, keyed by path, load options, frame size and frame rate (`GyroSource::parse_telemetry_file`), not by content. A .braw first loaded without motion data (0.6.9's, from Gyro2 v0.2.0 footage) parses successfully and is cached; a corrected file written over it isn't read until Resolve restarts. The plugin's "Status: OK" only says the timestamps are reliable, true for every Blackmagic file, with or without readings. The log (`~/Library/Application Support/Gyroflow/gyroflow-openfx.log`) shows a fresh parse as "camera_identifier: Camera ID Brand: Blackmagic…" lines after "Loading video file"; a cached load has none.
- **Checked** in the user's B001_003.braw (147 frames, 0.6.10): 15 321 gyroscope and 15 321 accelerometer readings at 2499 Hz over the whole clip, gyro RMS 1–3 °/s, gravity +9.6 m/s² on Y. **Not yet seen:** a fresh load of a 0.6.11 file stabilising in the plugin. The app labels the BRAW output BETA until then. Resolve's own gyro stabilisation for a non-Blackmagic camera is still untested. cDNG Raw Convert FINDINGS 14–15 have the Gyro2 v0.2.0 format change behind this.

## Next

1. **More paired references** (needs a shoot):
   - Video Assist + internal DNG at 5600 K daylight and at ISO 100, 3200 and 6400 (colour and gain at other settings);
   - DCI 4K, FHD and open-gate recordings (layouts).
2. ~~Audio~~ (done, finding 39). ~~Gyro~~ (done, finding 41; Gyroflow plugin v0.1.10 still to confirm, finding 45).
3. ~~Merge into cDNG Raw Convert~~ (done: cDNG Raw Convert 0.4 has Blackmagic RAW as its third output).
4. **Optional:** per-edge filter strengths (rate-distortion optimised), a perceptual matrix, and GPU entropy coding for
   more speed.
5. **Parsing camera files:** the remaining code space (DC `11110…`, `1111111110…`; AC `111111101101`,
   `111111110111`) and Q0's `FE` first byte.
