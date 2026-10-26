// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Placement attributes of the rt10xx image (OS-PORT-RT10XX): where the linker script (rt10xx.ld, generated from mem_map.h) puts an
// object. The names and the rules are the MK66's (platform/mk66 sections.h explains the `.bss.` trick: GCC makes such a section NOBITS
// and refuses a non-zero initialiser in it at compile time, so a value given to a not-initialised object cannot be silently dropped).
// Placement never decides alignment: an object that needs 4- or 8-byte alignment carries alignas and a static_assert of its own.

// Hot code that runs from the ITCM: copied there from flash by the reset sequence before main. The section keeps its MK66 name,
// `.ramfunc_l`, because the audio layer's O3OS_AUDIO_HOT names it (audio/CMakeLists.txt). `long_call`: flash (0x6000_0000) and the ITCM
// (0x0) are too far apart for a plain BL; `noinline` so the placement is not lost to inlining.
#define O3OS_RAMFUNC_L [[gnu::section(".ramfunc_l"), gnu::noinline, gnu::long_call]]

// An object in the SDRAM slack window (mem_map.h kSdramSlackBase, 1 MiB): undefined until written, lost at power-off.
#define O3OS_SDRAM_NOINIT [[gnu::section(".bss.noinit_sdram")]]

// An object in the DTCM that survives a reset: not initialised, not cleared. Whoever uses it validates it (a magic and a CRC, as the
// crash record does). Whether the FlexRAM holds through every reset source of this chip is an open hardware check.
#define O3OS_NOINIT [[gnu::section(".bss.noinit")]]

// An object in the OCRAM (32 KiB, non-cacheable, reached by every bus master): the display's DMA staging, the USB controller's queue
// heads and buffers. Not initialised, not cleared.
#define O3OS_OCRAM_NOINIT [[gnu::section(".bss.noinit_ocram")]]

// An audio DMA ring: in `.dma_rings`, a NOLOAD section of the DTCM ahead of `.data`, aligned to mem_map.h kDmaRingAlign. Not cleared at
// reset: the code that starts the DMA zeroes the ring first.
#define O3OS_DMA_RING [[gnu::section(".bss.dma_rings")]]

// The audio taps' memory (audio/tap_mem.h): `.tapmem_l`, a NOLOAD section of the ITCM behind `.ramfunc_l`, budgeted by mem_map.h
// kItcmDataBudget. TapMem's constructor zeroes it.
#define O3OS_TAP_MEM_L [[gnu::section(".bss.tap_mem_l")]]
