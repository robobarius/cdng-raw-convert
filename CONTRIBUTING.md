# Contributing to cDNG Raw Convert

Thanks for helping. This is a small community project for SIGMA fp shooters (the [fpSup](https://ijigen.github.io/fpSup/) community), and contributions of every size are welcome: test reports from your own footage, bug reports, documentation fixes, ports, and code.

## Ways to help

- **Test with your own clips** and report what you find (open an issue). The most useful reports say which recording mode (UHD, DCI 4K, FHD, OG3K, OG2K…), bit depth, frame rate and fpSup build the clip came from, and which output format and settings you used.
- **Report bugs** with the *Bug report* issue form. Attach the app's log (the Log panel can be copied), and a screenshot if it's visual.
- **Suggest features** with the *Feature request* form.
- **Port it.** [`PORTING.md`](PORTING.md) explains how the encoders work and what's platform-specific. Windows and Linux builds of the command-line encoder exist; a native GUI for them doesn't yet.
- **Improve the docs.** [`FINDINGS.md`](FINDINGS.md) (ProRes RAW, compressed CinemaDNG, gyro) and [`source/braw/FINDINGS.md`](source/braw/FINDINGS.md) (Blackmagic RAW) record what was measured and why the code does what it does.

## Making a change

1. **Fork** the repository on GitHub, and clone your fork.
2. **Make a branch** for your change: `git checkout -b fix-og2k-crop`.
3. **Build and test** (below). For anything that changes the files the encoders write, also check the result in a real decoder: see "Changing the output" below.
4. **Commit** with a message that says what changed and why.
5. **Open a pull request** against `main`, and fill in the template. Small, focused pull requests are easier to review than large ones.

If you're planning something big, open an issue first so we can talk it through.

## Building

**macOS app** (macOS 12 or later, Apple's command-line developer tools; no Xcode needed):

```bash
bash "build/macos-app/Build cDNG Raw Convert.command"
```

The app is put in `macOS/` (which git ignores). Blackmagic RAW *playback* in the viewer needs Blackmagic's free [Blackmagic RAW SDK](https://www.blackmagicdesign.com/developer/product/camera) installed when you build; without it the app builds fine and shows a message for `.braw` clips. The SDK is never copied into the repository or the app.

**Command-line encoder and tests, Linux or macOS** (a C compiler and Python 3 with numpy):

```bash
bash build/linux/build.sh            # builds dng2prr and libprr, runs tests/selftest.py and tests/test_lj92.py
cc -O2 -ffp-contract=off -o test_fsg2 tests/test_fsg2.c -lm && ./test_fsg2    # gyro conversion tests
```

**Windows:** `build\windows\build.bat`.

GitHub runs the same builds and tests on every pull request (see the *Actions* tab).

## Changing the output

The encoders write formats that are checked against other companies' decoders, so a change that looks harmless can break files in Resolve. Before changing anything listed in section 6 of [`PORTING.md`](PORTING.md) ("Things you must not change without re-validating in a real decoder"):

- run `tests/selftest.py` (it compares the bytes with `tests/reference.json`), and
- for ProRes RAW, run the Resolve validation in [`tests/validation/`](tests/validation/README.md); for Blackmagic RAW, decode the result with Blackmagic's SDK or Resolve; for compressed CinemaDNG, use `--verify`; for gyro data, `tests/verify_gyro2.py` and `tests/check_gyro.py`.

Say in the pull request what you checked and with which software versions.

## Ground rules

- **Black-box only.** The Blackmagic RAW and ProRes RAW writers were made by testing files against the decoders, plus open-source code (FFmpeg). Don't contribute code, tables or data taken from disassembled or decompiled proprietary software, or from SDKs and documents whose licences don't allow it.
- **Don't add Blackmagic's SDK, or any other proprietary SDK, to the repository.**
- **No camera footage in the repository.** Test clips are large and may be personal. Link to them in an issue instead, or describe how to make a test pattern.
- **Licences:** by contributing you agree that your contribution is licensed under the licence of the file you change (see [`NOTICE.md`](NOTICE.md)): LGPL-2.1-or-later for most files, GPL-3.0-or-later for the gyro code. New files should carry an LGPL-2.1-or-later header unless they're derived from GPL code.
- Be kind: see the [Code of Conduct](CODE_OF_CONDUCT.md).

## Releases

Versions follow `0.MINOR.PATCH` while the project is experimental. Each release gets a section in [`CHANGELOG.md`](CHANGELOG.md), a git tag (`v0.6.11`), and a GitHub Release with the built macOS app attached.
