// SPDX-License-Identifier: GPL-3.0-or-later
//
// Startup of the rt10xx image (OS-PORT-RT10XX): the vector table with every handler named, and the reset sequence. Order, and why
// (boot.cpp, flexram.h, mpu_config.h and clock_config.h have the sources of each step):
//
//   0. (asm, no stack) interrupts off; the FlexRAM banks assigned (flexram.h): the stack top of this image, 0x2005_8000, exists only
//      once the DTCM has its 352 KiB, so the bank word is written before the first push, then the stack pointer is loaded again from
//      the vector table (the bootloader handed over with its own)
//   1. VTOR: this image's table (a fault before this would go through the bootloader's); the crash context: stage Reset
//   2. the watchdogs off, within the window the chip allows
//   3. FPU on (the compiler may use it anywhere after this)
//   4. the C runtime: .data and .ramfunc_l copied from flash, .bss cleared (.noinit, .noinit_ocram and .noinit_sdram left alone)
//   5. the MPU
//   6. the cycle counter, then the clock tree (VDD_SOC, ARM PLL, dividers), failing visibly if a step does not complete
//   7. the SDRAM (phase 2, src/sdram.cpp): the SEMC and the tests, the outcome recorded, the boot going on either way; then the caches
//      (after the tests, which must read the cells, not a cache line)
//   8. interrupt priorities and fault handlers, SysTick, interrupts on
//   9. static constructors, then main
//
// Not here, on purpose: every peripheral but the SEMC (the USB controller is brought up by main, usb/src/usb1.cpp). The image is a probe
// (docs/specs/OS-PORT-RT10XX.md section 5): it proves the boot path on the unit before anything is built on it.
#include "internal.h"
#include "regs.h"

#include <o3os/platform/rt10xx/flexram.h>
#include <o3os/platform/rt10xx/irq_list.h>
#include <o3os/platform/rt10xx/mem_map.h>
#include <o3os/platform/rt10xx/platform.h>

#include <cstddef>
#include <cstdint>

// Linker script symbols (rt10xx.ld).
extern "C" {
extern std::uint32_t __data_load[];
extern std::uint32_t __data_start[];
extern std::uint32_t __data_end[];
extern std::uint32_t __ramfunc_l_load[];
extern std::uint32_t __ramfunc_l_start[];
extern std::uint32_t __ramfunc_l_end[];
extern std::uint32_t __bss_start[];
extern std::uint32_t __bss_end[];
extern std::uint32_t __stack_top[];
extern std::uint32_t __fault_stack_top[];
extern void (*__preinit_array_start[])();
extern void (*__preinit_array_end[])();
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

void Reset_Handler();
void Default_Handler() noexcept;
[[noreturn]] void o3os_reset_sequence();
[[noreturn]] void o3os_fault_entry(const std::uint32_t* frame, std::uint32_t exc_return);  // crash_store.cpp
}

// `main` is reached through an asm label: C++ does not let a program call ::main by its own name.
extern "C" int o3os_target_main() __asm__("main");

namespace rt10xx = o3os::platform::rt10xx;

// ---- Exception entry stubs ---------------------------------------------------------------------------------------------------------
// HardFault, MemManage, BusFault, UsageFault, NMI and every interrupt without a handler of its own end here. The stub finds the stack the
// exception frame was pushed on (EXC_RETURN bit 2), then moves to the fault stack before any C++ runs, so that a stack overflow does
// not make the reporter itself fault, and passes the frame and EXC_RETURN to o3os_fault_entry (crash_store.cpp). The MK66 image's stub.
extern "C" [[gnu::naked]] void Default_Handler() noexcept {
  asm volatile(
      "cpsid i\n\t"
      "tst lr, #4\n\t"
      "ite eq\n\t"
      "mrseq r0, msp\n\t"
      "mrsne r0, psp\n\t"
      "mov r1, lr\n\t"
      "ldr r2, =__fault_stack_top\n\t"
      "mov sp, r2\n\t"
      "b o3os_fault_entry\n\t"
      ".ltorg");
}

extern "C" void NMI_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void HardFault_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void MemManage_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void BusFault_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void UsageFault_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void SVC_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void DebugMon_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void PendSV_Handler() noexcept __attribute__((weak, alias("Default_Handler")));
extern "C" void SysTick_Handler();  // clock.cpp

// `noexcept` on every alias: GCC 13 refuses an alias of a nothrow target (the naked stub cannot throw) without it (-Wmissing-attributes).
#define O3OS_WEAK_IRQ(number, name) extern "C" void name##_IRQHandler() noexcept __attribute__((weak, alias("Default_Handler")));
O3OS_RT10XX_IRQ_LIST(O3OS_WEAK_IRQ)
#undef O3OS_WEAK_IRQ

// ---- Reset: step 0, before there is a stack --------------------------------------------------------------------------------------
// The two IOMUXC_GPR writes of flexram.h, then the stack pointer from the table, then the C++ sequence. Naked: no prologue may push
// anything before the DTCM is the size the table assumes. The operands are the headers' constants, so the values have one source.
static_assert(rt10xx::regs::kGpr17Address == 0x400AC044u && rt10xx::regs::kGpr16Address == 0x400AC040u, "the asm below names GPR17 and GPR16");
extern "C" [[gnu::naked]] void Reset_Handler() {
  asm volatile(
      "cpsid i\n\t"
      "ldr r0, =%c[gpr17]\n\t"
      "ldr r1, =%c[banks]\n\t"
      "str r1, [r0]\n\t"
      "ldr r0, =%c[gpr16]\n\t"
      "ldr r1, [r0]\n\t"
      "orr r1, r1, %[sel]\n\t"
      "str r1, [r0]\n\t"
      "dsb\n\t"
      "isb\n\t"
      "ldr r0, =__stack_top\n\t"
      "msr msp, r0\n\t"
      "isb\n\t"
      "b o3os_reset_sequence\n\t"
      ".ltorg"
      :
      : [gpr17] "i"(rt10xx::regs::kGpr17Address), [gpr16] "i"(rt10xx::regs::kGpr16Address),
        [banks] "i"(rt10xx::flexram::kBankConfig), [sel] "i"(rt10xx::flexram::kGpr16BankCfgSel));
}

// ---- Reset: steps 1 to 8, on the stack ---------------------------------------------------------------------------------------------
extern "C" [[noreturn]] void o3os_reset_sequence() {
  namespace regs = rt10xx::regs;
  namespace boot = rt10xx::boot;

  // 1. The vector table is this image's from here on; the crash context says Reset (it touches `.noinit` only, no C runtime needed).
  regs::scb_vtor() = rt10xx::mem::kImageBase;
  asm volatile("dsb\n\tisb" ::: "memory");
  boot::reset_context();

  // 2. Watchdogs.
  boot::disable_watchdogs();

  // 3. FPU: CP10 and CP11 full access.
  regs::scb_cpacr() = regs::scb_cpacr() | regs::kCpacrFullFpu;
  asm volatile("dsb\n\tisb" ::: "memory");

  // 4. C runtime. The loops must stay loops (startup.cpp is compiled with -fno-tree-loop-distribute-patterns).
  {
    const std::uint32_t* src = __data_load;
    for (std::uint32_t* dst = __data_start; dst < __data_end;) {
      *dst++ = *src++;
    }
    src = __ramfunc_l_load;
    for (std::uint32_t* dst = __ramfunc_l_start; dst < __ramfunc_l_end;) {
      *dst++ = *src++;
    }
    for (std::uint32_t* dst = __bss_start; dst < __bss_end;) {
      *dst++ = 0;
    }
    asm volatile("dsb\n\tisb" ::: "memory");  // code was just written as data: make the instruction stream see it
  }
  rt10xx::set_stage(rt10xx::crash::Stage::Memory);
  boot::capture_reset_cause();
  boot::capture_boot_record();

  // 5. The MPU table.
  boot::init_memory_protection();

  // 6. Clocks.
  rt10xx::set_stage(rt10xx::crash::Stage::Clocks);
  boot::enable_dwt();
  boot::init_clocks();

  // 7. The SDRAM and its tests (its outcome is sdram::status()), then the caches.
  rt10xx::set_stage(rt10xx::crash::Stage::Sdram);
  boot::init_sdram();
  boot::enable_caches();

  // 8. Interrupts.
  rt10xx::set_stage(rt10xx::crash::Stage::Interrupts);
  boot::init_interrupts();
  boot::start_systick();
  asm volatile("cpsie i" ::: "memory");

  // 9. Constructors and main.
  for (void (**f)() = __preinit_array_start; f < __preinit_array_end; ++f) {
    (*f)();
  }
  for (void (**f)() = __init_array_start; f < __init_array_end; ++f) {
    (*f)();
  }
  rt10xx::set_stage(rt10xx::crash::Stage::Running);
  o3os_target_main();

  // main never returns; if it ever does, that is a bug worth a crash record, not a silent spin.
  rt10xx::panic();
}

// ---- C++ runtime hooks -------------------------------------------------------------------------------------------------------------
extern "C" void __cxa_pure_virtual() { rt10xx::panic(); }
extern "C" {
void* __dso_handle = nullptr;
}
extern "C" int __cxa_atexit(void (*)(void*), void*, void*) { return 0; }

// ---- Vector table ------------------------------------------------------------------------------------------------------------------
namespace {

using Handler = void (*)();

#define O3OS_VECTOR_IRQ(number, name) name##_IRQHandler,

[[gnu::section(".vectors"), gnu::used]] const Handler kVectors[16 + rt10xx::kIrqCount] = {
    reinterpret_cast<Handler>(__stack_top),
    Reset_Handler,
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    nullptr,  // reserved
    nullptr,
    nullptr,
    nullptr,
    SVC_Handler,
    DebugMon_Handler,
    nullptr,  // reserved
    PendSV_Handler,
    SysTick_Handler,
    O3OS_RT10XX_IRQ_LIST(O3OS_VECTOR_IRQ)
};
#undef O3OS_VECTOR_IRQ

static_assert(sizeof(kVectors) == rt10xx::mem::kVectorTableBytes, "the table is the size the linker script reserves at the image base");

}  // namespace
