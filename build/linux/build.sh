#!/usr/bin/env bash
# Builds dng2prr on Linux: compiles the C core next to dng2prr.py, checks numpy,
# runs the self-test, and (optionally) adds a menu entry for the GUI.
#
#   ./build.sh            build + self-test
#   ./build.sh --desktop  also install a desktop menu entry for dng2prr_gui.py
#
# Needs: gcc (or cc), python3 with numpy. For the GUI also Tkinter
# (Debian/Ubuntu: sudo apt install python3-tk; Fedora: sudo dnf install python3-tkinter).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
SRC="$ROOT/source"
CC="${CC:-$(command -v gcc || command -v cc || true)}"
PY="${PYTHON:-python3}"

[ -n "$CC" ] || { echo "No C compiler found. Install gcc (e.g. sudo apt install build-essential)."; exit 1; }
"$PY" -c "import numpy" 2>/dev/null || {
  echo "numpy is missing for $PY. Install it, e.g.:"
  echo "  sudo apt install python3-numpy      (Debian/Ubuntu)"
  echo "  or: $PY -m pip install --user numpy"
  exit 1; }

# -ffp-contract=off: no fused multiply-add, so output is byte-identical to the reference build
"$CC" -O2 -shared -fPIC -ffp-contract=off -o "$SRC/libprr.so" "$SRC/prr_core.c" "$SRC/dng_lj92.c" "$SRC/dng_jpeg.c" -lm
echo "Built $SRC/libprr.so"
# native command-line encoder (no Python needed to run it)
"$CC" -O2 -ffp-contract=off -o "$SRC/dng2prr" "$SRC/dng2prr_cli.c" "$SRC/prr_core.c" "$SRC/dng_lj92.c" "$SRC/dng_jpeg.c" "$SRC/fsg2.c" -lm -lpthread
echo "Built $SRC/dng2prr (native encoder)"

"$PY" "$ROOT/tests/selftest.py"
"$PY" "$ROOT/tests/test_lj92.py"

if [ "${1:-}" = "--desktop" ]; then
  if ! "$PY" -c "import tkinter" 2>/dev/null; then
    echo "Tkinter is missing, so the GUI won't start. Install python3-tk first."; exit 1
  fi
  APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"; mkdir -p "$APPS"
  cat > "$APPS/fpsup-dng2prr.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=cDNG Raw Convert
Comment=Convert SIGMA fp CinemaDNG clips to ProRes RAW (fpSup, experimental)
Exec=$PY "$SRC/dng2prr_gui.py" %F
Terminal=false
Categories=AudioVideo;Video;
MimeType=inode/directory;
EOF
  echo "Added $APPS/fpsup-dng2prr.desktop (folders can be opened with it from the file manager)."
fi
echo
echo "Done. Command line:  \"$SRC/dng2prr\" OUT.mov CLIP_FOLDER     (native, fastest)"
echo "                     $PY \"$SRC/dng2prr.py\" OUT.mov CLIP_FOLDER"
echo "      Window:        $PY \"$SRC/dng2prr_gui.py\""
