// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A builder of synthetic ProTracker MOD and Scream Tracker 3 S3M files for the tests of the importers (OS-MODIMP): the C++ twin of tools/mod_build.py, written from the layouts of
// docs/formats/mod.md and not from the reader. Nothing here is a real module: every note, name and sample is made up.

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace o3os::test::modimp {

using Bytes = std::vector<std::uint8_t>;

inline constexpr std::uint16_t kPeriods[36] = {856, 808, 762, 720, 678, 640, 604, 570, 538, 508, 480, 453, 428, 404, 381, 360, 339, 320,
                                               302, 285, 269, 254, 240, 226, 214, 202, 190, 180, 170, 160, 151, 143, 135, 127, 120, 113};

// ---- MOD --------------------------------------------------------------------------------------------------------------------------------

struct ModSample {
  Bytes data;                 // signed 8-bit; an odd length is padded to whole words
  std::string name;
  int volume = 64;
  int finetune = 0;           // -8..7
  int loop_start = 0;         // words
  int loop_len = 0;           // words (<= 1: no loop)
};

struct ModCell {
  int period = 0;             // the raw 12 bits (kPeriods[i] for note i)
  int sample = 0;
  int effect = 0;
  int param = 0;
};

struct ModSpec {
  std::string tag = "M.K.";
  int channels = 4;
  std::string name = "synthetic";
  int restart = 127;
  std::vector<int> orders{0};                             // the stored pattern numbers (FLT8: even)
  std::map<int, int> junk;                                // order position behind the song length -> pattern number
  std::vector<std::map<std::pair<int, int>, ModCell>> patterns{{}};   // each: (row, channel) -> cell
  std::vector<ModSample> samples = std::vector<ModSample>(31);
  int truncate_by = 0;
};

inline ModCell note(int index, int sample, int effect = 0, int param = 0) {
  ModCell c;
  c.period = kPeriods[index];
  c.sample = sample;
  c.effect = effect;
  c.param = param;
  return c;
}

inline Bytes build(const ModSpec& m) {
  const bool flt8 = m.tag == "FLT8";
  std::vector<int> table(128, 0);
  for (std::size_t i = 0; i < m.orders.size(); ++i) table[i] = m.orders[i];
  for (const auto& j : m.junk) table[static_cast<std::size_t>(j.first)] = j.second;
  int top = 0;
  for (int v : table) top = v > top ? v : top;
  const int count = flt8 ? (top >> 1) + 1 : top + 1;
  Bytes out;
  auto put_name = [&out](const std::string& s, std::size_t w) {
    for (std::size_t i = 0; i < w; ++i) out.push_back(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
  };
  put_name(m.name, 20);
  for (const ModSample& s : m.samples) {
    const std::size_t words = (s.data.size() + 1) / 2;
    put_name(s.name, 22);
    out.push_back(static_cast<std::uint8_t>(words >> 8));
    out.push_back(static_cast<std::uint8_t>(words));
    out.push_back(static_cast<std::uint8_t>(s.finetune & 15));
    out.push_back(static_cast<std::uint8_t>(s.volume));
    out.push_back(static_cast<std::uint8_t>(s.loop_start >> 8));
    out.push_back(static_cast<std::uint8_t>(s.loop_start));
    out.push_back(static_cast<std::uint8_t>(s.loop_len >> 8));
    out.push_back(static_cast<std::uint8_t>(s.loop_len));
  }
  out.push_back(static_cast<std::uint8_t>(m.orders.size()));
  out.push_back(static_cast<std::uint8_t>(m.restart));
  for (int v : table) out.push_back(static_cast<std::uint8_t>(v));
  for (char ch : m.tag) out.push_back(static_cast<std::uint8_t>(ch));
  auto cell_bytes = [&out](const ModCell& c) {
    out.push_back(static_cast<std::uint8_t>((c.sample & 0xF0) | (c.period >> 8)));
    out.push_back(static_cast<std::uint8_t>(c.period & 0xFF));
    out.push_back(static_cast<std::uint8_t>(((c.sample & 15) << 4) | (c.effect & 15)));
    out.push_back(static_cast<std::uint8_t>(c.param));
  };
  for (int p = 0; p < count; ++p) {
    static const std::map<std::pair<int, int>, ModCell> none;
    const auto& pat = static_cast<std::size_t>(p) < m.patterns.size() ? m.patterns[static_cast<std::size_t>(p)] : none;
    auto at = [&pat](int row, int ch) {
      auto it = pat.find({row, ch});
      return it == pat.end() ? ModCell() : it->second;
    };
    if (flt8) {
      for (int half = 0; half < 2; ++half)
        for (int row = 0; row < 64; ++row)
          for (int c = 0; c < 4; ++c) cell_bytes(at(row, c + 4 * half));
    } else {
      for (int row = 0; row < 64; ++row)
        for (int c = 0; c < m.channels; ++c) cell_bytes(at(row, c));
    }
  }
  for (const ModSample& s : m.samples) {
    out.insert(out.end(), s.data.begin(), s.data.end());
    if (s.data.size() % 2) out.push_back(0);
  }
  if (m.truncate_by > 0) out.resize(out.size() - static_cast<std::size_t>(m.truncate_by));
  return out;
}

// ---- S3M --------------------------------------------------------------------------------------------------------------------------------

struct S3mCell {
  int note = 255;             // the raw byte: 255 none, 254 cut, octave << 4 | semitone
  int inst = 0;
  int vol = 255;
  int cmd = 0;                // 1..26 = A..Z
  int info = 0;
};

struct S3mInst {
  Bytes data;                 // as stored
  std::string name;
  std::uint32_t c2spd = 8363;
  int volume = 64;
  bool loop = false;
  std::uint32_t loop_start = 0, loop_end = 0;
  bool sixteen = false;
  bool stereo = false;
  int type = 1;               // 1 PCM, 0 empty, 2.. AdLib
  bool packed = false;
};

struct S3mSpec {
  std::string name = "synthetic";
  std::vector<int> orders{0};
  std::vector<S3mInst> instruments;
  std::vector<std::map<std::pair<int, int>, S3mCell>> patterns{{}};
  std::vector<int> channels = {0, 8, 1, 9};   // padded with 255 to 32
  int speed = 6, tempo = 125, global_volume = 64, master_volume = 0xB0;
  bool is_signed = false;
  int flags = 0;
  int cwtv = 0x1320;
  std::vector<int> pan_table;                 // 32 entries or empty
  std::vector<int> empty_patterns;
};

inline Bytes build(const S3mSpec& m) {
  const std::size_t ord_num = m.orders.size(), ins_num = m.instruments.size(), pat_num = m.patterns.size();
  const std::size_t header_len = 96 + ord_num + 2 * ins_num + 2 * pat_num + (m.pan_table.empty() ? 0 : 32);
  std::size_t pos = (header_len + 15) / 16 * 16;
  std::vector<std::size_t> ins_pos, pat_pos, data_pos;
  std::vector<Bytes> packed;
  for (std::size_t i = 0; i < ins_num; ++i) {
    ins_pos.push_back(pos / 16);
    pos += 80;
  }
  for (std::size_t p = 0; p < pat_num; ++p) {
    bool empty = false;
    for (int e : m.empty_patterns) empty = empty || static_cast<std::size_t>(e) == p;
    if (empty) {
      pat_pos.push_back(0);
      packed.emplace_back();
      continue;
    }
    Bytes body;
    for (int row = 0; row < 64; ++row) {
      for (int ch = 0; ch < 32; ++ch) {
        auto it = m.patterns[p].find({row, ch});
        if (it == m.patterns[p].end()) continue;
        const S3mCell& c = it->second;
        int b = ch;
        Bytes e;
        if (c.note != 255 || c.inst != 0) {
          b |= 0x20;
          e.push_back(static_cast<std::uint8_t>(c.note));
          e.push_back(static_cast<std::uint8_t>(c.inst));
        }
        if (c.vol != 255) {
          b |= 0x40;
          e.push_back(static_cast<std::uint8_t>(c.vol));
        }
        if (c.cmd != 0 || c.info != 0) {
          b |= 0x80;
          e.push_back(static_cast<std::uint8_t>(c.cmd));
          e.push_back(static_cast<std::uint8_t>(c.info));
        }
        if (b == ch) continue;
        body.push_back(static_cast<std::uint8_t>(b));
        body.insert(body.end(), e.begin(), e.end());
      }
      body.push_back(0);
    }
    Bytes pk;
    pk.push_back(static_cast<std::uint8_t>((body.size() + 2) & 0xFF));
    pk.push_back(static_cast<std::uint8_t>((body.size() + 2) >> 8));
    pk.insert(pk.end(), body.begin(), body.end());
    pat_pos.push_back(pos / 16);
    pos += (pk.size() + 15) / 16 * 16;
    packed.push_back(pk);
  }
  for (const S3mInst& i : m.instruments) {
    data_pos.push_back(pos / 16);
    pos += (i.data.size() * (i.stereo ? 2 : 1) + 15) / 16 * 16;
  }
  Bytes out(pos, 0);
  auto w16 = [&out](std::size_t at, std::uint32_t v) {
    out[at] = static_cast<std::uint8_t>(v);
    out[at + 1] = static_cast<std::uint8_t>(v >> 8);
  };
  for (std::size_t i = 0; i < 28 && i < m.name.size(); ++i) out[i] = static_cast<std::uint8_t>(m.name[i]);
  out[28] = 0x1A;
  out[29] = 16;
  w16(32, static_cast<std::uint32_t>(ord_num));
  w16(34, static_cast<std::uint32_t>(ins_num));
  w16(36, static_cast<std::uint32_t>(pat_num));
  w16(38, static_cast<std::uint32_t>(m.flags));
  w16(40, static_cast<std::uint32_t>(m.cwtv));
  w16(42, m.is_signed ? 1u : 2u);
  std::memcpy(&out[44], "SCRM", 4);
  out[48] = static_cast<std::uint8_t>(m.global_volume);
  out[49] = static_cast<std::uint8_t>(m.speed);
  out[50] = static_cast<std::uint8_t>(m.tempo);
  out[51] = static_cast<std::uint8_t>(m.master_volume);
  out[53] = m.pan_table.empty() ? 0 : 252;
  for (std::size_t c = 0; c < 32; ++c) out[64 + c] = static_cast<std::uint8_t>(c < m.channels.size() ? m.channels[c] : 255);
  std::size_t at = 96;
  for (int o : m.orders) out[at++] = static_cast<std::uint8_t>(o);
  for (std::size_t p : ins_pos) {
    w16(at, static_cast<std::uint32_t>(p));
    at += 2;
  }
  for (std::size_t p : pat_pos) {
    w16(at, static_cast<std::uint32_t>(p));
    at += 2;
  }
  for (std::size_t i = 0; i < m.pan_table.size(); ++i) out[at + i] = static_cast<std::uint8_t>(m.pan_table[i]);
  for (std::size_t i = 0; i < ins_num; ++i) {
    const S3mInst& s = m.instruments[i];
    std::uint8_t* b = &out[ins_pos[i] * 16];
    b[0] = static_cast<std::uint8_t>(s.type);
    if (s.type == 1) {
      b[13] = static_cast<std::uint8_t>(data_pos[i] >> 16);
      b[14] = static_cast<std::uint8_t>(data_pos[i]);
      b[15] = static_cast<std::uint8_t>(data_pos[i] >> 8);
      const std::uint32_t frames = static_cast<std::uint32_t>(s.data.size() / (s.sixteen ? 2 : 1));
      for (int k = 0; k < 4; ++k) {
        b[16 + k] = static_cast<std::uint8_t>(frames >> (8 * k));
        b[20 + k] = static_cast<std::uint8_t>(s.loop_start >> (8 * k));
        b[24 + k] = static_cast<std::uint8_t>(s.loop_end >> (8 * k));
        b[32 + k] = static_cast<std::uint8_t>(s.c2spd >> (8 * k));
      }
      b[28] = static_cast<std::uint8_t>(s.volume);
      b[30] = s.packed ? 1 : 0;
      b[31] = static_cast<std::uint8_t>((s.loop ? 1 : 0) | (s.stereo ? 2 : 0) | (s.sixteen ? 4 : 0));
      std::memcpy(b + 76, "SCRS", 4);
    } else {
      std::memcpy(b + 76, "SCRI", 4);
    }
    for (std::size_t k = 0; k < 28 && k < s.name.size(); ++k) b[48 + k] = static_cast<std::uint8_t>(s.name[k]);
  }
  for (std::size_t p = 0; p < pat_num; ++p) {
    if (pat_pos[p] != 0) std::memcpy(&out[pat_pos[p] * 16], packed[p].data(), packed[p].size());
  }
  for (std::size_t i = 0; i < ins_num; ++i) {
    if (m.instruments[i].type == 1 && !m.instruments[i].data.empty()) std::memcpy(&out[data_pos[i] * 16], m.instruments[i].data.data(), m.instruments[i].data.size());
  }
  return out;
}

// A sine of `n` frames: signed 8-bit.
inline Bytes sine8(std::size_t n, int amp = 100) {
  Bytes b;
  for (std::size_t i = 0; i < n; ++i) {
    const double v = amp * std::sin(6.283185307179586 * static_cast<double>(i) / static_cast<double>(n));
    b.push_back(static_cast<std::uint8_t>(static_cast<int>(v < 0 ? v - 0.5 : v + 0.5) & 0xFF));
  }
  return b;
}

}  // namespace o3os::test::modimp
