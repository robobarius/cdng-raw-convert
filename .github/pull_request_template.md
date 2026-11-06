## What this changes

<!-- What does it do, and why? Link an issue if there is one: "Fixes #12". -->

## How it was checked

- [ ] `bash build/linux/build.sh` passes (encoder self-test and lossless-JPEG round trips)
- [ ] gyro tests pass (`tests/test_fsg2.c`), if the gyro code changed
- [ ] the macOS app builds, if app or encoder code changed
- [ ] output checked in a real decoder (Resolve, Blackmagic's SDK, `--verify`), if the files we write changed. Which, and which versions:

## Notes for the reviewer

<!-- Anything you're unsure about, screenshots for UI changes. -->
