// SPDX-License-Identifier: GPL-3.0-or-later
//
// The rt10xx's firmware update (update/firmware.h): checks over the MK66's FatFirmwareStore and the Tracker+ validator, then the official
// firmware's hand-over (_fwinfo, _turnon, reset).
#include <o3os/platform/rt10xx/update/firmware.h>

#include "internal.h"
#include "regs.h"

#include <o3os/platform/mk66/update/fat_firmware_store.h>
#include <o3os/platform/mk66/update/path_guard.h>
#include <o3os/platform/rt10xx/audio.h>
#include <o3os/platform/rt10xx/boot_image.h>
#include <o3os/platform/rt10xx/platform.h>
#include <o3os/platform/rt10xx/storage/card.h>

#include <cstring>

namespace o3os::platform::rt10xx::update {
namespace {

namespace mup = o3os::platform::mk66::update;

mup::FatFirmwareStore g_store(storage::fat(), storage::card());
char g_armed[mup::kImageNameMax + 1] = {};
std::uint32_t g_token = 0;
std::uint32_t g_armed_crc = 0;
std::uint32_t g_armed_at = 0;
std::uint8_t g_buffer[2048];

std::uint32_t now_ms() noexcept { return static_cast<std::uint32_t>(system_clock().uptime_ms()); }

}  // namespace

mup::FirmwareStore& store() noexcept { return g_store; }

CheckResult check(const char* name) noexcept {
  CheckResult r;
  if (name == nullptr || !image::listed_name(name) || !mup::valid_image_name(name)) {
    r.fault = Fault::BadName;
    return r;
  }
  std::uint32_t size = 0;
  r.fault = g_store.open_read(name, size);
  if (r.fault != Fault::Ok) {
    return r;
  }
  PtfValidator v;
  std::size_t got = 0;
  for (;;) {
    r.fault = g_store.read(g_buffer, sizeof g_buffer, got);
    if (r.fault != Fault::Ok || got == 0u) {
      break;
    }
    r.file_bytes += static_cast<std::uint32_t>(got);
    if (!v.feed(g_buffer, got)) {
      break;
    }
  }
  g_store.close_read();
  if (r.fault != Fault::Ok) {
    return r;
  }
  v.finish();
  r.fault = r.file_bytes != size ? Fault::Readback : v.fault();
  r.image_bytes = v.ok() ? v.image_length() : 0u;
  r.crc = v.crc();
  r.fault_offset = v.fault_offset();
  return r;
}

std::uint32_t arm(const char* name, CheckResult& result) noexcept {
  g_token = 0;
  result = check(name);
  if (result.fault != Fault::Ok) {
    return 0;
  }
  std::strncpy(g_armed, name, sizeof g_armed - 1u);
  g_armed[sizeof g_armed - 1u] = '\0';
  g_armed_crc = result.crc;
  g_armed_at = now_ms();
  // A one-use token from the cycle counter, the uptime and the CRC: not a secret, a guard against a typed or replayed command.
  std::uint32_t t = regs::dwt_cyccnt() ^ (g_armed_at * 2654435761u) ^ result.crc;
  t ^= t >> 15;
  t *= 0x2C1B3C6Du;
  t ^= t >> 12;
  g_token = t == 0u ? 1u : t;
  return g_token;
}

const char* armed_name() noexcept { return g_token != 0u ? g_armed : ""; }

Fault update(std::uint32_t token) noexcept {
  if (g_token == 0u || token != g_token) {
    return Fault::NotArmed;
  }
  g_token = 0;  // one use, whatever follows
  if (now_ms() - g_armed_at > kArmLifetimeMs) {
    return Fault::Expired;
  }
  const CheckResult again = check(g_armed);
  if (again.fault != Fault::Ok) {
    return again.fault;
  }
  if (again.crc != g_armed_crc) {
    return Fault::Crc;  // the file changed since `fw arm`
  }
  audio_stop();  // the outputs muted and the audio path stopped before anything is handed over
  Fault f = g_store.remove_fwinfo();
  if (f == Fault::Ok) {
    f = g_store.write_fwinfo(g_armed);
  }
  if (f == Fault::Ok) {
    f = g_store.write_turnon();
  }
  if (f != Fault::Ok) {
    (void)g_store.remove_turnon();
    (void)g_store.remove_fwinfo();
    return f;
  }
  boot::delay_ms(50);  // the console's last line leaves (the caller flushed it), the card is idle
  boot::reset_chip();
}

Fault clear() noexcept {
  g_token = 0;
  const Fault a = g_store.remove_turnon();
  const Fault b = g_store.remove_fwinfo();
  return a != Fault::Ok ? a : b;
}

}  // namespace o3os::platform::rt10xx::update
