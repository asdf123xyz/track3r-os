# OS-PORT-RT10XX — Porting the Community OS to the Tracker+ (NXP i.MX RT10xx, Cortex-M7)

Status: phase 1 specified and implemented 2026-10-26 (the probe image builds; nothing has run on a unit). Level C: a new boot path, a
new memory map, a new delivery path and, later, the audio path and the persistent formats on a second machine. Decisions in force:
- ADR 0011 (declared alignment, diagnostics, the crash record, driver licences), ADR 0013 (the memory map as one header, the NVIC priority
  table, DWT per stage), ADR 0002 (containment, fault handlers), the flashing-safety rules and `fw-gate-check`.
- `CLAUDE.md`, hard rules: nothing from the official firmware goes into this repository. This port uses the Tracker+ facts of the
  Modular-Trackular project (MIT; `docs/FINDINGS.md` there) only as **facts of the board and of its bootloader** (what RAM exists, where the
  image loads, what the update path reads from the card). No address of the official firmware's code, no symbol, no copied code or data,
  and nothing that describes how the official firmware is built, is in this tree.

**Hard rule:** the phases below produce images and host tests. Nothing is flashed, copied to removable media or handed over as a
candidate without the gate check and the maintainer's decision. The first images are probes; the recovery path (section 2.4) is what
makes a probe acceptable on a unit at all.

## 1. Why, and what "a port" means here

The Tracker+ is the same instrument as the Tracker (the same panel, pads, display, jacks, card, the same project files up to the 16-track
pattern files of OG-16TRACK-MTP) on a different computer: an i.MX RT10xx Cortex-M7 at hundreds of MHz with 512 KiB of on-chip RAM, 64 MiB
of SDRAM and 16 MiB of flash, where the Tracker has an MK66 Cortex-M4F at 180 MHz with 256 KiB and 16 MiB and 1 MiB. Everything above the
HAL (`core`, `model`, `formats`, `audio`, `seq`, `midi`, `module`, `ui`, `app`) is portable by construction (the layering of
`CMakeLists.txt`); what a port needs is a `platform/<chip>` layer that implements the HAL contracts (`hal/include/o3os/hal/*.h`: audio,
capture, clock, cycles, display, firmware, input, leds, storage, storage_write, irq) on the new chip, and a build and delivery path.

The port is done in phases, each of which ends in a check on a unit that nobody can do here:

| Phase | Delivers | The unit check that ends it |
|---|---|---|
| **1 (this spec, implemented)** | `platform/rt10xx`: the memory map, the FlexRAM and MPU words, the clock tree, the reset sequence with the vector table and fault handlers, the crash record, SysTick and the cycle counter, the `.ptf` tools and the static image checks, the build (`O3OS_PLATFORM=rt10xx`), the host tests of the pure parts | The probe image installs through the official update path and the unit stays up (no reset loop, no hang), then the official firmware is put back the same way. With a debug probe: the heartbeat counts seconds, the core runs at 600 MHz, the crash drill leaves a record |
| 2 | The USB device controller (OTG1, the chip's dedicated pins: no board fact needed) with the `diag` CDC console; the SEMC (SDRAM) bring-up and its tests; `diag crash`, `diag mem`, `diag boot` | The console answers on a computer; the SDRAM tests pass; the crash record of a drill image is read back |
| 3 | The audio path: SAI + eDMA + the codec, the render interrupt, hot code in the ITCM; the MIDI DIN jack (LPUART5); `core/audio_clock.h` made platform-aware (a 600 MHz core has other cycles per sample) | HW-A on the Tracker+: the test tone, the underrun recovery, left and right |
| 4 | The card (USDHC + FatFs, the read side then the writer), the firmware update from the OS (the `_fwinfo` / `_turnon` markers, section 2.4), the display (EVE over an LPSPI with eDMA), the inputs (keys, pads, encoder, pad LEDs), power | The OS app boots on the unit: the Pattern screen, keys, a project from the card |
| 5 | The rest of the HAL (capture: line in, radio; USB audio and the USB disk), the 16-track pattern files (OG-16TRACK-MTP), the CPU budget re-measured, the QA catalogue rows that differ | A full pass of the HIL scenarios on a Tracker+ |

Phases 2 to 5 are backlog rows (`docs/roadmap/BACKLOG.md`, OS-PORT-RT10XX-2 to -5), each with its own spec when it starts. The
layering means phases 3 to 5 mostly port drivers, not features: the Pattern, Song, Instrument, Perform and File screens, the sequencer,
the voices, the module host and the formats are the same code.

## 2. The hardware, with the evidence for each fact

Labels: **observed** (an observation of a unit or of its bootloader, with the source), **inferred** (follows from an observation),
**chip** (the i.MX RT1050 Reference Manual), **open** (an open research question: this port makes an assumption and says which).
The source `FINDINGS n` is section n of Modular-Trackular's `docs/FINDINGS.md` (MIT): its observations of the unit are used as facts of the
hardware; nothing of its description of the official firmware's code is.

### 2.1 The chip and its memories

| Fact | Value | Label and source |
|---|---|---|
| MCU | NXP i.MX RT10xx, Cortex-M7, most likely the RT1050/1052 class: no RAM answers at `0x2028_0000` (the RT106x's second OCRAM), and the unit froze on an access there | observed, FINDINGS 2 and 11 |
| On-chip RAM | FlexRAM, 512 KiB in sixteen 32 KiB banks; the unit runs with 128 KiB ITCM, 352 KiB DTCM, 32 KiB OCRAM (`IOMUXC_GPR17 = 0xAAAAABFD`, `GPR16[2]` set) | observed, FINDINGS 2; decoded in `flexram.h` |
| Stack top | `0x2005_8000`, the end of the 352 KiB DTCM | observed, FINDINGS 2 |
| OCRAM | 32 KiB at `0x2020_0000`; the window above it has no RAM behind it | observed, FINDINGS 8, 11, 15 |
| Flash | 16 MiB FlexSPI NOR at `0x6000_0000` (read on the unit from the FlexSPI configuration block and the controller) | observed, FINDINGS 8 |
| Image base | `0x6003_0000`: a `.ptf` covers nothing below it; the bootloader and the i.MX boot header live there; VTOR of the running image | observed, FINDINGS 1, 2 |
| Image size | an image ending 8 MiB into the flash installs and runs; larger ones are untested | observed, FINDINGS 8 |
| SDRAM | 64 MiB at `0x8000_0000` through the SEMC | observed, FINDINGS 2 and 15 (the top of it is used); the controller's timings are **open** (section 2.5) |
| MPU | 16 regions; a table of the official firmware's regions is recorded (FINDINGS 8); this port writes its own (`mpu_config.h`) | chip; the attributes are this port's choice |
| Crystal | 24 MHz (every RT10xx boots from it) | chip |
| Core clock | the port targets 600 MHz (VDD_SOC 1.25 V, ARM PLL 1.2 GHz / 2), the part's rated configuration | chip; **open**: the official firmware's clock was not measured, the phase-1 check measures ours |
| Caches | 32 KiB I-cache, 32 KiB D-cache; the TCMs are never cached | chip |
| Priority bits | 4 (the HAL's table of multiples of 16 applies unchanged) | chip |
| Interrupt lines | 160 (`irq_list.h`) | chip |

### 2.2 Peripherals the board uses

| Function | What is known | Label and source | Port phase |
|---|---|---|---|
| Display | an EVE (FT81x/BT81x) graphics controller over an LPSPI, the Tracker's family; which LPSPI, which pins, the panel's timing and orientation: **open** | observed (the controller family), FINDINGS 4 | 4 |
| Audio | 128-frame blocks at 44.1 kHz through the eDMA (the output DMA raises line 0); the codec, the SAI instance, the MCLK path and the pins: **open** | observed (block, rate, DMA line), FINDINGS 12, 15, 19 | 3 |
| MIDI DIN | LPUART5 (`0x4019_4000`); the pins: **open** (the LPUART5 pads follow from the IOMUXC once the pins are read on the unit) | observed, FINDINGS 12 | 3 |
| USB | one device port (MIDI, the card as a disk, USB audio in); the RT10xx's OTG1 controller on its dedicated USB pins: no board fact needed | observed (the functions), chip (the pins) | 2 |
| SD card | the vendor's SD stack (USDHC); the instance and pins: **open** (USDHC1 on its usual pads is the assumption) | observed (the stack), FINDINGS 11 | 4 |
| Pad LEDs | an IS31FL3731 at I2C address `0x74` on an LPI2C at 400 kHz; single-colour pads, 48 of 144 slots, the same gamma table as the Tracker's | observed, FINDINGS 20 | 4 |
| Keys, encoder | present; the scanner (a keypad controller or a GPIO matrix), the encoder's counter and the pins: **open** | observed (they exist), the mechanism open | 4 |
| Radio | present (a recorder source); the receiver and its bus: **open** (the Tracker's is an Si4703 on I2C) | observed, FINDINGS 28 | 5 |
| Power switch, status LED | **open** | | 4 |

Everything marked open is settled on the unit, not guessed into a driver: phase 2's console gets `diag pins` and `diag i2c scan` first,
so that the pins and the buses are read from a running unit before a driver assumes them (the MK66 port's method: README.md there).

### 2.3 What the official firmware does that this port does not copy

Modular-Trackular describes, with addresses, how the official firmware is built and where its objects live. None of that is used here:
no address, no symbol, no vtable, no string. The three things this port shares with it are facts of the board: the RAM split, the image
base, and the update path's two marker files (below), which are the bootloader's interface, not the application's code.

### 2.4 Delivery: the `.ptf` and the update path

| Fact | Label and source |
|---|---|
| A `.ptf` is Intel HEX with CRLF line endings: type 04 records, type 00 data records, one type 05 (the reset handler) and one type 01; one contiguous range from `0x6003_0000` | observed, FINDINGS 1 |
| The official Config menu lists files in `Firmware/` whose name starts `TrackerPlus` and ends `.ptf` (case-sensitive), writes the chosen name to `Firmware/_fwinfo` and `start` to `Firmware/_turnon`, and resets; the bootloader programs the flash from the file and clears `_turnon` | observed, FINDINGS 3, 8 |
| The pinhole next to the USB port starts the same update from the bootloader: with only the official `.ptf` in `Firmware/`, it puts the official firmware back. **This is the recovery path**, used on a unit | observed, FINDINGS 3 |
| Images whose records differ from the official ones install; images up to 8 MiB install | observed, FINDINGS 3, 8 |
| An image whose last byte lies in the first 256-byte page of a new 4 KiB sector is programmed without an erase (it comes out ANDed with old data): pad to 16 bytes into the second page | observed, FINDINGS 26 |

`platform/rt10xx/tools/make_ptf.py` writes the file (16-byte records, the padding rule, the name rule, the vector checks) and
`check_image.py` is the static gate (the `.hex` and the `.ptf`: base, range, contiguity, the vector table, the stack pointer, the reset
vector, the start record, the line endings, the end rule). `boot_image.h` holds the same rules as functions with their static_asserts, and
`rt10xx_boot_image_test.cpp` pins them. The OS's own update screen (phase 4) writes the two marker files and resets, through
`hal::firmware::FirmwareSink::install()`: the bootloader does the programming, the OS never writes the flash (the MPU marks it read-only).

### 2.5 Open research questions

1. The core clock and the DCDC voltage the official firmware runs at (the port targets the part's rated 600 MHz at 1.25 V).
2. The SDRAM part and the SEMC timings (phase 2: a probe build that configures the SEMC from the data sheet of the likeliest part and
   runs the wiring and capacity tests; the official firmware's register values may be read as a fact of the board with the emulator of
   Modular-Trackular, which is not this repository's tool).
3. The codec, the SAI instance, MCLK and the pins; the display's LPSPI and pins; the card's USDHC and pins; the key scanner, the encoder,
   the power switch, the status LED, the radio's bus (phase 2's `diag pins` and `diag i2c scan`).
4. Whether the eDMA reaches the DTCM on this part (the chip's TCM backdoor): the port puts bus-master buffers in the OCRAM and the DMA
   rings in the DTCM, and phase 3 moves the rings if the backdoor is not there.
5. Whether the FlexRAM holds through a software reset (the crash record's premise).
6. Whether the boot ROM hands over with the ARM PLL off and the core on the 24 MHz oscillator (`clock_config.h` kBootHz: only the
   first delays are sized by it).

## 3. Design of phase 1 (what is in the tree)

### 3.1 Build

- `O3OS_PLATFORM=rt10xx` (`CMakeLists.txt`), a cross build with `cmake/rt10xx-toolchain.cmake`: the same arm-none-eabi toolchain as
  the MK66, `-mcpu=cortex-m7 -mfpu=fpv5-sp-d16 -mfloat-abi=hard`. Single-precision FPU on purpose: the tree's "no double" rule stays
  visible (a stray double is a library call, as on the MK66), not a silent fast path.
- `platform/rt10xx/CMakeLists.txt`: the pure parts as `platform_rt10xx_pure` (headers; built and header-checked on every platform), the
  image `o3os.elf` with the generated `rt10xx.ld`, then `o3os.hex`, `o3os.bin` and `TrackerPlus_o3os.ptf` behind the static checks (a
  failing file is deleted). `tools/ci.sh rt10xx` runs it. The audio layer's hot section keeps its name (`.ramfunc_l`), linked into the
  ITCM, so the audio layer needs no change for the ITCM (`audio/CMakeLists.txt` defines `O3OS_AUDIO_RAMFUNC` for rt10xx too).
- Reused as they are: `platform/mk66/tools/gen_ld.py` (its parser and renderer; `rt10xx_ld.py` adds this chip's checks) and the MK66's
  crash record format and pure functions (`platform/mk66/crash_record.h`, `crash_record.cpp`: Cortex-M registers and a stage word,
  nothing of the K66 in them). The frame plausibility check is this chip's (`mem_map.h` `in_ram`). Moving the chip-neutral pure parts of
  `platform/mk66` (the crash record, the SD protocol, the FatFs storage, the HEX validator, the pad LED and keypad drivers, the radio) to a
  `platform/common` is a backlog row (OS-PORT-COMMON), not part of this port: the port links `platform_mk66_pure` for them.

### 3.2 Memory map (`mem_map.h`, one header, the linker script generated from it)

| Region | Base | Size | Use |
|---|---|---|---|
| FLASH (image) | `0x6003_0000` | 8,000 KiB (to the 8 MiB the bootloader is known to program) | vectors at the base (VTOR), code, constants, the load images of `.ramfunc_l` and `.data`; one contiguous run, asserted |
| ITCM | `0x0000_0000` | 128 KiB | `.ramfunc_l` (hot code, budget 72 KiB) and `.tapmem_l` (24 KiB) below a 32 KiB module slab at the top |
| DTCM | `0x2000_0000` | 352 KiB | `.noinit` (the crash block first), the fault stack, `.dma_rings`, `.data`, `.bss`, at least 64 KiB free, the 32 KiB main stack at the top (SP `0x2005_8000`) |
| OCRAM | `0x2020_0000` | 32 KiB | `.noinit_ocram`: what every bus master must reach (display staging, USB queue heads), not cached |
| SDRAM | `0x8000_0000` | 64 MiB | the ADR 0013 D7 table with the fixed rows unchanged and the sample arena 59,136 KiB (the MK66's is 10,816); a 1 MiB slack window for `.noinit_sdram`, empty until phase 2 (asserted) |

The DTCM plan (`kDtcmBudget`) carries the MK66's SRAM_U rows over; the MK66's moves of main-loop objects to SDRAM for room (OS-MEM1) may
be undone here, where the DTCM is nearly twice the size.

### 3.3 The reset sequence (`startup.cpp`, `boot.cpp`)

0. In naked asm, before the first push: interrupts off; `GPR17` = the bank word, `GPR16[2]` set; the stack pointer loaded from the
   vector table. The stack top exists only once the DTCM has its 352 KiB.
1. VTOR = the image base; the crash context set to stage Reset.
2. WDOG1, WDOG2 and the RTWDOG off.
3. FPU on.
4. `.data` and `.ramfunc_l` copied, `.bss` cleared (the noinit sections untouched).
5. The MPU table (`mpu_config.h`: a deny-all background, the ITCM executable, the DTCM and the OCRAM not cached, the peripherals Device,
   the flash read-only and cacheable, the SDRAM cacheable and executable for module code), then the instruction cache and, with
   `O3OS_RT10XX_DCACHE` (default ON), the data cache. The soundness rule of the data cache: no bus master touches a cacheable region.
6. The cycle counter (the Cortex-M7's DWT needs its lock key), then the clock tree (`clock_config.h`): VDD_SOC to 1.25 V, the periph
   clock onto the oscillator, the ARM PLL to 1.2 GHz, ARM_PODF /2, AHB /1, IPG /4, the periph clock back onto the divided PLL, PERCLK
   /2. Every handshake bounded; the FlexSPI's clock (PLL3 PFD0, the code runs from it) is never touched.
7. The NVIC: every line disabled, priorities from `hal/irq.h` through `irq_map.h` (the DMA line 0 and the SAIs are AudioDma, LPUART5
   is MidiUart, USB_OTG1 is Usb, the USDHCs, LPSPIs and LPI2Cs are Completion, PIT is MidiOutTimer, the reserved line 70 is the
   software-pended Render line, the GPIO combined lines are PinChange), the fault handlers on, divide-by-zero trapping; SysTick at 1 kHz.
8. Constructors, then `main`.

A step that cannot complete calls `boot::fail(module)`: a panic record with that module id, then a halt in WFI (never a reset loop: a
unit with no output yet must not restart for ever; a power cycle and the pinhole are the way back). A fault while the OS runs (stage
Running) records and resets, as on the MK66.

### 3.4 The probe `main`

Idles in WFI; once a second a function in the ITCM increments a heartbeat; the reset cause and the crash count of the record found at
boot are kept in variables for a debugger (the record itself is not cleared: nothing can report it yet). `O3OS_RT10XX_FAULT_DRILL`
raises one UsageFault after 10 s for the record round trip.

### 3.5 Tests and checks

- Host (doctest, in `o3os_tests`, also as the named ctest `rt10xx_pure`): `rt10xx_mem_map_test` (the regions pinned, the SDRAM table, the
  DTCM plan, `in_ram`, the model-arena mirror against `model::kModelArenaBytes`), `rt10xx_flexram_test` (the bank word decodes to the
  map), `rt10xx_mpu_test` (RBAR/RASR decoded field by field), `rt10xx_clock_config_test`, `rt10xx_boot_image_test` (the name rule, the end
  rule, the range), `rt10xx_irq_map_test`.
- Python self-tests as ctests: `rt10xx_make_ptf_selftest`, `rt10xx_check_image_selftest`, `rt10xx_ld_selftest`; `rt10xx_ld` (template
  and header), and in the rt10xx build `rt10xx_ld_file`, `rt10xx_image` (the `.hex`), `rt10xx_ptf` (the `.ptf`, CRLF and start record).
- Not yet: a QEMU model of the Cortex-M7 for the target suites (the mps2-an500 is a candidate), the `check_ramfunc` rule for the ITCM
  (phase 3, with the audio path).

### 3.6 Costs

The probe image: FLASH 3,728 B, ITCM 4 B, DTCM 752 B (the crash block, the fault stack, 8 B of `.data`, 104 B of `.bss`), OCRAM 0,
SDRAM 0. No CPU figure: nothing runs per block yet.

## 4. Acceptance of phase 1

Done in the tree when: `tools/ci.sh host` is green with the new tests; `tools/ci.sh rt10xx` builds the image and its checks pass; 0
warnings. Done on a unit when the maintainer, with the official `.ptf` beside it in `Firmware/` and the pinhole known to work on that
unit (FINDINGS 3 says it does), installs `TrackerPlus_o3os.ptf` through the official Config menu, sees the unit stay dark and quiet
without resetting for a minute, then puts the official firmware back with the pinhole. With a debug probe (optional): the heartbeat
advances once a second at 600 MHz (SysTick reload 599,999), the drill image leaves a record whose exception is 6 and whose module is
0xD1. A unit that boot-loops or does not take the official firmware back is a FAIL that stops the port until understood.

## 5. Decisions taken in this phase (reversible, recorded)

- The platform's name is the chip family (`rt10xx`), like `mk66`, not the product's.
- The ITCM keeps the MK66's section name `.ramfunc_l` (no change in the audio layer).
- The MK66's pure parts are linked, not copied (OS-PORT-COMMON moves them later).
- A boot failure halts; a run-time fault resets.
- The data cache is on by default with the "no bus master in a cacheable region" rule; the option exists for an A/B.
- The image region ends at 8 MiB, the tested bound, not at the end of the flash.
- The render line is the reserved line 70 (the MK66 uses its SWI line); PendSV stays the fallback.
