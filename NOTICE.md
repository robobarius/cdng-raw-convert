# Licensing and notices

## The project as a whole: GPL-3.0-or-later

cDNG Raw Convert as distributed here (the macOS app, and the `dng2prr` and `dng2braw` command-line tools with their gyro support) is licensed under the **GNU General Public License, version 3 or (at your option) any later version**. The full text is in [`LICENSE`](LICENSE).

That is because the fpSup Gyro2 conversion, `source/fsg2.c` and `source/fsg2.h` (and their copies in `source/braw/`), is a C translation of GPL-3.0-or-later code from [ijigen/gyroflow-plugins](https://github.com/ijigen/gyroflow-plugins) (`common/src/fsg2.rs`, `lensfit.rs`, `cdng.rs`). Anything built with it must be GPL-3.0-or-later.

## Individual files: mostly LGPL-2.1-or-later

Each source file says its own licence in its header:

| Files | Licence |
|---|---|
| `source/fsg2.c`, `source/fsg2.h`, `source/braw/fsg2.c`, `source/braw/fsg2.h`, `tests/test_fsg2.c` | GPL-3.0-or-later ([`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt)) |
| Everything else in `source/` and `build/`, and the tests that carry an LGPL header | LGPL-2.1-or-later ([`LICENSES/LGPL-2.1.txt`](LICENSES/LGPL-2.1.txt)) |
| Scripts and files without a licence header | LGPL-2.1-or-later, as part of this project |
| `tests/gyroflow.proto` | MIT or Apache-2.0, from [AdrianEddy/telemetry-parser](https://github.com/AdrianEddy/telemetry-parser) |

The LGPL-2.1-or-later files can be reused on their own under the LGPL, for example in a library (`libprr`, the Python version `source/dng2prr.py`) that leaves the gyro code out. To build the encoder without it, leave `fsg2.c` out and compile with `-DNO_GYRO`.

## Third-party code and data

- **FFmpeg** (LGPL-2.1-or-later): `source/prr_core.c` contains the ProRes RAW codebook tables and scan order from libavcodec (`prores_raw.c`, `proresdata.c`), Copyright (c) 2023-2025 Paul B Mahol, Copyright (c) 2025 Lynne and the FFmpeg developers.
- **Gyroflow (fpSup) plugin** (GPL-3.0-or-later): the Gyro2 decoder in `fsg2.c` is a translation of ijigen's code; see above.
- **telemetry-parser** (MIT or Apache-2.0): `tests/gyroflow.proto`, the Gyroflow Protobuf schema, by AdrianEddy.
- **Blackmagic RAW SDK:** not included. The Blackmagic RAW playback helper (`source/braw_view/`) is built against the headers of Blackmagic's free SDK if it's installed on the build machine, and loads the SDK at run time from the user's own installation of the SDK, Blackmagic RAW Player or DaVinci Resolve. No part of the SDK is in this repository or in the app.

## Not affiliated

This is experimental, community-built software. It is not Apple's, Blackmagic Design's or SIGMA's software, and it is not licensed, certified or endorsed by them. The ProRes RAW and Blackmagic RAW writers were made by testing files against those companies' own decoders.

Apple and ProRes are trademarks of Apple Inc., registered in the U.S. and other countries. Blackmagic RAW and DaVinci Resolve are trademarks of Blackmagic Design Pty. Ltd. SIGMA is a trademark of SIGMA Corporation. CinemaDNG is a trademark of Adobe.

**Use at your own risk, and keep your original camera files.**
