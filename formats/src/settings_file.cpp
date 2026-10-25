// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/settings.h>

#include "settings_chunks.h"

#include <o3os/formats/crc32.h>

#include <cstring>

namespace o3os::formats::settings {

using native::codec::Cursor;
using native::codec::Enc;
using native::codec::get16;
using native::codec::get32;

namespace {

namespace sc = settings_chunks;

constexpr std::size_t kHeader = native::codec::kChunkHeader;
constexpr std::size_t kFormHeader = 12;  // "FORM", length, "O3ST"

struct Found {
  bool present = false;
  std::size_t at = 0;   // offset of the payload
  std::size_t len = 0;
  bool newer_minor = false;
};

}  // namespace

const char* error_name(Error e) noexcept {
  switch (e) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad magic";
    case Error::BadCrc: return "bad CRC";
    case Error::NewerMajor: return "newer major version";
    case Error::Malformed: return "malformed";
    case Error::TooBig: return "too big";
  }
  return "unknown";
}

bool sniff(const std::uint8_t* head, std::size_t n) noexcept {
  return n >= kFormHeader && std::memcmp(head, "FORM", 4) == 0 && get32(head + 8) == kTypeO3st;
}

Report read(const std::uint8_t* data, std::size_t size, model::Settings& out) noexcept {
  Report r;
  out.init(core::ParamKind::Device);
  const auto fail = [&](Error e, const char* why, std::size_t at) {
    r.error = e;
    r.detail = why;
    r.offset = static_cast<std::uint32_t>(at);
    out.init(core::ParamKind::Device);  // never half-read: the defaults
    return r;
  };
  if (data == nullptr || size < kFormHeader) {
    return fail(size >= 4 && data != nullptr && std::memcmp(data, "FORM", 4) != 0 ? Error::BadMagic : Error::Truncated,
                size >= 4 ? "shorter than the FORM header" : "empty", 0);
  }
  if (std::memcmp(data, "FORM", 4) != 0 || get32(data + 8) != kTypeO3st) {
    return fail(Error::BadMagic, "not an O3ST settings file", 0);
  }
  const std::uint32_t len = get32(data + 4);
  if (len < 4 || (len & 1u) != 0) {
    return fail(Error::Malformed, "bad FORM length", 4);
  }
  if (len > kMaxFileBytes - 8u) {
    return fail(Error::TooBig, "file larger than the limit", 4);
  }
  if (size < static_cast<std::size_t>(len) + 8u) {
    return fail(Error::Truncated, "the file ends before its FORM does", size);
  }

  // Pass 1: the container. Every chunk's header, length, pad and version, then the CRC (the last chunk). Bytes after the FORM are not looked at.
  Found sets;
  Found lrnb;
  const std::size_t end = static_cast<std::size_t>(len) + 8u;
  std::size_t pos = kFormHeader;
  bool crc_seen = false;
  while (pos < end && !crc_seen) {
    if (end - pos < kHeader) {
      return fail(Error::Malformed, "chunk header overruns the FORM", pos);
    }
    const std::uint32_t id = get32(data + pos);
    const std::size_t plen = get32(data + pos + 4);
    const std::size_t pad = plen & 1u;
    r.chunk = id;
    if (plen > end - pos - kHeader || plen + pad > end - pos - kHeader) {
      return fail(Error::Malformed, "chunk overruns the FORM", pos);
    }
    const std::size_t payload = pos + kHeader;
    if (pad != 0 && data[payload + plen] != 0) {
      return fail(Error::Malformed, "pad byte is not zero", payload + plen);
    }
    if (id == native::kChunkCrc || id == sc::kChunkSets || id == sc::kChunkLrnb) {
      if (plen < 2) {
        return fail(Error::Malformed, "chunk length out of range", pos);
      }
      const std::uint16_t version = get16(data + payload);
      if ((version >> 8) == 0) {
        return fail(Error::Malformed, "chunk version 0", payload);
      }
      if ((version >> 8) > (native::kChunkVersion >> 8)) {
        return fail(Error::NewerMajor, "chunk written by a newer major version of the format", payload);
      }
      const bool newer = (version & 0xFFu) > (native::kChunkVersion & 0xFFu);
      r.newer_minor = r.newer_minor || newer;
      if (id == native::kChunkCrc) {
        if (plen < native::codec::kCrcPayload || (!newer && plen != native::codec::kCrcPayload)) {
          return fail(Error::Malformed, "chunk length out of range", pos);
        }
        if (pos + kHeader + plen + pad != end) {
          return fail(Error::Malformed, "the CRC chunk is not the last", pos);
        }
        if (get32(data + payload + 2) != crc32(data, pos)) {
          return fail(Error::BadCrc, "the CRC chunk does not match the file", pos);
        }
        crc_seen = true;
      } else {
        Found& f = id == sc::kChunkSets ? sets : lrnb;
        if (f.present) {
          return fail(Error::Malformed, "chunk repeated", pos);
        }
        f = Found{true, payload, plen, newer};
      }
    } else {
      if (!model::is_printable_fourcc(id)) {
        return fail(Error::Malformed, "chunk id is not four printable characters", pos);
      }
      ++r.unknown_chunks;  // a chunk of a newer version of this format, or someone else's: ignored
    }
    pos += kHeader + plen + pad;
  }
  if (!crc_seen) {
    return fail(Error::Malformed, "the FORM ends without a CRC chunk", pos);
  }

  // Pass 2: the settings, then the bindings, into the defaults.
  sc::Counts counts;
  if (sets.present) {
    Cursor c(data + sets.at, sets.len);
    r.chunk = sc::kChunkSets;
    if (!sc::decode_sets(c, sets.newer_minor, out, counts)) {
      return fail(Error::Malformed, "SETS does not fit its entry count", sets.at);
    }
  }
  if (lrnb.present) {
    Cursor c(data + lrnb.at, lrnb.len);
    r.chunk = sc::kChunkLrnb;
    if (!sc::decode_lrnb(c, lrnb.newer_minor, out, counts)) {
      return fail(Error::Malformed, "LRNB does not fit its entry count", lrnb.at);
    }
  }
  r.skipped_entries = counts.skipped;
  return r;
}

std::size_t write(const model::Settings& s, std::uint8_t* out, std::size_t cap) noexcept {
  if (out == nullptr || s.kind() != core::ParamKind::Device || !s.check_invariants()) {
    return 0;
  }
  const std::size_t sets_payload = sc::sets_payload_size(s);
  const bool has_lrnb = s.learn_count() != 0;
  const std::size_t lrnb_payload = has_lrnb ? sc::lrnb_payload_size(s) : 0;
  const std::size_t total = kFormHeader + native::codec::chunk_total(sets_payload) +
                            (has_lrnb ? native::codec::chunk_total(lrnb_payload) : 0u) +
                            native::codec::chunk_total(native::codec::kCrcPayload);
  if (total > cap || total > kMaxFileBytes) {
    return 0;
  }
  Enc e(out, cap);
  e.u32(native::kForm);
  e.u32(static_cast<std::uint32_t>(total - 8));
  e.u32(kTypeO3st);
  e.u32(sc::kChunkSets);
  e.u32(static_cast<std::uint32_t>(sets_payload));
  sc::encode_sets(e, s);
  if ((sets_payload & 1u) != 0) {
    e.u8(0);
  }
  if (has_lrnb) {
    e.u32(sc::kChunkLrnb);
    e.u32(static_cast<std::uint32_t>(lrnb_payload));
    sc::encode_lrnb(e, s);
    if ((lrnb_payload & 1u) != 0) {
      e.u8(0);
    }
  }
  const std::size_t body = e.size();
  e.u32(native::kChunkCrc);
  e.u32(static_cast<std::uint32_t>(native::codec::kCrcPayload));
  e.u16(native::kChunkVersion);
  e.u32(crc32(out, body));
  return e.overflow() || e.size() != total ? 0 : total;
}

}  // namespace o3os::formats::settings
