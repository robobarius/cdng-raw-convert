# Checking a build against a real ProRes RAW decoder

`selftest.py` (one folder up) checks that a build produces the right bytes. This folder checks the thing that actually matters: that a real decoder, Apple's (inside DaVinci Resolve or Final Cut Pro), turns those bytes into the right picture.

**Do this before sharing a build for a new platform, and after any change to the encoder.** All of the calibration in FINDINGS.md came from measurements like this one, and the magenta bug in 0.1-pre was caught by one.

## 1. Make the test clips

```
python3 make_validation_clips.py wedge_out
```

This writes four one-frame clips, `wedge_q1/q2/q4/q8.mov`, one per quality level.

**The picture:** 32 vertical grey patches from 0.09% to 90% of white, at 1920×1080.

**How the raw data is set up:**
- It's written as if shot under tungsten light. Red and blue are scaled down, and the header asks the decoder to scale them back up (×1.36 and ×2.72).
- A correct decode is therefore neutral grey, with brightness proportional to each patch's level.

## 2. Set up a measuring project in Resolve

Create a new project, then open File → Project Settings → Color Management and set:

| Setting | Value |
|---|---|
| Color science | DaVinci YRGB Color Managed |
| Color processing mode | Custom; untick "Automatic color management"; tick "Use separate color space and gamma" |
| Timeline color space | Rec.709 / **Linear** |
| Output color space | Rec.709 / **Linear** |

Also set the timeline resolution to **1920×1080** in Master Settings.

Leave each clip's Camera Raw settings at their defaults (Decode Using: Camera metadata).

## 3. Export a still of each clip

**Automatic:** with Resolve running and the project open, run:

```
python3 resolve_export_stills.py wedge_out
```

It needs external scripting enabled in Preferences → System → General.

**By hand, for each clip:**
1. Make a timeline from the clip.
2. On the Color page, right-click the viewer → Grab Still.
3. In the Gallery, right-click the still → Export, and choose TIFF.

## 4. Measure

```
python3 measure_still.py wedge_out/stills/wedge_q*.tif
```

Each still should report **PASS** for linearity and neutrality. The last line should say the brightness scale is the same across all four stills, within 2%.

For comparison, `reference_results_resolve_21.1.1_macos.txt` holds the numbers from a correct decode on macOS:
- **Scale:** about 1.11.
- **R, G and B:** identical from 5% up.
- **Small deviations** in the darkest judged patches, which are only Resolve's 10-bit still export rounding.

## What a failure looks like

| Symptom | Likely cause |
|---|---|
| R/G and B/G well above 1 (magenta), different per quality | The tile quantiser or code-value mapping is wrong. This was the 0.1-pre bug: quantiser 1 assumptions applied at 2/4/8. |
| Neutral, but G/level not constant (bent curve) | The code-value mapping (`LIN_CURVE`/`LIN_KNOTS`, black at u = 4096) is broken. |
| Blocky or garbage tiles | Entropy-coding or tile-layout bug. Run `selftest.py` and decode with FFmpeg to localise it. |
| Picture shifted or striped by one pixel | Bayer phase is wrong (odd widths, crop or padding). |
| Clip won't import | Container problem. Compare against a known-good file with an MP4/QuickTime atom viewer. |

## Other decoders

The same clips and script work for any decoder that can export a linear still:
- **Final Cut Pro:** set the library to wide gamut HDR and export a linear-light still. We haven't tested this.
- **Resolve on Windows or Linux:** use the same project settings. Please send in your results. This is the main open question for the ports.
- **FFmpeg** decodes these files too, though not with Apple's decoder.
  - `ffmpeg -i wedge_q1.mov -f rawvideo -pix_fmt bayer_rggb16le out.raw` gives the linear Bayer data, before white balance. Black is 256 and white is the header's sensel range + 256.
  - On x86, use an FFmpeg built with `--disable-asm` (FINDINGS.md finding 8).
  - That's useful for debugging, but it isn't proof that Apple's decoder agrees (see FINDINGS.md finding 2).
