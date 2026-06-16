# Host build (run the editor on the simulator)

This folder is how you build and run **Japi Base Editor on your desktop**,
against the Japi Base *host simulator* instead of real Pico hardware. The same
editor source that runs on the Raspberry Pi Pico 2 compiles and runs here, so
you can develop and test without flashing anything.

The simulator itself is **not** in this repo — it is part of the platform and
lives in the [JapiBase repository](https://github.com/JanFromBelgium/JapiBase)
under `sim/`. The `Makefile` here just links the editor component against that
simulator (`japi_sim.c`) plus the platform headers. It expects JapiBase checked
out next to this repo, with exactly these folder names:

```
parent/
  JapiBase/          # the platform: japi_base.h + sim/japi_sim.c
  JapiBaseEditor/    # this repo
```

## Quick start

From a fresh clone of the editor, **`./setup.sh`** clones the platform as the
sibling `JapiBase` and builds + tests in one step — no manual cloning needed.

## Build and run

Dependency-free: gcc only. Run these from inside this folder:

```sh
make jbe      # build the editor on the host simulator -> ./jbe
make demo     # build, drop a sample file on A: and open it
make test     # run the editor's own headless tests (jbe_test)
make clean    # remove the built binaries
```

Run the editor with a file: `./jbe A:scratch.txt` (a drive letter + path, just
like on the device).

## simdisk_A/

The simulator presents this directory as the **A:** drive. Two sample files are
committed so there is something to open out of the box:

- `scratch.txt` — the default file the editor opens when you pass no argument.
- `demo.z80` — a small Z80 assembly sample to show the syntax highlighting.

Anything else the editor writes here (and the whole `simdisk_C/` flash drive)
is ignored by git — it is local scratch data.
