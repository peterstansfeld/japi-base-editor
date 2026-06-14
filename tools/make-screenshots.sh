#!/usr/bin/env bash
# Build the screenshot generator and produce the manual's PNGs from the REAL
# VGA 8x12 font + 6-bit palette, so the images match the hardware exactly.
# Host-only tool; needs gcc + ffmpeg. Safe to re-run any time.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"          # JapiBaseEditor
JB="$HERE/../JapiBase"
OUT="$HERE/screenshots"
mkdir -p "$OUT"

gcc -std=c11 -Wall -Wextra -O2 -DJAPI_SIM \
  -I"$JB/Japi Base Pico 2" -I"$JB/sim" -I"$JB/sim/shim" -I"$HERE/editor" \
  -o "$HERE/tools/screenshot" \
  "$JB/sim/japi_sim.c" "$HERE/editor/jbe.c" "$HERE/editor/ui_filelist.c" "$HERE/tools/screenshot.c"

work="$(mktemp -d)"                                # keep sim disk artifacts out of the repo
for scene in overview select filemenu showcase; do
  ( cd "$work" && "$HERE/tools/screenshot" "$scene" "$work/$scene.ppm" )
  ffmpeg -y -loglevel error -i "$work/$scene.ppm" "$OUT/$scene.png"
  echo "  -> screenshots/$scene.png"
done
rm -rf "$work"
echo "Done."
