/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Linker script for the QEMU `mps2-an386` target test layer (OS-007a, Cortex-M4F, ARM MPS2 board model).
 *
 * The board model has RAM at both ends of the picture: ZBT SSRAM1 at 0x00000000 plays the part of flash
 * (code, rodata and the load image of .data live there) and SSRAM2/3 at 0x20000000 the part of SRAM. The
 * regions are far larger than the MK66's 1 MiB flash and 256 KiB SRAM on purpose: a suite that does not fit
 * fails the link, it is never trimmed (spec OS-007a).
 *
 * Layout of RAM, low to high:
 *   .data, .bss            (the heap is not here: it is the board's 16 MiB PSRAM at 0x21000000, src/syscalls.cpp, so
 *                          a suite's static arenas and its heap do not compete for the 4 MiB; two 1.9 MiB project
 *                          arenas left ~200 KiB of heap, too little for the OG round trip after OS-004d)
 *   guard, 32 KiB          an MPU no-access region (src/startup.cpp): the first touch of it is a MemManage fault,
 *                          which the fault reporter turns into "stack overflow", exit status 3
 *   thread stack, 60 KiB   the program runs on PSP here; painted at reset, high-water mark reported by the runner
 *   exception stack, 4 KiB MSP: reset, SysTick and every fault handler, so a handler never needs the (possibly
 *                          overflowed) thread stack
 */
ENTRY(Reset_Handler)

MEMORY
{
  ROM (rx)  : ORIGIN = 0x00000000, LENGTH = 4M
  RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 4M
  PSRAM (rwx) : ORIGIN = 0x21000000, LENGTH = 16M
}

__exc_stack_size = 4K;
__stack_region = 64K;   /* thread + exception stack, immediately below the top of RAM */
__guard_size = 32K;     /* a frame larger than this can still jump over the guard: keep test frames below it */

/* Two load segments with their own permissions: code and read-only data (r-x), and .data + .bss (rw-). Without this,
   .data's load image (in ROM, right after the read-only sections) is merged into the code segment and the linker
   warns about a segment that is both writable and executable. */
PHDRS
{
  text PT_LOAD FLAGS(5);
  data PT_LOAD FLAGS(6);
}

SECTIONS
{
  .text :
  {
    KEEP(*(.isr_vector))
    *(.text .text.* .gnu.linkonce.t.*)
    *(.rodata .rodata.* .gnu.linkonce.r.*)
    . = ALIGN(4);
  } > ROM :text

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } > ROM :text

  /* libgcc's unwinder looks these two symbols up; C++ library code references it even though first-party
     code is built without exceptions. */
  .ARM.exidx :
  {
    __exidx_start = .;
    *(.ARM.exidx* .gnu.linkonce.armexidx.*)
    __exidx_end = .;
  } > ROM :text

  /* Constructors: run by Reset_Handler in this order (src/startup.cpp). */
  .preinit_array : { . = ALIGN(4); __preinit_array_start = .; KEEP(*(.preinit_array*)) __preinit_array_end = .; } > ROM :text
  .init_array    : { . = ALIGN(4); __init_array_start = .;    KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) __init_array_end = .; } > ROM :text
  .fini_array    : { . = ALIGN(4); __fini_array_start = .;    KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) __fini_array_end = .; } > ROM :text

  /* Load image of .data: copied to RAM by Reset_Handler. */
  __data_load = LOADADDR(.data);

  .data : ALIGN(4)
  {
    __data_start = .;
    *(.data .data.* .gnu.linkonce.d.*)
    . = ALIGN(8);  /* .bss follows at the same offset in RAM and in the load image */
    __data_end = .;
  } > RAM AT > ROM :data

  /* .bss has no load image and belongs to no segment (:NONE): QEMU's loader allots every byte of a segment's memory
     size at its load address in the code RAM, so a .bss inside the PT_LOAD of .data would count against the 4 MiB of
     ROM and a suite with a large .bss (OS-004b: two 1.8 MiB arenas) would die at load, not at link. Reset_Handler
     zeroes it. */
  .bss (NOLOAD) : ALIGN(8)
  {
    __bss_start = .;
    *(.bss .bss.* COMMON .gnu.linkonce.b.*)
    . = ALIGN(4);
    __bss_end = .;
  } > RAM :NONE

  end = .;
  __end = .;
}

/* The heap: all of the PSRAM, nothing loaded there. */
__heap_start = ORIGIN(PSRAM);
__heap_end = ORIGIN(PSRAM) + LENGTH(PSRAM);

/* Top of RAM: initial MSP. Thread stack: [__stack_limit, __thread_stack_top). Guard: [__guard_base, __stack_limit). */
__stack_top = ORIGIN(RAM) + LENGTH(RAM);
__thread_stack_top = __stack_top - __exc_stack_size;
__stack_limit = __stack_top - __stack_region;
__guard_base = __stack_limit - __guard_size;

/* The load cliff, checked where the linker can see it: the code, the read-only data and the load image of .data
   have to fit the code RAM (the MEMORY region says so too), and nothing but .data is loaded into RAM. */
ASSERT(LOADADDR(.data) + SIZEOF(.data) <= ORIGIN(ROM) + LENGTH(ROM), "ROM overflow: code, rodata and the .data image do not fit the code RAM")
ASSERT(SIZEOF(.data) + SIZEOF(.bss) <= LENGTH(RAM), "RAM overflow: .data + .bss do not fit the data RAM")
ASSERT(end <= __guard_base, "RAM overflow: .data + .bss leave no room for the guard and the stacks")
ASSERT((__guard_base & (__guard_size - 1)) == 0, "the MPU guard must be aligned to its own (power of two) size")
