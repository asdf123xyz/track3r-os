// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/need.h"
#include "support/og_fixture.h"

#include <o3os/formats/og.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace o3os;
using namespace o3os::test::og;
using formats::FormatError;
using formats::Progress;
using o3os::test::MemorySource;
using o3os::test::VectorSink;

namespace {

model::Project& fresh(int i = 0) {
  model::Project& p = arena(i).project;
  p.reset();
  return p;
}

formats::og::InstrumentReader& mti_reader() {
  static formats::og::InstrumentReader r(false);
  return r;
}
formats::og::InstrumentReader& pti_reader() {
  static formats::og::InstrumentReader r(true);
  return r;
}

Progress import_instrument(const Bytes& file, bool pti, unsigned slot, formats::og::ImportReport* rep = nullptr,
                           std::uint32_t size_hint = 0, std::size_t budget = 4096) {
  formats::ReadParams params;
  params.index = static_cast<std::uint16_t>(slot);
  params.size_hint = size_hint;
  formats::og::InstrumentReader& r = pti ? pti_reader() : mti_reader();
  const Progress p = run_reader(r, file, arena(0), params, budget);
  if (rep != nullptr) *rep = r.import_report();
  return p;
}

// The values instrument_head() puts in every field the model holds, as the model should see them after a read
// of structure version `v`.
void check_fields(const model::Instrument& in, unsigned v, const char* name = "Kick 808.wav", unsigned slot = 4) {
  CHECK(in.kind == model::InstrumentKind::Sample);
  CHECK(in.midi_channel == 0);
  CHECK(in.module_index == model::kNoModule);
  CHECK(in.param_len == 0);
  CHECK(std::string(in.name) == name);
  char path[32];
  std::snprintf(path, sizeof path, "samples/instr%02u.wav", slot + 1);
  CHECK(std::string(in.sample_path) == path);
  CHECK(model::is_safe_relative_path(in.sample_path, std::strlen(in.sample_path)));
  const model::SamplerParams& s = in.sampler;
  CHECK(s.sample_type == 1);
  CHECK(s.wavetable_window_size == 1024);
  CHECK(s.play_mode == (v == 1 ? 6 : 5));
  CHECK(s.start == 111);
  CHECK(s.loop_start == 222);
  CHECK(s.loop_end == 333);
  CHECK(s.end == 444);
  CHECK(s.wavetable_window == 9);
  for (unsigned i = 0; i < 6; ++i) {
    const model::Envelope& e = s.envelope[i];
    if (i == 5 && v <= 2) {  // the fine-tune envelope did not exist yet: the defaults of 1.1
      CHECK(e.amount == 1.0f);
      CHECK(e.delay == 0);
      CHECK(e.attack == 3000);
      CHECK(e.hold == 0);
      CHECK(e.decay == 0);
      CHECK(e.sustain == 1.0f);
      CHECK(e.release == 1000);
      CHECK(e.loop == 0);
      CHECK(e.enable == 0);
      continue;
    }
    CHECK(e.amount == 0.1f * static_cast<float>(i + 1));
    CHECK(e.delay == 10 + i);
    CHECK(e.attack == 200 + i);
    CHECK(e.hold == 30 + i);
    CHECK(e.decay == 400 + i);
    CHECK(e.sustain == 0.05f * static_cast<float>(i + 2));
    CHECK(e.release == 500 + i);
    CHECK(e.loop == (i & 1));
    CHECK(e.enable == ((i + 1) & 1));
  }
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(s.lfo[i].shape == i % 5);
    CHECK(s.lfo[i].speed == 3 + i);
    CHECK(s.lfo[i].amount == 0.25f + 0.1f * static_cast<float>(i));
  }
  CHECK(s.cutoff == 0.37f);
  CHECK(s.resonance == 0.62f);
  CHECK(s.filter_type == 2);
  CHECK(s.filter_enable == 1);
  CHECK(s.tune == -5);
  CHECK(s.fine_tune == 17);
  CHECK(s.volume == 91);
  CHECK(s.glide == 123);
  CHECK(s.panning == 31);
  CHECK(s.delay_send == 44);
  for (unsigned i = 0; i < 48; ++i) CHECK(s.slices[i] == 1000 + 1000 * i);
  CHECK(s.slice_count == 37);
  CHECK(s.selected_slice == 12);
  CHECK(s.grain_length == 4410);
  CHECK(s.grain_position == 7777);
  CHECK(s.grain_shape == 2);
  CHECK(s.grain_type == 1);
  CHECK(s.reverb_send == (v <= 3 ? 0 : 23));
  CHECK(s.overdrive == (v <= 5 ? 0 : 65));
  CHECK(s.bit_depth == (v < 8 ? 16 : 9));
}

}  // namespace

TEST_CASE("og instrument: a .pti v9 maps every field") {
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_instrument(instrument_bytes({}), true, 4, &rep) == Progress::Done);
  CHECK(rep.version == 9);
  CHECK(rep.pcm_frames == 1000);
  CHECK(rep.instruments == 1);
  CHECK(rep.sanitised == 0);
  CHECK(!rep.unverified_version);
  CHECK(!rep.crc_mismatch);
  const model::Instrument* in = proj.instrument(4);
  NEED(in != nullptr);
  check_fields(*in, 9);
  CHECK(proj.check_invariants());
  CHECK(proj.instrument_count() == 1);
}

TEST_CASE("og instrument: .mti of every version from 1 to 9") {
  for (unsigned v = 1; v <= 9; ++v) {
    InstrumentSpec spec;
    spec.version = v;
    spec.pti = false;
    spec.canonical = false;  // junk in the padding, as real files have
    formats::og::ImportReport rep;
    model::Project& proj = fresh();
    const Bytes f = instrument_bytes(spec);
    CHECK(f.size() == (v <= 3 ? 388u : 392u));
    NEED(import_instrument(f, false, 4, &rep) == Progress::Done);
    CHECK_MESSAGE(rep.version == v, "version " << v);
    CHECK(rep.instruments == 1);
    CHECK(rep.sanitised == 0);
    const model::Instrument* in = proj.instrument(4);
    NEED(in != nullptr);
    check_fields(*in, v);
  }
}

TEST_CASE("og instrument: .pti of versions 4 to 9, with and without a size hint") {
  for (unsigned v = 4; v <= 9; ++v) {
    InstrumentSpec spec;
    spec.version = v;
    spec.frames = 77;
    spec.canonical = false;
    formats::og::ImportReport rep;
    model::Project& proj = fresh();
    NEED(import_instrument(instrument_bytes(spec), true, 4, &rep) == Progress::Done);
    CHECK(rep.pcm_frames == 77);
    check_fields(*proj.instrument(4), v);
    // with the file size known the PCM is not read at all: a source that ends at the head is enough
    fresh();
    const Bytes whole = instrument_bytes(spec);
    const Bytes head(whole.begin(), whole.begin() + 392);
    NEED(import_instrument(head, true, 4, &rep, static_cast<std::uint32_t>(whole.size())) == Progress::Done);
    CHECK(rep.pcm_frames == 77);
    check_fields(*proj.instrument(4), v);
  }
}

TEST_CASE("og instrument: a .pti of versions 1 to 3 does not exist") {
  InstrumentSpec spec;
  spec.version = 3;
  spec.pti = false;
  formats::og::ImportReport rep;
  fresh();
  CHECK(import_instrument(instrument_bytes(spec), true, 0, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::UnsupportedVersion);
}

TEST_CASE("og instrument: version 0 is refused, 10 and up read as unverified") {
  InstrumentSpec spec;
  spec.pti = false;
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  Bytes f = instrument_bytes(spec);
  for (unsigned i = 8; i < 12; ++i) f[i] = 0;
  seal(f);
  p32(f, 388, formats::crc32(f.data(), 388));
  CHECK(import_instrument(f, false, 4, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::UnsupportedVersion);
  for (unsigned v : {10u, 11u, 99u, 255u}) {
    spec.version = v;
    Bytes g = instrument_bytes(spec);
    fresh();
    CHECK(import_instrument(g, false, 4, &rep) == Progress::Done);
    CHECK_MESSAGE(rep.unverified_version, "version " << v);
    NEED(proj.instrument(4) != nullptr);
    CHECK(proj.instrument(4)->sampler.bit_depth == 9);  // read as the newest known layout
    g[200] ^= 1;                                          // nothing vouches for it but the CRC: that must match
    fresh();
    CHECK(import_instrument(g, false, 4, &rep) == Progress::Failed);
    CHECK(rep.error == FormatError::BadCrc);
    CHECK(proj.instrument_count() == 0);
  }
}

TEST_CASE("og instrument: values the model cannot hold are replaced and counted, nothing else changes") {
  Bytes f = instrument_head({});
  f[20] = 4;                   // sample type
  f[76] = 9;                   // play mode
  f[268] = 9;                  // filter type
  f[272] = 200;                // volume -> 50
  p16(f, 276, 500);            // panning -> 50
  f[278] = 150;                // delay send -> 0
  f[376] = 60;                 // slice count -> 48
  f[377] = 99;                 // selected slice -> 0
  f[382] = 5; f[383] = 7;      // granular shape and type -> 0
  f[384] = 120;                // reverb send -> 0
  f[385] = 101;                // overdrive -> 0
  f[386] = 3;                  // bit depth -> 16
  pf(f, 260, std::numeric_limits<float>::quiet_NaN());     // cutoff -> 1.0
  pf(f, 264, std::numeric_limits<float>::infinity());      // resonance -> 0.0
  pf(f, 92, -std::numeric_limits<float>::infinity());      // envelope 1 amount -> 1.0
  pf(f, 92 + 12 + 40, std::numeric_limits<float>::quiet_NaN());  // envelope 3 sustain -> 1.0
  pf(f, 212 + 8 * 2 + 4, std::numeric_limits<float>::quiet_NaN());  // LFO 3 amount -> 0.5
  f[270] = static_cast<std::uint8_t>(-100);   // a tune far outside -24..24 is kept: sanitised where used
  p32(f, 388, formats::crc32(f.data(), 388));
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  NEED(import_instrument(f, false, 4, &rep) == Progress::Done);
  CHECK(rep.sanitised == 18);
  const model::Instrument* in = proj.instrument(4);
  NEED(in != nullptr);
  const model::SamplerParams& s = in->sampler;
  CHECK(s.sample_type == 0);
  CHECK(s.play_mode == 0);
  CHECK(s.filter_type == 0);
  CHECK(s.volume == 50);
  CHECK(s.panning == 50);
  CHECK(s.delay_send == 0);
  CHECK(s.slice_count == 48);
  CHECK(s.selected_slice == 0);
  CHECK(s.grain_shape == 0);
  CHECK(s.grain_type == 0);
  CHECK(s.reverb_send == 0);
  CHECK(s.overdrive == 0);
  CHECK(s.bit_depth == 16);
  CHECK(s.cutoff == 1.0f);
  CHECK(s.resonance == 0.0f);
  CHECK(s.envelope[0].amount == 1.0f);
  CHECK(s.envelope[2].sustain == 1.0f);
  CHECK(s.lfo[2].amount == 0.5f);
  CHECK(s.tune == -100);
  CHECK(s.fine_tune == 17);          // the rest untouched
  CHECK(s.grain_length == 4410);
  CHECK(proj.check_invariants());
}

TEST_CASE("og instrument: a hostile name stays a name, the sample path is ours") {
  for (const char* evil : {"../../etc/passwd", "..\\..\\x", "/abs/path.wav", "C:\\x.wav", "a/../b"}) {
    InstrumentSpec spec;
    spec.pti = false;
    spec.file_name = evil;
    model::Project& proj = fresh();
    NEED(import_instrument(instrument_bytes(spec), false, 11) == Progress::Done);
    const model::Instrument* in = proj.instrument(11);
    NEED(in != nullptr);
    CHECK(std::string(in->name) == evil);
    CHECK(std::string(in->sample_path) == "samples/instr12.wav");
    CHECK(model::is_safe_relative_path(in->sample_path, std::strlen(in->sample_path)));
  }
  // a 32-character name has no terminator: 31 characters are kept
  InstrumentSpec spec;
  spec.pti = false;
  spec.file_name = std::string(32, 'n');
  model::Project& proj = fresh();
  NEED(import_instrument(instrument_bytes(spec), false, 0) == Progress::Done);
  CHECK(std::string(proj.instrument(0)->name) == std::string(31, 'n'));
}

TEST_CASE("og instrument: CRC policy, .mti enforced, .pti only warned about") {
  const Bytes mti = instrument_bytes([] { InstrumentSpec s; s.pti = false; return s; }());
  Bytes bad = mti;
  bad[300] ^= 0x40;
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  CHECK(import_instrument(bad, false, 2, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadCrc);
  CHECK(proj.instrument_count() == 0);
  Bytes pti = instrument_bytes({});
  pti[300] ^= 0x40;
  CHECK(import_instrument(pti, true, 2, &rep) == Progress::Done);
  CHECK(rep.crc_mismatch);
  CHECK(proj.instrument_count() == 1);
  // legacy .mti: the CRC is at 384
  InstrumentSpec old;
  old.version = 3;
  old.pti = false;
  Bytes legacy = instrument_bytes(old);
  legacy[100] ^= 1;
  fresh();
  CHECK(import_instrument(legacy, false, 2, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::BadCrc);
}

TEST_CASE("og instrument: an inactive .mti, a .pti without PCM and a stereo .pti are not instruments") {
  formats::og::ImportReport rep;
  model::Project& proj = fresh();
  Bytes off = instrument_head([] { InstrumentSpec s; s.pti = false; return s; }());
  off[16] = 0;
  p32(off, 388, formats::crc32(off.data(), 388));
  CHECK(import_instrument(off, false, 3, &rep) == Progress::Done);
  CHECK(rep.instruments == 0);
  CHECK(proj.instrument_count() == 0);

  const Bytes head = instrument_head({});  // a .pti with no PCM after the head
  CHECK(import_instrument(head, true, 3, &rep) == Progress::Done);
  CHECK(rep.instruments == 0);
  CHECK(rep.pcm_frames == 0);
  CHECK(proj.instrument_count() == 0);

  InstrumentSpec stereo;
  stereo.frames = 500;
  Bytes st = instrument_bytes(stereo);
  st.resize(392 + 2000);  // 1000 frames of data where the header says 500: planar stereo, or damaged
  CHECK(import_instrument(st, true, 3, &rep) == Progress::Failed);
  CHECK(rep.error == FormatError::Unsupported);
  CHECK(proj.instrument_count() == 0);

  InstrumentSpec unstated;  // a header that states no length: mono, the frames are the PCM
  unstated.frames = 321;
  Bytes un = instrument_bytes(unstated);
  p32(un, 60, 0);
  p32(un, 388, formats::crc32(un.data(), 388));
  CHECK(import_instrument(un, true, 3, &rep) == Progress::Done);
  CHECK(rep.pcm_frames == 321);
  // an odd trailing byte is ignored
  Bytes odd = instrument_bytes(unstated);
  odd.push_back(0x55);
  CHECK(import_instrument(odd, true, 3, &rep) == Progress::Done);
  CHECK(rep.pcm_frames == 321);
}

TEST_CASE("og instrument: truncated at every offset, bytes after the end, wrong header") {
  for (unsigned v : {2u, 9u}) {
    InstrumentSpec spec;
    spec.version = v;
    spec.pti = false;
    const Bytes good = instrument_bytes(spec);
    model::Project& proj = fresh();
    for (std::size_t n = 0; n < good.size(); ++n) {
      const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
      formats::og::ImportReport rep;
      CHECK_MESSAGE(import_instrument(cut, false, 4, &rep) == Progress::Failed, "version " << v << " cut at " << n);
      CHECK(rep.error == FormatError::Truncated);
      CHECK(proj.instrument_count() == 0);
    }
    Bytes extra = good;
    extra.push_back(0);
    formats::og::ImportReport rep;
    CHECK(import_instrument(extra, false, 4, &rep) == Progress::Failed);
    CHECK(rep.error == FormatError::Malformed);
  }
  for (unsigned at : {0u, 1u, 2u, 3u}) {  // magic and type
    Bytes f = instrument_bytes([] { InstrumentSpec s; s.pti = false; return s; }());
    f[at] ^= 0x20;
    seal(f);
    p32(f, 388, formats::crc32(f.data(), 388));
    formats::og::ImportReport rep;
    CHECK(import_instrument(f, false, 4, &rep) == Progress::Failed);
    CHECK(rep.error == FormatError::BadMagic);
  }
  formats::og::ImportReport rep;
  CHECK(import_instrument(instrument_bytes({}), true, 48, &rep) == Progress::Failed);  // slot outside the 48 of an OG folder
  CHECK(rep.error == FormatError::Malformed);
  CHECK(import_instrument(instrument_bytes({}), true, 0xFFFF, &rep) == Progress::Failed);
}

#if !defined(__arm__)
namespace {
// The head of an instrument followed by zeros without end.
class HeadThenZeros final : public core::ByteSource {
 public:
  explicit HeadThenZeros(const Bytes& head) : head_(head) {}
  core::IoResult read(void* dst, std::size_t max) noexcept override {
    std::size_t n = 0;
    auto* d = static_cast<std::uint8_t*>(dst);
    while (n < max && pos_ < head_.size()) d[n++] = head_[pos_++];
    while (n < max) d[n++] = 0;
    return {n, core::IoStatus::Ok};
  }

 private:
  const Bytes& head_;
  std::size_t pos_ = 0;
};
}  // namespace

TEST_CASE("og instrument: a .pti that never ends stops at the sample memory") {
  Bytes head = instrument_head({});
  p32(head, 60, 0);
  p32(head, 388, formats::crc32(head.data(), 388));
  HeadThenZeros src(head);
  model::Project& proj = fresh();
  formats::og::InstrumentReader& r = pti_reader();
  formats::ReadParams params;
  params.index = 1;
  r.begin(src, arena(0), params);
  std::size_t steps = 0;
  while (r.step(65536) == Progress::Running) ++steps;
  CHECK(r.report().error == FormatError::LimitExceeded);
  CHECK(proj.instrument_count() == 0);
  CHECK(steps < 400);
}
#endif

// ---- export -------------------------------------------------------------------------------------------------

namespace {
formats::og::InstrumentWriter& writer() {
  static formats::og::InstrumentWriter w;
  return w;
}
Progress export_instrument(const model::Project& p, unsigned slot, Bytes& out, const Bytes* pcm, unsigned frames,
                           std::size_t budget = 4096) {
  formats::WriteParams params;
  params.index = static_cast<std::uint16_t>(slot);
  MemorySource src(pcm != nullptr ? pcm->data() : nullptr, pcm != nullptr ? pcm->size() : 0);
  params.pcm = pcm != nullptr ? &src : nullptr;
  params.pcm_frames = frames;
  return run_writer(writer(), p, out, params, budget);
}
}  // namespace

TEST_CASE("og instrument: OG -> model -> OG is byte-identical for a .pti v9") {
  for (unsigned frames : {1000u, 1u, 600u}) {
    InstrumentSpec spec;
    spec.frames = frames;
    const Bytes in = instrument_bytes(spec);
    model::Project& proj = fresh();
    NEED(import_instrument(in, true, 9) == Progress::Done);
    const Bytes pcm(in.begin() + 392, in.end());
    for (std::size_t budget : {std::size_t{4096}, std::size_t{1}, std::size_t{100}}) {
      Bytes out;
      NEED(export_instrument(proj, 9, out, &pcm, frames, budget) == Progress::Done);
      CHECK_MESSAGE(out == in, "frames " << frames << " budget " << budget);
    }
  }
}

TEST_CASE("og instrument: LFO and SOURCE edits made with the MOD page's rows survive a save and a reload, and nothing else of the file moves") {
  // OS-INSTUI S6: import -> edit with the rows of the page (model::instrument_param::mod, the path write_param takes) -> Export -> import again. The fixture's envelopes carry loop = i & 1 and
  // enable = (i + 1) & 1 and a distinct LFO each (instrument_head), so slot 1 is a loop byte on a slot that is not enabled (Off on the page) and slot 2 is an Envelope.
  namespace ip = model::instrument_param;
  InstrumentSpec spec;
  spec.frames = 600;
  const Bytes original = instrument_bytes(spec);
  model::Project& proj = fresh();
  NEED(import_instrument(original, true, 9) == Progress::Done);
  const auto id = [](unsigned field) { return core::param_id(core::ParamKind::Instrument, 9, field); };
  CHECK(proj.get_param(id(ip::mod(0, ip::ModSource))) == 1.0f);  // loop 0, enable 1
  CHECK(proj.get_param(id(ip::mod(1, ip::ModSource))) == 0.0f);  // loop 1, enable 0: Off
  CHECK(proj.get_param(id(ip::mod(2, ip::ModSource))) == 1.0f);
  CHECK(proj.get_param(id(ip::mod(1, ip::LfoShape))) == 1.0f);  // lfo[1].shape = 1 % 5, speed 4, amount 0.35
  CHECK(proj.get_param(id(ip::mod(1, ip::LfoSpeed))) == 4.0f);
  CHECK(proj.get_param(id(ip::mod(1, ip::LfoAmount))) == 0.25f + 0.1f * 1.0f);
  // the edits: destination 1 becomes an LFO with a new shape, speed and amount; destination 2 an LFO; destination 0's LFO speed (the amp table's last rate) changes though it stays an envelope
  NEED(proj.set_param(id(ip::mod(1, ip::ModSource)), 2.0f) == model::Status::Ok);
  NEED(proj.set_param(id(ip::mod(1, ip::LfoShape)), 4.0f) == model::Status::Ok);
  NEED(proj.set_param(id(ip::mod(1, ip::LfoSpeed)), 20.0f) == model::Status::Ok);
  NEED(proj.set_param(id(ip::mod(1, ip::LfoAmount)), 0.9f) == model::Status::Ok);
  NEED(proj.set_param(id(ip::mod(2, ip::ModSource)), 2.0f) == model::Status::Ok);
  NEED(proj.set_param(id(ip::mod(0, ip::LfoSpeed)), 23.0f) == model::Status::Ok);
  const model::SamplerParams edited = proj.instrument(9)->sampler;
  CHECK((edited.envelope[1].enable == 1 && edited.envelope[1].loop == 1));
  CHECK((edited.envelope[2].enable == 1 && edited.envelope[2].loop == 1));
  CHECK((edited.envelope[0].enable == 1 && edited.envelope[0].loop == 0));
  const Bytes pcm(original.begin() + 392, original.end());
  Bytes saved;
  NEED(export_instrument(proj, 9, saved, &pcm, 600) == Progress::Done);
  NEED(saved.size() == original.size());
  // the bytes that changed are the loop / enable of the two envelopes, the LFO records of destinations 0, 1 and the CRC: nothing else of the OG's file moved
  const auto allowed = [](std::size_t at) {
    if (at >= 388 && at < 392) return true;                                   // the CRC
    for (unsigned e = 0; e < 6; ++e) {
      if (at == 92u + 20u * e + 18u || at == 92u + 20u * e + 19u) return e == 1 || e == 2;  // loop, enable of the slots whose SOURCE changed
      if (at >= 212u + 8u * e && at < 212u + 8u * e + 8u) return e <= 1;                    // lfo[e]
    }
    return false;
  };
  unsigned changed = 0;
  for (std::size_t i = 0; i < original.size(); ++i) {
    if (saved[i] != original[i]) {
      CHECK_MESSAGE(allowed(i), "byte " << i << " changed");
      ++changed;
    }
  }
  CHECK(changed >= 8);
  // load it again: the sampler block is the edited one, every field
  model::Project& again = fresh(1);
  formats::ReadParams params;
  params.index = 9;
  NEED(run_reader(pti_reader(), saved, arena(1), params, 4096) == Progress::Done);
  const model::Instrument* in = again.instrument(9);
  NEED(in != nullptr);
  CHECK(std::memcmp(&in->sampler, &edited, sizeof edited) == 0);
  CHECK(again.get_param(core::param_id(core::ParamKind::Instrument, 9, ip::mod(1, ip::ModSource))) == 2.0f);
  CHECK(again.get_param(core::param_id(core::ParamKind::Instrument, 9, ip::mod(1, ip::LfoAmount))) == 0.9f);
  // and saving the reload gives the same file
  Bytes saved_again;
  NEED(export_instrument(again, 9, saved_again, &pcm, 600) == Progress::Done);
  CHECK(saved_again == saved);
}

// OS-FILES-C C1b / OS-BUG-PTIRATE: the standalone .pti save may write another fine tune (an Amiga-engine instrument: the MED step the voice plays) and another level than the model's; nothing else changes.
TEST_CASE("og instrument: fine_override and level_override replace the written fine tune and volume, a value out of range keeps the model's, the rest of the file is unchanged") {
  InstrumentSpec spec;
  spec.frames = 300;
  const Bytes in = instrument_bytes(spec);
  model::Project& proj = fresh();
  NEED(import_instrument(in, true, 9) == Progress::Done);
  const Bytes pcm(in.begin() + 392, in.end());
  const model::SamplerParams base = proj.instrument(9)->sampler;
  struct Case {
    std::int16_t fine;
    std::int16_t level;
    int want_fine;   // -1000: the model's
    int want_level;  // -1: the model's
  };
  const Case cases[] = {{formats::WriteParams::kNoFineOverride, -1, -1000, -1}, {25, 40, 25, 40}, {-100, 0, -100, 0}, {100, 100, 100, 100}, {101, 101, -1000, -1}, {-101, -2, -1000, -1}, {0, 7, 0, 7}};
  for (const Case& c : cases) {
    INFO("fine " << c.fine << " level " << c.level);
    formats::WriteParams params;
    params.index = 9;
    MemorySource src(pcm.data(), pcm.size());
    params.pcm = &src;
    params.pcm_frames = 300;
    params.fine_override = c.fine;
    params.level_override = c.level;
    Bytes out;
    NEED(run_writer(writer(), proj, out, params, 4096) == Progress::Done);
    NEED(out.size() == in.size());
    model::Project& again = fresh();
    NEED(import_instrument(out, true, 9) == Progress::Done);
    const model::SamplerParams& got = again.instrument(9)->sampler;
    CHECK(got.fine_tune == (c.want_fine == -1000 ? base.fine_tune : c.want_fine));
    CHECK(got.volume == (c.want_level < 0 ? base.volume : c.want_level));
    CHECK(got.tune == base.tune);
    CHECK(got.cutoff == base.cutoff);
    CHECK(std::memcmp(out.data() + 392, pcm.data(), pcm.size()) == 0);
    NEED(import_instrument(in, true, 9) == Progress::Done);  // (the project the next case writes from)
  }
}

TEST_CASE("og instrument: Export writes the head alone without PCM, and names the file by the instrument") {
  model::Project& proj = fresh();
  NEED(import_instrument(instrument_bytes({}), true, 0) == Progress::Done);
  Bytes out;
  NEED(export_instrument(proj, 0, out, nullptr, 0) == Progress::Done);
  CHECK(out.size() == 392);
  CHECK(r32(out, 60) == 0);
  CHECK(r32(out, 388) == formats::crc32(out.data(), 388));
  // no name: the stem of the sample path; a long name is cut to 31 characters and counted
  model::Instrument in = *proj.instrument(0);
  std::memset(in.name, 0, sizeof in.name);
  NEED(proj.set_instrument(0, in) == model::Status::Ok);
  NEED(export_instrument(proj, 0, out, nullptr, 0) == Progress::Done);
  CHECK(std::string(reinterpret_cast<const char*>(&out[21])) == "instr01.wav");
  std::memset(in.name, 'x', 32);
  NEED(proj.set_instrument(0, in) == model::Status::Ok);
  NEED(export_instrument(proj, 0, out, nullptr, 0) == Progress::Done);
  CHECK(out[21 + 30] == 'x');
  CHECK(out[21 + 31] == 0);
  CHECK(writer().export_report().drops.instrument_names_cut == 1);
  // the header: fw 1.9.0.1, version 9, size 372
  const std::uint8_t head[16] = {'T', 'I', 1, 0, 1, 9, 0, 1, 9, 9, 9, 9, 0x74, 0x01, 0, 0};
  CHECK(std::memcmp(out.data(), head, 16) == 0);
}

TEST_CASE("og instrument: what Export refuses or cannot finish") {
  model::Project& proj = fresh();
  Bytes out;
  CHECK(export_instrument(proj, 0, out, nullptr, 0) == Progress::Failed);  // empty slot
  CHECK(writer().report().error == FormatError::Malformed);
  model::Instrument in;
  model::init_instrument(in, model::InstrumentKind::Sample);
  NEED(proj.set_instrument(5, in) == model::Status::Ok);
  NEED(proj.set_instrument(50, in) == model::Status::Ok);
  CHECK(export_instrument(proj, 50, out, nullptr, 0) == Progress::Failed);  // beyond slot 48
  const Bytes pcm(2000, 1);
  CHECK(export_instrument(proj, 5, out, &pcm, 5'898'241) == Progress::Failed);
  CHECK(writer().report().error == FormatError::LimitExceeded);
  CHECK(export_instrument(proj, 5, out, &pcm, 2000) == Progress::Failed);  // the source has 1000 frames, not 2000
  CHECK(writer().report().error == FormatError::Truncated);
  {
    MemorySource src(pcm);
    src.fail_at = 500;
    formats::WriteParams params;
    params.index = 5;
    params.pcm = &src;
    params.pcm_frames = 1000;
    VectorSink sink;
    writer().begin(proj, sink, params);
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SourceFailed);
  }
  {
    VectorSink sink;
    sink.fail_after = 100;
    formats::WriteParams params;
    params.index = 5;
    writer().begin(proj, sink, params);
    while (writer().step(4096) == Progress::Running) {}
    CHECK(writer().report().error == FormatError::SinkFailed);
  }
  {  // changed between two steps
    VectorSink sink;
    formats::WriteParams params;
    params.index = 5;
    writer().begin(proj, sink, params);
    CHECK(writer().step(100) == Progress::Running);
    NEED(proj.set_instrument(5, in) == model::Status::Ok);
    CHECK(writer().step(100) == Progress::Failed);
    CHECK(writer().report().error == FormatError::Modified);
  }
}

TEST_CASE("og instrument: which instruments Export can write") {
  model::Project& proj = fresh();
  model::Instrument s;
  model::init_instrument(s, model::InstrumentKind::Sample);
  model::Instrument midi;
  model::init_instrument(midi, model::InstrumentKind::Midi);
  midi.midi_channel = 3;
  NEED(proj.set_instrument(0, s) == model::Status::Ok);
  NEED(proj.set_instrument(47, s) == model::Status::Ok);
  NEED(proj.set_instrument(48, s) == model::Status::Ok);       // a sample beyond 48
  NEED(proj.set_instrument(50, midi) == model::Status::Ok);    // channel 3 in slot 51: its own slot (OS-INSTRMIDI: 48 + the channel)
  NEED(proj.set_instrument(51, midi) == model::Status::Ok);    // channel 3 in slot 52: moved
  NEED(proj.set_instrument(2, midi) == model::Status::Ok);     // a MIDI instrument among the samples
  NEED(proj.add_module("synth", 5, 1, 0) == model::Status::Ok);
  model::Instrument mod;
  model::init_instrument(mod, model::InstrumentKind::Module);
  mod.module_index = 0;
  NEED(proj.set_instrument(10, mod) == model::Status::Ok);
  using formats::og::ExportClass;
  CHECK(formats::og::export_class(proj, 0) == ExportClass::Pti);
  CHECK(formats::og::export_class(proj, 47) == ExportClass::Pti);
  CHECK(formats::og::export_class(proj, 1) == ExportClass::Empty);
  CHECK(formats::og::export_class(proj, 48) == ExportClass::SlotBeyond48);
  CHECK(formats::og::export_class(proj, 50) == ExportClass::ImplicitMidi);
  CHECK(formats::og::export_class(proj, 51) == ExportClass::MidiMoved);
  CHECK(formats::og::export_class(proj, 2) == ExportClass::MidiMoved);
  CHECK(formats::og::export_class(proj, 10) == ExportClass::Module);
  CHECK(formats::og::export_class(proj, 64) == ExportClass::Empty);
  formats::og::ExportDrops d;
  formats::og::count_instrument_drops(proj, d);
  CHECK(d.sample_slots_beyond_48 == 1);
  CHECK(d.midi_instruments == 2);
  CHECK(d.module_instruments == 1);
  CHECK(d.any_lost());
}
