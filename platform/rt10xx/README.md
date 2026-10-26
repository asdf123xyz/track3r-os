# platform/rt10xx: the Tracker+ platform layer (OS-PORT-RT10XX, phase 1)

The port of the Community OS to the Polyend Tracker+ (NXP i.MX RT10xx, Cortex-M7, 512 KiB FlexRAM, 64 MiB SDRAM, 16 MiB flash). The design,
the facts of the board with their evidence, the phases and the open questions are in `docs/specs/OS-PORT-RT10XX.md`; this page is the
build and the layout. Phase 1 (this tree) is the **probe image**: the boot path to a WFI heartbeat with a crash record, and the tools that
make and check a `.ptf`. No display, audio, card, USB or input yet.

**Nothing here has run on a unit.** `TrackerPlus_o3os.ptf` is an artifact of a build, not a candidate: a candidate needs the
`fw-gate-check` skill and the maintainer's decision, and the recovery path (the official `.ptf` beside it in `Firmware/`, the pinhole next
to the USB port) must be known to work on that unit first. Nothing in this directory flashes, copies to removable media or hands over.

## Build and check

```sh
cmake -S . -B build-os-rt10xx -DO3OS_PLATFORM=rt10xx -DCMAKE_TOOLCHAIN_FILE=$PWD/cmake/rt10xx-toolchain.cmake [-DARM_GNU_ROOT=<dir>]
cmake --build build-os-rt10xx && ctest --test-dir build-os-rt10xx
```

Builds `o3os.elf` (`build-os-rt10xx/platform/rt10xx/`), then `o3os.hex`, `o3os.bin` and `TrackerPlus_o3os.ptf` behind the static image
checks (`tools/check_image.py`: a failing file is deleted), and prints the linker's per-region usage and `size -A`. `tools/ci.sh rt10xx`
does the same. Options:

| Option | Default | Meaning |
|---|---|---|
| `O3OS_RT10XX_FAULT_DRILL` | OFF | raise one deliberate UsageFault 10 s after power-up, to prove the crash record round trip (a separate image) |
| `O3OS_RT10XX_DCACHE` | ON | enable the data cache after the MPU table (TCMs and OCRAM are never cached; the SDRAM and the flash are write-back). OFF for an A/B on the unit |

The pure parts (`include/`: the memory map, the FlexRAM and MPU words, the clock tree, the interrupt list and map, the image facts) build on
every platform as `platform_rt10xx_pure` (headers, header-checked); their tests are `tests/rt10xx_*_test.cpp`, in the host's `o3os_tests`
(ctest `rt10xx_pure`). The tools' self-tests and the linker-script check run in the host build too (`ctest -R rt10xx`).

## Layout

| Path | What |
|---|---|
| `include/o3os/platform/rt10xx/mem_map.h` | the memory map, one header; `rt10xx.ld` is generated from it (`tools/rt10xx_ld.py`, with `platform/mk66/tools/gen_ld.py`'s parser) |
| `flexram.h`, `mpu_config.h`, `clock_config.h` | the FlexRAM bank word decoded, the MPU table as register words, the clock tree as field values with their arithmetic |
| `irq_list.h`, `irq_map.h` | the 160 device interrupts in vector order; which line has which role of `hal/irq.h` |
| `boot_image.h` | the `.ptf` and update-path facts as functions (the name rule, the sector end rule, the range) |
| `sections.h`, `platform.h` | placement attributes (`.ramfunc_l` in the ITCM, `.noinit`, `.noinit_ocram`, `.noinit_sdram`, the DMA rings); the clock, the boot report, the crash API |
| `src/startup.cpp` | the vector table, the fault stub, the reset sequence (the FlexRAM word in naked asm before the first push) |
| `src/boot.cpp` | watchdogs, DWT, MPU and caches, the clock tree, NVIC, SysTick, `fail()` |
| `src/clock.cpp`, `src/crash_store.cpp`, `src/main.cpp` | `hal::Clock` on SysTick and `hal::cycles()`; the `.noinit` crash block and the fault entry; the probe loop |
| `src/regs.h` | the registers the layer touches, from the i.MX RT1050 Reference Manual and the ARMv7-M ARM |
| `rt10xx.ld.in` | the linker script template (placeholders are `mem_map.h` constants) |
| `tools/make_ptf.py` | the `.ptf` writer: CRLF, 16-byte records, the start record from the vector table, the sector end rule, the `TrackerPlus<...>.ptf` name rule |
| `tools/check_image.py` | the static gate of a `.hex` or `.ptf` (base, range, contiguity, vectors, SP, reset vector, start record, line endings, end rule) |
| `tools/rt10xx_ld.py` | generates and checks the linker script |

## Memory map (`mem_map.h` is the single source)

| Region | Base | Size | Use |
|---|---|---|---|
| FLASH (image) | `0x60030000` | 8,000 KiB | vectors, code, constants, the load images; below `0x60030000` is the bootloader, never written |
| ITCM | `0x00000000` | 128 KiB | `.ramfunc_l` (hot code, 72 KiB budget), `.tapmem_l` (24 KiB), the 32 KiB module slab at the top |
| DTCM | `0x20000000` | 352 KiB | `.noinit` (crash block 128 B first), fault stack 512 B, `.dma_rings`, `.data`, `.bss`, at least 64 KiB free, main stack 32 KiB at the top (SP `0x20058000`) |
| OCRAM | `0x20200000` | 32 KiB | `.noinit_ocram`: bus-master buffers (display staging, USB), not cached |
| SDRAM | `0x80000000` | 64 MiB | the ADR 0013 D7 table: sample arena 59,136 KiB, delay 768, reverb 256, module region 768, project 1,920, undo 384, file job 512, LUT 192, UI cache 512, diag log 64, slack 1,024 (the `.noinit_sdram` window, empty until phase 2) |

## The probe image, by the numbers (2026-10-26)

FLASH 3,728 B · ITCM 4 B (the heartbeat's function) · DTCM 752 B · OCRAM 0 · SDRAM 0. `check_image`: 10/10 on the `.hex` and on the `.ptf`.

## First hardware session (what the probe can and cannot show)

The probe has no output. What a person sees: the unit, after the official update, stays dark and quiet and does **not** reset in a loop (a
loop would mean the boot path faults before `main`; the record of a halted boot is kept for the next image). Then the official firmware
goes back through the pinhole. With a debug probe: `g_heartbeat` advances once a second (600 MHz, SysTick reload 599,999), the crash
block at `0x20000000` holds the drill's record (exception 6, module 0xD1) after the drill image. The spec's section 4 is the acceptance.
