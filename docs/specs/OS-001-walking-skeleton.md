# OS-001 — Community OS walking skeleton (host-first)

Status: done — 001a and 001b merged, 2026-10-10. Open: CMD_SNAPSHOT2 calibration (hw-session), DL text strategy before the style guide. · Maintainer decisions in force: D-OS-1 (new tree), D-OS-2 (C++17), D-OS-3 (headless simulator first), ADR 0001 (UI look), ADR 0003 (12 typed tracks), ADR 0004 (step format), ADR 0005 (GPLv3 core, MIT SDK).

## Goal
A new top-level `os/` tree that builds warning-free, runs headless on the host, and renders the main **pattern view** to PNG from a scripted input scenario. The pixels come from interpreting the same FT812 display-list words the device will receive. The portable libraries also cross-compile for the MK66 (static libraries only, size report, no image).

Out of scope: booting on hardware, audio, sequencer timing, storage, MIDI, the module loader, SDL2 live mode, any change outside the new tree.

## Layering (enforced)
`core` ← `hal` (contracts only) ← `eve` (display-list encoding) ← `ui` (renderer, style, font) ← `app` (screens, state, input handling). `platform/host` and `sim` implement HAL contracts; nothing below `app` includes them. CMake targets express the dependencies (no cycles), and a ctest check (`tools/check_layers.py`, stdlib Python) fails when a header include breaks the order.

## Split
- **OS-001a — tree, core, HAL contracts, DL encoder.**
  - `CMakeLists.txt` is a standalone project.
    - Host build: `-DO3OS_SANITIZE=ON` enables ASan/UBSan.
    - MK66 cross build: reuses `../cmake/arm-none-eabi-toolchain.cmake` and the firmware CPU flags (`-mcpu=cortex-m4 -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb`). It builds the static libraries and prints `arm-none-eabi-size` per library.
  - `LICENSE` is the GPL-3.0 text (`/usr/share/common-licenses/GPL-3`). Every source file carries an SPDX header.
  - **`core`:**
    - a lock-free single-producer/single-consumer ring (power-of-two capacity, documented memory ordering, wrap-around tested);
    - fixed-capacity vector/string without heap;
    - an input event type (button/pad/encoder with timestamp);
    - a monotonic tick clock interface.
  - **`hal` contracts:** `Display` (a sink that accepts DL words and frame begin/end), `Input` (event source), `Clock`. Headers only, with a host fake each in `platform/host`.
  - **`eve`:** `constexpr` encoders for the DL subset below, bit-exact to the FT81X Series Programmer Guide (cite section/page per opcode), plus a `DisplayListBuilder` that writes into a fixed buffer through `hal::Display`.
    - **DL subset:** CLEAR_COLOR_RGB, CLEAR, COLOR_RGB, COLOR_A, BEGIN (BITMAPS, LINES, RECTS), END, VERTEX2F, VERTEX2II, VERTEX_FORMAT, LINE_WIDTH, SCISSOR_XY, SCISSOR_SIZE, BITMAP_HANDLE, BITMAP_SOURCE, BITMAP_LAYOUT (+_H), BITMAP_SIZE (+_H), CELL, SAVE_CONTEXT, RESTORE_CONTEXT, DISPLAY.
    - The UI emits only these words: no coprocessor widgets and no CMD_TEXT. Text is drawn as glyph bitmaps from our own font.
  - **Tests:** doctest (vendor `third_party/doctest/doctest.h` with its licence), cases for the ring, the containers and every encoder against hand-computed words.
- **OS-001b — simulator, font, UI, pattern view, scenarios** (after 001a merges).
  - **`sim/eve_raster`:** interprets the DL subset into an 800×480 RGBA framebuffer. It covers clear, color/alpha (source-over blending), rects and lines with LINE_WIDTH in 1/16 px and square-ended (no antialiasing; a documented fidelity gap), L1/L4/L8 bitmaps with CELL, scissor, and save/restore. It also models RAM_G as a byte array, where the font lives. Unknown words fail loudly.
  - **PNG:** `stb_image_write.h` (vendored, licence kept).
  - **Font:** a permissive monospace bitmap font (e.g. Spleen, BSD-2-Clause; verify the LICENSE and keep it). It is converted at build time by a stdlib Python script into an FT81x L1 bitmap and a metric table, usable unchanged on the device later. Sizes: one body face of about 8×16 and one small face of about 6×12. Marked PROVISIONAL until the style guide.
  - **`ui`:** an immediate-mode renderer on `eve`: `panel`, `frame` (1 px border), `text`, `text_grid`, `highlight_row`, `rule`. `style.h` holds a PROVISIONAL palette of at most 16 colours in the OctaMED/Amiga spirit of ADR 0001 (flat greys and blues, one strong highlight); no colour literals outside `style.h`.
  - **`app`:** pattern view, per `docs/design/product-ux.md` §4 (main pattern mock-up).
    - Content: top status strip (transport, BPM, pattern, instrument); track header row; 12 tracks of which 4–6 are visible with horizontal scrolling; per track Note / Instr / Vol / FX1 / FX2 (ADR 0004); a selected-row highlight; F1–F8 soft-key labels in the bottom strip.
    - Data is static demo data compiled in; no storage.
    - Input handling: arrows move the cursor, the encoder scrolls steps, F-keys print their label in a status line. Button names come from §3 of the product/UX draft.
  - **`o3sim`:** `o3sim --scenario <file> --out <dir>` runs the app loop on a fixed tick with no wall-clock dependence and writes PNGs at `snap` lines.
    - **Scenario grammar** (one command per line, `#` comments): `wait <ms>`, `press|release|tap <button>`, `pad <0-47>`, `encoder <±n>`, `snap <name>`.
    - Scenarios in `sim/scenarios/` include at least `pattern_basic.txt`: initial view, cursor moved right ×3 and down ×5, encoder +8, scrolled to tracks 7–12.
  - **Golden check:** ctest runs the scenarios and compares the SHA-256 of each RGBA framebuffer with `sim/goldens.txt`. An explicit `--update-goldens` rewrites the file; the diff is then reviewed by looking at the PNGs. No PNGs are committed.

## Rules
Everything in the fw-feature Rules block applies, plus:
- **Flags:** C++17 (`-std=gnu++17`); `-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion` for `core`, `eve`, `ui` and `app` (relax `-Wconversion` only in `sim`/third-party with a comment); `-fno-rtti -fno-exceptions` on every target, host included.
- **Memory:** no heap in `core`, `hal`, `eve`, `ui` or `app` (fixed buffers sized by `constexpr`); `sim` and `platform/host` may use the standard library.
- **No `double` in** `core`/`eve`/`ui`/`app`.
- **Determinism:** two runs of a scenario produce identical framebuffers.
- **Boundaries:** don't touch the root `CMakeLists.txt`, `tools/` or `tests/`.

## Done when
1. `cmake -S . -B build-os-host -DO3OS_SANITIZE=ON && cmake --build build-os-host && ctest --test-dir build-os-host --output-on-failure` exits 0 with zero warnings: unit tests, the layer check, and (001b) the scenario goldens.
2. `cmake -S . -B build-os-mk66 -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/mk66-toolchain.cmake -DO3OS_PLATFORM=mk66 && cmake --build build-os-mk66` builds `core`, `eve`, `ui` and `app` warning-free and prints their sizes (text/data/bss), which go into the report.
3. (001b) `build-os-host/sim/o3sim --scenario sim/scenarios/pattern_basic.txt --out <tmp>` writes the PNGs. The maintainer views them, and they follow ADR 0001: flat panels, thin borders, dense monospace, a strong selected-row highlight, a limited palette.
4. Reviewer verdict APPROVE, or every finding folded.

## Provisional choices (maintainer may change; not ADRs)
Font family, palette values, visible track count (4–6), exact pattern-view layout. All live in `ui/style.h` or `app/` constants, and nothing else depends on them.
