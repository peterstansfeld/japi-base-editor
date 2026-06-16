#!/usr/bin/env bash
# One-command setup to build and test the Japi Base Editor on the host simulator.
#
# The editor builds against the Japi Base *platform* simulator, which lives in
# the separate (public) JapiBase repository under sim/. This script makes sure
# that repo is checked out right next to this one -- with the exact folder name
# the build expects (JapiBase) -- and then builds and tests the editor on the
# simulator. No Raspberry Pi Pico hardware is needed; gcc and git are enough.
#
# Run it from anywhere (e.g. straight after cloning just this repo):
#     ./sim/setup.sh
set -euo pipefail

# Locate this repo (the parent of the sim/ folder that holds this script) and the
# directory it lives in, so the platform repo can be cloned as a sibling.
SIM_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EDITOR_ROOT="$(cd "$SIM_DIR/.." && pwd)"
PARENT="$(cd "$EDITOR_ROOT/.." && pwd)"

PLATFORM_URL="https://github.com/JanFromBelgium/JapiBase.git"
PLATFORM_DIR="$PARENT/JapiBase"

# The sim Makefile refers to the platform as ../../JapiBase, so the folder name
# must be exactly "JapiBase". Cloning explicitly into that path guarantees it
# (a bare `git clone <url>` would name the folder after the URL slug instead).
if [ -d "$PLATFORM_DIR/.git" ]; then
    echo "[setup] Platform already present: $PLATFORM_DIR"
else
    echo "[setup] Cloning the platform next to the editor: $PLATFORM_DIR"
    git clone "$PLATFORM_URL" "$PLATFORM_DIR"
fi

if [ ! -f "$PLATFORM_DIR/sim/japi_sim.c" ]; then
    echo "[setup] ERROR: $PLATFORM_DIR/sim/japi_sim.c not found." >&2
    echo "[setup] The JapiBase folder must sit next to this repo and be named 'JapiBase'." >&2
    exit 1
fi

echo "[setup] Building the editor on the simulator (make jbe)..."
make -C "$SIM_DIR" jbe

echo "[setup] Running the editor's headless tests (make test)..."
make -C "$SIM_DIR" test

cat <<EOF

[setup] Done -- the editor builds and its tests pass on the simulator.
Run it with:
    cd "$SIM_DIR" && ./jbe A:scratch.txt
EOF
