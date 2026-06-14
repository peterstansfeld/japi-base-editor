# Context for AI-assisted coding on Japi Base Editor (JBE)

This document is for people who want to work on Japi Base Editor together
with an AI coding assistant (Claude, Cursor, Aider, ChatGPT, or any future
tool). It gives the assistant the conventions and workflow that are not
visible from the code alone. For the *what* read [`README.md`](README.md);
for the user-facing feature reference read [`JBE_MANUAL.pdf`](JBE_MANUAL.pdf).

JBE is built **on top of the Japi Base platform** and inherits its rules, so
read the platform's context as well:
[Japi Base context.md](https://github.com/JanFromBelgium/japi-base/blob/master/context.md).

## Repository layout — the short version

Only the editor *component* lives in this repo:

- `editor/jbe.c` + `editor/jbe.h` — the editor itself: editor logic written
  against the Japi Base API.
- `editor/jbe_main.c` — the host (simulator) entry point and embedding loop;
  `editor/jbe_main_pico.c` — the on-device entry point.
- `editor/ui_filelist.c/.h` — a reusable scrollable directory-list widget
  (used by File > Open and the Japi Commander).
- `sim/` — the host build. It does **not** contain the platform; it links
  against JapiBase's canonical simulator and headers (see Build).
- `tools/` — the screenshot generator used for the README images.
- `Font Editor/` — a standalone Linux tool for designing the device font.

The platform API the editor is written against (`japi_base.h`) and the
desktop simulator (`japi_sim.c`) live in the **JapiBase** repository, not
here. The editor never calls anything outside `japi_base.h`; that seam is
what guarantees the same source compiles on the Pico.

## Development model — sim first, Pico last

JBE is developed on Linux against the Japi Base simulator. The same C source
compiles against the on-Pico `japi_base.h`. **Maximise everything in the sim
before the Pico port** — the Pico is the closing act, not the rehearsal
venue.

Sim fidelity is good enough for editor logic, but **not** for keyboard layout
details, font rendering and VGA timing. Those have to be verified on real
hardware.

## Build

The host build expects the **JapiBase** repository checked out next to this
one (the sim Makefile points at `../../JapiBase`):

```
parent/
  JapiBase/          # the platform: japi_base.h + sim/japi_sim.c
  JapiBaseEditor/    # this repo
```

Then:

```
cd sim
make jbe      # build the editor on the host simulator
make demo     # build, drop a sample file on A: and open it
make test     # run the headless editor + simulator self-tests
```

Dependency-free: gcc only, `-std=c11`. The same `editor/*.c` source compiles
for the Pico against the on-device `japi_base.h` (via `jbe_main_pico.c`).

## Coding conventions

- **Stay inside the seam.** The editor may use only `japi_base.h`. Anything
  that will not exist on the Pico (host libc file I/O, threads, unbounded
  allocation) is off-limits in editor code.
- **Deferred-request pattern.** Heavy actions (open, new, run, save) are *not*
  executed inside the key handler. The handler sets a `*_request` flag on the
  editor state and the embedding loop (`jbe_main.c` and the on-device host)
  does the file/flash work at shallow stack depth. The RP2350 core-0 stack is
  only ~2 KB, so a deep `key-handler -> flash-write` chain overflows it and the
  screen goes blank ("no signal").
- **Large structs are static, never on the stack** — same 2 KB stack reason.
- **English comments and self-documenting names**, including in headers.
- **A menu is a capability inventory.** Every action a user can take is
  reachable from the menu bar; a shortcut is an accelerator, not the only way
  in.
- **New syntax keywords** go into the scheme tables in `jbe.c`; extra schemes
  are loaded at runtime from `C:config/syntax/` on the device.

## Workflow conventions (when contributing changes)

- **Maximise everything in the sim before the Pico port.** Sim-only is not
  release-ready, but it is where the bulk of the work happens.
- **Safety-anchor tag first** before any non-trivial refactor, so a
  `git reset --hard` always has somewhere to land.
- **Hardware verify before merge.** Build-success in the sim is not "done":
  keyboard layout, font rendering and VGA timing are not faithfully simulated
  and have to be exercised on real hardware.
- **No force-push to `master`.**

## JBE keyboard notes

These reflect the Japi Base keyboard driver:

- **Ctrl + any printable ASCII works.** Bind freely with the macro form
  `JAPI_KEY_CTRL(c)` (for example `JAPI_KEY_CTRL('B')`), and keep the macro
  rather than a bare hex code so the intent stays readable.
- **Ctrl+M is Enter.** The keyboard layer maps it to carriage return (the
  50-year-old ASCII convention), so it cannot be a free binding — which is why
  the macro shortcuts are Ctrl+T (record) and Ctrl+P (play).
- **The numeric keypad types ASCII when Num Lock is on** (digits, `. + - * /`
  and Enter); with Num Lock off it acts as navigation. The editor therefore
  sees plain characters from the keypad and needs no special handling.

## Things not to do

- Don't reach around the `japi_base.h` seam for convenience. If the sim makes
  something easy that the Pico cannot do, it does not belong in the editor.
- Don't do file or flash writes deep inside the key handler — use the
  deferred-request pattern.
- Don't assume the sim's keyboard layout or font is accurate; verify those on
  hardware.

## Licence

BSD-3-Clause, the same as the Japi Base platform. See [`LICENSE`](LICENSE).
