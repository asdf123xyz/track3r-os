// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/image.h>

#include <cstring>

namespace o3os::module {

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the file format is little endian and is read in place");

namespace {

constexpr std::uint32_t kNameLimit = O3M_NAME_MAX;            // bytes of a module name, with the NUL
constexpr std::uint32_t kParamNameLimit = O3M_PARAM_NAME_MAX;
constexpr std::uint8_t kVersionAbiMinor = 2;  // the ABI minor that made module_version a field: below it the word is reserved and must be zero
constexpr std::uint8_t kCapsAbiMinor = 3;     // the ABI minor that made reserved2[0] the capability mask (apps only): below it the word is reserved and must be zero
constexpr std::uint8_t kTapAbiMinor = 4;      // the ABI minor that granted audio_tap (OS-H2a)
constexpr std::uint8_t kExportAbiMinor = 7;   // the ABI minor that granted export_files (OS-H4)
constexpr std::uint8_t kPlayAbiMinor = 10;    // the ABI minor that granted play_notes (OS-APPUX S3)

// The capability bits a file of ABI minor `minor` may set (OS-H1, OS-H2a, OS-H4, OS-APPUX S3): 1.3 knows MIDI in, read project and edit project, 1.4 adds the audio tap, 1.7 adds the export files, 1.10 adds
// the play notes; the bits of later slices are refused until their minor.
constexpr std::uint32_t caps_known(std::uint8_t minor) noexcept {
  return minor >= kPlayAbiMinor ? static_cast<std::uint32_t>(O3M_CAPS_ABI_1_10)
                                : (minor >= kExportAbiMinor ? static_cast<std::uint32_t>(O3M_CAPS_ABI_1_7)
                                                            : (minor >= kTapAbiMinor ? static_cast<std::uint32_t>(O3M_CAPS_ABI_1_4) : (minor >= kCapsAbiMinor ? static_cast<std::uint32_t>(O3M_CAPS_ABI_1_3) : 0u)));
}

// Every capability bit of the published ABI (ADR 0014 amendment D, C4: values frozen).
constexpr std::uint32_t kCapsPublished = O3M_CAP_MIDI_IN | O3M_CAP_READ_PROJECT | O3M_CAP_AUDIO_TAP | O3M_CAP_EDIT_PROJECT | O3M_CAP_EXPORT_FILES | O3M_CAP_MIDI_OUT;
// The published bits this OS does not grant yet (a later slice's: midi_out): asked by an app of ABI 1.3 or later they are a clean refusal (CapUnsupported); a bit this OS DOES grant but that the file's
// minor is too low to use (audio_tap in a file of ABI 1.3, export_files in a file of ABI 1.6) is a reserved field: the file is wrong, not the OS old.
constexpr std::uint32_t kCapsNotGranted = kCapsPublished & ~static_cast<std::uint32_t>(O3M_CAPS_ABI_1_10);

bool all_zero(const std::uint32_t* words, unsigned n) noexcept {
  for (unsigned i = 0; i < n; ++i) {
    if (words[i] != 0u) {
      return false;
    }
  }
  return true;
}

// [addr, addr + length) in the placed hot part / cold part, as an offset into the image (link space); false when it is in neither.
bool locate(const o3m_file_header& h, const Placement& p, std::uint32_t addr, std::uint32_t length, const std::uint8_t*& out) noexcept {
  const std::uint64_t end = static_cast<std::uint64_t>(addr) + length;
  if (h.hot_size != 0u && addr >= p.hot_addr && end <= static_cast<std::uint64_t>(p.hot_addr) + h.hot_size) {
    out = p.hot_mem + (addr - p.hot_addr);
    return true;
  }
  const std::uint32_t cold = h.image_size - h.hot_size;
  if (cold != 0u && addr >= p.cold_addr && end <= static_cast<std::uint64_t>(p.cold_addr) + cold) {
    out = p.cold_mem + (addr - p.cold_addr);
    return true;
  }
  return false;
}

// A NUL-terminated string of fewer than `limit` bytes inside the placed image.
bool string_ok(const o3m_file_header& h, const Placement& p, std::uint32_t addr, std::uint32_t limit) noexcept {
  for (std::uint32_t i = 0; i < limit; ++i) {
    const std::uint8_t* c = nullptr;
    if (addr + i < addr || !locate(h, p, addr + i, 1, c)) {
      return false;
    }
    if (*c == 0u) {
      return i != 0u;  // an empty name is no name
    }
    if (*c < 0x20u || *c > 0x7Eu) {
      return false;
    }
  }
  return false;
}

bool code_ok(const o3m_file_header& h, const Placement& p, std::uint32_t fn) noexcept {
  const std::uint8_t* unused = nullptr;
  return (fn & 1u) != 0u && locate(h, p, fn & ~1u, 2, unused);  // a Thumb address whose first instruction is inside the image
}

// ABI 1.8 (OS-INSTUI S10): the page table. `pages` is zero exactly when there are no pages; otherwise 1..4 entries, the first at parameter 0, strictly ascending, each page 1..8 parameters, the last
// one ending at param_count (so the pages cover the parameters exactly), and every name a string of at most 7 characters inside the image.
bool pages_ok(const o3m_file_header& h, const Placement& p, const Desc32& d, const DescExt32& t) noexcept {
  if ((t.pages == 0u) != (t.page_count == 0u)) {
    return false;
  }
  if (t.page_count == 0u) {
    return true;
  }
  const std::uint8_t* at = t.page_count <= O3M_PAGES_MAX && (t.pages & 3u) == 0u ? resolve(h, p, t.pages, t.page_count * static_cast<std::uint32_t>(sizeof(Page32))) : nullptr;
  if (at == nullptr) {
    return false;
  }
  for (std::uint32_t k = 0; k < t.page_count; ++k) {
    Page32 page;
    std::memcpy(&page, at + k * sizeof(Page32), sizeof page);
    std::uint32_t next = d.param_count;
    if (k + 1u < t.page_count) {
      Page32 after;
      std::memcpy(&after, at + (k + 1u) * sizeof(Page32), sizeof after);
      next = after.first_param;
    }
    if ((k == 0u && page.first_param != 0u) || next <= page.first_param || next > d.param_count || next - page.first_param > O3M_PAGE_PARAMS_MAX ||
        !string_ok(h, p, page.name, O3M_PAGE_NAME_MAX)) {
      return false;
    }
  }
  return true;
}

// The label table: zero, or `param_count` entries parallel to the parameters. An entry is {0, 0} or the labels of an INT parameter: count = max - min + 1 in 2..16 (the range is whole numbers),
// the string table and every string inside the image, each of at most 11 characters.
bool labels_ok(const o3m_file_header& h, const Placement& p, const Desc32& d, const DescExt32& t) noexcept {
  if (t.labels == 0u) {
    return true;
  }
  const std::uint8_t* at = (t.labels & 3u) == 0u ? resolve(h, p, t.labels, d.param_count * static_cast<std::uint32_t>(sizeof(Labels32))) : nullptr;
  if (at == nullptr) {
    return false;
  }
  for (std::uint32_t i = 0; i < d.param_count; ++i) {
    Labels32 e;
    std::memcpy(&e, at + i * sizeof(Labels32), sizeof e);
    if (e.count == 0u) {
      if (e.names != 0u) {
        return false;
      }
      continue;
    }
    Param32 q;
    if (!read_param(h, p, d, i, q) || (q.flags & O3M_PARAM_INT) == 0u || e.count < O3M_LABELS_MIN || e.count > O3M_LABELS_MAX || !(q.min >= -1.0e6f && q.max <= 1.0e6f) ||
        q.min != static_cast<float>(static_cast<std::int32_t>(q.min)) || q.max != static_cast<float>(static_cast<std::int32_t>(q.max)) ||
        static_cast<std::int32_t>(q.max) - static_cast<std::int32_t>(q.min) + 1 != static_cast<std::int32_t>(e.count)) {
      return false;
    }
    const std::uint8_t* names = (e.names & 3u) == 0u ? resolve(h, p, e.names, e.count * 4u) : nullptr;
    if (names == nullptr) {
      return false;
    }
    for (std::uint32_t v = 0; v < e.count; ++v) {
      std::uint32_t str;
      std::memcpy(&str, names + 4u * v, 4);
      if (!string_ok(h, p, str, O3M_LABEL_NAME_MAX)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace

Status check_header(const o3m_file_header& h, std::uint32_t file_bytes) noexcept {
  if (file_bytes < sizeof(o3m_file_header)) {
    return Status::TooShort;
  }
  if (h.magic != O3M_FILE_MAGIC) {
    return Status::BadMagic;
  }
  if (h.header_size != O3M_FILE_HEADER_SIZE) {
    return Status::BadHeaderSize;
  }
  if (h.target != O3M_TARGET_ARMV7EM) {
    return Status::BadTarget;
  }
  if (h.kind != O3M_KIND_INSTRUMENT && h.kind != O3M_KIND_EFFECT && !(h.kind == O3M_KIND_APP && h.abi_minor >= 1u)) {
    return Status::BadKind;
  }
  if (h.abi_major != kOsAbiMajor) {
    return Status::AbiMajor;
  }
  if (h.abi_minor > kOsAbiMinor) {
    return Status::AbiMinor;
  }
  if (h.flags != 0u || (h.abi_minor < kVersionAbiMinor && h.module_version != 0u) || h.reserved2[1] != 0u || !all_zero(h.reserved3, 3)) {
    return Status::BadReserved;
  }
  if (h.reserved2[0] != 0u && (h.kind != O3M_KIND_APP || (h.reserved2[0] & ~caps_known(h.abi_minor)) != 0u)) {
    // a capability request: only an app of ABI 1.3 or later may make one. A bit that is published ABI but that this OS does not grant (export_files, midi_out) is a clean
    // refusal that names it (CapUnsupported, ADR 0014 amendment D, C4); any other bit is a reserved field.
    const bool published_only = h.kind == O3M_KIND_APP && h.abi_minor >= kCapsAbiMinor && (h.reserved2[0] & ~caps_known(h.abi_minor) & ~kCapsNotGranted) == 0u;
    return published_only ? Status::CapUnsupported : Status::BadReserved;
  }
  if (h.image_size == 0u || h.image_size > kMaxImageBytes || (h.image_size & 3u) != 0u || h.hot_size > h.image_size || (h.hot_size & 3u) != 0u ||
      h.hot_size == h.image_size /* no cold part: no descriptor */ || h.state_bytes == 0u || h.state_bytes > kMaxStateBytes ||
      h.param_count > kMaxParams || h.view_count > kMaxViews || h.reloc_count > kMaxRelocs || h.reloc_count > h.image_size / 4u) {
    return Status::BadSize;
  }
  if ((h.desc_offset & 3u) != 0u || h.desc_offset < h.hot_size || h.desc_offset > h.image_size || h.image_size - h.desc_offset < desc_size(h.abi_minor)) {
    return Status::BadSize;
  }
  const std::uint64_t want = static_cast<std::uint64_t>(sizeof(o3m_file_header)) + h.image_size + 4ull * h.reloc_count;
  if (want != file_bytes) {
    return Status::BadFileLength;
  }
  return Status::Ok;
}

Status Relocator::apply(const std::uint32_t* offsets, std::uint32_t count) noexcept {
  const std::uint32_t image = h_.image_size;
  const std::uint32_t hot = h_.hot_size;
  const std::uint32_t cold = image - hot;
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::uint32_t off = offsets[i];
    if (applied_ >= h_.reloc_count) {
      return Status::BadFileLength;  // more entries than the header says
    }
    if ((off & 3u) != 0u || (applied_ != 0u && off <= last_)) {
      return Status::BadRelocOrder;
    }
    // The word lies wholly in the hot part or wholly in the cold part (both multiples of 4, off aligned: it cannot straddle).
    std::uint8_t* where = nullptr;
    if (off < hot) {
      where = p_.hot_mem + off;
    } else if (off < image) {
      where = p_.cold_mem + (off - hot);
    } else {
      return Status::BadRelocOffset;
    }
    std::uint32_t value;
    std::memcpy(&value, where, 4);
    std::uint32_t moved;
    if (value < hot) {
      moved = p_.hot_addr + value;
    } else if (value - hot <= cold) {  // one past the end of the image is a legal address (an end pointer)
      moved = p_.cold_addr + (value - hot);
    } else {
      return Status::BadRelocValue;
    }
    std::memcpy(where, &moved, 4);
    last_ = off;
    ++applied_;
  }
  return Status::Ok;
}

const std::uint8_t* resolve(const o3m_file_header& h, const Placement& p, std::uint32_t addr, std::uint32_t length) noexcept {
  const std::uint8_t* out = nullptr;
  return locate(h, p, addr, length, out) ? out : nullptr;
}

Status read_desc(const o3m_file_header& h, const Placement& p, Desc32& out, DescExt32* ext) noexcept {
  std::memset(&out, 0, sizeof out);
  DescExt32 tail{};
  // The descriptor is at its link address in the cold part, which has been relocated to cold_addr.
  const std::uint32_t hot = h.hot_size;
  const std::uint32_t size = desc_size(h.abi_minor);  // the declared minor's descriptor: 64 bytes up to 1.7, 80 from 1.8 (the members past 64 are not read below it)
  if (h.desc_offset < hot || h.image_size - h.desc_offset < size) {
    return Status::BadDescriptor;
  }
  std::memcpy(&out, p.cold_mem + (h.desc_offset - hot), sizeof out);
  if (size > sizeof(Desc32)) {
    std::memcpy(&tail, p.cold_mem + (h.desc_offset - hot) + sizeof(Desc32), sizeof tail);
  }
  if (ext != nullptr) {
    *ext = tail;
  }
  if (out.struct_size != size || out.abi_major != h.abi_major || out.abi_minor != h.abi_minor || out.kind != h.kind ||
      out.state_bytes != h.state_bytes || out.param_count != h.param_count || out.param_count > kMaxParams) {
    return Status::BadDescriptor;
  }
  if (out.view_count != h.view_count || out.view_count > kMaxViews || (out.view_count == 0u) != (out.ui == 0u)) {
    return Status::BadDescriptor;
  }
  if (h.kind == O3M_KIND_APP && out.view_count == 0u) {
    return Status::BadDescriptor;  // an app is its view
  }
  if (out.ui != 0u) {  // the views' table: checked for every module, so that none with a bad one is ever loaded (called only for ABI 1.1 and up)
    Ui32 ui;
    std::uint32_t keys = 0;
    const std::uint32_t usize = ui_size(h.abi_minor);  // 20 bytes up to ABI 1.8, 24 from 1.9 (the `keys` word is read only then)
    const std::uint8_t* at = (out.ui & 3u) == 0u ? resolve(h, p, out.ui, usize) : nullptr;
    if (at == nullptr) {
      return Status::BadDescriptor;
    }
    std::memcpy(&ui, at, sizeof ui);
    if (usize > sizeof ui) {
      std::memcpy(&keys, at + sizeof ui, sizeof keys);
    }
    if (ui.struct_size != usize || !code_ok(h, p, ui.view_draw) || (ui.enter != 0u && !code_ok(h, p, ui.enter)) || (ui.input != 0u && !code_ok(h, p, ui.input)) ||
        (ui.exit != 0u && !code_ok(h, p, ui.exit)) || (keys != 0u && !code_ok(h, p, keys))) {
      return Status::BadDescriptor;
    }
  }
  if (!string_ok(h, p, out.name, kNameLimit)) {
    return Status::BadDescriptor;
  }
  const std::uint32_t fns[] = {out.init, out.render, out.set_param, out.note_on, out.note_off, out.shutdown};
  for (std::uint32_t i = 0; i < 6; ++i) {
    const bool optional = h.kind == O3M_KIND_APP && i >= 1u && i <= 4u;  // an app has no sound: render, set_param, note_on, note_off may be NULL
    if (!(optional && fns[i] == 0u) && !code_ok(h, p, fns[i])) {
      return Status::BadDescriptor;
    }
  }
  if (tail.pitch != 0u && !code_ok(h, p, tail.pitch)) {
    return Status::BadDescriptor;
  }
  if (out.param_count == 0u) {
    return out.params == 0u && tail.page_count == 0u && tail.pages == 0u && tail.labels == 0u ? Status::Ok : Status::BadDescriptor;  // (no parameters: no pages, no labels)
  }
  if ((out.params & 3u) != 0u || resolve(h, p, out.params, out.param_count * static_cast<std::uint32_t>(sizeof(Param32))) == nullptr) {
    return Status::BadDescriptor;
  }
  for (std::uint32_t i = 0; i < out.param_count; ++i) {
    Param32 q;
    if (!read_param(h, p, out, i, q) || !string_ok(h, p, q.name, kParamNameLimit) || !(q.min <= q.def && q.def <= q.max)) {
      return Status::BadDescriptor;  // (a NaN fails the comparisons)
    }
  }
  return pages_ok(h, p, out, tail) && labels_ok(h, p, out, tail) ? Status::Ok : Status::BadDescriptor;
}

bool read_param(const o3m_file_header& h, const Placement& p, const Desc32& d, std::uint32_t index, Param32& out) noexcept {
  if (index >= d.param_count) {
    return false;
  }
  const std::uint8_t* at = resolve(h, p, d.params + index * static_cast<std::uint32_t>(sizeof(Param32)), sizeof(Param32));
  if (at == nullptr) {
    return false;
  }
  std::memcpy(&out, at, sizeof out);
  return true;
}

std::uint32_t crc32_update(std::uint32_t crc, const void* data, std::size_t length) noexcept {
  // zlib's CRC-32 (reflected, polynomial 0xEDB88320), a nibble at a time: no table to keep.
  static constexpr std::uint32_t kNibble[16] = {0x00000000u, 0x1DB71064u, 0x3B6E20C8u, 0x26D930ACu, 0x76DC4190u, 0x6B6B51F4u, 0x4DB26158u, 0x5005713Cu,
                                                0xEDB88320u, 0xF00F9344u, 0xD6D6A3E8u, 0xCB61B38Cu, 0x9B64C2B0u, 0x86D3D2D4u, 0xA00AE278u, 0xBDBDF21Cu};
  const std::uint8_t* bytes = static_cast<const std::uint8_t*>(data);
  std::uint32_t c = ~crc;
  for (std::size_t i = 0; i < length; ++i) {
    c ^= bytes[i];
    c = (c >> 4) ^ kNibble[c & 15u];
    c = (c >> 4) ^ kNibble[c & 15u];
  }
  return ~c;
}

}  // namespace o3os::module
