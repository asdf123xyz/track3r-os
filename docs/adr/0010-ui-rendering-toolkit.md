# ADR 0010 — UI rendering, type scale and graphics toolkit

Status: **Accepted** (2026-10-11). Maintainer concerns: "mock-ups look cheap", "font size must matter", "decent graphics library for waveform editing", "well optimised", "easy to redesign screens". Design from the UI review, adopted by the maintainer.

## Decisions
1. **Our own thin immediate-mode toolkit**, no LVGL or Bridgetek libraries.
   - LVGL needs a pool allocator, timers and the coprocessor FIFO, costs 150–250 KB of flash, and its simulator path is not the EVE.
   - Ours targets about 20 KB of flash and 2 KB of state, and a frame stays a pure function of state.
   - EmbeddedVideoEngine (MIT) is reference only, for DMA bursts and coprocessor recovery.
2. **Display-list subset grows to:** VERTEX_TRANSLATE_X/Y, LINE_STRIP, EDGE_STRIP_A/B, POINTS/POINT_SIZE, BLEND_FUNC and translucent COLOR_A fills. Add `hal::Display::upload(addr, bytes)` for RAM_G.
   - No coprocessor widgets or ROM fonts: they are rounded/antialiased, can't be restyled, and contradict ADR 0001.
   - The 2048-word cap applies to RAM_DL, so CMD_APPEND does not raise it; fewer vertices do.
3. **Type scale.** Four roles, all L1 pixel faces at native size. Measured against ~135 ppi viewed at ~50 cm: data cap ≥ 12 px, labels ≥ 9 px, nothing actionable below 9 px.

   | Role | Size | Use |
   |---|---|---|
   | Display | 16×32 | BPM, pattern number, value being edited |
   | Data | 10×20 (Terminus, OFL) | grid, lists, parameters |
   | Label | 8×16 | labels |
   | Micro | 6×12 | axis ticks only |

   L4 coverage is used for MCU-rendered curves and waveforms, never for text.
4. **Visual system (style guide v1).**
   - Two materials: light, desaturated grey-blue chrome strips with dark text, and a dark data pane.
   - One strong blue selected row.
   - Orange only for the cursor and record-arm.
   - Transport as a filled badge.
   - F-keys as key-caps.
   - A 16×16 L1 icon face, recoloured.
   - Track headers carry a type badge and a 4 px meter.
   - Grid at 24 px pitch with 20 px cells: beats marked by a bright row number, bars by a 1 px rule, empty dashes dimmed to about 2:1 contrast.
   - Motion: none for navigation; playhead and meters at 30 Hz with peak hold; record-arm blinks at 2 Hz; a one-frame flash when a pad writes a step.
5. **Pattern grid** uses tile faces (256 hex pairs, 123 note cells) and one VERTEX_TRANSLATE_X per track column.
   - A step costs about 5 words instead of 13, so the worst case drops from about 1855 to about 800 words and 6 tracks × 24 rows becomes possible.
   - The tiles use about 25 KB of RAM_G.
6. **Graphics recipes.**
   - **Waveform overview:** an MCU-rendered L1 min/max column bitmap from a peak cache (min/max per 64 samples, built by a cooperative job at load), uploaded only on zoom or scroll. LINE_STRIP only below 2 samples per pixel.
   - **Markers, selection, playhead:** RECTS, with a translucent rectangle for the selection.
   - **Envelope, LFO, filter response:** LINE_STRIP plus an EDGE_STRIP fill, with RECTS handles.
   - **Meters and scope:** fed only from the audio→main SPSC ring.
   - **Sliders:** rectangular bars.
7. **Frame pipeline.**
   - Dirty-flagged layers (chrome, content, overlay, live), each a pre-encoded word span in SRAM. Two 8 KB display-list buffers; one DMA burst to RAM_DL, then a swap.
   - Rate tiers: on change; 30 Hz for live elements; 20 Hz for the scope; zero SPI traffic when idle.
   - Uploads of at most 4 KB per pass, run by the job runner.
   - SPI DMA runs below I2S priority. Never spin on SPI.
   - SPI clock: 20 MHz today, 30 MHz to be measured in a `hw-session`.
8. **Redesign ease, standalone first** (ADR 0009 principle).
   - Theme tokens: 16 colours, 4 faces, spacing, borders.
   - Per-screen `constexpr Layout`: named rectangles derived from tokens, no literals.
   - Components: Strip, Badge, SoftKeys, FieldRow, List, Bar, Meter, Curve, Wave.
   - Screens render (model, layout, theme). Actions never see rectangles.
   - Compiled defaults always ship. SD overrides parse into the same fixed structs, limited to declared knobs; each override is validated by a worst-case render into a scratch buffer and falls back with a notification.
   - Flash cost: about 15–20 KB with CMD_INFLATE for faces, tiles and icons.
9. **Iteration loop and guards.**
   - `o3sim --theme/--layout` reads the SD pack format, so the simulator stands in for the SD slot.
   - `--watch` regenerates a gallery: every screen × typical and worst-case state, as a PNG contact sheet with a diff against goldens.
   - One ctest per screen asserts a worst case of ≤ 1792 words.
   - The simulator antialiases LINES, LINE_STRIP and POINTS like the chip, calibrated later against CMD_SNAPSHOT2.

## Order
- **OS-003a:** display-list extension, simulator antialiasing, RAM_G upload, VERTEX_TRANSLATE, budget harness, gallery.
- **OS-003b:** style guide v1, type scale, palette v2, tile and icon faces, restyled pattern view.
- **Then OS-002b**, on top of these.
- **Later:**
  - 003c: components and an Instrument page generated from descriptors (with 002d).
  - 003d: peak cache, waveform and the Chop screen.
  - 003e: curves, meters and a scope with a fake producer.
  - A `hw-session` to calibrate SPI speed and antialiasing.

## Amendment 1 — colour themes (maintainer, 2026-10-11)
- **Built-in themes.** Three are compiled in:
  - **Amiga** (default): Workbench grey-blue chrome, dark data pane, orange accent.
  - **Black**: high-contrast dark, for stage use.
  - **White**: light, for daylight.
- **SD themes.** Further themes are SD data packs (standalone-first: the built-ins always exist). A theme is the token set: palette, plus optional face and spacing overrides within declared knobs. Switching happens at runtime from Config, without a reboot.
- **Verification.**
  - The gallery renders every screen × state × theme.
  - A ctest checks the minimum contrast of every text/background token pair, per theme: ≥ 4.5:1 for data and labels, ≥ 3:1 for large display text and non-text indicators.
  - Theme packs from the SD pass the same check on load or are rejected with a notification.

## Note — LVGL re-examined at the maintainer's request (2026-10-11)
LVGL would add ready-made breadth: widgets, styles, flex/grid layouts, animations and a visual designer. Its flash cost would fit. We still keep our own toolkit, for four reasons:
- The core screens (pattern grid, waveform, chop, envelopes) would be custom widgets under LVGL too.
- LVGL does not manage the FT812's 2048-word display-list cap.
- Its PC path renders through a different engine, so simulator pixels would no longer be device pixels, and automatic visual verification would be lost.
- It needs RAM for its pool and buffers, and that RAM is shared with audio.

Open option: a time-boxed spike that builds LVGL 9 for the MK66 with the ADR 0001 look, measures flash, RAM and the pattern screen, and lets the decision be revisited with numbers.

## Amendment 2 — `ui::plot` library (maintainer, 2026-10-11: "nice, fast, efficient plots: waves, x/y…")
A dedicated plotting module on top of the 003a primitives. Every plot declares a maximum number of display-list words, and decimation guarantees it holds whatever the data size.

**Plot types:**
- **Wave view.**
  - Multi-level peak cache (min/max per 64/256/1024/4096 samples, a mipmap), built by a cooperative job at load.
  - Overview drawn as an MCU-rendered L1/L8 column bitmap in RAM_G: one vertex, uploaded only on zoom or scroll, in 4 KB DMA chunks from an aligned staging buffer.
  - LINE_STRIP below 2 samples per pixel.
  - Stereo lanes, slice markers, loop region, translucent selection, playhead.
  - The int16 min/max kernels use M4 SIMD (`__SSUB16` + `__SEL`, two samples per cycle).
- **Curve / XY plot.** A generic series (float or Q15) decimated to at most one point per pixel column, or Ramer–Douglas–Peucker for smooth curves. Drawn as LINE_STRIP with an EDGE_STRIP translucent fill. Axes, grid and ticks use the Micro face. Axes can be linear, log-frequency or dB. Used for envelopes, LFO shapes and filter response.
- **XY scope / Lissajous.** POINTS with additive blend (ONE/ONE) and a short phosphor trail from decaying alpha, at 30 Hz.
- **Spectrum.** CMSIS-DSP `arm_rfft_fast_f32` (256/512) in a job at 20 Hz, fed from the audio→main ring. Drawn as bars or a fill.
- **Meters.** RECTS with peak hold, about 4 words per bar.

**Targets (estimates, measured in a `hw-session`):**
- Wave column bitmap regenerated from the cache in ≤ 3 ms for 740 columns.
- ≤ 400 words per plot.
- Spectrum ≤ 2 % CPU.
- Live plots never touch audio buffers directly.

**Verification:**
- A plot gallery: synthetic sweeps, a generated drum-like signal, envelopes, filter curves, Lissajous. Every plot in every theme.
- Word-budget ctests per plot.
- Host benchmarks for the relative cost of the kernels.
- MK66 cycle benches queued for the next `hw-session`.
