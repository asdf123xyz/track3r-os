// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The i.MX RT10xx's 160 device interrupts in vector-table order, once: the vector table, the weak default handlers and the `Irq`
// enumeration are all generated from this list, so a line cannot be in one and missing from another. Numbers and names follow the
// i.MX RT1050 Reference Manual's interrupt table (chapter "Interrupts and DMA Events"; the names are the device header's IRQn names, a
// fact table). A handler is `<name>_IRQHandler`, `extern "C"`: defining one in any file replaces the weak default, which records a crash
// with the interrupt's number (src/startup.cpp).
//
// "Reserved" lines exist in the table but have no peripheral behind them; they are named so that a spurious one shows up in the crash
// record by name. Reserved86 (line 70) is this port's software-pended render line (irq_map.h): a line nobody drives is a software
// interrupt once the NVIC pends it, the way the MK66 image uses its SWI line.

#include <cstdint>

#define O3OS_RT10XX_IRQ_LIST(X) \
    X(0, DMA0_DMA16) X(1, DMA1_DMA17) X(2, DMA2_DMA18) X(3, DMA3_DMA19) \
    X(4, DMA4_DMA20) X(5, DMA5_DMA21) X(6, DMA6_DMA22) X(7, DMA7_DMA23) \
    X(8, DMA8_DMA24) X(9, DMA9_DMA25) X(10, DMA10_DMA26) X(11, DMA11_DMA27) \
    X(12, DMA12_DMA28) X(13, DMA13_DMA29) X(14, DMA14_DMA30) X(15, DMA15_DMA31) \
    X(16, DMA_ERROR) X(17, CTI0_ERROR) X(18, CTI1_ERROR) X(19, CORE) \
    X(20, LPUART1) X(21, LPUART2) X(22, LPUART3) X(23, LPUART4) \
    X(24, LPUART5) X(25, LPUART6) X(26, LPUART7) X(27, LPUART8) \
    X(28, LPI2C1) X(29, LPI2C2) X(30, LPI2C3) X(31, LPI2C4) \
    X(32, LPSPI1) X(33, LPSPI2) X(34, LPSPI3) X(35, LPSPI4) \
    X(36, CAN1) X(37, CAN2) X(38, FLEXRAM) X(39, KPP) \
    X(40, TSC_DIG) X(41, GPR_IRQ) X(42, LCDIF) X(43, CSI) \
    X(44, PXP) X(45, WDOG2) X(46, SNVS_HP_WRAPPER) X(47, SNVS_HP_WRAPPER_TZ) \
    X(48, SNVS_LP_WRAPPER) X(49, CSU) X(50, DCP) X(51, DCP_VMI) \
    X(52, Reserved68) X(53, TRNG) X(54, SJC) X(55, BEE) \
    X(56, SAI1) X(57, SAI2) X(58, SAI3_RX) X(59, SAI3_TX) \
    X(60, SPDIF) X(61, PMU_EVENT) X(62, Reserved78) X(63, TEMP_LOW_HIGH) \
    X(64, TEMP_PANIC) X(65, USB_PHY1) X(66, USB_PHY2) X(67, ADC1) \
    X(68, ADC2) X(69, DCDC) X(70, Reserved86) X(71, Reserved87) \
    X(72, GPIO1_INT0) X(73, GPIO1_INT1) X(74, GPIO1_INT2) X(75, GPIO1_INT3) \
    X(76, GPIO1_INT4) X(77, GPIO1_INT5) X(78, GPIO1_INT6) X(79, GPIO1_INT7) \
    X(80, GPIO1_Combined_0_15) X(81, GPIO1_Combined_16_31) X(82, GPIO2_Combined_0_15) X(83, GPIO2_Combined_16_31) \
    X(84, GPIO3_Combined_0_15) X(85, GPIO3_Combined_16_31) X(86, GPIO4_Combined_0_15) X(87, GPIO4_Combined_16_31) \
    X(88, GPIO5_Combined_0_15) X(89, GPIO5_Combined_16_31) X(90, FLEXIO1) X(91, FLEXIO2) \
    X(92, WDOG1) X(93, RTWDOG) X(94, EWM) X(95, CCM_1) \
    X(96, CCM_2) X(97, GPC) X(98, SRC) X(99, Reserved115) \
    X(100, GPT1) X(101, GPT2) X(102, PWM1_0) X(103, PWM1_1) \
    X(104, PWM1_2) X(105, PWM1_3) X(106, PWM1_FAULT) X(107, Reserved123) \
    X(108, FLEXSPI) X(109, SEMC) X(110, USDHC1) X(111, USDHC2) \
    X(112, USB_OTG2) X(113, USB_OTG1) X(114, ENET) X(115, ENET_1588_Timer) \
    X(116, XBAR1_IRQ_0_1) X(117, XBAR1_IRQ_2_3) X(118, ADC_ETC_IRQ0) X(119, ADC_ETC_IRQ1) \
    X(120, ADC_ETC_IRQ2) X(121, ADC_ETC_ERROR_IRQ) X(122, PIT) X(123, ACMP1) \
    X(124, ACMP2) X(125, ACMP3) X(126, ACMP4) X(127, Reserved143) \
    X(128, Reserved144) X(129, ENC1) X(130, ENC2) X(131, ENC3) \
    X(132, ENC4) X(133, TMR1) X(134, TMR2) X(135, TMR3) \
    X(136, TMR4) X(137, PWM2_0) X(138, PWM2_1) X(139, PWM2_2) \
    X(140, PWM2_3) X(141, PWM2_FAULT) X(142, PWM3_0) X(143, PWM3_1) \
    X(144, PWM3_2) X(145, PWM3_3) X(146, PWM3_FAULT) X(147, PWM4_0) \
    X(148, PWM4_1) X(149, PWM4_2) X(150, PWM4_3) X(151, PWM4_FAULT) \
    X(152, Reserved168) X(153, Reserved169) X(154, Reserved170) X(155, Reserved171) \
    X(156, Reserved172) X(157, Reserved173) X(158, SJC_ARM_DEBUG) X(159, NMI_WAKEUP)

namespace o3os::platform::rt10xx {

inline constexpr unsigned kIrqCount = 160;

enum class Irq : std::uint8_t {
#define O3OS_RT10XX_IRQ_ENUM(number, name) name = number,
  O3OS_RT10XX_IRQ_LIST(O3OS_RT10XX_IRQ_ENUM)
#undef O3OS_RT10XX_IRQ_ENUM
};

namespace detail {
inline constexpr std::uint8_t kIrqNumbers[] = {
#define O3OS_RT10XX_IRQ_NUMBER(number, name) number,
    O3OS_RT10XX_IRQ_LIST(O3OS_RT10XX_IRQ_NUMBER)
#undef O3OS_RT10XX_IRQ_NUMBER
};
constexpr bool irq_numbers_sequential() noexcept {
  for (unsigned i = 0; i < sizeof(kIrqNumbers); ++i) {
    if (kIrqNumbers[i] != i) {
      return false;
    }
  }
  return true;
}
}  // namespace detail

static_assert(sizeof(detail::kIrqNumbers) == kIrqCount, "160 device interrupts");
static_assert(detail::irq_numbers_sequential(), "the list is in vector order, 0..159, none skipped or repeated");

}  // namespace o3os::platform::rt10xx
