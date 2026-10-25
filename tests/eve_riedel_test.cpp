// SPDX-License-Identifier: GPL-3.0-or-later
//
// The pinned Riedel library (third_party/eve-riedel, MIT, tag 5.0.9, unmodified) as an oracle for the FT812 display driver
// (platform/mk66/display): it is compiled for the host with a test port that routes its SPI, pin and delay hooks into the
// FT812 model of the display tests, and its register definitions, panel preset, EVE_init() order, command-FIFO burst and
// coprocessor fault recovery are compared with the driver's own, which is written fresh from the Programmers Guide. The
// driver follows his order with attribution (display/chip.h); this file is where every difference is written down, so a
// change on either side is a failing test, not a silent drift. Host only (C library, no QEMU).
#include <doctest/doctest.h>

#include "../platform/mk66/tests/mk66_display_fakes.h"

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/platform/mk66/display/chip.h>
#include <o3os/platform/mk66/display/display.h>
#include <o3os/platform/mk66/display/ft812_regs.h>
#include <o3os/platform/mk66/display/panel.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

extern "C" {
// The oracle's C side (tests/support/eve_riedel_oracle.c, eve_riedel_rvt70.c) ...
int o3_riedel_lookup(const char* name, std::uint32_t* out);
unsigned o3_riedel_constant_count(void);
void o3_riedel_panel(std::uint32_t out[15]);
void o3_riedel_rvt70_panel(std::uint32_t out[15]);
std::uint8_t o3_riedel_init(void);
std::uint8_t o3_riedel_busy(void);
void o3_riedel_burst_word(std::uint32_t word);
// ... and the hooks of its target port (tests/support/eve_riedel_port/EVE_target.h), implemented below on the model.
void o3_port_pdn(int asserted);
void o3_port_cs(int asserted);
void o3_port_delay_ms(unsigned ms);
std::uint8_t o3_port_xfer(std::uint8_t out);
}

namespace display = o3os::platform::mk66::display;
namespace ft = o3os::platform::mk66::display::ft812;
using o3os::platform::mk66::display::fake::FakeChip;

namespace {

FakeChip* g_chip = nullptr;

// Binds the library's port to a model for the duration of a scope.
struct Bind {
  explicit Bind(FakeChip& chip) { g_chip = &chip; }
  ~Bind() { g_chip = nullptr; }
  Bind(const Bind&) = delete;
  Bind& operator=(const Bind&) = delete;
};

using Write = std::pair<std::uint32_t, std::uint32_t>;

std::uint32_t riedel(const char* name) {
  std::uint32_t v = 0;
  const int found = o3_riedel_lookup(name, &v);
  CHECK_MESSAGE(found == 1, name);
  return v;
}

std::vector<std::string> host_commands(const FakeChip& chip) {
  std::vector<std::string> out;
  for (const std::string& line : chip.transcript()) {
    if (line.compare(0, 5, "host ") == 0) {
      out.push_back(line.substr(5, 2));
    }
  }
  return out;
}

std::vector<std::uint32_t> delays(const FakeChip& chip) {
  std::vector<std::uint32_t> out;
  for (const std::string& line : chip.transcript()) {
    if (line.compare(0, 6, "delay ") == 0) {
      out.push_back(static_cast<std::uint32_t>(std::stoul(line.substr(6))));
    }
  }
  return out;
}

bool contains(const std::vector<Write>& v, const Write& w) {
  for (const Write& x : v) {
    if (x == w) {
      return true;
    }
  }
  return false;
}

}  // namespace

extern "C" void o3_port_pdn(int asserted) { g_chip->power_down(asserted != 0); }
extern "C" void o3_port_cs(int asserted) {
  if (asserted != 0) {
    g_chip->select();
  } else {
    g_chip->deselect();
  }
}
extern "C" void o3_port_delay_ms(unsigned ms) { g_chip->delay_ms(ms); }
extern "C" std::uint8_t o3_port_xfer(std::uint8_t out) { return g_chip->xfer(out); }

TEST_CASE("eve_riedel: every register, host command and display-list word the driver uses equals the pinned header's") {
  struct Item {
    const char* name;
    std::uint32_t ours;
  };
  const Item items[] = {
      {"RAM_G", ft::kRamG},
      {"RAM_DL", ft::kRamDl},
      {"RAM_REG", ft::kRamReg},
      {"RAM_CMD", ft::kRamCmd},
      {"RAM_G_SIZE", ft::kRamGBytes},
      {"RAM_DL_SIZE", ft::kRamDlBytes},
      {"CMDFIFO_SIZE", ft::kCmdFifoBytes},
      {"REG_ID", ft::reg::kId},
      {"REG_FRAMES", ft::reg::kFrames},
      {"REG_CLOCK", ft::reg::kClock},
      {"REG_CPURESET", ft::reg::kCpuReset},
      {"REG_HCYCLE", ft::reg::kHcycle},
      {"REG_HOFFSET", ft::reg::kHoffset},
      {"REG_HSIZE", ft::reg::kHsize},
      {"REG_HSYNC0", ft::reg::kHsync0},
      {"REG_HSYNC1", ft::reg::kHsync1},
      {"REG_VCYCLE", ft::reg::kVcycle},
      {"REG_VOFFSET", ft::reg::kVoffset},
      {"REG_VSIZE", ft::reg::kVsize},
      {"REG_VSYNC0", ft::reg::kVsync0},
      {"REG_VSYNC1", ft::reg::kVsync1},
      {"REG_DLSWAP", ft::reg::kDlSwap},
      {"REG_ROTATE", ft::reg::kRotate},
      {"REG_OUTBITS", ft::reg::kOutBits},
      {"REG_DITHER", ft::reg::kDither},
      {"REG_SWIZZLE", ft::reg::kSwizzle},
      {"REG_CSPREAD", ft::reg::kCspread},
      {"REG_PCLK_POL", ft::reg::kPclkPol},
      {"REG_PCLK", ft::reg::kPclk},
      {"REG_VOL_PB", ft::reg::kVolPb},
      {"REG_VOL_SOUND", ft::reg::kVolSound},
      {"REG_SOUND", ft::reg::kSound},
      {"REG_GPIOX_DIR", ft::reg::kGpioXDir},
      {"REG_GPIOX", ft::reg::kGpioX},
      {"REG_PWM_HZ", ft::reg::kPwmHz},
      {"REG_PWM_DUTY", ft::reg::kPwmDuty},
      {"REG_CMD_READ", ft::reg::kCmdRead},
      {"REG_CMD_WRITE", ft::reg::kCmdWrite},
      {"REG_CMD_DL", ft::reg::kCmdDl},
      {"REG_CMDB_SPACE", ft::reg::kCmdbSpace},
      {"REG_CMDB_WRITE", ft::reg::kCmdbWrite},
      {"HOST_ACTIVE", ft::host::kActive},
      {"HOST_CLKEXT", ft::host::kClkExt},
      {"HOST_CLKINT", ft::host::kClkInt},
      {"HOST_RST_PULSE", ft::host::kRstPulse},
      {"DLSWAP_FRAME", ft::kDlSwapFrame},
      {"DL_DISPLAY", ft::kDlDisplay},
      {"DL_CLEAR_COLOR_RGB", ft::kDlClearColorBlack},  // CLEAR_COLOR_RGB(0, 0, 0)
      {"DL_CLEAR_ALL", ft::kDlClearAll},
      {"MUTE", ft::kSoundMute},
      {"CMD_SNAPSHOT2", ft::kCmdSnapshot2},  // OS-HIL `diag shot`
      {"RGB565", ft::kFmtRgb565},
  };
  for (const Item& item : items) {
    CHECK_MESSAGE(riedel(item.name) == item.ours, item.name);
  }
  // every constant the oracle exports and the driver does not use is one of the Riedel-only registers, listed here
  CHECK(o3_riedel_constant_count() == sizeof(items) / sizeof(items[0]) + 3u);  // REG_GPIO, REG_TOUCH_MODE, REG_TOUCH_RZTHRESH
  CHECK(riedel("REG_GPIO") == 0x302094u);
  CHECK(riedel("REG_TOUCH_MODE") == 0x302104u);
  CHECK(riedel("REG_TOUCH_RZTHRESH") == 0x302118u);
}

TEST_CASE("eve_riedel: the Tracker's panel is the library's RVT70 preset except CSPREAD, and the custom module header agrees") {
  std::uint32_t custom[15];
  std::uint32_t preset[15];
  o3_riedel_panel(custom);
  o3_riedel_rvt70_panel(preset);
  const display::PanelTiming& p = display::kPanel800x480;
  const std::uint32_t ours[15] = {p.hsize, p.hcycle, p.hoffset, p.hsync0, p.hsync1, p.vsize, p.vcycle, p.voffset,
                                  p.vsync0, p.vsync1, p.pclk, p.pclk_pol, p.swizzle, p.cspread, 2u};
  for (unsigned i = 0; i < 15; ++i) {
    CHECK(custom[i] == ours[i]);  // the test's own custom header (o3_panel.h) is our panel
    if (i != 13) {
      CHECK(preset[i] == ours[i]);  // the library's RVT70: the same timing
    }
  }
  CHECK(preset[13] == 1u);  // his CSPREAD is 1 ...
  CHECK(p.cspread == 0);    // ... this OS writes 0: a bench item (DSP-9), not a decision made here
}

TEST_CASE("eve_riedel: EVE_init and the driver's init, run on the same model: what is shared keeps his order, what differs is listed") {
  FakeChip theirs;
  {
    Bind bind(theirs);
    CHECK(o3_riedel_init() == 0);  // E_OK
  }
  FakeChip ours;
  display::Chip driver(ours);
  CHECK(driver.init(display::InitConfig{}).status == display::InitStatus::Ok);

  const std::vector<Write>& rw = theirs.writes();
  const std::vector<Write>& ow = ours.writes();

  // Riedel-only: the touch engine (this OS's app never reads touch), REG_GPIO (the driver sets DISP through
  // REG_GPIOX), and his default backlight of 25 % written as the last thing (the driver leaves the
  // backlight off until the first real frame is on screen).
  const std::vector<Write> riedel_only = {{riedel("REG_TOUCH_MODE"), 3u},
                                          {riedel("REG_TOUCH_RZTHRESH"), 1200u},
                                          {riedel("REG_GPIO"), 0x80u},
                                          {riedel("REG_PWM_DUTY"), 0x20u}};
  // Driver-only: the pixel clock stopped before the timing is rewritten, the output depth and dither (his library sets
  // OUTBITS for one module only and no dither), and DISP through REG_GPIOX with its direction bit.
  const std::vector<Write> ours_only = {{ft::reg::kPclk, 0u},
                                        {ft::reg::kOutBits, 0x1B6u},
                                        {ft::reg::kDither, 1u},
                                        {ft::reg::kGpioX, 0x8000u},
                                        {ft::reg::kGpioXDir, 0x8000u}};

  // 1. Nothing the library writes is missing from the driver except the listed ones, and nothing extra is in the driver.
  for (const Write& w : rw) {
    if (!contains(riedel_only, w)) {
      CHECK_MESSAGE(contains(ow, w), "a register write of EVE_init the driver does not make: addr " << w.first << " value " << w.second);
    }
  }
  for (const Write& w : ow) {
    if (!contains(ours_only, w)) {
      CHECK_MESSAGE(contains(rw, w), "a register write of the driver that EVE_init does not make: addr " << w.first << " value " << w.second);
    }
  }
  for (const Write& w : riedel_only) {
    CHECK_MESSAGE(contains(rw, w), "listed Riedel-only write not made by EVE_init: " << w.first);
    CHECK_FALSE(contains(ow, w));
  }
  for (const Write& w : ours_only) {
    CHECK_MESSAGE(contains(ow, w), "listed driver-only write not made by the driver: " << w.first);
    CHECK_FALSE(contains(rw, w));
  }

  // 2. The shared writes happen in the same relative order.
  std::size_t at = 0;
  for (const Write& w : rw) {
    if (contains(riedel_only, w)) {
      continue;
    }
    std::size_t i = at;
    while (i < ow.size() && !(ow[i] == w)) {
      ++i;
    }
    CHECK_MESSAGE(i < ow.size(), "order: " << w.first << " = " << w.second << " is not after its predecessors in the driver");
    if (i >= ow.size()) {
      break;
    }
    at = i + 1;
  }

  // 3. The host commands: the same reset and ACTIVE; Riedel also selects the internal clock, which the driver leaves at the
  //    chip's reset default (HostClock::Internal sends it).
  CHECK(host_commands(theirs) == std::vector<std::string>({"68", "48", "00"}));
  CHECK(host_commands(ours) == std::vector<std::string>({"68", "00"}));

  // 4. Timings: the driver waits at least as long as his everywhere, and polls at least as long (the Guide's 300 ms
  //    for the reset where his is 50).
  const std::vector<std::uint32_t> dt = delays(theirs);
  const std::vector<std::uint32_t> da = delays(ours);
  CHECK(dt == std::vector<std::uint32_t>({6, 21, 40}));  // PD# low, PD# high, after ACTIVE
  CHECK(da == std::vector<std::uint32_t>({display::kPdLowMs, display::kPdHighWaitMs, display::kActiveWaitMs}));
  CHECK(display::kPdLowMs >= dt[0]);
  CHECK(display::kPdHighWaitMs >= dt[1]);
  CHECK(display::kActiveWaitMs >= dt[2]);
  CHECK(display::kIdPollLimit == 400u);   // his wait_regid
  CHECK(display::kResetPollLimit >= 50u);  // his wait_reset

  // 5. Both end with the pixel clock started (his with the backlight after it), and the driver's data speed switch is after it.
  CHECK(theirs.reg(ft::reg::kPclk) == 2u);
  CHECK(ours.reg(ft::reg::kPclk) == 2u);
  CHECK(theirs.reg(ft::reg::kRotate) == 0u);
  CHECK(ours.reg(ft::reg::kRotate) == 0u);
  CHECK(rw.back() == Write(riedel("REG_PWM_DUTY"), 0x20u));
  CHECK(ow.back() == Write(ft::reg::kPclk, 2u));
}

TEST_CASE("eve_riedel: the fault recovery is the same five register writes in the same order") {
  FakeChip theirs;
  display::Chip their_driver(theirs);
  CHECK(their_driver.init(display::InitConfig{}).status == display::InitStatus::Ok);  // a chip that is up for both
  FakeChip ours;
  display::Chip our_driver(ours);
  CHECK(our_driver.init(display::InitConfig{}).status == display::InitStatus::Ok);

  const std::size_t their_base = theirs.writes().size();
  theirs.inject_fault();
  {
    Bind bind(theirs);
    CHECK(o3_riedel_busy() == 14u);  // EVE_FAULT_RECOVERED
  }
  const std::size_t our_base = ours.writes().size();
  ours.inject_fault();
  CHECK(our_driver.cmd_check());

  const std::vector<Write> a(theirs.writes().begin() + static_cast<std::ptrdiff_t>(their_base), theirs.writes().end());
  const std::vector<Write> b(ours.writes().begin() + static_cast<std::ptrdiff_t>(our_base), ours.writes().end());
  CHECK(a.size() == 5u);
  CHECK(a == b);
  CHECK(a[0] == Write(ft::reg::kCpuReset, 1u));
  CHECK(a[4] == Write(ft::reg::kCpuReset, 0u));
}

TEST_CASE("eve_riedel: a command word goes through the REG_CMDB_WRITE window exactly as the library's burst sends it") {
  FakeChip theirs;
  display::Chip their_driver(theirs);
  CHECK(their_driver.init(display::InitConfig{}).status == display::InitStatus::Ok);
  FakeChip ours;
  display::Chip our_driver(ours);
  CHECK(our_driver.init(display::InitConfig{}).status == display::InitStatus::Ok);

  const std::uint32_t word = 0xFFFFFF00u;  // CMD_DLSTART
  const std::size_t their_txns = theirs.txns().size();
  {
    Bind bind(theirs);
    o3_riedel_burst_word(word);
  }
  const std::size_t our_txns = ours.txns().size();
  CHECK(our_driver.cmd_write(&word, 1, 50));

  // the library's burst is the last write transaction; the driver's is the last write transaction after its own space read
  const auto last_write = [](const FakeChip& chip, std::size_t from) {
    for (std::size_t i = chip.txns().size(); i-- > from;) {
      if (chip.txns()[i].kind == o3os::platform::mk66::display::fake::Txn::Kind::Write) {
        return chip.txns()[i];
      }
    }
    return o3os::platform::mk66::display::fake::Txn{o3os::platform::mk66::display::fake::Txn::Kind::Read, 0, 0, 0};
  };
  const auto a = last_write(theirs, their_txns);
  const auto b = last_write(ours, our_txns);
  CHECK(a.addr == ft::reg::kCmdbWrite);
  CHECK(b.addr == a.addr);
  CHECK(b.bytes == a.bytes);
  CHECK(b.bytes == 4u);
  CHECK(b.value == a.value);
  CHECK(a.value == word);
}

TEST_CASE("eve_riedel: the display's frame budget is the chip's list memory, and the builder's") {
  // The frame buffer of the driver, RAM_DL and eve::kMaxDisplayListWords are the same number: a frame the builder accepts is a
  // frame the display can take whole.
  CHECK(display::Ft812Display::kMaxFrameWords == o3os::eve::kMaxDisplayListWords);
  CHECK(display::Ft812Display::kMaxFrameWords == riedel("RAM_DL_SIZE") / 4u);
}
