// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/formats/med/mmd.h>

#include <algorithm>
#include <cstring>

namespace o3os::formats::med {

namespace {

// Offsets of the structures of docs/formats/med.md section 2, from Kinnunen's Rev 6 layout document (docs/formats/med.md [S1]).
constexpr std::uint32_t kHdrSong = 8, kHdrBlockArr = 16, kHdrSmplArr = 24, kHdrExp = 32, kHdrExtraSongs = 51;
constexpr std::uint32_t kSongNumBlocks = 504, kSongSongLen = 506, kSongPlaySeq = 508;
constexpr std::uint32_t kSongDefTempo = 764, kSongTrkVol = 770, kSongNumSamples = 787;
constexpr std::uint32_t kBlockHdr0 = 2, kBlockHdr1 = 8, kBlockInfoBytes = 36;
constexpr std::uint32_t kSynthWf = 278;   // SynthInstr.wf[64]
constexpr std::uint32_t kPlaySeqHdr = 42;  // PlaySeq: name[32], reserved[2], u16 length, then the u16 entries
constexpr std::uint8_t kOctavesOfType[6] = {5, 3, 2, 4, 6, 7};  // IFF5OCT, IFF3OCT, IFF2OCT, IFF4OCT, IFF6OCT, IFF7OCT

inline std::uint32_t be16(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 8) | p[1];
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
  return (static_cast<std::uint32_t>(p[0]) << 24) | (static_cast<std::uint32_t>(p[1]) << 16) |
         (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
inline Status fail(Error e, std::uint32_t offset, const char* detail) noexcept { return Status{e, offset, detail}; }

}  // namespace

const char* error_name(Error e) noexcept {
  switch (e) {
    case Error::None: return "none";
    case Error::Truncated: return "truncated";
    case Error::BadMagic: return "bad magic";
    case Error::BadPointer: return "bad pointer";
    case Error::LimitExceeded: return "limit exceeded";
    case Error::Malformed: return "malformed";
    case Error::Unsupported: return "unsupported";
    case Error::SourceFailed: return "source failed";
  }
  return "";
}

FormatError to_format_error(Error e) noexcept {
  switch (e) {
    case Error::None: return FormatError::None;
    case Error::Truncated: return FormatError::Truncated;
    case Error::BadMagic: return FormatError::BadMagic;
    case Error::BadPointer: return FormatError::Malformed;
    case Error::LimitExceeded: return FormatError::LimitExceeded;
    case Error::Malformed: return FormatError::Malformed;
    case Error::Unsupported: return FormatError::Unsupported;
    case Error::SourceFailed: return FormatError::SourceFailed;
  }
  return FormatError::Internal;
}

bool SpanSource::read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept {
  if (static_cast<std::uint64_t>(offset) + n > size_) {
    return false;
  }
  if (n != 0) {
    std::memcpy(dst, data_ + offset, n);
  }
  return true;
}

// ---- plumbing ----------------------------------------------------------------------------------------------------------

Status Reader::get(std::uint32_t offset, void* dst, std::uint32_t n) const noexcept {
  if (static_cast<std::uint64_t>(offset) + n > size_) {
    return fail(Error::Truncated, offset, "read past the end of the file");
  }
  if (n != 0 && !src_->read_at(offset, dst, n)) {
    return fail(Error::SourceFailed, offset, "the byte source failed");
  }
  return Status{};
}

// A pointer found at file offset `at`, to a structure of `bytes` bytes: 52 <= ptr and ptr + bytes <= file size, in 64 bits.
Status Reader::table(std::uint32_t at, std::uint32_t ptr, std::uint64_t bytes, const char* what) const noexcept {
  if (ptr < caps::kMinPointer || static_cast<std::uint64_t>(ptr) + bytes > size_) {
    return fail(Error::BadPointer, at, what);
  }
  return Status{};
}

// `field_len` bytes at `offset` as text: up to the first NUL, at most cap - 1 characters, bytes outside 0x20..0x7E as '?'.
Status Reader::copy_text(std::uint32_t offset, std::uint32_t field_len, char* out, std::size_t cap) const noexcept {
  if (cap == 0) {
    return Status{};
  }
  out[0] = '\0';
  std::uint8_t tmp[64];
  const std::uint32_t n = std::min<std::uint32_t>({field_len, static_cast<std::uint32_t>(cap - 1), 64u});
  if (n == 0) {
    return Status{};
  }
  const Status s = get(offset, tmp, n);
  if (!s.ok()) {
    return s;
  }
  std::size_t len = 0;
  while (len < n && tmp[len] != 0) {
    out[len] = (tmp[len] < 0x20 || tmp[len] >= 0x7F) ? '?' : static_cast<char>(tmp[len]);
    ++len;
  }
  out[len] = '\0';
  return Status{};
}

// ---- open --------------------------------------------------------------------------------------------------------------

Status Reader::open(RandomSource& source) noexcept {
  const Status s = open_impl(source);
  if (!s.ok()) {
    *this = Reader();  // nothing half-read stays behind
  }
  return s;
}

Status Reader::open_impl(RandomSource& source) noexcept {
  *this = Reader();
  src_ = &source;
  size_ = source.size();
  song_.file_size = size_;
  if (size_ < 4) {
    return fail(Error::Truncated, size_, "file shorter than the id");
  }
  std::uint8_t h[caps::kHeaderBytes];
  Status s = get(0, h, 4);
  if (!s.ok()) {
    return s;
  }
  if (h[0] != 'M' || h[1] != 'M' || h[2] != 'D') {
    return fail(Error::BadMagic, 0, "not an MMD file");
  }
  if (h[3] == 'C') {
    return fail(Error::Unsupported, 0, "MMDC (packed MMD0) is not read");
  }
  if (h[3] >= '4' && h[3] <= '9') {
    return fail(Error::Unsupported, 0, "MMD4 and later are not defined");
  }
  if (h[3] < '0' || h[3] > '3') {
    return fail(Error::BadMagic, 0, "not an MMD0 to MMD3 file");
  }
  song_.version = static_cast<std::uint8_t>(h[3] - '0');
  if (size_ < caps::kHeaderBytes) {
    return fail(Error::Truncated, size_, "file shorter than the 52-byte header");
  }
  s = get(0, h, caps::kHeaderBytes);
  if (!s.ok()) {
    return s;
  }
  song_ptr_ = be32(h + kHdrSong);
  blockarr_ = be32(h + kHdrBlockArr);
  smplarr_ = be32(h + kHdrSmplArr);
  exp_ = be32(h + kHdrExp);
  song_.extra_songs = h[kHdrExtraSongs];
  const unsigned ver = song_.version;

  // ---- the song record
  if (song_ptr_ == 0) {
    return fail(Error::BadPointer, kHdrSong, "the song pointer is missing");
  }
  s = table(kHdrSong, song_ptr_, caps::kSongBytes, "song record outside the file");
  if (!s.ok()) {
    return s;
  }
  std::uint8_t sb[caps::kSongBytes];
  s = get(song_ptr_, sb, caps::kSongBytes);
  if (!s.ok()) {
    return s;
  }
  song_.num_blocks = static_cast<std::uint16_t>(be16(sb + kSongNumBlocks));
  song_.song_len = static_cast<std::uint16_t>(be16(sb + kSongSongLen));
  song_.def_tempo = static_cast<std::uint16_t>(be16(sb + kSongDefTempo));
  song_.play_transpose = static_cast<std::int8_t>(sb[766]);
  song_.flags = sb[767];
  song_.flags2 = sb[768];
  song_.tempo2 = sb[769];
  song_.master_volume = sb[786];
  song_.num_samples = sb[kSongNumSamples];
  if (song_.num_blocks > (ver <= 1 ? caps::kMaxBlocksMmd01 : caps::kMaxBlocksMmd23)) {
    return fail(Error::LimitExceeded, song_ptr_ + kSongNumBlocks, "more blocks than the format allows");
  }
  if (song_.num_samples > caps::kMaxSamples) {
    return fail(Error::LimitExceeded, song_ptr_ + kSongNumSamples, "more than 63 instruments");
  }
  if (ver <= 1) {
    if (song_.song_len > caps::kMaxSongLenMmd01) {
      return fail(Error::LimitExceeded, song_ptr_ + kSongSongLen, "play sequence longer than 256");
    }
    song_.play_entries = song_.song_len;
  } else {
    playseqtable_ = be32(sb + 508);
    sectiontable_ = be32(sb + 512);
    trackvols_ = be32(sb + 516);
    song_.num_tracks = static_cast<std::uint16_t>(be16(sb + 520));
    song_.num_pseqs = static_cast<std::uint16_t>(be16(sb + 522));
    trackpans_ = be32(sb + 524);
    song_.flags3 = be32(sb + 528);
    song_.vol_adjust = static_cast<std::uint16_t>(be16(sb + 532));
    song_.channels = static_cast<std::uint16_t>(be16(sb + 534));
    song_.echo_type = sb[536];
    song_.echo_depth = sb[537];
    song_.echo_len = static_cast<std::uint16_t>(be16(sb + 538));
    song_.stereo_sep = static_cast<std::int8_t>(sb[540]);
    if (song_.song_len > caps::kMaxSections) {
      return fail(Error::LimitExceeded, song_ptr_ + kSongSongLen, "more than 256 sections");
    }
    if (song_.num_pseqs > caps::kMaxPlaySeqs) {
      return fail(Error::LimitExceeded, song_ptr_ + 522, "more than 256 play sequences");
    }
    if (song_.num_tracks > caps::kMaxTracks) {
      return fail(Error::LimitExceeded, song_ptr_ + 520, "more than 64 tracks");
    }
  }

  // ---- the tables the header points to
  if (song_.num_blocks > 0) {
    if (blockarr_ == 0) {
      return fail(Error::BadPointer, kHdrBlockArr, "the block table pointer is missing");
    }
    s = table(kHdrBlockArr, blockarr_, 4ull * song_.num_blocks, "block table outside the file");
    if (!s.ok()) {
      return s;
    }
  }
  if (song_.num_samples > 0) {
    if (smplarr_ == 0) {
      return fail(Error::Unsupported, kHdrSmplArr, "the instruments are stored outside the file");
    }
    s = table(kHdrSmplArr, smplarr_, 4ull * song_.num_samples, "instrument table outside the file");
    if (!s.ok()) {
      return s;
    }
  }
  if (ver >= 2) {
    if (song_.song_len > 0) {
      if (sectiontable_ == 0) {
        return fail(Error::BadPointer, song_ptr_ + 512, "the section table pointer is missing");
      }
      s = table(song_ptr_ + 512, sectiontable_, 2ull * song_.song_len, "section table outside the file");
      if (!s.ok()) {
        return s;
      }
    }
    if (song_.num_pseqs > 0) {
      if (playseqtable_ == 0) {
        return fail(Error::BadPointer, song_ptr_ + 508, "the play sequence table pointer is missing");
      }
      s = table(song_ptr_ + 508, playseqtable_, 4ull * song_.num_pseqs, "play sequence table outside the file");
      if (!s.ok()) {
        return s;
      }
    }
    if (trackvols_ != 0 && song_.num_tracks > 0) {
      s = table(song_ptr_ + 516, trackvols_, song_.num_tracks, "track volume table outside the file");
      if (!s.ok()) {
        return s;
      }
    }
    if (trackpans_ != 0 && song_.num_tracks > 0) {
      s = table(song_ptr_ + 524, trackpans_, song_.num_tracks, "track pan table outside the file");
      if (!s.ok()) {
        return s;
      }
    }
  }

  // ---- expansion data: the prefix that lies inside the file, the rest reads as 0 (there is no size field)
  if (exp_ != 0) {
    if (exp_ < caps::kMinPointer || exp_ >= size_) {
      return fail(Error::BadPointer, kHdrExp, "expansion data outside the file");
    }
    std::uint8_t eb[caps::kExpPrefix] = {};
    const std::uint32_t avail = std::min<std::uint32_t>(size_ - exp_, caps::kExpPrefix);
    s = get(exp_, eb, avail);
    if (!s.ok()) {
      return s;
    }
    song_.has_exp = true;
    exp_smp_ = be32(eb + 4);
    song_.s_ext_entries = static_cast<std::uint16_t>(be16(eb + 8));
    song_.s_ext_entrsz = static_cast<std::uint16_t>(be16(eb + 10));
    const std::uint32_t annotxt = be32(eb + 12);
    song_.annotation_len = be32(eb + 16);
    iinfo_ = be32(eb + 20);
    song_.i_ext_entries = static_cast<std::uint16_t>(be16(eb + 24));
    song_.i_ext_entrsz = static_cast<std::uint16_t>(be16(eb + 26));
    std::memcpy(song_.channel_split, eb + 36, 4);
    const std::uint32_t n_info = be32(eb + 40);
    const std::uint32_t songname = be32(eb + 44);
    const std::uint32_t songnamelen = be32(eb + 48);
    if (annotxt != 0 && song_.annotation_len != 0) ignored_ |= kIgnoreAnnotation;
    if (n_info != 0) ignored_ |= kIgnoreNotation;
    if (be32(eb + 52) != 0) ignored_ |= kIgnoreMidiDumps;
    if (be32(eb + 56) != 0) ignored_ |= kIgnoreMmdInfo;
    if (be32(eb + 60) != 0) ignored_ |= kIgnoreArexx;
    if (be32(eb + 64) != 0) ignored_ |= kIgnoreMidiCmd3x;
    if ((song_.channel_split[0] | song_.channel_split[1] | song_.channel_split[2] | song_.channel_split[3]) != 0) {
      ignored_ |= kIgnoreChannelSplit;
    }
    // The InstrExt and instrument-name arrays: the first min(entries, 63) entries must lie inside the file (entries above
    // 63 are ignored: MMD3 can announce more). A stride of 0 or no pointer means the array is absent.
    const std::uint32_t used_s = std::min<std::uint32_t>(song_.s_ext_entries, caps::kMaxSamples);
    if (exp_smp_ != 0 && used_s != 0 && song_.s_ext_entrsz != 0) {
      s = table(exp_ + 4, exp_smp_, static_cast<std::uint64_t>(used_s) * song_.s_ext_entrsz, "InstrExt array outside the file");
      if (!s.ok()) {
        return s;
      }
    } else {
      exp_smp_ = 0;
    }
    const std::uint32_t used_i = std::min<std::uint32_t>(song_.i_ext_entries, caps::kMaxSamples);
    if (iinfo_ != 0 && used_i != 0 && song_.i_ext_entrsz != 0) {
      s = table(exp_ + 20, iinfo_, static_cast<std::uint64_t>(used_i) * song_.i_ext_entrsz, "instrument info array outside the file");
      if (!s.ok()) {
        return s;
      }
    } else {
      iinfo_ = 0;
    }
    if (songname != 0 && songnamelen != 0) {
      const std::uint32_t need = std::min<std::uint32_t>(songnamelen, caps::kSongNameMax);
      s = table(exp_ + 44, songname, need, "song name outside the file");
      if (!s.ok()) {
        return s;
      }
      s = copy_text(songname, need, song_.name, sizeof song_.name);
      if (!s.ok()) {
        return s;
      }
    }
  }

  // ---- MMD2+: the flattened play list is the play sequences of the sections one after the other
  if (ver >= 2) {
    std::uint32_t total = 0;
    for (std::uint32_t sec = 0; sec < song_.song_len; ++sec) {
      std::uint32_t seq_ptr = 0, length = 0;
      s = section_seq(sec, seq_ptr, length);
      if (!s.ok()) {
        return s;
      }
      sec_start_[sec] = static_cast<std::uint16_t>(total);
      total += length;
      if (total > caps::kMaxPlayEntries) {
        return fail(Error::LimitExceeded, sectiontable_ + 2 * sec, "play list longer than 4096 entries");
      }
    }
    sec_start_[song_.song_len] = static_cast<std::uint16_t>(total);
    song_.play_entries = total;
  }
  return Status{};
}

// ---- play list ---------------------------------------------------------------------------------------------------------

// The play sequence of section `section` (MMD2+): its pointer (0 when the section names a sequence that does not exist) and
// entry count. Both are checked against the file: the header, and the entries that `length` promises.
Status Reader::section_seq(std::uint32_t section, std::uint32_t& seq_ptr, std::uint32_t& length) const noexcept {
  seq_ptr = 0;
  length = 0;
  std::uint8_t b[4];
  Status s = get(sectiontable_ + 2 * section, b, 2);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t seq = be16(b);
  if (seq >= song_.num_pseqs) {
    return Status{};  // a section that names nothing plays nothing; validate() counts it
  }
  s = get(playseqtable_ + 4 * seq, b, 4);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t p = be32(b);
  s = table(playseqtable_ + 4 * seq, p, kPlaySeqHdr, "play sequence outside the file");
  if (!s.ok()) {
    return s;
  }
  s = get(p + 40, b, 2);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t n = be16(b);
  s = table(playseqtable_ + 4 * seq, p, kPlaySeqHdr + 2ull * n, "play sequence entries outside the file");
  if (!s.ok()) {
    return s;
  }
  seq_ptr = p;
  length = n;
  return Status{};
}

Status Reader::play_entry(std::uint32_t pos, PlayEntry& out) const noexcept {
  out = PlayEntry();
  if (pos >= song_.play_entries) {
    return fail(Error::Malformed, 0, "position past the play list");
  }
  std::uint8_t b[4];
  if (song_.version <= 1) {
    const Status s = get(song_ptr_ + kSongPlaySeq + pos, b, 1);
    if (!s.ok()) {
      return s;
    }
    out.block = b[0];
    out.kind = out.block < song_.num_blocks ? PlayKind::Block : PlayKind::Bad;
    out.section_len = song_.song_len;
    return Status{};
  }
  // The last section whose first entry is at or before `pos` (empty sections share the start of the next one).
  std::uint32_t lo = 0, hi = song_.song_len;  // sec_start_[lo] <= pos < sec_start_[hi]
  while (hi - lo > 1) {
    const std::uint32_t mid = (lo + hi) / 2;
    if (sec_start_[mid] <= pos) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  std::uint32_t seq_ptr = 0, length = 0;
  Status s = section_seq(lo, seq_ptr, length);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t index = pos - sec_start_[lo];
  if (seq_ptr == 0 || index >= length) {
    return fail(Error::Malformed, 0, "the play list changed under the reader");
  }
  out.section = static_cast<std::uint16_t>(lo);
  out.section_first = sec_start_[lo];
  out.section_len = length;
  s = get(seq_ptr + kPlaySeqHdr + 2 * index, b, 2);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t number = be16(b);
  if (number >= 0x8000) {
    out.kind = PlayKind::Skip;
  } else {
    out.block = static_cast<std::uint16_t>(number);
    out.kind = number < song_.num_blocks ? PlayKind::Block : PlayKind::Bad;
  }
  // The command table of the play sequence (MMD2+): 4-byte entries {u16 position, u8 command, u8 extra size}, the extra
  // bytes after each entry, ended by position 0xFFFF. 1 stops the song, 2 jumps to the u16 in the extra bytes. The layout
  // is from OpenMPT's reader (docs/formats/med.md 2.3, inference); an entry that does not fit the file ends the table.
  s = get(seq_ptr + 32, b, 4);
  if (!s.ok()) {
    return s;
  }
  std::uint32_t cur = be32(b);
  if (cur == 0) {
    return Status{};
  }
  s = table(seq_ptr + 32, cur, 4, "play sequence command table outside the file");
  if (!s.ok()) {
    return s;
  }
  for (std::uint32_t hop = 0; hop < caps::kMaxPlayCommands; ++hop) {
    if (static_cast<std::uint64_t>(cur) + 4 > size_) {
      break;
    }
    s = get(cur, b, 4);
    if (!s.ok()) {
      return s;
    }
    const std::uint32_t at = be16(b);
    const unsigned cmd = b[2];
    const std::uint32_t extra = b[3];
    if (at == 0xFFFF) {
      break;
    }
    if (at == index) {
      if (cmd == 1) {
        out.kind = PlayKind::Stop;
      } else if (cmd == 2 && extra >= 2 && static_cast<std::uint64_t>(cur) + 6 <= size_) {
        std::uint8_t t[2];
        s = get(cur + 4, t, 2);
        if (!s.ok()) {
          return s;
        }
        out.kind = PlayKind::Jump;
        out.target = static_cast<std::uint16_t>(be16(t));
      }
    }
    cur += 4 + extra;
  }
  return Status{};
}

// ---- blocks ------------------------------------------------------------------------------------------------------------

Status Reader::block(std::uint32_t index, BlockInfo& out) const noexcept {
  out = BlockInfo();
  if (index >= song_.num_blocks) {
    return fail(Error::Malformed, 0, "block number past the block table");
  }
  const std::uint32_t entry_at = blockarr_ + 4 * index;
  std::uint8_t b[8];
  Status s = get(entry_at, b, 4);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t p = be32(b);
  if (p == 0) {
    return Status{};  // empty: out.empty stays true
  }
  const bool wide = song_.version >= 1;
  const std::uint32_t hdr = wide ? kBlockHdr1 : kBlockHdr0;
  s = table(entry_at, p, hdr, "block pointer outside the file");
  if (!s.ok()) {
    return s;
  }
  s = get(p, b, hdr);
  if (!s.ok()) {
    return s;
  }
  std::uint32_t tracks, lines, info = 0;
  if (wide) {
    tracks = be16(b);
    lines = be16(b + 2) + 1u;
    info = be32(b + 4);
  } else {
    tracks = b[0];
    lines = b[1] + 1u;
  }
  if (tracks == 0) {
    return fail(Error::Malformed, p, "a block with no tracks");
  }
  if (tracks > caps::kMaxTracks) {
    return fail(Error::LimitExceeded, p, "more than 64 tracks in a block");
  }
  if (lines > caps::kMaxLines) {
    return fail(Error::LimitExceeded, p, "more than 9,999 lines in a block");
  }
  out.offset = p;
  out.cells = p + hdr;
  out.tracks = static_cast<std::uint16_t>(tracks);
  out.lines = static_cast<std::uint16_t>(lines);
  out.cell_bytes = wide ? 4 : 3;
  out.empty = false;
  const std::uint64_t cells = static_cast<std::uint64_t>(tracks) * lines;
  if (static_cast<std::uint64_t>(out.cells) + cells * out.cell_bytes > size_) {
    return fail(Error::Truncated, out.cells, "the cells of a block run past the end of the file");
  }
  if (info == 0) {
    return Status{};
  }
  // BlockInfo: name, command pages, command extension. Offsets 0 (highlight mask) and 20.. are not used.
  std::uint8_t ib[kBlockInfoBytes];
  s = table(p + 4, info, kBlockInfoBytes, "BlockInfo outside the file");
  if (!s.ok()) {
    return s;
  }
  s = get(info, ib, kBlockInfoBytes);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t name = be32(ib + 4), name_len = be32(ib + 8), pagetable = be32(ib + 12), cmdext = be32(ib + 16);
  if (name != 0 && name_len != 0) {
    s = table(info + 4, name, std::min<std::uint32_t>(name_len, caps::kBlockNameMax + 1), "block name outside the file");
    if (!s.ok()) {
      return s;
    }
    out.name = name;
    out.name_len = name_len;
  }
  if (pagetable != 0) {
    s = table(info + 12, pagetable, 4, "command page table outside the file");
    if (!s.ok()) {
      return s;
    }
    std::uint8_t pb[4];
    s = get(pagetable, pb, 2);
    if (!s.ok()) {
      return s;
    }
    const std::uint32_t pages = be16(pb);
    if (pages > caps::kMaxPages) {
      return fail(Error::LimitExceeded, pagetable, "more than 16 command pages in a block");
    }
    s = table(info + 12, pagetable, 4 + 4ull * pages, "command page pointers outside the file");
    if (!s.ok()) {
      return s;
    }
    out.pages = static_cast<std::uint8_t>(pages);
    for (std::uint32_t i = 0; i < pages; ++i) {
      s = get(pagetable + 4 + 4 * i, pb, 4);
      if (!s.ok()) {
        return s;
      }
      const std::uint32_t pp = be32(pb);
      if (pp == 0) {
        continue;  // a page of empty cells
      }
      if (pp < caps::kMinPointer) {
        return fail(Error::BadPointer, pagetable + 4 + 4 * i, "command page pointer inside the header");
      }
      if (static_cast<std::uint64_t>(pp) + cells * 2 > size_) {
        return fail(Error::Truncated, pp, "a command page runs past the end of the file");
      }
      out.page[i] = pp;
    }
  }
  if (cmdext != 0) {
    // SoundStudio: a pointer to a pointer to tracks x lines x (1 + pages) bytes, the default page first
    // (docs/formats/med.md 2.3, from OpenMPT's reader).
    s = table(info + 16, cmdext, 4, "command extension pointer outside the file");
    if (!s.ok()) {
      return s;
    }
    std::uint8_t pb[4];
    s = get(cmdext, pb, 4);
    if (!s.ok()) {
      return s;
    }
    const std::uint32_t target = be32(pb);
    if (target != 0) {
      if (target < caps::kMinPointer) {
        return fail(Error::BadPointer, cmdext, "command extension table inside the header");
      }
      if (static_cast<std::uint64_t>(target) + cells * (1u + out.pages) > size_) {
        return fail(Error::Truncated, target, "the command extension table runs past the end of the file");
      }
      out.cmd_ext = target;
    }
  }
  return Status{};
}

Status Reader::read_line(const BlockInfo& b, std::uint32_t line, std::uint32_t page, Cell* out, bool with_ext) const noexcept {
  if (b.empty || line >= b.lines || page > b.pages || b.tracks == 0 || b.tracks > caps::kMaxTracks ||
      (b.cell_bytes != 3 && b.cell_bytes != 4)) {
    return fail(Error::Malformed, b.offset, "line or page outside the block, or a block that block() did not make");
  }
  const std::uint32_t tracks = b.tracks;
  std::uint8_t buf[caps::kMaxTracks * 4];
  if (page == 0) {
    const std::uint32_t n = tracks * b.cell_bytes;
    const Status s = get(b.cells + line * n, buf, n);
    if (!s.ok()) {
      return s;
    }
    for (std::uint32_t t = 0; t < tracks; ++t) {
      const std::uint8_t* c = buf + t * b.cell_bytes;
      Cell& cell = out[t];
      if (b.cell_bytes == 3) {  // xynnnnnn iiiicccc dddddddd
        cell.note = c[0] & 0x3F;
        cell.note_hi = false;
        cell.instrument = static_cast<std::uint8_t>((c[1] >> 4) | ((c[0] & 0x80) >> 3) | ((c[0] & 0x40) >> 1));
        cell.command = c[1] & 0x0F;
        cell.data = c[2];
      } else {  // xnnnnnnn xxiiiiii cccccccc dddddddd
        cell.note = c[0] & 0x7F;
        cell.note_hi = (c[0] & 0x80) != 0;
        cell.instrument = c[1] & 0x3F;
        cell.command = c[2];
        cell.data = c[3];
      }
      cell.ext = 0;
    }
  } else {
    const std::uint32_t p = b.page[page - 1];
    if (p == 0) {
      for (std::uint32_t t = 0; t < tracks; ++t) {
        out[t] = Cell();
      }
    } else {
      const Status s = get(p + line * tracks * 2, buf, tracks * 2);
      if (!s.ok()) {
        return s;
      }
      for (std::uint32_t t = 0; t < tracks; ++t) {
        out[t] = Cell();
        out[t].command = buf[2 * t];
        out[t].data = buf[2 * t + 1];
      }
    }
  }
  if (with_ext && b.cmd_ext != 0) {
    const Status s = get(b.cmd_ext + (page * b.lines + line) * tracks, buf, tracks);
    if (!s.ok()) {
      return s;
    }
    for (std::uint32_t t = 0; t < tracks; ++t) {
      out[t].ext = buf[t];
    }
  }
  return Status{};
}

Status Reader::block_name(const BlockInfo& b, char* out, std::size_t cap) const noexcept {
  if (cap != 0) {
    out[0] = '\0';
  }
  if (b.name == 0 || b.name_len == 0) {
    return Status{};
  }
  return copy_text(b.name, b.name_len, out, std::min<std::size_t>(cap, caps::kBlockNameMax + 1));
}

// ---- instruments -------------------------------------------------------------------------------------------------------

// InstrExt entry and the name of instrument `slot`, as far as the file's strides reach (a field is read only if the stride
// covers it, a skip is never negative).
Status Reader::read_instrument_ext(std::uint32_t slot, InstrumentInfo& out) const noexcept {
  if (exp_smp_ != 0 && slot < song_.s_ext_entries) {
    std::uint8_t e[18] = {};
    const std::uint32_t n = std::min<std::uint32_t>(song_.s_ext_entrsz, 18);
    const Status s = get(exp_smp_ + slot * song_.s_ext_entrsz, e, n);
    if (!s.ok()) {
      return s;
    }
    out.ext_bytes = static_cast<std::uint8_t>(n);
    out.hold = e[0];
    out.decay = e[1];
    out.finetune = static_cast<std::int8_t>(e[3]);
    out.default_pitch = e[4];
    out.ext_flags = e[5];
    out.long_midi_preset = static_cast<std::uint16_t>(be16(e + 6));
    out.output_device = e[8];
    out.disabled = (e[5] & 4) != 0;
    out.long_repeat = be32(e + 10);
    out.long_replen = be32(e + 14);
  }
  if (iinfo_ != 0 && slot < song_.i_ext_entries && song_.i_ext_entrsz >= 40) {
    return copy_text(iinfo_ + slot * song_.i_ext_entrsz, 40, out.name, sizeof out.name);
  }
  return Status{};
}

Status Reader::instrument(std::uint32_t slot, InstrumentInfo& out) const noexcept {
  out = InstrumentInfo();
  if (slot >= song_.num_samples) {
    return fail(Error::Malformed, 0, "instrument number past the instrument table");
  }
  // The song record: rep, replen (both in units of two frames), MIDI channel and preset, default volume, transpose.
  std::uint8_t sr[8];
  Status s = get(song_ptr_ + 8 * slot, sr, 8);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t rep_raw = be16(sr), replen_raw = be16(sr + 2);
  out.midi_channel = sr[4];
  out.midi_preset = sr[5];
  out.volume = sr[6];
  out.transpose = static_cast<std::int8_t>(sr[7]);
  s = read_instrument_ext(slot, out);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t entry_at = smplarr_ + 4 * slot;
  std::uint8_t hb[6];
  s = get(entry_at, hb, 4);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t p = be32(hb);
  if (p == 0) {
    return Status{};  // Empty
  }
  s = table(entry_at, p, 6, "instrument outside the file");
  if (!s.ok()) {
    return s;
  }
  s = get(p, hb, 6);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t length = be32(hb);
  const std::int32_t type = static_cast<std::int16_t>(be16(hb + 4));
  out.offset = p;
  out.raw_type = static_cast<std::int16_t>(type);

  bool sample_like = false;
  if (type == -1 || type == -2) {
    // SynthInstr: the header fields, and every waveform pointer checked against the file.
    out.kind = type == -1 ? InstKind::Synth : InstKind::Hybrid;
    if (static_cast<std::uint64_t>(p) + caps::kSynthBytes > size_) {
      return fail(Error::Truncated, p, "synth instrument runs past the end of the file");
    }
    std::uint8_t sy[22];
    s = get(p, sy, 22);
    if (!s.ok()) {
      return s;
    }
    out.voltbl_len = static_cast<std::uint16_t>(be16(sy + 14));
    out.wftbl_len = static_cast<std::uint16_t>(be16(sy + 16));
    out.vol_speed = sy[18];
    out.wf_speed = sy[19];
    const std::uint32_t wforms = be16(sy + 20);
    if (out.voltbl_len > caps::kMaxSynthTable || out.wftbl_len > caps::kMaxSynthTable) {
      return fail(Error::LimitExceeded, p + 14, "synth table longer than 128");
    }
    if (wforms != 0xFFFF && wforms > caps::kMaxWaveforms) {
      return fail(Error::LimitExceeded, p + 20, "more than 64 waveforms");
    }
    out.waveforms = static_cast<std::uint16_t>(wforms == 0xFFFF ? 0 : wforms);
    for (std::uint32_t w = 0; w < out.waveforms; ++w) {
      std::uint32_t header = 0, data = 0, bytes = 0;
      s = synth_waveform(out, w, header, data, bytes);
      if (!s.ok()) {
        return s;
      }
      if (type == -2 && w == 0 && data != 0) {  // a hybrid's first waveform is a sample of its own
        out.data = data;
        out.data_bytes = bytes;
        out.frames = bytes;
        out.base_frames = bytes;
        out.pcm_bytes = static_cast<std::uint64_t>(bytes) * 2;
        sample_like = true;
      }
    }
    if (!sample_like) {
      return Status{};
    }
  } else if (type < 0) {
    out.kind = InstKind::Unsupported;  // HighLife, VSTi, or a type nobody has described
    return Status{};
  } else {
    // Sample types: the low nibble is the type, 0x10 = 16-bit, 0x20 = stereo; 0x18 is the obsolete Aura 16-bit sample.
    // Delta (0x40) and packed (0x80) data are not in Rev 6 and not read.
    std::uint32_t low;
    if (type == 0x18) {
      out.sixteen_bit = true;
      low = 0;
    } else {
      if ((type & 0xC0) != 0 || (type & 0x0F) > 7) {
        out.kind = InstKind::Unsupported;
        return Status{};
      }
      out.sixteen_bit = (type & 0x10) != 0;
      out.stereo = (type & 0x20) != 0;
      low = static_cast<std::uint32_t>(type) & 0x0F;
    }
    out.kind = InstKind::Sample;
    out.data = p + 6;
    out.data_bytes = length;
    const unsigned channels = out.stereo ? 2u : 1u;
    if (static_cast<std::uint64_t>(out.data) + static_cast<std::uint64_t>(length) * channels > size_) {
      return fail(Error::Truncated, p, "instrument data runs past the end of the file");
    }
    out.frames = out.sixteen_bit ? length / 2 : length;
    if (out.sixteen_bit && (length & 1u) != 0) {
      out.notes |= kNoteTrimmed;
    }
    out.ext_sample = low == 7;
    out.octaves = (low >= 1 && low <= 6) ? kOctavesOfType[low - 1] : 1;
    out.base_frames = out.frames;
    if (out.octaves > 1 && out.frames != 0) {
      const std::uint32_t parts = (1u << out.octaves) - 1u;
      if (out.frames < parts) {
        return fail(Error::Malformed, p, "an octave split of a sample shorter than its octaves");
      }
      out.base_frames = out.frames / parts;
      if (out.frames % parts != 0) {
        out.notes |= kNoteTrimmed;
      }
    }
    out.pcm_bytes = static_cast<std::uint64_t>(out.frames) * channels * 2u;
    sample_like = true;
  }
  if (out.pcm_bytes > caps::kSampleArenaBytes) {
    out.notes |= kNoteOversize;
  }

  // The loop, in frames of the shortest octave. MMD2+ files carry a loop flag in InstrExt (stride >= 6); older ones loop
  // when replen > 1. SoundStudio's long_repeat / long_replen (stride >= 18) override the song record in MMD3, as libxmp does;
  // the unit is frames for 16-bit samples too (libxmp and OpenMPT agree). Everything is clamped into the sample.
  if (sample_like) {
    std::uint32_t start = rep_raw * 2u, len = replen_raw * 2u;
    bool loops = replen_raw > 1;
    if (song_.version >= 2 && out.ext_bytes >= 6) {
      loops = (out.ext_flags & 1) != 0;
      out.ping_pong = (out.ext_flags & 8) != 0;
    }
    if (song_.version >= 3 && out.ext_bytes >= 18) {
      start = out.long_repeat;
      len = out.long_replen;
      out.notes |= kNoteLongLoop;
    }
    if (loops) {
      const std::uint32_t s0 = start, l0 = len;
      start = std::min(start, out.base_frames);
      len = std::min(len, out.base_frames - start);
      if (start != s0 || len != l0) {
        out.notes |= kNoteLoopClamped;
      }
      if (len > 1) {
        out.has_loop = true;
        out.loop_start = start;
        out.loop_len = len;
      } else {
        out.notes |= kNoteLoopClamped;
        out.ping_pong = false;
      }
    } else {
      out.ping_pong = false;
    }
  }
  return Status{};
}

Status Reader::synth_waveform(const InstrumentInfo& info, std::uint32_t w, std::uint32_t& header, std::uint32_t& data,
                              std::uint32_t& bytes) const noexcept {
  header = 0;
  data = 0;
  bytes = 0;
  if ((info.kind != InstKind::Synth && info.kind != InstKind::Hybrid) || w >= info.waveforms) {
    return fail(Error::Malformed, info.offset, "waveform number past the instrument");
  }
  const std::uint32_t at = info.offset + kSynthWf + 4 * w;
  std::uint8_t b[6];
  Status s = get(at, b, 4);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t off = be32(b);
  if (off == 0) {
    return Status{};  // no waveform in this slot
  }
  // The pointer is relative to the instrument and must lie beyond the SynthInstr struct itself.
  const std::uint64_t abs = static_cast<std::uint64_t>(info.offset) + off;
  if (off < caps::kSynthBytes || abs + 2 > size_) {
    return fail(Error::BadPointer, at, "waveform pointer outside the instrument data");
  }
  const std::uint32_t a = static_cast<std::uint32_t>(abs);
  if (info.kind == InstKind::Hybrid && w == 0) {
    if (abs + 6 > size_) {
      return fail(Error::BadPointer, at, "hybrid sample header outside the file");
    }
    s = get(a, b, 6);
    if (!s.ok()) {
      return s;
    }
    const std::uint32_t len = be32(b);
    if (abs + 6 + len > size_) {
      return fail(Error::Truncated, a, "hybrid sample runs past the end of the file");
    }
    header = a;
    data = a + 6;
    bytes = len;
    return Status{};
  }
  s = get(a, b, 2);
  if (!s.ok()) {
    return s;
  }
  const std::uint32_t n = 2u * be16(b);
  if (abs + 2 + n > size_) {
    return fail(Error::Truncated, a, "waveform runs past the end of the file");
  }
  header = a;
  data = a + 2;
  bytes = n;
  return Status{};
}

// ---- track volume and pan ---------------------------------------------------------------------------------------------

std::uint8_t Reader::track_volume(std::uint32_t t) const noexcept {
  std::uint8_t v = 64;
  if (song_.version <= 1) {
    if (t < 16 && !get(song_ptr_ + kSongTrkVol + t, &v, 1).ok()) {
      v = 64;
    }
  } else if (trackvols_ != 0 && t < song_.num_tracks && !get(trackvols_ + t, &v, 1).ok()) {
    v = 64;
  }
  return std::min<std::uint8_t>(v, 64);
}

std::int8_t Reader::track_pan(std::uint32_t t) const noexcept {
  std::uint8_t v = 0;
  if (song_.version >= 2 && trackpans_ != 0 && t < song_.num_tracks && !get(trackpans_ + t, &v, 1).ok()) {
    v = 0;
  }
  const int p = static_cast<std::int8_t>(v);
  return static_cast<std::int8_t>(std::min(16, std::max(-16, p)));
}

// ---- the chain of songs and the whole-file check -----------------------------------------------------------------------

// MMD0exp.nextmod chains songs (multi-modules). Offsets must increase strictly, at most 256 songs; each header must carry
// an MMD0..3 or MCNT / MCN1..3 id. No recursion, no revisit: strictly increasing offsets cannot cycle.
Status Reader::chain(std::uint32_t& songs) const noexcept {
  songs = 1;
  std::uint32_t exp = exp_;
  std::uint32_t prev = 0;
  std::uint8_t b[4];
  while (exp != 0 && static_cast<std::uint64_t>(exp) + 4 <= size_) {
    Status s = get(exp, b, 4);
    if (!s.ok()) {
      return s;
    }
    const std::uint32_t next = be32(b);
    if (next == 0) {
      break;
    }
    if (next <= prev || next < caps::kMinPointer || static_cast<std::uint64_t>(next) + caps::kHeaderBytes > size_) {
      return fail(Error::BadPointer, exp, "next song pointer does not increase or lies outside the file");
    }
    if (songs >= caps::kMaxSongs) {
      return fail(Error::LimitExceeded, exp, "more than 256 chained songs");
    }
    s = get(next, b, 4);
    if (!s.ok()) {
      return s;
    }
    const bool mmd = b[0] == 'M' && b[1] == 'M' && b[2] == 'D' && b[3] >= '0' && b[3] <= '3';
    const bool mcn = b[0] == 'M' && b[1] == 'C' && b[2] == 'N' && (b[3] == 'T' || (b[3] >= '1' && b[3] <= '3'));
    if (!mmd && !mcn) {
      return fail(Error::BadMagic, next, "a chained song has no MMD or MCN id");
    }
    ++songs;
    prev = next;
    s = get(next + kHdrExp, b, 4);
    if (!s.ok()) {
      return s;
    }
    exp = be32(b);
    if (exp != 0 && (exp < caps::kMinPointer || static_cast<std::uint64_t>(exp) + 4 > size_)) {
      return fail(Error::BadPointer, next + kHdrExp, "expansion data of a chained song outside the file");
    }
  }
  return Status{};
}

Status Reader::validate(Summary& out) noexcept {
  out = Summary();
  if (src_ == nullptr) {
    return fail(Error::Malformed, 0, "the reader is not open");
  }
  ValidateState st;
  while (st.phase != ValidateState::Phase::Done) {
    const Status s = validate_step(st, out, ~static_cast<std::size_t>(0));
    if (!s.ok()) {
      return s;
    }
  }
  return Status{};
}

Status Reader::validate_step(ValidateState& st, Summary& out, std::size_t budget) const noexcept {
  if (src_ == nullptr) {
    return fail(Error::Malformed, 0, "the reader is not open");
  }
  using Phase = ValidateState::Phase;
  std::size_t work = 0;
  while (st.phase != Phase::Done && (work == 0 || work < budget)) {
    Status s;
    switch (st.phase) {
      case Phase::Chain:  // the songs of the chain: at most 256, each a few reads
        s = chain(out.songs);
        work += out.songs;
        st.phase = song_.version >= 2 ? Phase::Sections : Phase::Play;
        st.index = 0;
        break;
      case Phase::Sections: {
        if (st.index >= song_.song_len) {
          st.phase = Phase::Play;
          st.index = 0;
          break;
        }
        std::uint32_t seq_ptr = 0, length = 0;
        s = section_seq(st.index, seq_ptr, length);
        if (s.ok() && seq_ptr == 0) {
          ++out.bad_sections;
        }
        ++st.index;
        ++work;
        break;
      }
      case Phase::Play: {
        // The play list: every entry, and every block it plays once (a 4096-bit set covers block numbers below 4096).
        if (st.index >= song_.play_entries) {
          st.phase = Phase::Instruments;
          st.index = 0;
          break;
        }
        const std::uint32_t pos = st.index++;
        ++work;
        PlayEntry e;
        s = play_entry(pos, e);
        if (!s.ok()) {
          break;
        }
        if (e.kind == PlayKind::Skip) {
          ++out.skip_play_entries;
          break;
        }
        if (e.kind == PlayKind::Bad) {
          ++out.bad_play_entries;
          break;
        }
        if (e.kind != PlayKind::Block) {
          ++out.play_commands;
          break;
        }
        if (e.block < 4096) {
          std::uint8_t& byte = st.seen[e.block >> 3];
          const std::uint8_t bit = static_cast<std::uint8_t>(1u << (e.block & 7));
          if ((byte & bit) != 0) {
            break;
          }
          byte = static_cast<std::uint8_t>(byte | bit);
        }
        BlockInfo b;
        s = block(e.block, b);
        ++work;
        if (!s.ok()) {
          break;
        }
        if (b.empty) {
          ++out.empty_blocks;
          ++out.bad_play_entries;
          break;
        }
        ++out.blocks_referenced;
        out.max_tracks = std::max(out.max_tracks, b.tracks);
        out.max_lines = std::max(out.max_lines, b.lines);
        out.pages_max = std::max<std::uint32_t>(out.pages_max, b.pages);
        out.cells += static_cast<std::uint64_t>(b.tracks) * b.lines;
        break;
      }
      case Phase::Instruments: {
        if (st.index >= song_.num_samples) {
          out.ignored = ignored_;
          if (song_.version >= 2) {
            if (song_.echo_type != 0) out.ignored |= kIgnoreEcho;
            if (song_.vol_adjust != 0 && song_.vol_adjust != 100) out.ignored |= kIgnoreVolAdjust;
            if (song_.stereo_sep != 0) out.ignored |= kIgnoreStereoSep;
          }
          if (out.songs > 1 || song_.extra_songs != 0) {
            out.ignored |= kIgnoreExtraSongs;
          }
          st.phase = Phase::Done;
          break;
        }
        const std::uint32_t slot = st.index++;
        work += 4;  // a synth instrument reads up to 64 waveform headers
        InstrumentInfo ii;
        s = instrument(slot, ii);
        if (!s.ok() || ii.kind == InstKind::Empty) {
          break;
        }
        ++out.instruments;
        const std::uint64_t bit = 1ull << slot;
        if (ii.kind == InstKind::Unsupported) {
          out.unsupported_mask |= bit;
          break;
        }
        if (ii.kind == InstKind::Synth || ii.kind == InstKind::Hybrid) {
          ++out.synth;
        }
        if ((ii.notes & kNoteOversize) != 0) {
          out.oversize_mask |= bit;
        }
        out.pcm_bytes += ii.pcm_bytes;
        break;
      }
      case Phase::Done:
        break;
    }
    if (!s.ok()) {
      st.phase = Phase::Done;  // a problem ends the job
      return s;
    }
  }
  return Status{};
}

}  // namespace o3os::formats::med
