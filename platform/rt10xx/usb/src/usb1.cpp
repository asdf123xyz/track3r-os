// SPDX-License-Identifier: GPL-3.0-or-later
//
// The OTG1 USB device of the rt10xx image (OS-PORT-RT10XX phase 2): the clock and PHY bring-up and the controller's set-up from the
// i.MX RT1050 Reference Manual ("USB", "USB PHY", "CCM Analog"), the register policy the driver (ci_device.h) is written against, the
// driver objects, the interrupt handler. The driver's memory (queue heads, descriptors, packet buffers) is in the OCRAM, which the
// controller's bus master reaches and the core does not cache (mpu_config.h).
#include <o3os/platform/rt10xx/usb/usb1.h>

#include "../../src/internal.h"
#include "../../src/regs.h"

#include <o3os/platform/rt10xx/irq_list.h>
#include <o3os/platform/rt10xx/sections.h>
#include <o3os/platform/rt10xx/usb/soc_facts.h>
#include <o3os/usb/descriptors.h>

#include <cstdint>
#include <new>

namespace o3os::platform::rt10xx::usb1 {
namespace {

namespace ci = o3os::platform::rt10xx::usb;
namespace soc = ci::soc;
using regs::at;

template <typename Predicate>
bool wait_until(Predicate done, std::uint32_t polls = 1'000'000u) noexcept {
  for (std::uint32_t i = 0; i < polls; ++i) {
    if (done()) {
      return true;
    }
  }
  return false;
}

// ---- The register policy ----------------------------------------------------------------------------------------------------------------
struct Hw {
  std::uint32_t usbsts() noexcept { return at<std::uint32_t>(soc::kUsbsts); }
  void usbsts_clear(std::uint32_t bits) noexcept { at<std::uint32_t>(soc::kUsbsts) = bits; }
  std::uint32_t usbcmd() noexcept { return at<std::uint32_t>(soc::kUsbcmd); }
  void usbcmd(std::uint32_t v) noexcept { at<std::uint32_t>(soc::kUsbcmd) = v; }
  std::uint32_t portsc() noexcept { return at<std::uint32_t>(soc::kPortsc1); }
  void portsc(std::uint32_t v) noexcept { at<std::uint32_t>(soc::kPortsc1) = v; }
  std::uint32_t endptsetupstat() noexcept { return at<std::uint32_t>(soc::kEndptsetupstat); }
  void endptsetupstat_clear(std::uint32_t bits) noexcept { at<std::uint32_t>(soc::kEndptsetupstat) = bits; }
  std::uint32_t endptcomplete() noexcept { return at<std::uint32_t>(soc::kEndptcomplete); }
  void endptcomplete_clear(std::uint32_t bits) noexcept { at<std::uint32_t>(soc::kEndptcomplete) = bits; }
  std::uint32_t endptprime() noexcept { return at<std::uint32_t>(soc::kEndptprime); }
  void endptprime_set(std::uint32_t bits) noexcept { at<std::uint32_t>(soc::kEndptprime) = bits; }
  std::uint32_t endptstat() noexcept { return at<std::uint32_t>(soc::kEndptstat); }
  std::uint32_t endptflush() noexcept { return at<std::uint32_t>(soc::kEndptflush); }
  void endptflush_set(std::uint32_t bits) noexcept { at<std::uint32_t>(soc::kEndptflush) = bits; }
  std::uint32_t endptctrl(unsigned n) noexcept { return at<std::uint32_t>(soc::kEndptctrl0 + 4u * n); }
  void endptctrl(unsigned n, std::uint32_t v) noexcept { at<std::uint32_t>(soc::kEndptctrl0 + 4u * n) = v; }
  void deviceaddr(std::uint32_t v) noexcept { at<std::uint32_t>(soc::kDeviceaddr) = v; }
  void controller_reinit() noexcept;
  std::uint32_t bus_address(const void* p) noexcept { return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(p)); }
  void dmb() noexcept { asm volatile("dmb" ::: "memory"); }
  std::uint32_t cycles() noexcept { return regs::dwt_cyccnt(); }
};

// ---- The objects ------------------------------------------------------------------------------------------------------------------------
O3OS_OCRAM_NOINIT ci::Memory g_memory;  // the controller's: OCRAM, 2 KiB aligned by its type
Hw g_hw;
char g_serial[17] = "0000000000000000";
o3os::usb::Config g_config;
std::uint8_t g_device_descriptor[o3os::usb::kDeviceDescriptorSize];
std::uint8_t g_config_descriptor[o3os::usb::kMaxConfigDescriptorSize];
alignas(o3os::usb::ControlPipe) unsigned char g_pipe_storage[sizeof(o3os::usb::ControlPipe)];
o3os::usb::ControlPipe* g_pipe = nullptr;
o3os::usb::CdcTxRing g_cdc_tx;
o3os::usb::CdcRxRing g_cdc_rx;
o3os::usb::MidiRing g_midi_to_host;
o3os::usb::MidiRing g_midi_from_host;
alignas(ci::CiDevice<Hw>) unsigned char g_dev_storage[sizeof(ci::CiDevice<Hw>)];
ci::CiDevice<Hw>* g_dev = nullptr;
alignas(o3os::usb::CdcTxWriter) unsigned char g_writer_storage[sizeof(o3os::usb::CdcTxWriter)];
o3os::usb::CdcTxWriter* g_writer = nullptr;
bool g_attached = false;
const char* g_failure = "";

constexpr unsigned kIrqLine = static_cast<unsigned>(Irq::USB_OTG1);

void hex16(std::uint32_t high, std::uint32_t low, char* out) noexcept {
  const char* digits = "0123456789ABCDEF";
  for (unsigned i = 0; i < 8u; ++i) {
    out[i] = digits[(high >> (28u - 4u * i)) & 0xFu];
    out[8u + i] = digits[(low >> (28u - 4u * i)) & 0xFu];
  }
  out[16] = '\0';
}

// The controller's set-up (RM "Device Controller Initialization"): reset, device mode with the setup lockout off, the queue head list,
// full speed forced, the interrupts of the working set. The run bit (the pull-up) is the caller's.
bool controller_setup() noexcept {
  at<std::uint32_t>(soc::kUsbcmd) = soc::kCmdReset;
  if (!wait_until([] { return (at<std::uint32_t>(soc::kUsbcmd) & soc::kCmdReset) == 0u; })) {
    return false;
  }
  at<std::uint32_t>(soc::kUsbmode) = soc::kModeDevice | soc::kModeSetupLockoutOff;
  if (!wait_until([] { return (at<std::uint32_t>(soc::kUsbmode) & 0x3u) == soc::kModeDevice; })) {
    return false;
  }
  at<std::uint32_t>(soc::kEndptlistaddr) = g_hw.bus_address(&g_memory.heads[0]);
  at<std::uint32_t>(soc::kPortsc1) = at<std::uint32_t>(soc::kPortsc1) | soc::kPortForceFullSpeed;
  at<std::uint32_t>(soc::kUsbsts) = 0xFFFFFFFFu;
  at<std::uint32_t>(soc::kUsbintr) = soc::kIntrWorkingSet;
  return true;
}

void Hw::controller_reinit() noexcept {
  (void)controller_setup();  // a failure here leaves the driver's prepare() to run on a controller that is not listening; counted by it as a late reset
  at<std::uint32_t>(soc::kUsbcmd) = at<std::uint32_t>(soc::kUsbcmd) | soc::kCmdRunStop;
}

}  // namespace

bool start() noexcept {
  g_failure = "";
  // The unique id, for the serial. The OCOTP's clock gate is opened first: a bootloader that closed it would make the read a bus fault.
  regs::at<std::uint32_t>(soc::kCcgr2) = regs::at<std::uint32_t>(soc::kCcgr2) | soc::kCcgr2Ocotp;
  hex16(at<std::uint32_t>(soc::kOcotpCfg1), at<std::uint32_t>(soc::kOcotpCfg0), g_serial);

  // The descriptors and the control pipe: STANDARD mode, full speed, no USB RETURN (there is no audio yet).
  g_config.mode = o3os::usb::Mode::Standard;
  g_config.speed = o3os::usb::Speed::Full;
  g_config.usb_return = false;
  g_config.serial = g_serial;
  const std::size_t dev_len = o3os::usb::build_device_descriptor(g_config, g_device_descriptor, sizeof g_device_descriptor);
  const std::size_t cfg_len = o3os::usb::build_config_descriptor(g_config, g_config_descriptor, sizeof g_config_descriptor);
  if (dev_len == 0u || cfg_len == 0u) {
    g_failure = "descriptors";
    return false;
  }
  g_pipe = new (g_pipe_storage) o3os::usb::ControlPipe(g_config, g_device_descriptor, dev_len, g_config_descriptor, cfg_len);
  g_dev = new (g_dev_storage) ci::CiDevice<Hw>(g_hw, g_memory, *g_pipe, g_cdc_tx, g_cdc_rx, g_midi_to_host, g_midi_from_host);
  g_writer = new (g_writer_storage) o3os::usb::CdcTxWriter(g_cdc_tx);

  // The clocks: the USB controller's gate, then PLL3's USB clock output (the PLL itself is already running: the FlexSPI the code runs from
  // takes its clock from PLL3's PFD0, so it is enabled, powered and not bypassed; the SET alias adds the USB clock without touching the rest).
  regs::ccm_ccgr6() = regs::ccm_ccgr6() | regs::kCcgr6Usboh3;
  // The 3.0 V regulator of the PHY on, the charger detection off (it loads the data lines), as the proven RT10xx sequences do.
  at<std::uint32_t>(soc::kPmuReg3p0) = soc::kPmuReg3p0Value;
  at<std::uint32_t>(soc::kUsb1ChrgDetectSet) = soc::kChrgDetectOff;
  at<std::uint32_t>(soc::kPllUsb1 + 4u) = soc::kPllUsb1EnUsbClks | soc::kPllUsb1Power | soc::kPllUsb1Enable;
  if (!wait_until([] { return (at<std::uint32_t>(soc::kPllUsb1) & soc::kPllUsb1Lock) != 0u; })) {
    g_failure = "pll3 lock";
    return false;
  }
  if ((at<std::uint32_t>(soc::kPllUsb1) & soc::kPllUsb1Bypass) != 0u) {
    // A bypassed PLL3 gives the PHY 24 MHz, not 480. Taken out of bypass unless the FlexSPI the code runs from is clocked from it.
    const std::uint32_t flexspi_sel = (at<std::uint32_t>(soc::kCscmr1) >> 29) & 3u;
    if (flexspi_sel == 1u || flexspi_sel == 3u) {
      g_failure = "pll3 bypassed under the FlexSPI";
      return false;
    }
    at<std::uint32_t>(soc::kPllUsb1 + 8u) = soc::kPllUsb1Bypass;
  }
  // The PHY held in soft reset while the controller resets (whatever the bootloader left running), then out of reset and clock gating,
  // powered up, the UTMI levels on (RM "USB PHY" initialisation).
  at<std::uint32_t>(soc::kPhyCtrl + 4u) = soc::kPhyCtrlSoftReset;
  at<std::uint32_t>(soc::kUsbcmd) = at<std::uint32_t>(soc::kUsbcmd) & ~soc::kCmdRunStop;
  at<std::uint32_t>(soc::kUsbcmd) = soc::kCmdReset;
  (void)wait_until([] { return (at<std::uint32_t>(soc::kUsbcmd) & soc::kCmdReset) == 0u; });
  regs::nvic_icpr(kIrqLine / 32u) = 1u << (kIrqLine % 32u);
  at<std::uint32_t>(soc::kPhyCtrl + 8u) = soc::kPhyCtrlSoftReset;
  at<std::uint32_t>(soc::kPhyCtrl + 8u) = soc::kPhyCtrlClockGate;
  at<std::uint32_t>(soc::kPhyPwd) = 0u;
  at<std::uint32_t>(soc::kPhyCtrl + 4u) = soc::kPhyCtrlUtmiLevel2 | soc::kPhyCtrlUtmiLevel3;
  // The port is a device: the over-current input is ignored.
  at<std::uint32_t>(soc::kUsbncOtg1Ctrl) = at<std::uint32_t>(soc::kUsbncOtg1Ctrl) | soc::kUsbncOverCurrentDisable;

  if (!controller_setup()) {
    g_failure = "controller reset";
    return false;
  }
  g_dev->prepare();
  // The interrupt line (its priority is the boot's, irq_map.h: the Usb row), then the pull-up.
  regs::nvic_iser(kIrqLine / 32u) = 1u << (kIrqLine % 32u);
  at<std::uint32_t>(soc::kUsbcmd) = at<std::uint32_t>(soc::kUsbcmd) | soc::kCmdRunStop;
  g_attached = true;
  return true;
}

const char* start_failure() noexcept { return g_failure; }
bool attached() noexcept { return g_attached; }
const o3os::usb::ControlPipe& control() noexcept { return *g_pipe; }
const ci::CiCounters& counters() noexcept { return g_dev->counters(); }
o3os::usb::CdcTxWriter& cdc_writer() noexcept { return *g_writer; }
o3os::usb::CdcRxRing& cdc_rx() noexcept { return g_cdc_rx; }
o3os::usb::MidiRing& midi_to_host() noexcept { return g_midi_to_host; }
o3os::usb::MidiRing& midi_from_host() noexcept { return g_midi_from_host; }
const char* serial() noexcept { return g_serial; }

void kick() noexcept {
  if (!g_attached) {
    return;
  }
  std::uint32_t primask;
  asm volatile("mrs %0, primask\n\tcpsid i" : "=r"(primask)::"memory");
  g_dev->service_rings();
  asm volatile("msr primask, %0" ::"r"(primask) : "memory");
}

}  // namespace o3os::platform::rt10xx::usb1

extern "C" void USB_OTG1_IRQHandler() noexcept {
  if (o3os::platform::rt10xx::usb1::attached()) {
    o3os::platform::rt10xx::usb1::g_dev->isr();
  }
}
