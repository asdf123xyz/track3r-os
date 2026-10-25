// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/og.h>

#include "og_codec.h"

#include <algorithm>
#include <cstring>

namespace o3os::formats::og {

using namespace codec;  // NOLINT: the shared field access

namespace {
constexpr std::uint32_t kNameBytes = 31;  // up to 30 characters and a NUL
}

// patterns/patternsMetadata ("PAMD", firmware 1.8+; PAR-008): a 16-byte header (magic, u16 version, two reserved
// bytes, u32 total size, u32 control flags) and 50-byte records: a name of up to 30 characters in 31 bytes, 19
// bytes nobody has seen used. Record i names pattern i + 1 (INFERRED from the pattern numbering; one file with
// two named patterns would settle it, PAR-008 OQ-8.2). The one PAMD fixture holds 256 records.
//
// The reader takes the whole file in (at most 12,816 bytes) and checks it before it changes anything, so a file that
// fails leaves the project as it was. The names go to the patterns that exist, and the file itself is kept verbatim,
// minus its trailing zeros, in an OGPN chunk (u16 version 0x0100, u32 length of the file, the bytes): the writer
// gives it back unchanged, since what is in it besides the names (reserved bytes, flags, the 19 bytes of a record,
// names of patterns this project does not have) is nobody's to rewrite. Names given in the OS are not written until
// the mapping is confirmed on a real device: they are counted as dropped.

// ---- reader -----------------------------------------------------------------------------------------------

void PamdReader::begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept {
  begin_base(source, arena);
  have_ = 0;
  probing_ = false;
  if (params.size_hint > kPamdBytes) {  // the platform knows the file is too long: nothing is read
    fail(FormatError::LimitExceeded, "more than 256 records");
  }
}

void PamdReader::finish() noexcept {
  const std::uint32_t n = static_cast<std::uint32_t>(have_);
  if (n < kPamdHeader) {
    fail(FormatError::Truncated, "the file ends inside the header");
    return;
  }
  if (std::memcmp(file_, "PAMD", 4) != 0) {
    fail(FormatError::BadMagic, "not an OG pattern names file");
    return;
  }
  const std::uint32_t version = get16(file_ + 4);
  report_.version = static_cast<std::uint8_t>(version > 255 ? 255 : version);
  if (version == 0) {
    fail(FormatError::UnsupportedVersion, "PAMD version 0");
    return;
  }
  if (version > kPamdVerified) {  // a record layout nobody has seen: no names are read, nothing is kept
    report_.unverified_version = true;
    progress_ = Progress::Done;
    return;
  }
  for (std::uint32_t i = 0; i < project_->unknown_count(); ++i) {
    if (project_->unknown(i)->id == kChunkNames) {  // as a pattern whose id is taken: never a replacement
      fail(FormatError::Malformed, "the project already holds a pattern names file");
      return;
    }
  }
  // Everything is checked: from here on nothing can fail (a name of a pattern that exists is always accepted).
  const std::uint32_t records = (n - kPamdHeader) / kPamdRecord;
  report_.partial_record = (n - kPamdHeader) % kPamdRecord != 0;
  for (std::uint32_t i = 0; i < records && i + 1 <= model::kMaxPatternId; ++i) {
    const std::uint8_t* rec = file_ + kPamdHeader + kPamdRecord * i;
    const std::size_t len = field_text_len(rec, kNameBytes, kNameBytes - 1);
    if (len == 0) continue;
    if (project_->set_pattern_name(i + 1, reinterpret_cast<const char*>(rec), len) == model::Status::Ok) {
      ++report_.names_applied;
    } else {
      ++report_.names_skipped;  // no such pattern in the project
    }
  }
  // the file, without its trailing zeros, for Export. The empty file Export writes without being told (a header, no
  // names, 256 records) needs no chunk, which also makes a read of an export leave the project as the first read did.
  std::uint32_t kept = n;
  while (kept > 0 && file_[kept - 1] == 0) --kept;
  std::uint8_t empty[kPamdHeader] = {};
  std::memcpy(empty, "PAMD", 4);
  put16(empty + 4, 1);
  put32(empty + 8, kPamdBytes);
  if (n == kPamdBytes && kept <= kPamdHeader && std::memcmp(file_, empty, kPamdHeader) == 0) {
    report_.names_kept = true;
    progress_ = Progress::Done;
    return;
  }
  const std::uint8_t head[codec::settings::kNamesHeader] = {
      static_cast<std::uint8_t>(codec::settings::kVersion >> 8), static_cast<std::uint8_t>(codec::settings::kVersion),
      static_cast<std::uint8_t>(n >> 24), static_cast<std::uint8_t>(n >> 16), static_cast<std::uint8_t>(n >> 8),
      static_cast<std::uint8_t>(n)};
  if (project_->unknown_begin(kChunkNames, codec::settings::kNamesHeader + kept) == model::Status::Ok) {
    report_.names_kept = project_->unknown_append(head, sizeof head) == model::Status::Ok &&
                         project_->unknown_append(file_, kept) == model::Status::Ok;
    if (report_.names_kept) {
      project_->note_names();  // OS-FILES-C H15: the names are those of the patterns as they are now; the writer stops giving them back when the set of patterns changes
    }
  }  // else the pool is full: the model has counted a dropped chunk, the names are applied all the same
  progress_ = Progress::Done;
}

Progress PamdReader::step(std::size_t byte_budget) noexcept {
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (!probing_) {
      const Pull r = pull(file_, kPamdBytes, have_, budget, false);
      if (r == Pull::Partial) {
        return Progress::Running;
      }
      if (r == Pull::Error) {
        break;
      }
      if (r == Pull::End) {
        finish();
        break;
      }
      probing_ = true;  // 256 records: the source must be over
    } else {
      std::uint8_t extra;
      std::size_t got = 0;
      const Pull r = pull(&extra, 1, got, budget, false);
      if (r == Pull::Partial) {
        return Progress::Running;
      }
      if (r == Pull::Error) {
        break;
      }
      if (r == Pull::End) {
        finish();
      } else {
        fail(FormatError::LimitExceeded, "more than 256 records");
      }
      break;
    }
  }
  return progress_;
}

// ---- writer -----------------------------------------------------------------------------------------------

void PamdWriter::begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept {
  (void)params;
  begin_base(project, sink);
  off_ = 0;
  zeros_done_ = 0;
  // the file the project was read from, if it holds a valid one
  head_ = nullptr;
  for (std::uint32_t i = 0; i < project.unknown_count(); ++i) {
    const model::UnknownChunk* c = project.unknown(i);
    if (c->id != kChunkNames) continue;
    if (project.names_stale()) continue;  // OS-FILES-C H15: a pattern was added, removed or renumbered since the file was read: its names would land on other patterns; the empty file below, the names counted
    std::uint32_t length = 0;
    const std::uint8_t* data = project.unknown_data(i);
    if (head_ == nullptr && inspect_names(data, c->length, length)) {
      head_ = data + codec::settings::kNamesHeader;
      head_len_ = c->length - codec::settings::kNamesHeader;
      total_ = length;
    } else {
      ++report_.drops.settings_ignored;
    }
  }
  if (head_ == nullptr) {  // none: a header and empty records
    std::memset(header_, 0, sizeof header_);
    std::memcpy(header_, "PAMD", 4);
    put16(header_ + 4, 1);
    put32(header_ + 8, kPamdBytes);
    head_ = header_;
    head_len_ = kPamdHeader;
    total_ = kPamdBytes;
  }
  // names given in the OS are not written (the mapping is unconfirmed): those that are not what the file says
  // are counted
  for (std::uint32_t id = 1; id <= model::kMaxPatternId; ++id) {
    const model::Pattern* pat = project.pattern(id);
    if (pat == nullptr || pat->name[0] == '\0') continue;
    const std::uint32_t at = kPamdHeader + kPamdRecord * (id - 1);
    const std::size_t len = text_len(pat->name, model::kPatternNameMax);
    bool same = at + len <= head_len_;
    for (std::size_t i = 0; same && i < len; ++i) same = head_[at + i] == static_cast<std::uint8_t>(pat->name[i]);
    // the name in the file ends where the model's does: a NUL, or the end of what was kept (zeros follow)
    if (same && at + len < head_len_ && head_[at + len] != 0) same = false;
    if (!same) ++report_.drops.pattern_names;
  }
}

Progress PamdWriter::step(std::size_t byte_budget) noexcept {
  static const std::uint8_t kZeros[64] = {};
  std::size_t budget = byte_budget;
  while (progress_ == Progress::Running) {
    if (!unchanged()) {
      break;
    }
    if (off_ < head_len_) {
      const Flush f = flush(head_, head_len_, off_, budget);
      if (f == Flush::Partial) {
        return Progress::Running;
      }
      if (f == Flush::Error) {
        break;
      }
      continue;
    }
    if (head_len_ + zeros_done_ >= total_) {
      progress_ = Progress::Done;
      break;
    }
    // the zeros of the file past its last non-zero byte
    std::size_t at = 0;
    const std::size_t n = std::min<std::size_t>(sizeof kZeros, total_ - head_len_ - zeros_done_);
    const Flush f = flush(kZeros, n, at, budget);
    zeros_done_ += static_cast<std::uint32_t>(at);
    if (f == Flush::Partial) {
      return Progress::Running;
    }
    if (f == Flush::Error) {
      break;
    }
  }
  return progress_;
}

}  // namespace o3os::formats::og
