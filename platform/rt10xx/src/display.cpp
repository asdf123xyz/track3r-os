// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx's display (OS-PORT-RT10XX phase 4, display.h): an LPSPI4 link, polled, under the MK66's FT812 driver. The link is the MK66's
// DspiLink (platform/mk66 display/src/device.cpp) on this chip's LPSPI: a byte is one TDR write and one RDR read, bounded; chip select is a
// GPIO, so a transaction is whatever the driver puts between select() and deselect(). There is no bulk engine yet (max_len 0): the
// display runs its polled path for frames and uploads (Ft812Display::start(false)).
#include "internal.h"
#include "regs.h"

#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>
#include <o3os/platform/mk66/display/link.h>
#include <o3os/platform/rt10xx/display.h>
#include <o3os/platform/rt10xx/display_config.h>
#include <o3os/platform/rt10xx/platform.h>

namespace o3os::platform::rt10xx::display {
namespace {

namespace cfg = display_cfg;
namespace mdisplay = o3os::platform::mk66::display;
using regs::at;

class LpspiLink final : public mdisplay::Link {
 public:
  bool begin() noexcept {
    const std::uint32_t pll = at<std::uint32_t>(cfg::kPllSys);
    if ((pll & (1u << 31)) == 0u || (pll & ((1u << 16) | (1u << 12))) != 0u) {
      return false;  // PLL2 not locked, bypassed or powered down: the root would not be 132 MHz
    }
    at<std::uint32_t>(cfg::kCcgr1) &= ~cfg::kCcgr1Lpspi4;  // gated while the root changes
    at<std::uint32_t>(cfg::kCbcmr) = (at<std::uint32_t>(cfg::kCbcmr) & ~cfg::kCbcmrLpspiMask) | cfg::kCbcmrLpspiValue;
    at<std::uint32_t>(cfg::kCcgr1) |= cfg::kCcgr1Lpspi4;
    at<std::uint32_t>(cfg::kCcgr0) |= cfg::kCcgr0Gpio2;
    // Chip select: a GPIO output, high (idle), before the pad leaves its reset function.
    at<std::uint32_t>(cfg::kGpio2DrSet) = cfg::kCsBit;
    at<std::uint32_t>(cfg::kGpio2Gdir) |= cfg::kCsBit;
    at<std::uint32_t>(cfg::kCs.pad_ctl) = cfg::kPadWord;
    at<std::uint32_t>(cfg::kCs.mux_ctl) = cfg::kCs.alt;
    // The module: reset, master, the init rate, enabled; then the SPI pads.
    at<std::uint32_t>(cfg::kCr) = cfg::kCrRst;
    at<std::uint32_t>(cfg::kCr) = 0;
    at<std::uint32_t>(cfg::kIer) = 0;
    at<std::uint32_t>(cfg::kDer) = 0;
    at<std::uint32_t>(cfg::kCfgr0) = 0;
    at<std::uint32_t>(cfg::kCfgr1) = cfg::kCfgr1Master;
    at<std::uint32_t>(cfg::kFcr) = 0;
    set_speed(mdisplay::SpiSpeed::Init);
    const audio_pins::Pad pads[] = {cfg::kSdi, cfg::kSdo, cfg::kSck};
    for (const audio_pins::Pad& p : pads) {
      at<std::uint32_t>(p.pad_ctl) = cfg::kPadWord;
      at<std::uint32_t>(p.select_input) = p.select_value;
      at<std::uint32_t>(p.mux_ctl) = p.alt;
    }
    error_ = false;
    return true;
  }
  void select() noexcept override { at<std::uint32_t>(cfg::kGpio2DrClear) = cfg::kCsBit; }
  void deselect() noexcept override {
    // The last byte has been read back (xfer() waits for it), so the clock has stopped before chip select rises.
    at<std::uint32_t>(cfg::kGpio2DrSet) = cfg::kCsBit;
  }
  std::uint8_t xfer(std::uint8_t out) noexcept override {
    at<std::uint32_t>(cfg::kTdr) = out;
    for (std::uint32_t i = 0; i < cfg::kBytePolls; ++i) {
      if ((at<std::uint32_t>(cfg::kRsr) & cfg::kRsrRxEmpty) == 0u) {
        return static_cast<std::uint8_t>(at<std::uint32_t>(cfg::kRdr));
      }
    }
    error_ = true;
    ++errors_;
    at<std::uint32_t>(cfg::kCr) = cfg::kCrMen | cfg::kCrRtf | cfg::kCrRrf;  // the FIFOs flushed: the next byte starts clean
    return 0xFF;
  }
  void power_down(bool) noexcept override {}  // PD# is not driven (display_config.h): the driver's soft reset stands in
  void set_speed(mdisplay::SpiSpeed speed) noexcept override {
    const cfg::Rate r = speed == mdisplay::SpiSpeed::Init ? cfg::kInitRate : cfg::kDataRate;
    at<std::uint32_t>(cfg::kCr) = 0;  // CCR and TCR are written with the module off: no byte in flight takes half a rate
    at<std::uint32_t>(cfg::kCcr) = cfg::ccr(r);
    at<std::uint32_t>(cfg::kCr) = cfg::kCrMen | cfg::kCrRtf | cfg::kCrRrf;
    at<std::uint32_t>(cfg::kSr) = cfg::kSrClearAll;
    at<std::uint32_t>(cfg::kTcr) = cfg::tcr(r);
  }
  void delay_ms(std::uint32_t ms) noexcept override { boot::delay_ms(ms); }
  std::uint32_t now_ms() const noexcept override { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }
  bool take_error() noexcept override {
    const bool e = error_;
    error_ = false;
    return e;
  }
  std::uint32_t errors() const noexcept { return errors_; }

 private:
  bool error_ = false;
  std::uint32_t errors_ = 0;
};

// No DMA yet: every start() is refused, so the driver's polled path carries everything.
class NoBulk final : public mdisplay::BulkEngine {
 public:
  std::size_t max_len() const noexcept override { return 0; }
  bool start(const std::uint8_t*, std::size_t, std::size_t) noexcept override { return false; }
  mdisplay::BulkStatus poll() noexcept override { return mdisplay::BulkStatus::Idle; }
  void abort() noexcept override {}
};

LpspiLink g_link;
NoBulk g_bulk;
mdisplay::Chip g_chip(g_link);
mdisplay::Ft812Display g_screen(g_chip, g_bulk);
bool g_started = false;

}  // namespace

mdisplay::InitReport start() noexcept {
  mdisplay::InitReport report{};
  if (g_started) {
    report.status = mdisplay::InitStatus::Ok;
    return report;
  }
  if (!g_link.begin()) {
    report.status = mdisplay::InitStatus::NoChip;
    report.bus_error = true;
    return report;
  }
  const mdisplay::InitConfig config{};
  report = g_chip.init(config);
  if (report.status == mdisplay::InitStatus::Ok) {
    g_link.set_speed(mdisplay::SpiSpeed::Data);
    g_screen.start(false, mdisplay::kBacklightDefault, config);
    g_started = true;
  }
  return report;
}

bool started() noexcept { return g_started; }
mdisplay::Ft812Display& screen() noexcept { return g_screen; }
mdisplay::Chip& chip() noexcept { return g_chip; }
std::uint32_t link_errors() noexcept { return g_link.errors(); }

void service() noexcept {
  if (g_started) {
    g_screen.service();
  }
}

// Eight vertical colour bars and a white frame line (FT81x display-list words, the Programmers Guide's encodings).
bool draw_test_frame() noexcept {
  if (!g_started || !g_screen.begin_frame()) {
    return false;
  }
  constexpr std::uint32_t kClearColorBlack = 0x02000000u;
  constexpr std::uint32_t kClearAll = 0x26000007u;
  constexpr std::uint32_t kBeginRects = 0x1F000009u;
  constexpr std::uint32_t kBeginLineStrip = 0x1F000004u;
  constexpr std::uint32_t kEnd = 0x21000000u;
  constexpr std::uint32_t kLineWidth16 = 0x0E000010u;  // 1 px (1/16 units)
  auto vertex = [](std::uint32_t x, std::uint32_t y) { return 0x40000000u | ((x * 16u & 0x7FFFu) << 15) | (y * 16u & 0x7FFFu); };
  constexpr std::uint32_t kColours[8] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x404040};
  std::uint32_t words[4 + 8 * 3 + 9];
  std::size_t n = 0;
  words[n++] = kClearColorBlack;
  words[n++] = kClearAll;
  words[n++] = kBeginRects;
  for (std::uint32_t i = 0; i < 8u; ++i) {
    words[n++] = 0x04000000u | kColours[i];  // COLOR_RGB
    words[n++] = vertex(i * 100u, 40u);
    words[n++] = vertex(i * 100u + 99u, 439u);
  }
  words[n++] = 0x04FFFFFFu;
  words[n++] = kLineWidth16;
  words[n++] = kBeginLineStrip;
  words[n++] = vertex(0, 0);
  words[n++] = vertex(799, 0);
  words[n++] = vertex(799, 479);
  words[n++] = vertex(0, 479);
  words[n++] = vertex(0, 0);
  words[n++] = kEnd;
  const bool ok = g_screen.write(words, n);
  if (ok) {
    g_screen.end_frame();
  } else {
    g_screen.abort_frame();
  }
  return ok;
}

}  // namespace o3os::platform::rt10xx::display
