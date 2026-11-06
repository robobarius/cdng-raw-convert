#!/bin/bash
# Builds "cDNG Raw Convert Droplet.app" (a drag-and-drop droplet) from the files in this folder.
# Double-click it in Finder. It's safe to run again; it rebuilds the app.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"
APP="cDNG Raw Convert Droplet.app"
# dng2prr.py and prr_core.c: in ../../source (package layout), next to this script, or one level up
SRC="$(dirname "$HERE")"
for c in "$HERE/../../source" "$HERE" "$(dirname "$HERE")"; do
  if [ -f "$c/dng2prr.py" ]; then SRC="$(cd "$c" && pwd)"; break; fi
done

echo "== Building $APP"
echo

fail() { echo; echo "BUILD FAILED: $1"; echo; read -n 1 -s -r -p "Press any key to close..."; exit 1; }

# 1. Apple's command-line tools (C compiler, osacompile)
if ! xcode-select -p >/dev/null 2>&1; then
  echo "Apple's command-line developer tools are needed. A window will ask to install them."
  xcode-select --install >/dev/null 2>&1
  fail "Install the command-line tools, then double-click this script again."
fi
if ! [ -f "$SRC/dng2prr.py" ] || ! [ -f "$SRC/prr_core.c" ]; then
  echo "Looking in: $HERE"
  if ! ls "$HERE" >/dev/null 2>&1; then
    fail "Terminal isn't allowed to read this folder. Open System Settings > Privacy & Security > Files and Folders (or Full Disk Access), allow Terminal, then run this again. Or move the folder out of Desktop/Documents."
  fi
  ls -la "$HERE"
  fail "dng2prr.py / prr_core.c not found in ../../source, next to this script, or one folder up."
fi
[ -f "$HERE/droplet.applescript" ] || fail "droplet.applescript not found next to this script"

# 2. A Python 3 that has numpy (try common locations; install numpy if needed)
PY=""
for c in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
  if [ -x "$c" ] && "$c" -c "import numpy" >/dev/null 2>&1; then PY="$c"; break; fi
done
if [ -z "$PY" ]; then
  for c in /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    [ -x "$c" ] || continue
    echo "Installing numpy for $c ..."
    "$c" -m pip install --user numpy >/dev/null 2>&1 || "$c" -m pip install --user --break-system-packages numpy >/dev/null 2>&1
    if "$c" -c "import numpy" >/dev/null 2>&1; then PY="$c"; break; fi
  done
fi
[ -n "$PY" ] || fail "Couldn't find or set up a Python 3 with numpy. Try: python3 -m pip install --user numpy"
echo "Python: $PY ($("$PY" -c 'import numpy,sys;print(sys.version.split()[0],"numpy",numpy.__version__)'))"

# 3. Compile the encoder core (+ the lossless-JPEG DNG decoder when present)
CSRC=("$SRC/prr_core.c"); [ -f "$SRC/dng_lj92.c" ] && CSRC+=("$SRC/dng_lj92.c"); [ -f "$SRC/dng_jpeg.c" ] && CSRC+=("$SRC/dng_jpeg.c")
# universal (Apple silicon + Intel) when possible, so the app can be shared
if cc -O2 -shared -ffp-contract=off -arch arm64 -arch x86_64 -o libprr.dylib "${CSRC[@]}" 2>/dev/null; then
  echo "Compiled libprr.dylib (universal: arm64 + x86_64)"
else
  cc -O2 -shared -ffp-contract=off -o libprr.dylib "${CSRC[@]}" || fail "compiling the C core"
  echo "Compiled libprr.dylib ($(uname -m) only)"
fi

# 4. Build the droplet and put the tool inside it
rm -rf "$APP"
osacompile -o "$APP" droplet.applescript || fail "osacompile"
RES="$APP/Contents/Resources"
cp "$SRC/dng2prr.py" "$RES/" && mv libprr.dylib "$RES/" && printf %s "$PY" > "$RES/python_path"
/usr/bin/plutil -replace CFBundleIdentifier -string org.fpsup.dng2prr "$APP/Contents/Info.plist"
/usr/bin/plutil -replace CFBundleName -string "cDNG Raw Convert Droplet" "$APP/Contents/Info.plist"
# accept folders and .dng files dropped on the icon
/usr/bin/plutil -replace CFBundleDocumentTypes -json '[{"CFBundleTypeName":"Folders and DNG files","CFBundleTypeRole":"Viewer","LSItemContentTypes":["public.folder","public.directory","com.adobe.raw-image","public.camera-raw-image","public.item"]}]' "$APP/Contents/Info.plist"
codesign --force --deep --sign - "$APP" >/dev/null 2>&1 || echo "(note: ad-hoc signing skipped)"

# 5. Quick self-test of the bundled tool, plus the full byte-level self-test if it's available
"$PY" "$RES/dng2prr.py" --list-clips /tmp >/dev/null 2>&1 || fail "self-test of the bundled tool"
if [ -f "$SRC/../tests/selftest.py" ]; then
  cp "$RES/libprr.dylib" "$SRC/libprr.dylib"
  if ST="$("$PY" "$SRC/../tests/selftest.py" 2>&1)"; then echo "$ST" | tail -2
  else echo "$ST"; fail "tests/selftest.py"; fi
fi

echo
echo "Done: $(pwd)/$APP"
echo "Drag it to Applications or the Dock, then drop fp clip folders (or a card) on it."
open -R "$APP"
echo
read -n 1 -s -r -p "Press any key to close..."
