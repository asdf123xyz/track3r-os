// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The USB facts of the i.MX RT10xx that the OTG1 device driver uses (OS-PORT-RT10XX phase 2), from the i.MX RT1050 Reference Manual,
// chapters "Universal Serial Bus Controller (USB)" (an EHCI-derived dual-role controller in device mode: the endpoint queue heads and
// transfer descriptors in memory, the ENDPT* registers), "USB PHY" and "CCM Analog" (the USB1 PLL), and "On-Chip OTP Controller
// (OCOTP)" (the unique id the serial number is made of). The two USB ports of the chip have dedicated pins: OTG1's DP/DN need no IOMUXC
// fact of the board. The driver (ci_device.h) is written against a register policy, so these addresses are the image's (usb/src/usb1.cpp)
// and the host tests use a fake.

#include <cstdint>

namespace o3os::platform::rt10xx::usb::soc {

// ---- The controller: USB1 (OTG1), the capability registers at the base, the operational registers from +0x140 -------------------
inline constexpr std::uintptr_t kUsb1 = 0x402E0000u;
inline constexpr std::uintptr_t kUsbcmd = kUsb1 + 0x140u;
inline constexpr std::uintptr_t kUsbsts = kUsb1 + 0x144u;
inline constexpr std::uintptr_t kUsbintr = kUsb1 + 0x148u;
inline constexpr std::uintptr_t kFrindex = kUsb1 + 0x14Cu;
inline constexpr std::uintptr_t kDeviceaddr = kUsb1 + 0x154u;
inline constexpr std::uintptr_t kEndptlistaddr = kUsb1 + 0x158u;
inline constexpr std::uintptr_t kBurstsize = kUsb1 + 0x160u;
inline constexpr std::uintptr_t kPortsc1 = kUsb1 + 0x184u;
inline constexpr std::uintptr_t kOtgsc = kUsb1 + 0x1A4u;
inline constexpr std::uintptr_t kUsbmode = kUsb1 + 0x1A8u;
inline constexpr std::uintptr_t kEndptsetupstat = kUsb1 + 0x1ACu;
inline constexpr std::uintptr_t kEndptprime = kUsb1 + 0x1B0u;
inline constexpr std::uintptr_t kEndptflush = kUsb1 + 0x1B4u;
inline constexpr std::uintptr_t kEndptstat = kUsb1 + 0x1B8u;
inline constexpr std::uintptr_t kEndptcomplete = kUsb1 + 0x1BCu;
inline constexpr std::uintptr_t kEndptctrl0 = kUsb1 + 0x1C0u;  // ENDPTCTRL1..7 follow, one word each
inline constexpr unsigned kHardwareEndpoints = 8;

// USBCMD
inline constexpr std::uint32_t kCmdRunStop = 1u << 0;     // RS: the D+ pull-up (device mode)
inline constexpr std::uint32_t kCmdReset = 1u << 1;       // RST: controller reset, self-clearing
inline constexpr std::uint32_t kCmdSetupTripwire = 1u << 13;  // SUTW: the setup-buffer read guard
inline constexpr std::uint32_t kCmdAddTripwire = 1u << 14;    // ATDTW (not used: one descriptor a direction, primed only when idle)
// USBSTS (write 1 to clear) and USBINTR (the same bits)
inline constexpr std::uint32_t kStsUsbInt = 1u << 0;      // UI: a transfer completed or a SETUP arrived
inline constexpr std::uint32_t kStsUsbError = 1u << 1;    // UEI
inline constexpr std::uint32_t kStsPortChange = 1u << 2;  // PCI: connect, speed, resume
inline constexpr std::uint32_t kStsSystemError = 1u << 4; // SEI
inline constexpr std::uint32_t kStsReset = 1u << 6;       // URI: bus reset
inline constexpr std::uint32_t kStsSof = 1u << 7;         // SRI: start of frame (every 1 ms at full speed)
inline constexpr std::uint32_t kStsSuspend = 1u << 8;     // SLI
inline constexpr std::uint32_t kIntrWorkingSet = kStsUsbInt | kStsUsbError | kStsPortChange | kStsSystemError | kStsReset | kStsSof | kStsSuspend;
// PORTSC1
inline constexpr std::uint32_t kPortConnect = 1u << 0;    // CCS
inline constexpr std::uint32_t kPortReset = 1u << 8;      // PR: the bus reset is in progress
inline constexpr std::uint32_t kPortSuspend = 1u << 7;    // SUSP
inline constexpr std::uint32_t kPortForceResume = 1u << 6;
inline constexpr std::uint32_t kPortForceFullSpeed = 1u << 24;  // PFSC: never negotiate high speed (the CDC rings are 64-byte packets)
inline constexpr std::uint32_t kPortSpeedShift = 26;      // PSPD: 0 full, 1 low, 2 high
// USBMODE
inline constexpr std::uint32_t kModeDevice = 0x2u;         // CM
inline constexpr std::uint32_t kModeSetupLockoutOff = 1u << 3;  // SLOM: the tripwire replaces the lockout
// DEVICEADDR
inline constexpr std::uint32_t kDeviceAddrShift = 25;
inline constexpr std::uint32_t kDeviceAddrAdvance = 1u << 24;   // USBADRA: apply after the next IN status stage (not used: the control pipe says when)
// ENDPTCTRLn: receive in the low half, transmit in the high half
inline constexpr std::uint32_t kEpRxStall = 1u << 0;
inline constexpr std::uint32_t kEpRxTypeShift = 2;
inline constexpr std::uint32_t kEpRxToggleReset = 1u << 6;
inline constexpr std::uint32_t kEpRxEnable = 1u << 7;
inline constexpr std::uint32_t kEpTxStall = 1u << 16;
inline constexpr std::uint32_t kEpTxTypeShift = 18;
inline constexpr std::uint32_t kEpTxToggleReset = 1u << 22;
inline constexpr std::uint32_t kEpTxEnable = 1u << 23;
inline constexpr std::uint32_t kEpTypeControl = 0u;
inline constexpr std::uint32_t kEpTypeIsochronous = 1u;
inline constexpr std::uint32_t kEpTypeBulk = 2u;
inline constexpr std::uint32_t kEpTypeInterrupt = 3u;
// ENDPTPRIME / ENDPTSTAT / ENDPTCOMPLETE / ENDPTFLUSH: receive endpoint n is bit n, transmit endpoint n is bit 16 + n.
constexpr std::uint32_t endpoint_bit(unsigned number, bool transmit) noexcept { return 1u << (number + (transmit ? 16u : 0u)); }

// ---- The PHY: USBPHY1, with SET (+4), CLR (+8) and TOG (+C) aliases of its registers ---------------------------------------------
inline constexpr std::uintptr_t kUsbphy1 = 0x400D9000u;
inline constexpr std::uintptr_t kPhyPwd = kUsbphy1 + 0x00u;
inline constexpr std::uintptr_t kPhyCtrl = kUsbphy1 + 0x30u;
inline constexpr std::uint32_t kPhyCtrlSoftReset = 1u << 31;  // SFTRST
inline constexpr std::uint32_t kPhyCtrlClockGate = 1u << 30;  // CLKGATE
inline constexpr std::uint32_t kPhyCtrlUtmiLevel3 = 1u << 15; // ENUTMILEVEL3
inline constexpr std::uint32_t kPhyCtrlUtmiLevel2 = 1u << 14; // ENUTMILEVEL2
inline constexpr std::uint32_t kPhyCtrlHostDisconnect = 1u << 1;
// ---- The USB1 PLL (PLL3, 480 MHz): CCM_ANALOG_PLL_USB1 with its SET and CLR aliases ------------------------------------------------
inline constexpr std::uintptr_t kPllUsb1 = 0x400D8010u;
inline constexpr std::uint32_t kPllUsb1EnUsbClks = 1u << 6;   // the PHY's clock
inline constexpr std::uint32_t kPllUsb1Power = 1u << 12;
inline constexpr std::uint32_t kPllUsb1Enable = 1u << 13;
inline constexpr std::uint32_t kPllUsb1Bypass = 1u << 16;
inline constexpr std::uint32_t kPllUsb1Lock = 1u << 31;
// The non-core registers: the OTG1 control word (over-current input ignored: the port is a device).
inline constexpr std::uintptr_t kUsbncOtg1Ctrl = 0x402E0200u;
inline constexpr std::uint32_t kUsbncOverCurrentDisable = 1u << 7;

// ---- The unique id (OCOTP HW_OCOTP_CFG0 and CFG1: 64 bits) ----------------------------------------------------------------------
// The bring-up's other words (the vendor's and the hobbyist ecosystem's proven RT10xx USB sequence):
inline constexpr std::uintptr_t kCcgr2 = 0x400FC070u;
inline constexpr std::uint32_t kCcgr2Ocotp = 0x3u << 12;  // CG6: the OCOTP, whose fuses give the serial (opened before the first read)
inline constexpr std::uintptr_t kPmuReg3p0 = 0x400D8120u;  // the USB 3.0 V regulator: OUTPUT_TRG 12:8, BO_OFFSET 6:4, ENABLE_LINREG 0
inline constexpr std::uint32_t kPmuReg3p0Value = (0x0Fu << 8) | (6u << 4) | 1u;
inline constexpr std::uintptr_t kUsb1ChrgDetectSet = 0x400D81B4u;  // USB_ANALOG_USB1_CHRG_DETECT_SET: EN_B 20, CHK_CHRG_B 19 (charger detection off)
inline constexpr std::uint32_t kChrgDetectOff = (1u << 20) | (1u << 19);
inline constexpr std::uintptr_t kCscmr1 = 0x400FC01Cu;     // FLEXSPI_CLK_SEL 30:29: 1 = PLL3, 3 = PLL3 PFD0
inline constexpr std::uintptr_t kOcotpCfg0 = 0x401F4410u;
inline constexpr std::uintptr_t kOcotpCfg1 = 0x401F4420u;

// ---- What the memory must be ------------------------------------------------------------------------------------------------------
// The endpoint queue head list is 2 KiB aligned (ENDPTLISTADDR holds bits 31:11), each head 64 bytes, each transfer descriptor 32 bytes;
// all of it and the packet buffers in memory the controller's bus master reaches and the core does not cache: the OCRAM (sections.h).
inline constexpr std::uint32_t kQueueHeadListAlign = 2048u;
inline constexpr std::uint32_t kQueueHeadBytes = 64u;
inline constexpr std::uint32_t kTransferDescriptorBytes = 32u;

}  // namespace o3os::platform::rt10xx::usb::soc
