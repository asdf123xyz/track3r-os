# OS-003 — UI toolkit, type scale, style guide (ADR 0010)

Status: 003a merged 2026-10-11; 003b merged; 003d implemented (as built, at the end; in review) · Source of truth: `docs/adr/0010-ui-rendering-toolkit.md`.

## 003a — Display-list extension, simulator antialiasing, RAM_G upload, budget harness, gallery
**Scope:**
- **`eve`.** Bit-exact encoders, with guide citations and tests, for VERTEX_TRANSLATE_X/Y, BEGIN(LINE_STRIP, EDGE_STRIP_A/B, POINTS), POINT_SIZE and BLEND_FUNC.
- **`hal::Display::upload(addr, bytes)`.**
  - Documented contract: RAM_G bounds, and uploads land before the next committed frame.
  - The host fake records uploads.
- **`sim`.**
  - Interpret the new words: translate offsets, line strips, edge-strip fills (scissored), points with POINT_SIZE, and the BLEND_FUNC modes the toolkit uses (at least SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ONE/ONE, ZERO).
  - Coverage antialiasing for LINES, LINE_STRIP and POINTS that approximates the chip. Document the model and mark it [A] until a CMD_SNAPSHOT2 calibration.
  - RECTS with half-pixel-centred vertices stay exact.
  - `upload` writes into the simulated RAM_G.
- **Budget harness.** A reusable test helper that renders a screen in a named worst-case state and asserts ≤ 1792 words, printing a per-screen table. The existing pattern-view budget test moves onto it.
- **Gallery.** `o3sim --gallery <dir>` renders every screen × state listed in a registry. For now that is the pattern view: typical, worst case, and cursor at the end.
  - It writes the PNGs, a contact-sheet PNG, and a diff list against goldens.
  - `--watch` is optional in 003a. If omitted, document it as a follow-up.
- **Goldens.** Existing goldens must stay unchanged, because the UI draws no LINES yet. Add new unit goldens for the new primitives: hand-checkable small cases.

**Done when:**
- Clean host build with ASan/UBSan, ctest green, 0 warnings.
- MK66 build with 0 warnings and sizes before/after.
- Encoder tests bit-exact.
- Simulator tests for each new primitive, including blend and antialiasing.
- Budget harness and gallery run in ctest.
- Reviewer approves.

## 003b — Style guide v1, type scale, themes, tiles, restyled pattern view
Goal: the first screenshot that looks premium. It must pass the ADR 0001 north-star test ("OctaMED on an impossibly powerful 1990s Amiga") and the ADR 0010 rules.

**Scope**
1. **Fonts** (ADR 0010 §3). All L1 at native size.
   - **Data and Display faces:** Terminus 10×20 and 16×32. Licence SIL OFL 1.1. Download only from the official Terminus release (terminus-font.sourceforge.net / its official download) into an empty scratch directory, verify the licence file, and vendor the BDFs plus the licence under `third_party/fonts/terminus/`.
   - **Label face:** Spleen 8×16 (already vendored).
   - **Micro face:** Spleen 6×12, axis ticks only.
   - Fonts are converted at build time by `tools/font_convert.py`, with aligned output.
2. **Theme tokens** (`ui/theme.h`).
   - Each theme defines 16 colours, the 4 faces, spacing and borders.
   - Built-in themes:
     - **Amiga** (default): light, desaturated grey-blue chrome with dark text; dark data pane; one strong blue selected row; orange only for the cursor and record-arm.
     - **Black**: high-contrast dark.
     - **White**: light, for daylight.
   - `style.h` palette literals move into the theme tables. Colour literals stay banned elsewhere; the checker rule follows the move.
3. **Layouts and components.**
   - A per-screen `constexpr Layout`: named rectangles derived from theme metrics, no literals in screens.
   - Components: `Strip`, `Badge` (transport PLAY green / STOP grey, track type A/M/E), `SoftKeys` (F-keys drawn as key-caps: an inverted "F1" box plus label text), `FieldRow`, `Bar`, `Meter` (4 px track-header meter).
   - Screens render from (model, layout, theme). Actions never see rectangles.
4. **Tile faces for the grid** (ADR 0010 §5).
   - 256 hex-pair tiles and 123 note-cell tiles (C-0…B-9, `---`, `OFF`), generated from the Data face into RAM_G via `upload`.
   - One VERTEX_TRANSLATE_X per track column.
   - Target worst case is about 800 words. **Remove the interim 1920 limit:** every state must be ≤ 1792.
5. **Icon face.** 16×16 L1 icons drawn in-house as data in a script: play, stop, record, loop, mute, solo, lock, folder, module, MIDI, warning. Recoloured with COLOR_RGB. No third-party icon sets.
6. **Pattern view restyle** (ADR 0010 §4).
   - Two materials: chrome and data pane.
   - Display face for BPM and pattern number in the top strip.
   - Data face for the grid.
   - Grid at 24 px pitch with 20 px cells. Beats are marked by a bright row number and bars by a 1 px rule; there is no four-row banding.
   - Empty dashes dimmed to about 2:1 contrast.
   - Track headers carry the type badge, the name and a 4 px meter fed by fake demo levels.
   - Selected header shown with bright text and an underline.
   - F-key key-caps at the bottom.
   - Visible tracks: as many as the layout allows at the Data face (at least 5).
7. **Runtime theme switch.**
   - Action `ui.theme.next`, bound in the built-in global context to Shift+Master (provisional; Config does not exist yet).
   - Scenario command `theme <name>`.
   - `o3sim --theme <name>`.
8. **Checks.**
   - The gallery renders every screen × state × theme.
   - A contrast ctest checks every text/background token pair per theme: ≥ 4.5:1 for data and labels, ≥ 3:1 for Display text and indicators.
   - Budget ctest: ≤ 1792 for every state × theme.
   - Goldens are regenerated, because the look changes on purpose. Each new PNG is viewed with the Read tool, and the changed-golden list goes in the report.
9. **Style guide v1** (`docs/ui/style-guide.md`): grid and metrics, type scale with cap heights, the three palettes with contrast tables, components, icon list, motion rules, the budget rule, and how to add a theme or layout (SD override knobs come later).

**Done when**
- Clean host build with ASan/UBSan; ctest green with 0 warnings.
- MK66 build with 0 warnings and sizes reported, including font, tile and icon flash cost.
- Every state ≤ 1792 words in all themes.
- Contrast tests pass.
- The engineer has viewed the gallery contact sheets for all three themes.
- The maintainer views them before merging.
- The reviewer approves.

## 003c–003e (outline)
- **003c:** components (Strip, Badge, SoftKeys, FieldRow, List, Bar) and an Instrument page generated from parameter descriptors (with OS-002d).
- **003d `ui::plot`** (ADR 0010 amendment 2; specified 2026-10-11). It lives in `ui/plot/` and is part of `ui`.
  - **Peak-cache mipmap.**
    - Builds min/max pyramids from int16 mono or stereo frames into caller-provided fixed storage, which is SDRAM on the device.
    - Incremental update after a range edit costs O(edit + log n).
    - A query returns per-column min/max for any zoom and offset in O(columns).
    - The cost is stated in bytes per sample.
  - **Wave view.** Draws a waveform into a `Rect` in two modes, chosen per call and proven pixel-equivalent where they overlap:
    - (a) vector: EDGE_STRIP or LINE_STRIP over the min/max columns;
    - (b) bitmap: an L1 or L4 column bitmap rendered into a staging buffer and uploaded to a RAM_G region, at about 10 DL words regardless of width.

    Overlays: selection, playhead, loop start/end, slice markers, zero line, and a clipping tint. Zoom and scroll take the mipmap level into account.
  - **RAM_G map:** one header that owns fixed RAM_G regions (fonts and tiles from 003b, wave bitmaps, spare), with `static_assert` that nothing overlaps and that each region is 4-byte aligned.
  - **Curve/XY.**
    - Envelope and filter-response curves with fill (EDGE_STRIP_B), a log-frequency axis and a dB axis, grid and labels in the Micro face, and draggable handles drawn as points.
    - The math (log mapping, dB, biquad magnitude response) is pure and host-tested.
  - **XY scope:** a Lissajous plot from a stereo block, decimated to a word budget, using POINTS or LINE_STRIP.
  - **Spectrum.**
    - A real FFT of 256 or 512 points: CMSIS-DSP `arm_rfft_fast_f32` on the MK66; on the host, the same CMSIS source (Apache-2.0, vendor only what is needed, with its notice) or our own reference FFT behind the same interface.
    - Then log-binned bars, smoothing and peak hold.
  - **Meters:** peak and RMS bars with hold and clip, on a dB scale through a LUT, in vertical and horizontal forms.
  - **Theme tokens only.** New tokens are allowed only through the 003b theme process: contrast pairs, specimen and style guide.
  - **Gallery and budget.**
    - The plot gallery is every plot type × 3 themes, drawn from deterministic synthetic signals (sine, noise, drum hit, chirp, stereo phase).
    - Each state is composed with the standard chrome and stays at or below 1,792 words.
    - Ctest goldens.
  - **Host benches:** mipmap build per Msample, draw per frame and FFT per block. They are relative figures; MK66 cycle benches go into the `hw-session` list.
  - **Done when:**
    - Host ctest is green with ASan/UBSan and 0 warnings.
    - The MK66 build has 0 warnings, with `ui` text/data/bss before and after.
    - Kernel unit tests pass (mipmap equals brute-force min/max at every level; log/dB mapping; FFT against a DFT oracle within tolerance).
    - Every gallery state is within budget in all themes.
    - The maintainer has viewed the contact sheets.
    - The reviewer approves.
- **003e:** Chop screen and Instrument envelope/filter pages built on `ui::plot`.

## 003d as built (2026-10-11)

Delivered as specified in `ui/plot/` (part of `ui`; layout and commands in `docs/architecture/os-tree.md`, the look in `docs/ui/style-guide.md` section 11,
what the simulator is asked in `sim/README.md`). Where it differs from the text above:

- **FFT.** Written here, not CMSIS-DSP (the spec allowed "our own reference FFT behind the same interface" for the host; it is the
  same code on the MK66 so that the host tests run what the device runs, and it costs no table flash). `RealFft` has
  `arm_rfft_fast_f32`'s layout, so a platform can call CMSIS behind `forward()`; "own radix-2 against CMSIS on the device" is a
  `hw-session` bench item. Nothing is vendored (`THIRD_PARTY.md`).
- **Vector mode** is two `EDGE_STRIP_B` fills (the wave colour from each column's maximum down, the lane's background from just under
  its minimum down) on pixel centres, which makes it the L1 bitmap's picture exactly; it bounds its words by grouping columns
  (`max_vertices`, 192 by default: 394 words), and below one frame a pixel is a `LINE_STRIP` through the frames with dots. The lane
  must be a flat pane fill (the second fill paints over what is beneath it). That the chip rounds an edge strip's first row the way
  the simulator does is the one `[A]` the equivalence rests on.
- **Overlays** are translucent (zero line, clip bands, selection, loop tint) or one-pixel rules. **Clip is drawn in `Accent`** (maintainer decision
  after review): the palette has no red and is capped at sixteen tokens (ADR 0001), so the one orange also says "clipping": a
  translucent band over the clipped columns of a wave and a lamp on a meter; `Accent` on `Pane` is a pair of the contrast table already.
- **Meters** tell RMS from peak by shape (a full-width body, a half-width peak bar above it, a hold tick), because no second green
  contrasts with `Play` in all three themes.
- **Scope** draws older blocks with fewer points (96, 48, 32, 32) to stay under 400 words with its glow; **handles** are a ring and a disc.
- **Single precision at low frequencies.** The biquad response is evaluated in `sin^2(w/2)` (a table from `response_basis()`), not in
  `cos w`, `cos 2w`: the textbook form is visibly noisy below 300 Hz in float (found in the gallery), and a test now holds the
  384-point response of a resonant lowpass to 0.05 dB of double precision.
- **`ui::Renderer` raw sections** (`begin_raw` / `end_raw` / `raw` / `color_word` / `display`, two words each) and
  `DisplayListBuilder::sink()` are the only changes to shared code, besides the pattern view's 1300-word regression guard in
  `tests/budget_test.cpp`, which now looks at the pattern view alone (the plot gallery is held to the standard 1792).
- **No new display-list word** was needed in `eve` or in the simulator (003a had them); the plots' dependence on them is pinned in
  `tests/plot_raster_test.cpp`. A `CELL 0` word before the wave bitmap's vertex was found by the fallback mode's test: VERTEX2F
  draws the CELL register's cell.

Memory of the objects the app holds (bss or SDRAM): `WaveBitmap` 18.4 KB (shared by both lanes), `Spectrum` 12.1 KB, `CurveScratch` 4.5 KB,
`RealFft` 4.6 KB, `Scope` 1.5 KB, `DbScale` 0.5 KB, `PeakCache` 184 bytes plus its storage; none of them uses the stack beyond 3.2 KB (a vector wave's columns).

Numbers (host unit tests under ASan and UBSan, and the MK66 static libraries at `-Os`): the mipmap is **0.0833 bytes a sample**
(a twelfth of a byte; 87,384 bytes a channel for 2^20 frames); a wave bitmap lane is 13 words (14 past 511 columns, 16 at most) with up to 123
more for marks; the gallery states are 338 to 1432 words (translate) and 353 to 1446 (VERTEX2F fallback); `ui` on the MK66 grew from 16,828 to
35,260 bytes of text (data and bss 0 in both): the plots are 18.4 KB of code, nothing is a table (and the libm they pull in is
`sinf` and `cosf` only). Host figures (Release, relative):
mipmap build 1.75 ms per Msample, 740 columns queried in 12 to 177 us from 1 to 5,668 frames a column, an edit of 100 frames
updated in 0.4 us, an L1 or L4 wave bitmap of 740 x 150 made and uploaded in 120 to 130 us, a 512-point FFT in 6.7 us, a spectrum
analysis in 10 us, every plot's display list built in 1 to 150 us. The MK66's SSUB16 + SEL loop was run on QEMU's Cortex-M4F
(the `plot` suite of OS-007a's QEMU layer, ctest `qemu_plot`: 54 cases, 361,196 assertions, among them the real SSUB16 + SEL loop
against the scalar loop and the model, the peak cache against brute force, the wave bitmap against the pixel reference, the FFT
against a DFT and the word bound of every plot): the instruction semantics hold; cycle counts need the device (`hw-session`: mipmap build per Msample, a column
query, a wave bitmap, the FFT, the draw calls, the 4 KB uploads).

### 003d review folds (2026-10-11)

- **A cache that is not built is not read.** `PeakCache::range()` / `columns()` answer `kNoPeak` until the build is done (they read
  storage nobody had written), `WaveKey::valid()` and the wave view require `built()`, and the cache has an `epoch()` (init,
  `build_begin()`, the end of a build) that the key records, so a rebuilt cache is never shown by an old picture; `update()` before
  the build restarts it. Tests: `plot peak cache: until it is built ...`, `plot wave bitmap: nothing is rendered from a cache that is
  still building ...`, `plot wave view: a view opened while the cache is building ...`.
- **Spectrum**: `Config::f_lo` defaults to one FFT bin (86.13 Hz at 512 points) and `init()` refuses less (below it a bar could only
  repeat bin 1: 30 Hz made the first eight of 48 identical); `Spectrum::bin_hz()` says what the minimum is.
- **Axes** with a non-finite, overflowing or log-impossible range have no ticks and map to 0 (they hung `nice_up()`); tick labels
  out of range are empty, never a wrong number, and never longer than their buffer.
- **The declared word bounds are hard** (`tests/plot_bounds_test.cpp`, also on QEMU): measured in both translate modes, with the
  origin moved and a primitive open, every style colour distinct; the scope's 380 and the meters' `7 + 12 n` (the hold and the lamp
  are passes of their own now) are met exactly; `kRawOverheadWords` = 4 is in every formula.
- **Raw sections are checked**: `Renderer::in_raw()`; nesting, a raw word outside a section, a stray `end_raw()` or `end()` inside
  one abort in debug and host builds.
- **Clip is `Accent`** (maintainer decision), see above. **QEMU**: the `plot` suite (`qemu_plot`) replaces the scratch harness, including the
  real SSUB16 + SEL loop. **Flash**: libm is down to `sinf` / `cosf` (`exp2_fast` / `log2_fast` replace `powf`, `exp2f`, `log2f`,
  `log10f` and `ceilf`: about 7.7 KB of linked libm for 0.8 KB of ours). **Test time**: the gallery tests were 50 s a run under the
  sanitizers because the PNG encoder, SHA-256, rasteriser and thumbnails ran at `-O0`; they are `-O2` there now (PNG level 2): a run is
  about 25 s, `gallery_stale_diff_list` renders one theme for its run without `--check` (39 s from 115 s).


## 003f — Boot logo (maintainer request, 2026-10-11)
The maintainer's logo becomes the boot image: a pixel-art floppy disk with the Tracker on it, greyscale with checkerboard dither, on black.
- **Source.** `logo.png` (1254×1254 RGB, repo root) is copied as-is to `ui/assets/boot_logo.png`. That copy is the only source of truth.
- **Converter `tools/logo_convert.py`** (stdlib only, like `icon_convert.py`):
  - **Decode.** A PNG decoder covering non-interlaced 8-bit gray/RGB/RGBA and every filter type, built on `zlib`. Then convert to luminance.
  - **Downsample.** Detect the pixel-art grid (block size and phase) and sample block centres down to the native resolution. If detection fails, fall back to a fixed target size with box filtering.
  - **Format.** Quantise to the smallest FT81x format that keeps the dither faithful: L1, L2 or L4. Report an error metric against the source.
  - **Output.** A generated C++ source: a `const` array in flash plus width, height, format and stride. The output is deterministic, and a ctest re-runs the converter and fails if the committed output is stale.
  - **Compression.** Prefer an uncompressed bitmap if it is ≤ 8 KB of flash. Use `CMD_INFLATE` only if needed; the simulator then gains an inflate path, licence-gated.
- **Boot screen.**
  - The logo is the first frame the FT812 shows, centred on black and scaled by an integer factor through `BITMAP_TRANSFORM` (nearest) to about 360–420 px tall. Ink colour is set with `COLOR_RGB`.
  - Under it, one line in the Micro face carries the device name "Track3r" (maintainer, ADR 0012 amendment 3) and the build version.
  - The backlight ramp starts only after this frame (the OS-007c rule).
  - It stays on while boot continues, with a minimum of 800 ms, and then the first app screen replaces it. No added delay beyond that minimum.
  - It is identical in all themes.
- **RAM_G.** A dedicated region in the 003d RAM_G map, with the non-overlap and 4-byte-alignment `static_assert`s.
- **Tests.**
  - Converter tests: the PNG decoder on small synthetic PNGs of every filter and colour type, grid detection, and quantisation.
  - A boot-screen golden in the simulator.
  - Budget within limits.
  - The MK66 image keeps passing the static checks and stays reproducible.
- **Done when:**
  - Host, QEMU and MK66 ctest pass with 0 warnings.
  - The flash cost is reported (target ≤ 8 KB).
  - The maintainer has viewed the boot golden next to the source.
  - The reviewer approves.
  - The hw checklist gains "boot logo shows, sharp, centred, no garbage before it".

### 003f as built (2026-10-11)

- **Source and converter** as specified: `ui/assets/boot_logo.png` (byte-exact copy of `logo.png`, sha256 05ca5481...), `tools/logo_convert.py`. Two steps beyond the text: the picture is **cropped** to its bounding box (973 x 975 of 1254 x 1254: the black margin costs flash and would be centred instead of the floppy), and the grid detector takes an integer block size and phase per axis, accepted only if 85 % of the edge energy lies on the boundaries.
- **Grid detection failed, on purpose.** The logo is drawn as pixel art but is not on a grid: the checkerboard of the shutter is 7.9 px a square, the body's stipple 5-6.5, the shell about 11 (the best integer fit holds 52 % of the edge energy). The converter says so and takes the fallback: 124 x 124 cells (7.85 px, the shutter's dither pitch) box-filtered over the **central half** of each cell (a full box turns the shutter's checkerboard into flat grey; the central half keeps its contrast).
- **Format L4** (the smallest that keeps the dither: quantisation RMS L1 55.1, L2 19.7, L4 4.5 grey levels against the native image, threshold 6; tone error 5.9 over 4 x 4 patches). 124 x 124 x 4 bits = **7,688 bytes of flash**, uncompressed (the 8 KB target; no `CMD_INFLATE`, so no inflate path in the simulator and no licence gate). MK66 image: flash +8,628 B, SRAM_U +32 B.
- **Scale 3 (372 x 372)** with `BITMAP_TRANSFORM_A/E` = 85: the guide's coefficient is 8.8 fixed point, which cannot say a third (85.33). One block of 124 is 4 px wide and the last is 2 px ([A]: where depends on the chip's rounding; hardware checklist DSP-15). A power-of-two scale would be exact (4 x gives 100 cells of 9.7 px) but would average the 7.9 px dither away. `eve` gained BITMAP_TRANSFORM_A-F and the L2 format; the simulator gained the transform (pixel-centre sampling [A], nearest only, in the saved context) and L2.
- **Boot screen** (`ui/boot_screen.h`): logo exactly centred at (214, 54), the line `Track3r  o3os <build id>` in the Micro face at y = 440, drawn with `ui::boot_theme()` (the Black table) so it is identical in all themes; a registry state `boot` `logo` with a `fixed_theme` (76 words). The name is `usb::kProductString`, the id is `O3OS_BUILD_ID` (`diag version`'s). RAM_G: `kRamGBootLogo` 0x6F000, two pages, handle 11, in the 003d map with its `static_assert`s; `upload_assets()` sends it.
- **Boot path** (`platform/mk66/src/app_main.cpp`): the boot frame is committed right after the assets, before the app is built, so the backlight (which waits for the first committed frame) comes up behind it; `boot::delay_ms()` gained an idle hook that services the display during the audio start's blocking codec waits (the USB start does not use `delay_ms`), guarded against re-entry into the driver (`BootGate`, `IdleHook`, tested in `mk66_boot_gate_test.cpp`); the DMA-self-test blink code is played after `appimg::init` so that it cannot outlast the backlight's fallback before the logo is committed; `core::BootHold` holds the app's first frame 800 ms from the first light and not beyond the boot work. A refused boot frame is not fatal.
- **Tests:** `logo_convert_selftest`, `logo_convert_fresh` (staleness), `boot_screen_test` (host and `qemu_boot`), `boot_screen_pixels_test`, `display_boot_test`, `raster_transform_test`, `dl_test`, the RAM_G map and assets tests, gallery goldens `gallery/boot_logo_{amiga,black,white}` (one hash). Hardware checklist DSP-13..16 in `platform/mk66/README.md`.
