// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// A .tmod file made by hand (OS-012a), for the loader's tests: the same layout tools/o3m_pack.py writes, with every field reachable so that
// a test can break exactly one thing. Nothing here runs: the functions of the descriptor are addresses inside the image.
//
//   image offsets   0..31    hot part: 32 bytes. Offset 8 holds a pointer into the cold part, offset 12 a pointer one past the end of the image.
//                   32..191  cold part: the descriptor at 32 (64 bytes), two parameters at 96 and 116 (20 bytes each), the strings "Test" at 136,
//                            "Alpha" at 144 and "Beta" at 152, then padding to 192 bytes.
//   relocations     8, 12, 48 (name), 60 (params), 64..84 (the six functions), 96 and 116 (the parameter names)
#include <o3os/module/image.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace o3os::module::test {

// The good blob is a module of ABI 1.7: the last minor with the 64-byte descriptor (ABI 1.8 made the descriptor 80 bytes, blob18() below). The OS's own minor moves on; this file's idea of "an ordinary module" does not.
inline constexpr std::uint8_t kBlobMinor = 7;
inline constexpr std::uint32_t kHot = 32;
inline constexpr std::uint32_t kImage = 192;
inline constexpr std::uint32_t kDesc = 32;
inline constexpr std::uint32_t kHotAddr = 0x1FFFC000u;   // where the tests place the hot part (the slab)
inline constexpr std::uint32_t kColdAddr = 0x70000000u;  // and the cold part (the module region)

struct Blob {
  o3m_file_header h{};
  std::vector<std::uint8_t> image;     // kImage bytes, as linked at 0
  std::vector<std::uint32_t> relocs;   // image offsets

  std::vector<std::uint8_t> file() const {
    std::vector<std::uint8_t> out(sizeof h + image.size() + relocs.size() * 4);
    std::memcpy(out.data(), &h, sizeof h);
    if (!image.empty()) {
      std::memcpy(out.data() + sizeof h, image.data(), image.size());
    }
    if (!relocs.empty()) {
      std::memcpy(out.data() + sizeof h + image.size(), relocs.data(), relocs.size() * 4);
    }
    return out;
  }
  // The CRC the loader wants: the header with its CRC field zero, the image, the table. Call after the last change.
  void seal() {
    h.image_crc = 0;
    std::uint32_t c = crc32_update(0, &h, sizeof h);
    c = crc32_update(c, image.data(), image.size());
    c = crc32_update(c, relocs.data(), relocs.size() * 4);
    h.image_crc = c;
  }
  void put(std::uint32_t offset, std::uint32_t value) { std::memcpy(image.data() + offset, &value, 4); }
  std::uint32_t get(std::uint32_t offset) const {
    std::uint32_t v;
    std::memcpy(&v, image.data() + offset, 4);
    return v;
  }
  void put_param(std::uint32_t offset, std::uint32_t name, float mn, float mx, float def, std::uint32_t flags) {
    Param32 p{name, mn, mx, def, flags};
    std::memcpy(image.data() + offset, &p, sizeof p);
  }
};

inline Blob good_blob() {
  Blob b;
  b.image.assign(kImage, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  b.put(8, kHot + 112);  // hot code's literal: the address of a cold string ("Alpha")
  b.put(12, kImage);     // an end pointer
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = kBlobMinor;
  d.kind = O3M_KIND_INSTRUMENT;
  d.struct_size = sizeof(Desc32);
  d.name = kHot + 104;      // 136
  d.state_bytes = 64;
  d.param_count = 2;
  d.params = kHot + 64;     // 96
  d.init = 1;               // hot, Thumb
  d.render = 3;
  d.set_param = kHot + 120 + 1;  // a cold function
  d.note_on = 5;
  d.note_off = 7;
  d.shutdown = 9;
  std::memcpy(b.image.data() + kDesc, &d, sizeof d);
  b.put_param(96, kHot + 112, 0.0f, 1.0f, 0.5f, 0);
  b.put_param(116, kHot + 120, -10.0f, 10.0f, 0.0f, O3M_PARAM_LOG);
  std::memcpy(b.image.data() + 136, "Test", 5);
  std::memcpy(b.image.data() + 144, "Alpha", 6);
  std::memcpy(b.image.data() + 152, "Beta", 5);
  b.relocs = {8, 12, 48, 60, 64, 68, 72, 76, 80, 84, 96, 116};
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_INSTRUMENT;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = kBlobMinor;
  b.h.hot_size = kHot;
  b.h.image_size = kImage;
  b.h.desc_offset = kDesc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 2;
  b.seal();
  return b;
}

// The good blob as a module of ABI 1.`minor` (the header's and the descriptor's words), sealed. The module version stays 0 (a file below ABI 1.2 cannot carry one).
inline Blob blob_minor(std::uint8_t minor) {
  Blob b = good_blob();
  b.h.abi_minor = minor;
  b.put(kDesc + 4, minor);  // Desc32::abi_minor
  b.seal();
  return b;
}

// The good blob as a module of ABI 1.2 (the first to carry a version) with `version` (O3M_VERSION) in its header, sealed.
inline Blob blob_version(std::uint32_t version) {
  Blob b = blob_minor(2);
  b.h.module_version = version;
  b.seal();
  return b;
}

inline void put_desc(Blob& b, const Desc32& d) { std::memcpy(b.image.data() + kDesc, &d, sizeof d); }
inline Desc32 get_desc(const Blob& b) {
  Desc32 d;
  std::memcpy(&d, b.image.data() + kDesc, sizeof d);
  return d;
}

// A module of ABI 1.8 (OS-MODPITCH, OS-INSTUI S10): the 80-byte descriptor with pitch, two pages and labels. 10 parameters (0: an INT 0..2 with three labels, 3: an INT 0..1 with two labels,
// the rest floats 0..1), pages "Osc" (parameters 0..7) and "Env" (8..9). Image 512 bytes, hot 32 as in good_blob(); the offsets below are image offsets, exposed so that a test can break one word.
struct Layout18 {
  static constexpr std::uint32_t kImage18 = 512;
  static constexpr std::uint32_t desc = 32;          // 80 bytes
  static constexpr std::uint32_t params = 112;       // 10 x 20 bytes
  static constexpr std::uint32_t strings = 312;      // "Test" 312, "Alpha" 320, "Beta" 328 (unused names), "Osc" 336, "Env" 340, "Saw" 344, "Sqr" 348, "Tri" 352, "Lo" 356, "Hi" 360
  static constexpr std::uint32_t pages = 368;        // 2 x 8 bytes
  static constexpr std::uint32_t labels = 384;       // 10 x 8 bytes
  static constexpr std::uint32_t names0 = 464;       // parameter 0's three string addresses
  static constexpr std::uint32_t names3 = 480;       // parameter 3's two
  static constexpr std::uint32_t pitch_fn = 11;      // hot, Thumb
};

inline Blob blob18() {
  using L = Layout18;
  Blob b;
  b.image.assign(L::kImage18, 0);
  for (std::uint32_t i = 0; i < kHot; ++i) {
    b.image[i] = static_cast<std::uint8_t>(0xA0u + i);
  }
  const char* const text[] = {"Test", "Alpha", "Beta", "Osc", "Env", "Saw", "Sqr", "Tri", "Lo", "Hi"};
  std::uint32_t at[10];
  std::uint32_t cursor = L::strings;
  for (unsigned i = 0; i < 10; ++i) {
    at[i] = cursor;
    std::memcpy(b.image.data() + cursor, text[i], std::strlen(text[i]) + 1);
    cursor += i < 3 ? 8u : 4u;
  }
  Desc32 d{};
  d.abi_major = O3M_ABI_MAJOR;
  d.abi_minor = 8;
  d.kind = O3M_KIND_INSTRUMENT;
  d.struct_size = sizeof(Desc32) + sizeof(DescExt32);
  d.name = at[0];
  d.state_bytes = 64;
  d.param_count = 10;
  d.params = L::params;
  d.init = 1;
  d.render = 3;
  d.set_param = kHot + 120 + 1;  // a cold function (image offset 152 + 1: inside the image)
  d.note_on = 5;
  d.note_off = 7;
  d.shutdown = 9;
  std::memcpy(b.image.data() + L::desc, &d, sizeof d);
  const DescExt32 t{L::pitch_fn, L::pages, 2, L::labels};
  std::memcpy(b.image.data() + L::desc + sizeof d, &t, sizeof t);
  for (unsigned i = 0; i < 10; ++i) {
    const bool is_int = i == 0u || i == 3u;
    b.put_param(L::params + 20u * i, at[1], 0.0f, i == 0u ? 2.0f : 1.0f, 0.0f, is_int ? O3M_PARAM_INT : 0u);
  }
  const Page32 pg[2] = {{at[3], 0}, {at[4], 8}};
  std::memcpy(b.image.data() + L::pages, pg, sizeof pg);
  const Labels32 l0{L::names0, 3}, l3{L::names3, 2};
  std::memcpy(b.image.data() + L::labels, &l0, sizeof l0);
  std::memcpy(b.image.data() + L::labels + 8u * 3u, &l3, sizeof l3);
  b.put(L::names0, at[5]);
  b.put(L::names0 + 4, at[6]);
  b.put(L::names0 + 8, at[7]);
  b.put(L::names3, at[8]);
  b.put(L::names3 + 4, at[9]);
  std::vector<std::uint32_t> r = {L::desc + 16, L::desc + 28};  // name, params
  for (unsigned i = 0; i < 6; ++i) {
    r.push_back(L::desc + 32u + 4u * i);  // the six functions
  }
  r.push_back(L::desc + 64);  // pitch
  r.push_back(L::desc + 68);  // pages
  r.push_back(L::desc + 76);  // labels
  for (unsigned i = 0; i < 10; ++i) {
    r.push_back(L::params + 20u * i);  // parameter names
  }
  r.push_back(L::pages);
  r.push_back(L::pages + 8);
  r.push_back(L::labels);            // parameter 0's names
  r.push_back(L::labels + 8u * 3u);  // parameter 3's names
  for (unsigned i = 0; i < 3; ++i) {
    r.push_back(L::names0 + 4u * i);
  }
  r.push_back(L::names3);
  r.push_back(L::names3 + 4);
  std::sort(r.begin(), r.end());
  b.relocs = r;
  b.h.magic = O3M_FILE_MAGIC;
  b.h.header_size = O3M_FILE_HEADER_SIZE;
  b.h.target = O3M_TARGET_ARMV7EM;
  b.h.kind = O3M_KIND_INSTRUMENT;
  b.h.abi_major = O3M_ABI_MAJOR;
  b.h.abi_minor = 8;
  b.h.hot_size = kHot;
  b.h.image_size = L::kImage18;
  b.h.desc_offset = L::desc;
  b.h.state_bytes = 64;
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.h.param_count = 10;
  b.seal();
  return b;
}

// An app of ABI 1.8 (the 80-byte descriptor needs blob18()'s roomier layout): its views' table at image offset 488, the sound entries zero, the 1.8 members as blob18() has them.
inline Blob blob_app18(std::uint32_t views, std::uint8_t minor = 8) {
  Blob b = blob18();
  Desc32 d;
  std::memcpy(&d, b.image.data() + Layout18::desc, sizeof d);
  d.kind = O3M_KIND_APP;
  d.abi_minor = minor;  // (a later minor than this OS's is a test of the refusal: the same layout)
  d.render = d.set_param = d.note_on = d.note_off = 0;
  d.ui = views != 0u ? 488u : 0u;
  d.view_count = views;
  std::memcpy(b.image.data() + Layout18::desc, &d, sizeof d);
  for (const std::uint32_t at : {Layout18::desc + 36u, Layout18::desc + 40u, Layout18::desc + 44u, Layout18::desc + 48u}) {
    b.relocs.erase(std::remove(b.relocs.begin(), b.relocs.end(), at), b.relocs.end());
  }
  if (views != 0u) {
    const Ui32 ui{ui_size(minor), 0, 0, 9, 0};  // (ABI 1.9: 24 bytes, the `keys` word past it is zero)
    std::memcpy(b.image.data() + 488, &ui, sizeof ui);
    b.relocs.push_back(Layout18::desc + 56u);
    b.relocs.push_back(488u + 12u);
    std::sort(b.relocs.begin(), b.relocs.end());
  }
  b.h.kind = O3M_KIND_APP;
  b.h.abi_minor = minor;
  b.h.view_count = static_cast<std::uint16_t>(views);
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.seal();
  return b;
}

// An app (ABI 1.1): kind 4, no sound entries, one view; the views' table at image offset 160 (cold padding), view_draw at hot 8 (Thumb), enter / input / exit zero.
inline Blob blob_app(std::uint32_t views = 1, std::uint8_t minor = 1) {
  if (minor >= 8) {
    return blob_app18(views, minor);
  }
  Blob b = good_blob();
  Desc32 d = get_desc(b);
  d.abi_minor = minor;
  d.kind = O3M_KIND_APP;
  d.render = d.set_param = d.note_on = d.note_off = 0;
  d.ui = views != 0u ? kHot + 128 : 0u;  // image offset 160
  d.view_count = views;
  put_desc(b, d);
  if (views != 0u) {
    const Ui32 ui{sizeof(Ui32), 0, 0, 9, 0};
    std::memcpy(b.image.data() + 160, &ui, sizeof ui);
    b.relocs = {8, 12, 48, 60, 64, 84, 88, 96, 116, 172};
  } else {
    b.relocs = {8, 12, 48, 60, 64, 84, 96, 116};
  }
  b.h.kind = O3M_KIND_APP;
  b.h.abi_minor = minor;
  b.h.view_count = static_cast<std::uint16_t>(views);
  b.h.reloc_count = static_cast<std::uint32_t>(b.relocs.size());
  b.seal();
  return b;
}

// The address a link-space value has once the module is loaded at kHotAddr / kColdAddr: the reference the loader is held to.
inline std::uint32_t moved(std::uint32_t v) { return v < kHot ? kHotAddr + v : kColdAddr + (v - kHot); }

// Memory for a placement, aligned like an arena's.
struct Mem {
  alignas(64) std::uint8_t hot[256] = {};
  alignas(64) std::uint8_t cold[1024] = {};
  Placement place() { return Placement{hot, kHotAddr, cold, kColdAddr}; }
};

// Copies the image into the placement as the loader does (hot part, then cold).
inline void place_image(const Blob& b, Placement p) {
  std::memcpy(p.hot_mem, b.image.data(), b.h.hot_size);
  std::memcpy(p.cold_mem, b.image.data() + b.h.hot_size, b.h.image_size - b.h.hot_size);
}

}  // namespace o3os::module::test
