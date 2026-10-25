/* SPDX-License-Identifier: MIT
 *
 * Linker script of a Community OS module (module/sdk/o3module.h). Linked at address 0, as a static executable (-nostdlib
 * -nostartfiles -Wl,--gc-sections -Wl,--emit-relocs -Wl,-T,o3module.ld; tools/o3m_pack.py `build` runs exactly that).
 * The packer turns the result into the flat image of a .tmod file: .o3m_hot (code marked O3M_HOT, run from SRAM when the OS has
 * room) followed by .o3m_cold (everything else, read-only). Sections a module must not have (writable data, constructors) are
 * collected in output sections of their own so that the packer can name them in its refusal.
 */
ENTRY(o3m_entry)

SECTIONS
{
  /* O3M_HOT_AT / O3M_COLD_AT (--defsym) link the module somewhere else than 0: the loader test's oracle (tools/o3m_pack.py). */
  . = DEFINED(O3M_HOT_AT) ? O3M_HOT_AT : 0;
  .o3m_hot : ALIGN(16)
  {
    *(.text.o3m_hot .text.o3m_hot.*)
    . = ALIGN(16);
  }
  .o3m_cold (DEFINED(O3M_COLD_AT) ? O3M_COLD_AT : .) : ALIGN(16)
  {
    KEEP(*(.o3m_entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(16);
  }
  .o3m_forbidden_data : { *(.data .data.* .bss .bss.* COMMON) }
  .o3m_forbidden_init : { *(.init_array .init_array.* .ctors .ctors.* .preinit_array .fini_array .fini_array.* .dtors .dtors.*) }
  /DISCARD/ : { *(.ARM.exidx .ARM.exidx.* .ARM.extab .ARM.extab.* .comment .note .note.* .eh_frame .eh_frame.*) }
}
