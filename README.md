# Japi Base Editor (JBE)

JBE is the on-device code editor for the
[Japi Base](https://github.com/JanFromBelgium/japi-base) retro computer
platform (Raspberry Pi Pico 2 / RP2350). It feels like the classic
Turbo Pascal / QuickBASIC IDEs: a dark-blue full-screen editor, a menu bar
at the top, a status line at the bottom, and everything reachable from the
keyboard.

![Japi Base Editor running on the platform](screenshots/showcase.png)

**This is the first version.** It runs on real Japi Base hardware over
VGA + PS/2. A second version with a built-in contextual help system is on
the way.

## The hardware

Japi Base is a real machine. In the
[Japi Base repository](https://github.com/JanFromBelgium/japi-base) you will
find how to breadboard or perfboard the required hardware. You will also find
the PCB there together with instructions on how to order one from JLCPCB.

## How it is developed

JBE is built **on Linux against a Japi Base simulator**. The simulator
implements the same `japi_base.h` API as the real hardware (VGA text mode of
127 columns × 64 rows, PS/2 keyboard input, file I/O against the LittleFS
"flash floppy"), so the editor's source can be developed, run and tested
entirely on a desktop machine. The exact same source files compile against
the on-Pico implementation of `japi_base.h`.

This keeps the iteration loop fast (no flashing, no SD shuffling) while
guaranteeing that we stay inside the platform's seam — JBE may not touch
anything that will not exist on the Pico.

## What it does

- **Two open files at once** — one per pane of the split-screen view. Edit two
  files side by side, or two views of the same work.
- **File menu** — New, Open, Save, Save As, Close, all with an unsaved-changes
  guard so you never lose work by accident.
- **Stream and block selection.** A *stream* selection is the normal kind: it
  flows from the start point to the cursor across line ends, like selecting a
  sentence or a run of code. A *block* (column) selection grabs a rectangle of
  columns across several lines, regardless of how long each line is. The block
  mode is what makes column work easy — line up a set of assignments, cut a
  column of numbers out of a table, or comment a vertical strip.
- **Clipboard** with stream/block awareness (cut, copy, paste).
- **Toggle line comment** on the current line or selection.
- **Undo / Redo** with word-grain coalescing, multi-line steps, and a 32 KB
  per-buffer history budget.
- **Find** — incremental, case-insensitive, wrap-around.
- **Replace** — with per-match Y / N / All confirmation.
- **Keystroke macros** — record a sequence once, then replay it.
- **Syntax highlighting** for Z80 assembly and Basic, plus extra schemes
  loaded from `C:config/syntax/` on the device.
- **Long-line wrapping** with a marker glyph.
- **Horizontal split-screen**, Turbo-Pascal style — toggle the windowed view,
  swap focus between panes, and move the divider.
- **Full CP437 character entry** — accented letters, box-drawing glyphs and
  symbols, so layouts like French AZERTY type correctly.

### Keyboard

Every menu opens with an **Alt** accelerator, and the common actions also have
a **Ctrl** shortcut. Grouped by menu:

**File** (Alt+F)

| Action | Alt | Ctrl |
|---|---|---|
| New | Alt+F, N | Ctrl+N |
| Open | Alt+F, O | Ctrl+O |
| Save | Alt+F, S | Ctrl+S |
| Save As | Alt+F, A | — |
| Close | Alt+F, C | Ctrl+W |

**Edit** (Alt+E)

| Action | Alt | Ctrl |
|---|---|---|
| Cut | Alt+E, T | Ctrl+X |
| Copy | Alt+E, C | Ctrl+C |
| Paste | Alt+E, P | Ctrl+V |
| Toggle comment | Alt+E, G | Ctrl+A |
| Undo | Alt+E, U | Ctrl+Z |
| Redo | Alt+E, R | Ctrl+Y |

**View** (Alt+V)

| Action | Alt | Ctrl |
|---|---|---|
| Toggle windowed view | Alt+V, W | — |
| Other pane | Alt+V, O | Ctrl+Tab |
| Move divider up | Alt+V, U | Ctrl+Up |
| Move divider down | Alt+V, D | Ctrl+Down |

**Search** (Alt+S)

| Action | Alt | Ctrl |
|---|---|---|
| Find | Alt+S, F | Ctrl+F |
| Replace | Alt+S, R | Ctrl+R |

**Macro** (Alt+M)

| Action | Alt | Ctrl |
|---|---|---|
| Record start/stop | Alt+M, R | Ctrl+T |
| Play | Alt+M, P | Ctrl+P |

See [`JBE_MANUAL.pdf`](JBE_MANUAL.pdf) for the full user-facing documentation.

## Japi Commander

The editor ships with **Japi Commander** (Run > Japi Commander, or Ctrl+J), an
early two-pane file tool. For now it copies files between the drives — its first
job is to copy files from the SD card (A:) onto the built-in flash drive (C:),
which is exactly what you need to get programs and config onto the machine. This
is a v0: more file operations (move, delete, rename, make-directory) are
planned, and it is the seed of a small file manager.

## A note on Basic

The Japi Base **Basic** language is a separate work in progress and is **not**
part of this repository. When JBE is later built into the full Japi Base
Computer the menu bar gains a *Run > Basic Interpreter* entry to run the
current program, but that interpreter is developed on its own track.

## What is coming

- **A built-in contextual help system** — open straight to the page or item
  you have selected. This is the headline feature of the next version.
- **More Japi Commander** — move, delete, rename, make-directory.
- More syntax schemes and editor polish.

## License

See `LICENSE`.
