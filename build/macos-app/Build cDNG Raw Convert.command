#!/bin/bash
# Builds the native "cDNG Raw Convert.app" (SwiftUI window + native C encoder).
# Needs only Apple's command-line developer tools (no Python, no Xcode).
# Double-click it in Finder. The app is put in the package's macOS/ folder.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/source"
OUTDIR="$ROOT/macOS"
APP="$OUTDIR/cDNG Raw Convert.app"
VERSION=0.6.11
BUILD="$(mktemp -d /tmp/dng2prr-build.XXXXXX)"

fail() { echo; echo "BUILD FAILED: $1"; echo; if [ -t 0 ]; then read -n 1 -s -r -p "Press any key to close..."; fi; exit 1; }
echo "== Building cDNG Raw Convert.app ($VERSION, native)"; echo

if ! xcode-select -p >/dev/null 2>&1; then
  echo "Apple's command-line developer tools are needed. A window will ask to install them."
  xcode-select --install >/dev/null 2>&1
  fail "Install the command-line tools, then double-click this script again."
fi
if ! ls "$HERE" >/dev/null 2>&1; then
  fail "Terminal isn't allowed to read this folder. Allow Terminal in System Settings > Privacy & Security > Files and Folders (or Full Disk Access), or move the folder out of Desktop/Documents."
fi
for f in dng2prr_cli.c prr_core.c dng_lj92.c dng_jpeg.c fsg2.c fsg2.h; do [ -f "$SRC/$f" ] || fail "source/$f not found"; done
command -v swiftc >/dev/null || fail "swiftc not found (it comes with the command-line tools; try: xcode-select --install)"

# 1. native encoder (universal when possible)
CFLAGS="-O2 -ffp-contract=off -Wall"
GPUF=(-DFP_METAL -fno-objc-arc "$SRC/fp_metal.m" -framework Metal -framework Foundation)   # GPU transform stages (fp_metal.m)
if cc $CFLAGS -arch arm64 -arch x86_64 -o "$BUILD/dng2prr" "$SRC/dng2prr_cli.c" "$SRC/prr_core.c" "$SRC/dng_lj92.c" "$SRC/dng_jpeg.c" "$SRC/fsg2.c" "${GPUF[@]}" -lm 2>"$BUILD/cc.log"; then
  echo "Encoder: universal (arm64 + x86_64), Metal"
else
  cc $CFLAGS -o "$BUILD/dng2prr" "$SRC/dng2prr_cli.c" "$SRC/prr_core.c" "$SRC/dng_lj92.c" "$SRC/dng_jpeg.c" "$SRC/fsg2.c" "${GPUF[@]}" -lm 2>"$BUILD/cc.log" || { cat "$BUILD/cc.log"; fail "compiling the encoder"; }
  echo "Encoder: $(uname -m) only"
fi
"$BUILD/dng2prr" --version >/dev/null || fail "the encoder doesn't run"

# 1b. Blackmagic RAW encoder (dng2braw): Metal on the GPU, tuned for Apple silicon, universal when possible
B="$SRC/braw"
for f in dng2braw_cli.c braw_format.c braw_encode.c braw_wb.c fsg2.c braw_metal.m dng_frontend.c dng_jpeg.c dng_lj92.c; do [ -f "$B/$f" ] || fail "source/braw/$f not found"; done
BSRCS=("$B/dng2braw_cli.c" "$B/braw_format.c" "$B/braw_encode.c" "$B/braw_wb.c" "$B/fsg2.c" "$B/braw_metal.m" "$B/dng_frontend.c" "$B/dng_jpeg.c" "$B/dng_lj92.c")
BFLAGS=(-O3 -Wall -DBRAW_METAL -fno-objc-arc -framework Metal -framework Foundation -lm)
if clang -arch arm64 -arch x86_64 -Xarch_arm64 -mcpu=apple-m1 "${BFLAGS[@]}" -o "$BUILD/dng2braw" "${BSRCS[@]}" 2>"$BUILD/braw.log"; then
  echo "BRAW encoder: universal (arm64 tuned for M1+, x86_64), Metal"
else
  clang "${BFLAGS[@]}" -o "$BUILD/dng2braw" "${BSRCS[@]}" 2>"$BUILD/braw.log" || { cat "$BUILD/braw.log"; fail "compiling the BRAW encoder"; }
  echo "BRAW encoder: $(uname -m) only"
fi
"$BUILD/dng2braw" --version >/dev/null || fail "the BRAW encoder doesn't run"

# 1b2. DNG playback helper (dng_view): the fpSup DNG reader + a fast monitoring development, multithreaded
if clang -O3 -Wall -arch arm64 -arch x86_64 -Xarch_arm64 -mcpu=apple-m1 -I"$B" -o "$BUILD/dng_view" "$SRC/dng_view/dng_view.c" "$B/dng_frontend.c" "$B/dng_jpeg.c" "$B/dng_lj92.c" -lm -lpthread 2>"$BUILD/dngview.log" \
   || clang -O3 -Wall -I"$B" -o "$BUILD/dng_view" "$SRC/dng_view/dng_view.c" "$B/dng_frontend.c" "$B/dng_jpeg.c" "$B/dng_lj92.c" -lm -lpthread 2>>"$BUILD/dngview.log"; then
  echo "DNG playback helper: built"
else echo "(note: DNG playback helper not built, the viewer falls back to Core Image)"; cat "$BUILD/dngview.log"; fi

# 1c. Blackmagic RAW playback helper (optional): needs Blackmagic's free Blackmagic RAW SDK installed to build.
#     It loads Blackmagic RAW at run time from the SDK, Blackmagic RAW Player or DaVinci Resolve; nothing of
#     the SDK is copied into the app.
BSDK="${BRAW_SDK:-/Applications/Blackmagic RAW/Blackmagic RAW SDK/Mac}"
if [ -f "$BSDK/Include/BlackmagicRawAPIDispatch.cpp" ]; then
  BVF=(-std=c++14 -O2 -fobjc-arc -I"$BSDK/Include" "$SRC/braw_view/braw_view.mm" "$BSDK/Include/BlackmagicRawAPIDispatch.cpp" -framework CoreFoundation -framework Foundation -framework Metal -o "$BUILD/braw_view")
  if clang++ -arch arm64 -arch x86_64 "${BVF[@]}" 2>"$BUILD/brawview.log" || clang++ "${BVF[@]}" 2>>"$BUILD/brawview.log"; then
    echo "BRAW playback helper: built (Metal)"
  else echo "(note: BRAW playback helper not built; see errors)"; cat "$BUILD/brawview.log"; fi
else
  echo "(note: Blackmagic RAW SDK not found at $BSDK: .braw playback will be off in the viewer)"
fi

# 2. the app (SwiftUI, macOS 12+), universal when possible
SW="$HERE/CDNGRawConvert.swift"
ok=()
for arch in arm64 x86_64; do
  if swiftc -O -parse-as-library -target $arch-apple-macos12.0 "$SW" -o "$BUILD/app_$arch" 2>"$BUILD/swift_$arch.log"; then ok+=("$BUILD/app_$arch"); fi
done
if [ ${#ok[@]} -eq 0 ]; then
  if swiftc -O -parse-as-library "$SW" -o "$BUILD/app_native" 2>"$BUILD/swift_native.log"; then ok=("$BUILD/app_native")
  else cat "$BUILD/swift_arm64.log" "$BUILD/swift_native.log"; fail "compiling the app (Swift errors above)"; fi
fi
lipo -create "${ok[@]}" -output "$BUILD/app" || fail "lipo"
echo "App: $(lipo -archs "$BUILD/app")"

# 3. bundle
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BUILD/app" "$APP/Contents/MacOS/cDNG Raw Convert"
cp "$BUILD/dng2prr" "$APP/Contents/MacOS/dng2prr"
cp "$BUILD/dng2braw" "$APP/Contents/MacOS/dng2braw"
[ -f "$BUILD/braw_view" ] && cp "$BUILD/braw_view" "$APP/Contents/MacOS/braw_view"
[ -f "$BUILD/dng_view" ] && cp "$BUILD/dng_view" "$APP/Contents/MacOS/dng_view"
cp "$ROOT/LICENSE" "$ROOT/NOTICE.md" "$ROOT/README.md" "$APP/Contents/Resources/" 2>/dev/null
if [ -f "$HERE/AppIcon.png" ]; then
  IS="$BUILD/AppIcon.iconset"; mkdir -p "$IS"
  for s in 16 32 128 256 512; do
    sips -z $s $s "$HERE/AppIcon.png" --out "$IS/icon_${s}x${s}.png" >/dev/null 2>&1
    s2=$((s * 2)); sips -z $s2 $s2 "$HERE/AppIcon.png" --out "$IS/icon_${s}x${s}@2x.png" >/dev/null 2>&1
  done
  iconutil -c icns "$IS" -o "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null || echo "(note: icon skipped)"
fi
cat > "$APP/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>cDNG Raw Convert</string>
  <key>CFBundleIdentifier</key><string>org.fpsup.cdngrawconvert</string>
  <key>CFBundleName</key><string>cDNG Raw Convert</string>
  <key>CFBundleDisplayName</key><string>cDNG Raw Convert</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>$VERSION</string>
  <key>CFBundleVersion</key><string>$VERSION</string>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>LSMinimumSystemVersion</key><string>12.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSHumanReadableCopyright</key><string>fpSup community, GPL-3.0-or-later. Not Apple, Blackmagic Design or SIGMA software.</string>
</dict></plist>
PLIST
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || echo "(note: ad-hoc signing skipped)"
rm -rf "$BUILD"

echo
echo "Done: $APP"
echo "Open it from Finder (first time: right-click > Open). You can drag it to Applications."
if [ -z "${CI:-}" ]; then open -R "$APP"; fi        # show it in Finder (not on a build server)
echo
if [ -t 0 ]; then read -n 1 -s -r -p "Press any key to close..."; fi
