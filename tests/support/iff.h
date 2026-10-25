// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Test helpers to take a valid native file apart into chunks and put it back together, so a test can change
// one byte of one chunk (or a length, an order, a version) and still hand the reader a file whose FORM length
// and CRC are right: that is how the validation behind the CRC gets exercised.

#include <o3os/formats/crc32.h>
#include <o3os/formats/native.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace o3os::test {

struct RawChunk {
  std::uint32_t id;
  std::vector<std::uint8_t> payload;
};

inline std::uint32_t be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

inline void put_be32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 24));
  v.push_back(static_cast<std::uint8_t>(x >> 16));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}

inline void put_be16(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x >> 8));
  v.push_back(static_cast<std::uint8_t>(x));
}

// The chunks of a well-formed file in order, without the final CRC chunk.
inline std::vector<RawChunk> split_chunks(const std::vector<std::uint8_t>& file) {
  std::vector<RawChunk> out;
  std::size_t pos = 12;
  while (pos + 8 <= file.size()) {
    const std::uint32_t id = be32(&file[pos]);
    const std::uint32_t len = be32(&file[pos + 4]);
    pos += 8;
    if (id == formats::native::kChunkCrc) {
      break;
    }
    out.push_back({id, std::vector<std::uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(pos),
                                                 file.begin() + static_cast<std::ptrdiff_t>(pos + len))});
    pos += len + (len & 1u);
  }
  return out;
}

struct BuildOptions {
  bool crc_chunk = true;     // append the CRC chunk
  bool fix_crc = true;       // it matches; false: it is off by one bit
  std::uint8_t pad_byte = 0;
  std::uint32_t crc_version = formats::native::kChunkVersion;
  std::size_t crc_tail = 0;  // bytes appended to the CRC chunk's payload (what a newer minor would do)
};

// Stores the CRC of everything before `crc_chunk_at` (the offset of a CRC chunk's header) into that chunk.
inline void set_crc(std::vector<std::uint8_t>& file, std::size_t crc_chunk_at) {
  const std::uint32_t crc = formats::crc32(file.data(), crc_chunk_at);
  for (int i = 0; i < 4; ++i) {
    file[crc_chunk_at + 10 + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(crc >> (24 - 8 * i));
  }
}

inline std::vector<std::uint8_t> build_file(const std::vector<RawChunk>& chunks, const BuildOptions& opt = {}) {
  std::vector<std::uint8_t> body;
  for (const RawChunk& c : chunks) {
    put_be32(body, c.id);
    put_be32(body, static_cast<std::uint32_t>(c.payload.size()));
    body.insert(body.end(), c.payload.begin(), c.payload.end());
    if ((c.payload.size() & 1u) != 0) {
      body.push_back(opt.pad_byte);
    }
  }
  std::vector<std::uint8_t> file;
  put_be32(file, formats::native::kForm);
  put_be32(file, static_cast<std::uint32_t>(4 + body.size() +
                                            (opt.crc_chunk ? 14 + opt.crc_tail + (opt.crc_tail & 1u) : 0)));
  put_be32(file, formats::native::kTypeO3pj);
  file.insert(file.end(), body.begin(), body.end());
  if (opt.crc_chunk) {
    std::uint32_t crc = formats::crc32(file.data(), file.size());
    if (!opt.fix_crc) {
      crc ^= 1u;
    }
    put_be32(file, formats::native::kChunkCrc);
    put_be32(file, static_cast<std::uint32_t>(6 + opt.crc_tail));
    put_be16(file, opt.crc_version);
    put_be32(file, crc);
    file.insert(file.end(), opt.crc_tail, 0xCD);
    if ((opt.crc_tail & 1u) != 0) {
      file.push_back(opt.pad_byte);
    }
  }
  return file;
}

// Recomputes the FORM length and the CRC chunk of a file whose last 14 bytes are the CRC chunk (what a
// mutation leaves behind): lets a mutated file reach the parsers behind the integrity check.
inline void fix_up(std::vector<std::uint8_t>& file) {
  if (file.size() < 12 + 14) {
    return;
  }
  const std::uint32_t form = static_cast<std::uint32_t>(file.size() - 8);
  file[4] = static_cast<std::uint8_t>(form >> 24);
  file[5] = static_cast<std::uint8_t>(form >> 16);
  file[6] = static_cast<std::uint8_t>(form >> 8);
  file[7] = static_cast<std::uint8_t>(form);
  const std::size_t crc_at = file.size() - 4;
  const std::uint32_t crc = formats::crc32(file.data(), file.size() - 14);
  file[crc_at] = static_cast<std::uint8_t>(crc >> 24);
  file[crc_at + 1] = static_cast<std::uint8_t>(crc >> 16);
  file[crc_at + 2] = static_cast<std::uint8_t>(crc >> 8);
  file[crc_at + 3] = static_cast<std::uint8_t>(crc);
}

}  // namespace o3os::test
