// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"
#include "support/rng.h"

#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/mmd.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::test::mmd::Bytes;
using o3os::test::mmd::Layout;
using o3os::test::mmd::ModuleSpec;

// Hostile input in the spirit of PAR-011 and docs/formats/med.md 2.6: truncation at every offset of a small synthetic file of
// each version, the explicit vectors of the spec (pointer 0xFFFFFFFF, a chain that cycles, tracks and lines at 0xFFFF, strides
// 0..3, wforms 0xFFFF, an octave split of a tiny sample, a play-sequence entry past the block table...), a source that fails,
// and deterministic mutation fuzzing. Under ASan and UBSan the run proves that nothing reads or writes out of bounds; the
// assertions prove the rest: every file ends in a typed Status, the reader never asks its source for bytes past the end, and a
// file that validates can be walked to its last cell and linearised. O3OS_FUZZ_SCALE=<n> multiplies the run counts.

namespace {

#if defined(__arm__)
constexpr std::uint32_t kThin = 40;  // the target runs at a few MIPS
#elif defined(__SANITIZE_THREAD__)
constexpr std::uint32_t kThin = 20;
#else
constexpr std::uint32_t kThin = 1;
#endif

std::uint32_t runs(std::uint32_t base) {
  const char* e = std::getenv("O3OS_FUZZ_SCALE");
  const long v = e != nullptr ? std::strtol(e, nullptr, 10) : 1;
  const std::uint32_t scale = v < 1 ? 1u : (v > 1000 ? 1000u : static_cast<std::uint32_t>(v));
  const std::uint32_t n = base * scale / kThin;
  return n < 8 ? 8 : n;
}

// A source that checks the contract: never asked past the end (violations), optionally failing from an offset on.
class StrictSource final : public RandomSource {
 public:
  explicit StrictSource(const Bytes& b) : data_(b) {}
  std::uint32_t violations = 0;
  std::uint32_t fail_from = 0xFFFFFFFFu;
  std::uint32_t size() const noexcept override { return static_cast<std::uint32_t>(data_.size()); }
  bool read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept override {
    if (static_cast<std::uint64_t>(offset) + n > data_.size()) {
      ++violations;
      return false;
    }
    if (static_cast<std::uint64_t>(offset) + n > fail_from) {
      return false;
    }
    if (n != 0) std::memcpy(dst, data_.data() + offset, n);
    return true;
  }

 private:
  const Bytes& data_;
};

void put16(Bytes& b, std::size_t at, unsigned v) {
  b[at] = static_cast<std::uint8_t>(v >> 8);
  b[at + 1] = static_cast<std::uint8_t>(v);
}
void put32(Bytes& b, std::size_t at, std::uint32_t v) {
  put16(b, at, v >> 16);
  put16(b, at + 2, v & 0xFFFF);
}

// Everything a validated file promises: the play list, every referenced block to its last line (long blocks: a sample of
// lines and the last), all pages, names, instruments and synth waveforms, the track tables.
bool walk_all(const Reader& r) {
  const SongInfo& s = r.song();
  Cell cells[caps::kMaxTracks];
  char name[64];
  for (std::uint32_t pos = 0; pos < s.play_entries; ++pos) {
    PlayEntry e;
    if (!r.play_entry(pos, e).ok()) return false;
    if (e.kind != PlayKind::Block) continue;
    BlockInfo b;
    if (!r.block(e.block, b).ok()) return false;
    if (b.empty) continue;
    if (!r.block_name(b, name, sizeof name).ok()) return false;
    for (std::uint32_t line = 0; line < b.lines; line = line < 40 ? line + 1 : line + 211) {
      for (std::uint32_t page = 0; page <= b.pages; ++page) {
        if (!r.read_line(b, line, page, cells).ok()) return false;
      }
    }
    for (std::uint32_t page = 0; page <= b.pages; ++page) {
      if (!r.read_line(b, b.lines - 1u, page, cells).ok()) return false;
    }
  }
  for (std::uint32_t slot = 0; slot < s.num_samples; ++slot) {
    InstrumentInfo i;
    if (!r.instrument(slot, i).ok()) return false;
    for (std::uint32_t w = 0; w < i.waveforms; ++w) {
      std::uint32_t h, d, n;
      if (!r.synth_waveform(i, w, h, d, n).ok()) return false;
    }
  }
  for (std::uint32_t t = 0; t < 64; ++t) {
    if (r.track_volume(t) > 64) return false;
    const int p = r.track_pan(t);
    if (p < -16 || p > 16) return false;
  }
  return true;
}

LinearSong& flow_storage() {
  static LinearSong s;
  return s;
}

struct Outcome {
  Status open;
  Status validate;
  bool walked = false;
  Status flow;
  Status structural;
  std::uint32_t violations = 0;
};

// open -> validate -> (when ok) walk, linearise, structural; with the checks every file must pass.
bool sane_offset(const Status& s, std::size_t size) {
  // a Truncated read may name the offset it wanted, past the end; every other class names a field inside the file
  return s.error == Error::Truncated || s.offset <= size;
}

Outcome run_file(const Bytes& bytes, std::uint32_t fail_from = 0xFFFFFFFFu, std::uint32_t max_visits = 3000) {
  Outcome o;
  StrictSource src(bytes);
  src.fail_from = fail_from;
  const bool faulty = fail_from != 0xFFFFFFFFu;  // a source that fails mid-read may fail any later step too
  Reader r;
  o.open = r.open(src);
  if (!o.open.ok()) {
    CHECK(o.open.error != Error::None);
    CHECK(sane_offset(o.open, bytes.size()));
    o.violations = src.violations;
    return o;
  }
  Summary sum;
  o.validate = r.validate(sum);
  if (!o.validate.ok()) {
    CHECK(o.validate.error != Error::None);
    CHECK(sane_offset(o.validate, bytes.size()));
    o.violations = src.violations;
    return o;
  }
  o.walked = walk_all(r);
  CHECK((o.walked || faulty));
  FlowOptions opt;
  opt.max_visits = max_visits;
  LinearSong& out = flow_storage();
  o.flow = linearise(r, opt, out);
  if (o.flow.ok()) {
    CHECK(out.pattern_count <= flow_caps::kMaxPatterns);
    CHECK(out.order_count <= flow_caps::kMaxOrders);
    for (unsigned i = 0; i < out.order_count; ++i) CHECK(out.order[i] < out.pattern_count);
  } else {
    // a validated file fails the flow only by its limits (or by a source that fails)
    CHECK((o.flow.error == Error::LimitExceeded || (faulty && o.flow.error == Error::SourceFailed)));
  }
  o.structural = structural(r, opt, out);
  if (!o.structural.ok()) {
    CHECK((o.structural.error == Error::LimitExceeded || (faulty && o.structural.error == Error::SourceFailed)));
  }
  o.violations = src.violations;
  CHECK(o.violations == 0);
  return o;
}

Bytes build_of(const ModuleSpec& m, Layout* lay = nullptr) { return test::mmd::build(m, lay); }

Error validate_error(const Bytes& b) {
  const Outcome o = run_file(b);
  if (!o.open.ok()) return o.open.error;
  return o.validate.error;
}

}  // namespace

TEST_CASE("med hostile: a file cut at every offset ends in a typed Status and never reads past its end") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    const Bytes full = build_of(test::mmd::sample_module(v));
    Layout lay;
    build_of(test::mmd::sample_module(v), &lay);
    std::size_t first_ok = full.size();
    const std::size_t step = kThin;
    // every offset (every 40th on the target), and every offset of the last 64 bytes: that is where a prefix becomes whole
    for (std::size_t len = 0; len <= full.size(); len += (len + 64 >= full.size() ? 1 : step)) {
      const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
      const Outcome o = run_file(cut);
      CHECK(o.violations == 0);
      if (o.open.ok() && o.validate.ok()) {
        first_ok = std::min(first_ok, len);
      }
      if (len < 4) {
        CHECK(o.open.error == Error::Truncated);
      } else if (len < 52) {
        CHECK(o.open.error == Error::Truncated);
      } else if (len < lay.song + 788) {
        CHECK(o.open.error == Error::BadPointer);
      }
    }
    // the whole file validates; a file that is cut short fails until the last structure the song reaches is whole
    const Outcome whole = run_file(full);
    CHECK(whole.open.ok());
    CHECK(whole.validate.ok());
    CHECK(whole.walked);
    CHECK(first_ok > lay.song + 788);
    CHECK(first_ok <= full.size());
  }
}

TEST_CASE("med hostile: a file cut at every single offset (not thinned) for MMD1") {
  if (kThin != 1) return;  // the target run is thinned above
  const Bytes full = build_of(test::mmd::sample_module(1));
  for (std::size_t len = 0; len < full.size(); ++len) {
    const Bytes cut(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(len));
    const Outcome o = run_file(cut);
    CHECK(o.violations == 0);
  }
}

TEST_CASE("med hostile: the id") {
  Bytes b = build_of(test::mmd::sample_module(1));
  auto with_id = [&](const char* id) {
    Bytes c = b;
    std::memcpy(c.data(), id, 4);
    return validate_error(c);
  };
  CHECK(with_id("MMD1") == Error::None);
  CHECK(with_id("MMDC") == Error::Unsupported);
  CHECK(with_id("MMD4") == Error::Unsupported);
  CHECK(with_id("MMD9") == Error::Unsupported);
  CHECK(with_id("MMD:") == Error::BadMagic);
  CHECK(with_id("MMDx") == Error::BadMagic);
  CHECK(with_id("MCN1") == Error::BadMagic);
  CHECK(with_id("XMMD") == Error::BadMagic);
  CHECK(with_id("\0\0\0\0") == Error::BadMagic);
  CHECK(validate_error(Bytes()) == Error::Truncated);
  CHECK(validate_error(Bytes{'M', 'M', 'D'}) == Error::Truncated);
}

TEST_CASE("med hostile: pointers at 0xFFFFFFFF, inside the header, and just past the end") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    const Bytes base = build_of(test::mmd::sample_module(v));
    for (std::size_t field : {std::size_t(8), std::size_t(16), std::size_t(24), std::size_t(32)}) {
      CAPTURE(field);
      const std::uint32_t size = static_cast<std::uint32_t>(base.size());
      const std::uint32_t values[] = {0xFFFFFFFFu, 4u, 51u, size, size - 1u};  // (uint32_t is unsigned long on the target)
      for (std::uint32_t value : values) {
        CAPTURE(value);
        if (field == 32 && value == size - 1) continue;  // one byte of expansion data: the rest reads as zeros, valid
        Bytes b = base;
        put32(b, field, value);
        const Error e = validate_error(b);
        // every one of these is a pointer that cannot hold its structure: refused, never followed
        CHECK(e != Error::None);
        CHECK((e == Error::BadPointer || e == Error::Truncated || e == Error::Unsupported));
      }
    }
  }
  // zero is allowed where a structure is optional: no expansion data
  Bytes b = build_of(test::mmd::sample_module(1));
  put32(b, 32, 0);
  CHECK(validate_error(b) == Error::None);
  // and where there is nothing to point to: the block and instrument tables of a song without them
  ModuleSpec m = test::mmd::sample_module(1);
  m.blocks.clear();
  m.playseq.clear();
  m.instruments.clear();
  m.exp = false;
  m.s_ext_entrsz = m.i_ext_entrsz = 0;
  m.songname.clear();
  m.annotation.clear();
  CHECK(validate_error(build_of(m)) == Error::None);
  // a table promised by the song record but absent
  Layout lay;
  Bytes c = build_of(test::mmd::sample_module(1), &lay);
  put32(c, 16, 0);
  CHECK(validate_error(c) == Error::BadPointer);
  put32(c, 16, lay.blockarr);
  put32(c, 24, 0);
  CHECK(validate_error(c) == Error::Unsupported);  // the instruments are stored outside the file
}

TEST_CASE("med hostile: the counts of the song record") {
  Layout lay;
  struct Case {
    unsigned version;
    std::size_t at;       // offset inside the song record
    unsigned value;
    Error want;
  };
  const Case cases[] = {
      {0, 504, 256, Error::LimitExceeded}, {1, 504, 256, Error::LimitExceeded},
      {0, 506, 256, Error::None},          {0, 506, 257, Error::LimitExceeded},   {1, 506, 0xFFFF, Error::LimitExceeded},
      {0, 787, 64, Error::LimitExceeded},  {1, 787, 255, Error::LimitExceeded},
      {2, 504, 0x7FFF, Error::BadPointer}, {2, 504, 0x8000, Error::LimitExceeded}, {2, 506, 257, Error::LimitExceeded},
      {2, 522, 257, Error::LimitExceeded}, {2, 520, 65, Error::LimitExceeded},    {3, 520, 0xFFFF, Error::LimitExceeded},
  };
  for (const Case& c : cases) {
    CAPTURE(c.version);
    CAPTURE(c.at);
    CAPTURE(c.value);
    Bytes b = build_of(test::mmd::sample_module(c.version), &lay);
    if (c.at == 787) {
      b[lay.song + 787] = static_cast<std::uint8_t>(c.value);
    } else {
      put16(b, lay.song + c.at, c.value);
    }
    CHECK(validate_error(b) == c.want);
  }
}

TEST_CASE("med hostile: block headers claiming more than the file holds") {
  Layout lay;
  const Bytes base = build_of(test::mmd::sample_module(1), &lay);
  const std::size_t blk = lay.block[0];
  auto with = [&](unsigned tracks, unsigned lines) {
    Bytes b = base;
    put16(b, blk, tracks);
    put16(b, blk + 2, lines);
    return validate_error(b);
  };
  CHECK(with(0xFFFF, 0xFFFF) == Error::LimitExceeded);
  CHECK(with(64, 0xFFFF) == Error::LimitExceeded);
  CHECK(with(65, 0) == Error::LimitExceeded);
  CHECK(with(0, 7) == Error::Malformed);
  CHECK(with(64, 9998) == Error::Truncated);  // 9,999 lines of 64 tracks: 2.5 MB the file does not have
  CHECK(with(4, 9999) == Error::LimitExceeded);
  CHECK(with(4, 9998) == Error::Truncated);
  CHECK(with(4, 7) == Error::None);
  // MMD0: tracks is a byte
  Layout l0;
  const Bytes b0 = build_of(test::mmd::sample_module(0), &l0);
  Bytes c = b0;
  c[l0.block[0]] = 0xFF;
  CHECK(validate_error(c) == Error::LimitExceeded);
  c[l0.block[0]] = 0;
  CHECK(validate_error(c) == Error::Malformed);
  c[l0.block[0]] = 4;
  c[l0.block[0] + 1] = 0xFF;  // 256 lines of 4 tracks: runs past the end of this small file or into the next structures
  const Error e = validate_error(c);
  CHECK((e == Error::None || e == Error::Truncated));
  // a block pointer in the header or past the end
  Bytes d = base;
  put32(d, lay.blockarr, 10);
  CHECK(validate_error(d) == Error::BadPointer);
  put32(d, lay.blockarr, 0xFFFFFFFF);
  CHECK(validate_error(d) == Error::BadPointer);
}

TEST_CASE("med hostile: BlockInfo, command pages and the command-extension table") {
  Layout lay;
  const Bytes base = build_of(test::mmd::sample_module(1), &lay);
  const std::size_t info = lay.block_info[0];
  const std::size_t pt = lay.pagetable[0];
  NEED(info != 0);
  NEED(pt != 0);
  auto patched = [&](std::size_t at, std::uint32_t v32) {
    Bytes b = base;
    put32(b, at, v32);
    return validate_error(b);
  };
  CHECK(patched(lay.block[0] + 4, 0xFFFFFFFF) == Error::BadPointer);   // BlockInfo
  CHECK(patched(lay.block[0] + 4, 10) == Error::BadPointer);
  CHECK(patched(info + 4, 0xFFFFFFFF) == Error::BadPointer);           // name
  CHECK(patched(info + 12, 0xFFFFFFFF) == Error::BadPointer);          // page table
  CHECK(patched(info + 12, 20) == Error::BadPointer);
  CHECK(patched(info + 16, 0xFFFFFFFF) == Error::BadPointer);          // extension pointer
  Bytes b = base;
  put16(b, pt, 17);
  CHECK(validate_error(b) == Error::LimitExceeded);
  put16(b, pt, 0xFFFF);
  CHECK(validate_error(b) == Error::LimitExceeded);
  put16(b, pt, 16);  // sixteen page pointers: the table runs into other data, the pointers are garbage: no crash
  (void)run_file(b);
  b = base;
  put32(b, pt + 4, 0xFFFFFFFF);  // a page pointer past the end
  CHECK(validate_error(b) == Error::Truncated);
  put32(b, pt + 4, 5);
  CHECK(validate_error(b) == Error::BadPointer);
  put32(b, pt + 4, 0);  // a page of empty cells is allowed
  CHECK(validate_error(b) == Error::None);
  // the extension table: pointer to a pointer
  b = base;
  std::uint32_t ptr = 0;
  for (int i = 0; i < 4; ++i) ptr = (ptr << 8) | b[info + 16 + i];
  put32(b, ptr, 0xFFFFFFFF);
  CHECK(validate_error(b) == Error::Truncated);
  put32(b, ptr, 3);
  CHECK(validate_error(b) == Error::BadPointer);
  put32(b, ptr, 0);  // no table
  CHECK(validate_error(b) == Error::None);
  // a name that claims to be huge is read only as far as our limit
  b = base;
  put32(b, info + 8, 0xFFFFFFFF);
  CHECK(validate_error(b) == Error::None);
}

TEST_CASE("med hostile: InstrExt and instrument-name strides 0 to 3, and entry counts that overflow") {
  Layout lay;
  const Bytes base = build_of(test::mmd::sample_module(3), &lay);
  const std::size_t exp = lay.expdata;
  for (unsigned stride = 0; stride <= 3; ++stride) {
    CAPTURE(stride);
    Bytes b = base;
    put16(b, exp + 10, stride);
    put16(b, exp + 26, stride);
    const Outcome o = run_file(b);
    CHECK(o.open.ok());
    CHECK(o.validate.ok());
    CHECK(o.walked);
  }
  Bytes b = base;
  put16(b, exp + 8, 0xFFFF);
  put16(b, exp + 10, 0xFFFF);
  CHECK(validate_error(b) == Error::BadPointer);   // 63 entries of 65,535 bytes
  b = base;
  put16(b, exp + 24, 0xFFFF);
  put16(b, exp + 26, 0xFFFF);
  CHECK(validate_error(b) == Error::BadPointer);
  b = base;
  put16(b, exp + 8, 0xFFFF);  // many entries, the stride is the real one: only the first 63 count
  put32(b, exp + 4, 0xFFFFFFFF);
  CHECK(validate_error(b) == Error::BadPointer);
  b = base;
  put32(b, exp + 4, 0);       // no pointer: no array
  CHECK(validate_error(b) == Error::None);
  b = base;
  put32(b, exp + 44, 0xFFFFFFFF);  // the song name
  CHECK(validate_error(b) == Error::BadPointer);
  // expansion data that is cut short by the end of the file reads as zeros past the cut
  ModuleSpec m = test::mmd::sample_module(1);
  m.songname.clear();
  m.annotation.clear();
  m.s_ext_entrsz = m.i_ext_entrsz = 0;
  m.exp = true;
  Layout l2;
  Bytes c = build_of(m, &l2);
  c.resize(l2.expdata + 20);
  const Outcome o = run_file(c);
  CHECK(o.open.ok());
}

TEST_CASE("med hostile: instruments with lengths that do not fit") {
  Layout lay;
  const Bytes base = build_of(test::mmd::sample_module(1), &lay);
  auto len_of = [&](unsigned slot, std::uint32_t v) {
    Bytes b = base;
    put32(b, lay.instr[slot], v);
    return validate_error(b);
  };
  CHECK(len_of(0, 0xFFFFFFFF) == Error::Truncated);
  CHECK(len_of(0, 0x7FFFFFFF) == Error::Truncated);
  CHECK(len_of(1, 0x7FFFFFFF) == Error::Truncated);   // stereo needs twice that
  CHECK(len_of(1, 0x40000000) == Error::Truncated);
  CHECK(len_of(0, 0) == Error::None);                 // an empty sample is allowed
  CHECK(len_of(0, 1) == Error::None);
  // a pointer in the table
  Bytes b = base;
  put32(b, lay.smplarr, 0xFFFFFFFF);
  CHECK(validate_error(b) == Error::BadPointer);
  put32(b, lay.smplarr, 51);
  CHECK(validate_error(b) == Error::BadPointer);
  put32(b, lay.smplarr, static_cast<std::uint32_t>(base.size()) - 3);
  CHECK(validate_error(b) == Error::BadPointer);
  // a type nobody described is reported, not refused
  b = base;
  put16(b, lay.instr[0] + 4, 0x7FFF);
  const Outcome o = run_file(b);
  CHECK(o.validate.ok());
}

TEST_CASE("med hostile: an octave split of a sample shorter than its octaves") {
  for (int type : {1, 2, 3, 4, 5, 6}) {
    CAPTURE(type);
    const unsigned parts[] = {0, 31, 7, 3, 15, 63, 127};
    ModuleSpec m = test::mmd::sample_module(1);
    m.instruments[2].type = type;
    m.instruments[2].data.assign(parts[type] - 1, 0x33);
    CHECK(validate_error(build_of(m)) == Error::Malformed);
    m.instruments[2].data.assign(parts[type], 0x33);
    CHECK(validate_error(build_of(m)) == Error::None);
    m.instruments[2].data.assign(parts[type] * 3 + 2, 0x33);  // two stray bytes: ignored
    Bytes b = build_of(m);
    StrictSource src(b);
    Reader r;
    NEED(r.open(src).ok());
    InstrumentInfo i;
    NEED(r.instrument(2, i).ok());
    CHECK(i.base_frames == 3);
    CHECK((i.notes & kNoteTrimmed) != 0);
    m.instruments[2].data.clear();                             // empty: allowed
    CHECK(validate_error(build_of(m)) == Error::None);
  }
}

TEST_CASE("med hostile: the block and entry caps of MMD0/1 at their exact values") {
  ModuleSpec m;
  m.version = 0;
  m.blocks.assign(255, test::mmd::BlockSpec());
  for (test::mmd::BlockSpec& b : m.blocks) {
    b.tracks = 1;
    b.lines = 1;
  }
  m.playseq.assign(256, 0);
  for (unsigned i = 0; i < 256; ++i) m.playseq[i] = static_cast<std::uint8_t>(i % 255);
  const Outcome o = run_file(build_of(m));
  CHECK(o.open.ok());
  CHECK(o.validate.ok());
  CHECK(o.walked);
  m.blocks.push_back(m.blocks[0]);  // 256 blocks
  CHECK(validate_error(build_of(m)) == Error::LimitExceeded);
}

TEST_CASE("med hostile: synth instruments with tables, waveform counts and pointers that lie") {
  Layout lay;
  const Bytes base = build_of(test::mmd::sample_module(1), &lay);
  const std::size_t synth = lay.instr[3];
  const std::size_t hybrid = lay.instr[4];
  NEED(synth != 0);
  auto at = [&](std::size_t off, unsigned v16) {
    Bytes b = base;
    put16(b, off, v16);
    return validate_error(b);
  };
  CHECK(at(synth + 20, 0xFFFF) == Error::None);          // no waveforms
  CHECK(at(synth + 20, 0) == Error::None);
  CHECK(at(synth + 20, 64) == Error::None);              // waveforms 2..63 have no pointer: absent
  CHECK(at(synth + 20, 65) == Error::LimitExceeded);
  CHECK(at(synth + 20, 0xFFFE) == Error::LimitExceeded);
  CHECK(at(synth + 14, 128) == Error::None);
  CHECK(at(synth + 14, 129) == Error::LimitExceeded);
  CHECK(at(synth + 16, 0xFFFF) == Error::LimitExceeded);
  auto wf = [&](std::size_t inst, unsigned w, std::uint32_t v) {
    Bytes b = base;
    put32(b, inst + 278 + 4 * w, v);
    return validate_error(b);
  };
  CHECK(wf(synth, 0, 0xFFFFFFFF) == Error::BadPointer);
  CHECK(wf(synth, 0, 10) == Error::BadPointer);           // inside the SynthInstr struct
  CHECK(wf(synth, 0, 533) == Error::BadPointer);
  CHECK(wf(synth, 0, 0) == Error::None);
  CHECK(wf(hybrid, 0, 0xFFFFFFFF) == Error::BadPointer);
  // a waveform that claims 0xFFFF words
  Bytes b = base;
  std::uint32_t off = 0;
  for (int i = 0; i < 4; ++i) off = (off << 8) | b[synth + 278 + i];
  put16(b, synth + off, 0xFFFF);
  CHECK(validate_error(b) == Error::Truncated);
  // a hybrid sample that claims 0xFFFFFFFF bytes
  b = base;
  off = 0;
  for (int i = 0; i < 4; ++i) off = (off << 8) | b[hybrid + 278 + i];
  put32(b, hybrid + off, 0xFFFFFFFF);
  CHECK(validate_error(b) == Error::Truncated);
  // a synth instrument cut off inside its struct
  b = base;
  b.resize(synth + 300);
  CHECK(validate_error(b) != Error::None);
}

TEST_CASE("med hostile: play sequences, sections and their command tables") {
  // an entry past the block table: refused by nobody, counted
  ModuleSpec m = test::mmd::sample_module(1);
  m.playseq = {0, 9, 255, 1};
  const Outcome o = run_file(build_of(m));
  CHECK(o.validate.ok());
  const Bytes mb = build_of(m);
  StrictSource src(mb);
  Reader r;
  NEED(r.open(src).ok());
  Summary s;
  NEED(r.validate(s).ok());
  CHECK(s.bad_play_entries == 2);

  Layout lay;
  const Bytes base = build_of(test::mmd::sample_module(2), &lay);
  auto patched16 = [&](std::size_t at, unsigned v) {
    Bytes b = base;
    put16(b, at, v);
    return validate_error(b);
  };
  auto patched32 = [&](std::size_t at, std::uint32_t v) {
    Bytes b = base;
    put32(b, at, v);
    return validate_error(b);
  };
  CHECK(patched32(lay.song + 508, 0xFFFFFFFF) == Error::BadPointer);   // play sequence table
  CHECK(patched32(lay.song + 512, 0xFFFFFFFF) == Error::BadPointer);   // section table
  CHECK(patched32(lay.song + 516, 0xFFFFFFFF) == Error::BadPointer);   // track volumes
  CHECK(patched32(lay.song + 524, 0xFFFFFFFF) == Error::BadPointer);   // track pans
  CHECK(patched32(lay.song + 508, 0) == Error::BadPointer);
  CHECK(patched32(lay.song + 512, 0) == Error::BadPointer);
  CHECK(patched32(lay.playseqtable, 0xFFFFFFFF) == Error::BadPointer);  // a play sequence pointer
  CHECK(patched32(lay.playseqtable, 0) == Error::BadPointer);
  CHECK(patched16(lay.pseq[0] + 40, 0xFFFF) == Error::BadPointer);      // 65,535 entries
  CHECK(patched16(lay.sectiontable, 0xFFFF) == Error::None);            // a section naming no sequence
  CHECK(patched32(lay.pseq[1] + 32, 0xFFFFFFFF) == Error::BadPointer);  // the command table
  CHECK(patched32(lay.pseq[1] + 32, 4) == Error::BadPointer);

  // more than 4,096 entries in all: 256 sections of 17 entries
  ModuleSpec big = test::mmd::sample_module(2);
  big.pseqs.resize(1);
  big.pseqs[0].entries.assign(17, 0);
  big.pseqs[0].cmds.clear();
  big.sections.assign(256, 0);
  CHECK(validate_error(build_of(big)) == Error::LimitExceeded);
  big.pseqs[0].entries.assign(16, 0);
  CHECK(validate_error(build_of(big)) == Error::None);   // 4,096 exactly
  big.sections.push_back(0);                              // 257 sections
  CHECK(validate_error(build_of(big)) == Error::LimitExceeded);

  // a command table without its terminator, with entries that carry huge extras
  ModuleSpec t = test::mmd::sample_module(2);
  t.pseqs[1].cmds.clear();
  for (unsigned i = 0; i < 300; ++i) t.pseqs[1].cmds.push_back({static_cast<std::uint16_t>(i % 3), 0, std::vector<std::uint8_t>(5, 7)});
  Bytes tb = build_of(t);
  const Outcome to = run_file(tb);
  CHECK(to.validate.ok());
  // 0xFFFF as an offset ends the table; a table that runs into the end of the file ends there
  Layout tl;
  Bytes cut = build_of(test::mmd::sample_module(2), &tl);
  cut.resize(tl.pseq[1] + 50);
  CHECK(validate_error(cut) != Error::None);
}

TEST_CASE("med hostile: chained songs that cycle, shrink or never end") {
  ModuleSpec m = test::mmd::sample_module(1);
  m.extra_songs = 2;
  Layout lay;
  const Bytes base = build_of(m, &lay);
  CHECK(validate_error(base) == Error::None);
  NEED(lay.extra_song.size() == 2);
  // nextmod of the first song pointing at itself, at the file start, at an earlier header
  auto exp_of_song = [&](unsigned k) {  // the expansion record of chained song k
    std::uint32_t e = 0;
    for (int i = 0; i < 4; ++i) e = (e << 8) | base[lay.extra_song[k] + 32 + i];
    return e;
  };
  Bytes b = base;
  put32(b, lay.expdata, lay.expdata);
  CHECK(validate_error(b) == Error::BadMagic);        // an expansion record is not a song header
  put32(b, lay.expdata, 0);
  CHECK(validate_error(b) == Error::None);
  b = base;
  put32(b, exp_of_song(0), lay.extra_song[0]);          // song 1 points back at itself
  CHECK(validate_error(b) == Error::BadPointer);
  b = base;
  put32(b, exp_of_song(1), lay.extra_song[0]);          // song 2 points back at song 1
  CHECK(validate_error(b) == Error::BadPointer);
  b = base;
  put32(b, exp_of_song(1), 0xFFFFFFFF);
  CHECK(validate_error(b) == Error::BadPointer);
  b = base;
  b[lay.extra_song[0]] = 'X';                           // a chained header with no id
  CHECK(validate_error(b) == Error::BadMagic);
  // 256 songs are accepted, 257 are not
  m.extra_songs = 255;
  CHECK(validate_error(build_of(m)) == Error::None);
  m.extra_songs = 256;
  CHECK(validate_error(build_of(m)) == Error::LimitExceeded);
}

TEST_CASE("med hostile: a source that fails ends the read, wherever it fails") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    const Bytes full = build_of(test::mmd::sample_module(v));
    unsigned failed = 0;
    for (std::uint32_t from = 0; from < full.size(); from += 7 * kThin) {
      const Outcome o = run_file(full, from);
      CHECK(o.violations == 0);
      if (!o.open.ok() || !o.validate.ok()) {
        const Error e = o.open.ok() ? o.validate.error : o.open.error;
        CHECK((e == Error::SourceFailed || e == Error::Truncated));
        ++failed;
      }
    }
    CHECK(failed > 2);
  }
}

namespace {

// One to four mutations of a copy of `base`: bit flips, bytes set to 0 / 0xFF / random, truncation, appended junk, a run
// set to one value, a slice moved, a 16-bit or 32-bit field set to an interesting value (a count or a pointer, found at
// random offsets, in the header and the song record more often than not).
Bytes mutate(const Bytes& base, o3os::test::Rng& rng) {
  Bytes b = base;
  const unsigned n = 1 + rng.below(4);
  const std::uint32_t hot = static_cast<std::uint32_t>(std::min<std::size_t>(b.size(), 52 + 788 + 64));
  for (unsigned i = 0; i < n && !b.empty(); ++i) {
    const std::uint32_t any = static_cast<std::uint32_t>(b.size());
    const std::uint32_t at = rng.below(3) != 0 ? rng.below(hot) : rng.below(any);
    switch (rng.below(11)) {
      case 0: b[at] ^= static_cast<std::uint8_t>(1u << rng.below(8)); break;
      case 1: b[at] = 0; break;
      case 2: b[at] = 0xFF; break;
      case 3: b.resize(rng.below(any) + 1); break;
      case 4: for (unsigned k = rng.below(40); k > 0; --k) b.push_back(rng.byte()); break;
      case 5: {
        const std::uint8_t v = rng.byte();
        for (std::uint32_t k = at; k < any && k < at + 1 + rng.below(8); ++k) b[k] = v;
        break;
      }
      case 6:
        if (any > 8) {
          const std::uint32_t len = 1 + rng.below(std::min<std::uint32_t>(300, any / 2));
          const std::uint32_t from = rng.below(any - len);
          const std::uint32_t to = rng.below(any - len);
          std::memmove(&b[to], &b[from], len);
        }
        break;
      case 7:
        if (at + 4 <= any) {
          static const std::uint32_t vals[] = {0, 1, 51, 52, 0x7FFF, 0xFFFF, 0xFFFFFFFFu, 0x10000, 0x80000000u, 0xFFFFFFFEu};
          const std::uint32_t v = vals[rng.below(10)];
          put32(b, at, v);
        }
        break;
      case 8:
        if (at + 2 <= any) {
          static const unsigned vals[] = {0, 1, 2, 17, 63, 64, 65, 255, 256, 9999, 10000, 0x8000, 0xFFFF};
          put16(b, at, vals[rng.below(13)]);
        }
        break;
      case 9:
        if (at + 4 <= any) put32(b, at, static_cast<std::uint32_t>(rng.below(any + 8)));  // a pointer into or just past the file
        break;
      default: b[at] = rng.byte(); break;
    }
  }
  return b;
}

struct Tally {
  unsigned runs = 0, valid = 0, refused = 0, flowed = 0, limit = 0;
  unsigned by_error[8] = {};
};

void fuzz_one(const Bytes& input, Tally& t) {
  ++t.runs;
  const Outcome o = run_file(input, 0xFFFFFFFFu, 2000);
  CHECK(o.violations == 0);
  if (o.open.ok() && o.validate.ok()) {
    ++t.valid;
    if (o.flow.ok()) ++t.flowed;
    else ++t.limit;
  } else {
    ++t.refused;
    ++t.by_error[static_cast<unsigned>(o.open.ok() ? o.validate.error : o.open.error) & 7u];
  }
}

}  // namespace

TEST_CASE("med hostile: every byte of the header and the song record set to a few values") {
  const std::size_t stride = kThin;  // the target run is thinned
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    const Bytes base = build_of(test::mmd::sample_module(v));
    Tally t;
    for (std::size_t at = 0; at < 52 + 788; at += stride) {
      for (unsigned value : {0x00u, 0x01u, 0x7Fu, 0x80u, 0xFFu}) {
        Bytes b = base;
        b[at] = static_cast<std::uint8_t>(value);
        fuzz_one(b, t);
      }
    }
    MESSAGE("med sweep v" << v << ": runs " << t.runs << ", valid " << t.valid << ", refused " << t.refused);
    CHECK(t.valid > t.runs / 4);
    CHECK(t.refused > 0);
  }
}

TEST_CASE("med hostile: mutated files of every version and every feature") {
  std::vector<Bytes> corpus;
  for (unsigned v = 0; v <= 3; ++v) {
    corpus.push_back(build_of(test::mmd::sample_module(v)));
  }
  ModuleSpec chained = test::mmd::sample_module(2);
  chained.extra_songs = 3;
  corpus.push_back(build_of(chained));
  ModuleSpec flow = test::mmd::sample_module(1);
  flow.blocks[0].set(3, 1, 0, 0, 0x16, 0x03).set(5, 0, 0, 0, 0x1E, 0x02).set(1, 0, 0, 0, 0x0B, 0x01);
  flow.blocks[1].set(2, 0, 0, 0, 0x09, 0x04).set(1, 2, 0, 0, 0x0F, 0x60);
  corpus.push_back(build_of(flow));
  ModuleSpec empty = test::mmd::sample_module(1);
  empty.blocks[2].empty = true;
  corpus.push_back(build_of(empty));

  o3os::test::Rng rng(0x3ED1u);
  Tally t;
  for (std::uint32_t i = 0; i < runs(4000); ++i) {
    fuzz_one(mutate(corpus[rng.below(static_cast<std::uint32_t>(corpus.size()))], rng), t);
  }
  MESSAGE("med fuzz: runs " << t.runs << ", valid " << t.valid << " (linearised " << t.flowed << ", over a limit " << t.limit
          << "), refused " << t.refused << ": truncated " << t.by_error[1] << ", bad magic " << t.by_error[2] << ", bad pointer "
          << t.by_error[3] << ", limit " << t.by_error[4] << ", malformed " << t.by_error[5] << ", unsupported " << t.by_error[6]
          << ", source " << t.by_error[7]);
  CHECK(t.valid > t.runs / 20);
  CHECK(t.refused > t.runs / 20);
  CHECK(t.flowed > 0);
  // the mutations reach every typed error
  CHECK(t.by_error[static_cast<unsigned>(Error::Truncated)] > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::BadPointer)] > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::LimitExceeded)] > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::Malformed)] > 0);
  CHECK(t.by_error[static_cast<unsigned>(Error::BadMagic)] > 0);
}

TEST_CASE("med hostile: random bytes behind a valid id") {
  o3os::test::Rng rng(0xBADu);
  Tally t;
  for (std::uint32_t i = 0; i < runs(1500); ++i) {
    Bytes b(52 + rng.below(1500));
    for (std::uint8_t& x : b) x = rng.byte();
    std::memcpy(b.data(), "MMD", 3);
    b[3] = static_cast<std::uint8_t>('0' + rng.below(4));
    if (rng.below(2) != 0) put32(b, 8, 52);  // a song record where it usually is
    fuzz_one(b, t);
  }
  MESSAGE("med random: runs " << t.runs << ", valid " << t.valid << ", refused " << t.refused);
  CHECK(t.refused > t.runs / 2);
}
