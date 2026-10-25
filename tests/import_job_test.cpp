// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-002c: imports as jobs. A synthetic WAV is imported through the job runner of the app inside the simulator, with its progress in
// the notification strip, in simulated time, and then its peak cache is built as a second job; cancel, failure, busy refusals; the
// budget a pass gives the job; an import that ends the undo history and says so; a native project loaded as a job. The importer is
// wav_test.cpp and the registered format wav_sample_test.cpp, the runner job_test.cpp.
#include <doctest/doctest.h>

#include <o3os/app/app.h>
#include <o3os/app/import_job.h>
#include <o3os/app/peak_job.h>
#include <o3os/app/view_state.h>
#include <o3os/core/job.h>
#include <o3os/formats/builtin.h>
#include <o3os/formats/wav_sample.h>
#include <o3os/model/editor.h>
#include <o3os/sim/scenario.h>
#include <o3os/sim/simulator.h>
#include <o3os/ui/plot/peak_cache.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "support/memory_io.h"
#include "support/need.h"
#include "support/project_fixture.h"

using namespace o3os;
using core::Severity;

namespace {

void put16(std::vector<std::uint8_t>& v, std::uint32_t x) {
  v.push_back(static_cast<std::uint8_t>(x));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  put16(v, x & 0xFFFFu);
  put16(v, x >> 16);
}
void tag(std::vector<std::uint8_t>& v, const char* t) {
  for (int i = 0; i < 4; ++i) v.push_back(static_cast<std::uint8_t>(t[i]));
}

// A 16-bit file of `frames` frames of a saw wave, 22,050 Hz, with a loop and two cue points after the audio.
std::vector<std::uint8_t> make_wav(std::uint32_t frames, unsigned channels = 1) {
  std::vector<std::uint8_t> v;
  tag(v, "RIFF");
  put32(v, 0);
  tag(v, "WAVE");
  tag(v, "fmt ");
  put32(v, 16);
  put16(v, 1);
  put16(v, channels);
  put32(v, 22050);
  put32(v, 22050 * 2 * channels);
  put16(v, 2 * channels);
  put16(v, 16);
  tag(v, "data");
  put32(v, frames * 2 * channels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) put16(v, (i * 37u) & 0xFFFFu);
  }
  tag(v, "smpl");
  put32(v, 60);                              // 36 bytes of header and one loop of 24
  for (int i = 0; i < 3; ++i) put32(v, 0);   // manufacturer, product, sample period
  put32(v, 60);                              // unity note
  put32(v, 0);                               // pitch fraction
  put32(v, 0);                               // SMPTE format and offset
  put32(v, 0);
  put32(v, 1);                               // one loop
  put32(v, 0);                               // sampler data
  put32(v, 0);                               // the loop: id, type (forward), start, last frame, fraction, play count
  put32(v, 0);
  put32(v, 100);
  put32(v, frames - 1);
  put32(v, 0);
  put32(v, 0);
  tag(v, "cue ");
  put32(v, 4 + 48);
  put32(v, 2);
  for (std::uint32_t k = 0; k < 2; ++k) {
    put32(v, k);
    put32(v, 500 * (k + 1));
    tag(v, "data");
    put32(v, 0);
    put32(v, 0);
    put32(v, 500 * (k + 1));
  }
  const std::uint32_t size = static_cast<std::uint32_t>(v.size() - 8);
  v[4] = static_cast<std::uint8_t>(size);
  v[5] = static_cast<std::uint8_t>(size >> 8);
  v[6] = static_cast<std::uint8_t>(size >> 16);
  v[7] = static_cast<std::uint8_t>(size >> 24);
  return v;
}

std::string head(const app::App& a) { return a.message(); }

// The registry of the built-in formats, and a project arena of the fixtures.
struct Env {
  std::unique_ptr<formats::BuiltinFormats> storage = std::make_unique<formats::BuiltinFormats>();
  formats::FormatRegistry registry;
  Env() {
    formats::register_builtin_formats(registry, *storage);
    test::arena(0).project.reset();
  }
  model::ModelArena& arena() { return test::arena(0); }
  model::Project& project() { return test::arena(0).project; }
  const formats::FormatDesc* wav() const { return registry.find_id("wav"); }
};

// A sample import request: the file `source`, named `label`, into slot `slot`, the audio into `pcm`.
app::App::ImportRequest wav_request(Env& env, core::ByteSource& source, const char* label, std::uint16_t slot,
                                    std::vector<std::int16_t>& pcm, bool keep_stereo = false) {
  app::App::ImportRequest r;
  r.format = env.wav();
  r.source = &source;
  r.arena = &env.arena();
  r.label = label;
  r.params.index = slot;
  r.params.name = label;
  r.params.pcm = pcm.data();
  r.params.pcm_capacity = pcm.size();
  r.params.keep_stereo = keep_stereo;
  return r;
}

}  // namespace

TEST_CASE("import job: a synthetic WAV through the job runner, with its progress in the strip, then its peaks") {
  Env env;
  const std::vector<std::uint8_t> wav = make_wav(30000);  // 60 KB of audio: 8 KB a pass of 2 ms of budget
  std::vector<std::int16_t> samples(40000, 0x5A5A);
  const std::size_t peak_elems = ui::plot::PeakCache::elements_required(30000, 1);
  std::vector<std::int16_t> peaks_vec(peak_elems + 8);
  // (the storage must be word aligned: a vector of int16 from the heap is)
  std::vector<std::string> trace;
  std::vector<std::string> hashes;
  std::vector<std::size_t> consumed;

  const auto run = [&](std::vector<std::string>* out_trace, std::vector<std::string>* out_hashes) {
    env.project().reset();
    sim::Simulator s;
    s.step();
    test::MemorySource source(wav);
    std::fill(samples.begin(), samples.end(), std::int16_t{0x5A5A});
    app::App::ImportRequest rq = wav_request(env, source, "SINE.WAV", 4, samples);
    rq.peak_storage = peaks_vec.data();
    rq.peak_elems = peak_elems;
    CHECK(s.app().start_import(rq));
    CHECK(head(s.app()) == "IMPORT SINE.WAV 0%");
    CHECK(s.app().state().notify.head().severity == Severity::Info);
    std::string last = head(s.app());
    out_trace->push_back(last);  // "0%": the start (it is drawn with the first pass, so the first picture is after it)
    unsigned passes = 0;
    std::size_t before = 0;
    consumed.clear();
    while (passes < 200 && head(s.app()) != "PEAKS SINE.WAV DONE") {
      s.step();
      ++passes;
      consumed.push_back(source.position() - before);
      before = source.position();
      if (head(s.app()) != last) {
        last = head(s.app());
        out_trace->push_back(last);
        out_hashes->push_back(sim::framebuffer_sha256(s.raster()));
      }
    }
    CHECK(passes > 5u);
    CHECK(passes < 40u);
    CHECK(head(s.app()) == "PEAKS SINE.WAV DONE");
    CHECK(s.app().failed_frames() == 0u);
    CHECK(s.healthy());

    // The result: everything was converted, and the instrument says what the file said.
    const formats::FormatReport& rep = *s.app().import_job().report();
    CHECK(rep.error == formats::FormatError::None);
    CHECK(rep.pcm_frames == 30000u);
    CHECK(rep.pcm_channels == 1);
    const model::Instrument* in = env.project().instrument(4);
    NEED(in != nullptr);
    CHECK(std::strcmp(in->name, "SINE") == 0);
    CHECK(std::strcmp(in->sample_path, "samples/instr05.wav") == 0);
    CHECK(in->sample_rate == 22050u);
    CHECK(in->sampler.play_mode == 1);  // a forward loop
    CHECK(in->sampler.loop_start == 218);
    CHECK(in->sampler.loop_end == 65535);
    CHECK(in->sampler.slice_count == 2);
    CHECK(in->sampler.slices[0] == 1092);
    CHECK(in->sampler.slices[1] == 2185);
    bool same = true;
    for (std::uint32_t i = 0; i < 30000; ++i) {
      same = same && samples[i] == static_cast<std::int16_t>((i * 37u) & 0xFFFFu);
    }
    CHECK(same);
    CHECK(samples[30000] == 0x5A5A);  // nothing past the audio

    // The peak cache was built over that audio.
    const ui::plot::PeakCache& cache = s.app().peak_cache();
    CHECK(cache.valid());
    CHECK(cache.built());
    CHECK(cache.frames() == 30000u);
    const ui::plot::PeakPair all = cache.range(0, 0, 30000);
    int mn = 32767, mx = -32768;
    for (std::uint32_t i = 0; i < 30000; ++i) {
      mn = std::min<int>(mn, samples[i]);
      mx = std::max<int>(mx, samples[i]);
    }
    CHECK(all.min == mn);
    CHECK(all.max == mx);

    // The strip runs out after its time, and both slots are free for the next import.
    s.wait_ms(2100);
    CHECK(head(s.app()).empty());
    CHECK(s.app().session().jobs.occupied() == 0u);
    test::MemorySource again(wav);
    CHECK(s.app().start_import(wav_request(env, again, "SECOND.WAV", 5, samples)));
    CHECK(s.app().session().jobs.running() == 1u);
  };
  run(&trace, &hashes);

  // The strip said 0%, then the percentages of the import in order, then the peaks' (never "IMPORT SINE.WAV DONE": the
  // chain goes on), then PEAKS SINE.WAV DONE; every one of them a different picture.
  CHECK(trace.size() >= 5u);
  CHECK(trace.front() == "IMPORT SINE.WAV 0%");
  CHECK(trace.back() == "PEAKS SINE.WAV DONE");
  int last_percent = -1;
  bool in_peaks = false;
  bool saw_peak_progress = false;
  for (std::size_t i = 0; i + 1 < trace.size(); ++i) {
    CHECK(trace[i] != "IMPORT SINE.WAV DONE");
    if (trace[i].find("PEAKS SINE.WAV ") == 0u) {
      if (!in_peaks) {
        in_peaks = true;
        last_percent = -1;
      }
      saw_peak_progress = true;
      const int p = std::atoi(trace[i].c_str() + 15);
      CHECK(p > last_percent);
      last_percent = p;
    } else {
      CHECK_FALSE(in_peaks);  // no import progress after the peaks began
      CHECK(trace[i].find("IMPORT SINE.WAV ") == 0u);
      const int p = std::atoi(trace[i].c_str() + 16);
      CHECK(p > last_percent);
      last_percent = p;
    }
  }
  CHECK(saw_peak_progress);
  for (std::size_t i = 1; i < hashes.size(); ++i) {
    CHECK(hashes[i] != hashes[i - 1]);
  }
  // The budget: no pass moved more than 2 ms of budget's worth of bytes (4 bytes a microsecond), plus the one read in flight.
  for (const std::size_t n : consumed) {
    CHECK(n <= app::App::kJobBudgetUs * app::ImportJob::kBytesPerUs);
  }

  // The same run again is the same, byte for byte and pixel for pixel: time is the simulator's, nothing else.
  std::vector<std::string> trace2, hashes2;
  run(&trace2, &hashes2);
  CHECK(trace == trace2);
  CHECK(hashes == hashes2);
}

TEST_CASE("import job: a stereo WAV is mixed to mono unless the request keeps it") {
  Env env;
  const std::vector<std::uint8_t> wav = make_wav(1000, 2);
  std::vector<std::int16_t> pcm(2000, 0);
  sim::Simulator s;
  s.step();
  for (const bool keep : {false, true}) {
    test::MemorySource source(wav);
    CHECK(s.app().start_import(wav_request(env, source, "ST.WAV", 0, pcm, keep)));
    s.wait_ms(100);
    CHECK(head(s.app()) == "IMPORT ST.WAV DONE");
    const formats::FormatReport& rep = *s.app().import_job().report();
    CHECK(rep.pcm_frames == 1000u);
    CHECK(rep.pcm_channels == (keep ? 2 : 1));
    // L = R in the file: the mix is the sample itself; kept, it is interleaved.
    const std::int16_t want = static_cast<std::int16_t>((10u * 37u) & 0xFFFFu);
    CHECK(pcm[keep ? 20 : 10] == want);
    if (keep) {
      CHECK(pcm[21] == want);
    }
    s.wait_ms(2100);
  }
}

TEST_CASE("import job: cancel, failure and a second import while one runs") {
  Env env;
  const std::vector<std::uint8_t> wav = make_wav(30000);
  std::vector<std::int16_t> samples(40000, 0);
  const std::size_t peak_elems = ui::plot::PeakCache::elements_required(30000, 1);
  std::vector<std::int16_t> peaks_vec(peak_elems + 8);
  sim::Simulator s;
  s.step();

  // Cancel in the middle: the strip says so, the slot is free again, the project is as it was.
  test::MemorySource source(wav);
  const std::uint32_t generation = env.project().generation();
  CHECK(s.app().start_import(wav_request(env, source, "KICK.WAV", 3, samples)));
  s.step();
  s.step();
  CHECK(head(s.app()).find("IMPORT KICK.WAV ") == 0u);
  CHECK(head(s.app()).find("DONE") == std::string::npos);
  test::MemorySource other(wav);
  CHECK_FALSE(s.app().start_import(wav_request(env, other, "OTHER.WAV", 4, samples)));  // one at a time
  CHECK(s.app().state().notify.size() == 2u);  // the refusal waits behind the progress strip, which goes on being rewritten
  CHECK(s.app().state().notify.at(1).severity == Severity::Warn);
  const std::size_t at = source.position();
  CHECK(s.action(core::action_id("job.cancel"), 0) == app::DispatchResult::Changed);
  s.wait_ms(10);
  CHECK(s.app().session().jobs.running() == 0u);
  CHECK(s.app().session().jobs.occupied() == 0u);
  CHECK(head(s.app()) == "IMPORT KICK.WAV CANCELLED");
  CHECK(source.position() == at + 0u);  // not read from again once cancelled
  CHECK(s.app().import_job().report()->error == formats::FormatError::Cancelled);
  CHECK(env.project().generation() == generation);
  CHECK(env.project().instrument(3) == nullptr);
  CHECK(s.action(core::action_id("job.cancel"), 0) == app::DispatchResult::Unchanged);  // nothing to cancel

  // A damaged file fails with its reason, as an error. (The cancelled strip and the refusal behind it run out first.)
  s.wait_ms(4100);
  std::vector<std::uint8_t> cut(wav.begin(), wav.begin() + 20000);
  test::MemorySource bad(cut);
  CHECK(s.app().start_import(wav_request(env, bad, "BAD.WAV", 3, samples)));
  s.wait_ms(30);
  CHECK(head(s.app()) == "IMPORT BAD.WAV FAILED: TRUNCATED");
  CHECK(s.app().state().notify.head().severity == Severity::Error);
  CHECK(s.app().session().jobs.occupied() == 0u);
  CHECK(env.project().instrument(3) == nullptr);
  s.wait_ms(4100);
  CHECK(head(s.app()).empty());

  // A buffer that is too small is refused before anything is written, and a file that is not a WAV says so. No peak job follows a
  // failure.
  std::vector<std::int16_t> tiny(100, 0x5A5A);
  test::MemorySource fits_not(wav);
  app::App::ImportRequest big = wav_request(env, fits_not, "BIG.WAV", 3, tiny);
  big.peak_storage = peaks_vec.data();
  big.peak_elems = peak_elems;
  CHECK(s.app().start_import(big));
  s.wait_ms(10);
  CHECK(head(s.app()) == "IMPORT BIG.WAV FAILED: TOO BIG FOR MEMORY");
  CHECK(tiny[0] == 0x5A5A);
  CHECK(s.app().session().jobs.occupied() == 0u);
  s.wait_ms(4100);
  const std::vector<std::uint8_t> junk(200, 'x');
  test::MemorySource not_wav(junk);
  CHECK(s.app().start_import(wav_request(env, not_wav, "TEXT.TXT", 3, samples)));
  s.wait_ms(10);
  CHECK(head(s.app()) == "IMPORT TEXT.TXT FAILED: UNKNOWN FILE TYPE");
  CHECK(s.app().failed_frames() == 0u);

  // An incomplete request is refused with a word, not a crash.
  s.wait_ms(4100);
  app::App::ImportRequest none;
  CHECK_FALSE(s.app().start_import(none));
  CHECK(head(s.app()) == "NOTHING TO IMPORT WITH");
  app::App::ImportRequest no_reader = wav_request(env, source, "X.WAV", 0, samples);
  formats::FormatDesc writer_only;
  writer_only.id = "w";
  no_reader.format = &writer_only;
  CHECK_FALSE(s.app().start_import(no_reader));
}

TEST_CASE("import job: the peak job can be cancelled; the cache then answers nothing") {
  Env env;
  const std::vector<std::uint8_t> wav = make_wav(30000);
  std::vector<std::int16_t> samples(40000, 0);
  const std::size_t peak_elems = ui::plot::PeakCache::elements_required(30000, 1);
  std::vector<std::int16_t> peaks_vec(peak_elems + 8);
  sim::Simulator s;
  s.step();
  test::MemorySource source(wav);
  app::App::ImportRequest rq = wav_request(env, source, "PK.WAV", 0, samples);
  rq.peak_storage = peaks_vec.data();
  rq.peak_elems = peak_elems;
  CHECK(s.app().start_import(rq));
  unsigned passes = 0;
  while (passes < 100 && head(s.app()).find("PEAKS PK.WAV ") != 0u) {  // run the import out
    s.step();
    ++passes;
  }
  CHECK(head(s.app()).find("PEAKS PK.WAV ") == 0u);
  CHECK_FALSE(s.app().peak_cache().built());
  CHECK(s.action(core::action_id("job.cancel"), 0) == app::DispatchResult::Changed);
  s.wait_ms(10);
  CHECK(head(s.app()) == "PEAKS PK.WAV CANCELLED");
  CHECK_FALSE(s.app().peak_cache().built());
  CHECK(s.app().peak_cache().range(0, 0, 100).empty());
  CHECK(s.app().session().jobs.occupied() == 0u);
  CHECK(env.project().instrument(0) != nullptr);  // the import itself was done

  // Storage too small for the peaks: the import still counts, and the strip says there is no room for the rest.
  s.wait_ms(4100);
  test::MemorySource again(wav);
  app::App::ImportRequest small = wav_request(env, again, "SM.WAV", 1, samples);
  small.peak_storage = peaks_vec.data();
  small.peak_elems = 8;
  CHECK(s.app().start_import(small));
  s.wait_ms(100);
  CHECK(head(s.app()) == "IMPORT SM.WAV DONE");
  CHECK(s.app().state().notify.size() >= 1u);
  CHECK(env.project().instrument(1) != nullptr);
}

TEST_CASE("import job: an import ends the undo history of the document it changed, and says so; one that changed nothing does not") {
  Env env;
  alignas(8) static std::uint8_t ring[16384];
  model::UndoJournal journal;
  journal.begin(ring, sizeof ring);
  model::Editor editor(env.project(), journal);
  sim::Simulator s;
  s.step();
  s.app().attach_document(&editor);
  CHECK(s.action(core::action_id("project.tempo"), 10) == app::DispatchResult::Changed);
  CHECK(s.action(core::action_id("project.tempo"), 5) == app::DispatchResult::Changed);
  CHECK(editor.can_undo());

  const std::vector<std::uint8_t> wav = make_wav(1000);
  std::vector<std::int16_t> pcm(2000, 0);
  // A failed import changed nothing: the history stays.
  std::vector<std::uint8_t> cut(wav.begin(), wav.begin() + 300);
  test::MemorySource bad(cut);
  CHECK(s.app().start_import(wav_request(env, bad, "BAD.WAV", 2, pcm)));
  s.wait_ms(50);
  CHECK(head(s.app()) == "IMPORT BAD.WAV FAILED: TRUNCATED");
  CHECK(editor.can_undo());
  CHECK(editor.journal().depth() == 2u);
  s.wait_ms(4100);

  // A good one sets an instrument behind the editor's back: the history ends, with a notice, after the import's own line.
  test::MemorySource good(wav);
  CHECK(s.app().start_import(wav_request(env, good, "GOOD.WAV", 2, pcm)));
  s.wait_ms(50);
  CHECK(env.project().instrument(2) != nullptr);
  CHECK_FALSE(editor.can_undo());
  CHECK_FALSE(editor.can_redo());
  CHECK(editor.journal().depth() == 0u);
  bool said = false;
  for (unsigned i = 0; i < s.app().state().notify.size(); ++i) {
    said = said || std::string(s.app().state().notify.at(i).text.c_str()) == "UNDO HISTORY CLEARED BY THE IMPORT";
  }
  CHECK(said);
  // F8 now says there is nothing to undo, rather than undoing into a project that no longer matches.
  s.wait_ms(10000);
  CHECK(s.action(core::action_id("edit.undo"), 0) == app::DispatchResult::Changed);
  CHECK(head(s.app()) == "NOTHING TO UNDO HERE");

  // An import into a project with no history says nothing about history.
  s.wait_ms(10000);
  test::MemorySource again(wav);
  CHECK(s.app().start_import(wav_request(env, again, "AGAIN.WAV", 3, pcm)));
  s.wait_ms(50);
  CHECK(env.project().instrument(3) != nullptr);
  for (unsigned i = 0; i < s.app().state().notify.size(); ++i) {
    CHECK(std::string(s.app().state().notify.at(i).text.c_str()) != "UNDO HISTORY CLEARED BY THE IMPORT");
  }
  // And the editor goes on working afterwards: a new edit is an undo unit again.
  CHECK(s.action(core::action_id("project.tempo"), 3) == app::DispatchResult::Changed);
  CHECK(editor.can_undo());
}

TEST_CASE("import job: a native project is loaded as a job, cannot be cancelled half way, and ends the history") {
  Env env;
  // A full project in a file: 1.9 MB, 240 passes of 8 KB.
  model::ModelArena& src = test::arena(2);
  test::fill_maximal(src.project);
  std::vector<std::uint8_t> bytes;
  NEED(test::save(src.project, bytes) == formats::native::Progress::Done);

  alignas(8) static std::uint8_t ring[8192];
  model::UndoJournal journal;
  journal.begin(ring, sizeof ring);
  env.project().reset();
  model::Editor editor(env.project(), journal);
  sim::Simulator s;
  s.step();
  s.app().attach_document(&editor);
  CHECK(s.action(core::action_id("project.tempo"), 10) == app::DispatchResult::Changed);

  test::MemorySource source(bytes);
  app::App::ImportRequest rq;
  rq.format = env.registry.find_id("native");
  rq.source = &source;
  rq.arena = &env.arena();
  rq.label = "SONG.O3PJ";
  rq.params.size_hint = static_cast<std::uint32_t>(bytes.size());
  NEED(rq.format != nullptr);
  CHECK(s.app().start_import(rq));
  s.step();
  CHECK(head(s.app()).find("IMPORT SONG.O3PJ ") == 0u);
  CHECK(s.app().session().jobs.running() == 1u);
  CHECK(s.action(core::action_id("job.cancel"), 0) == app::DispatchResult::Unchanged);  // half a project is not an option
  CHECK(s.app().session().jobs.running() == 1u);
  s.wait_ms(1000);
  CHECK(s.app().session().jobs.running() == 0u);
  CHECK(head(s.app()) == "IMPORT SONG.O3PJ DONE");
  CHECK(env.project().pattern_count() == src.project.pattern_count());  // what fill_maximal made
  CHECK(env.project().instrument_count() == 64u);
  CHECK_FALSE(editor.can_undo());  // the history of the project that was there is over
  bool said = false;
  for (unsigned i = 0; i < s.app().state().notify.size(); ++i) {
    said = said || std::string(s.app().state().notify.at(i).text.c_str()) == "UNDO HISTORY CLEARED BY THE IMPORT";
  }
  CHECK(said);
  CHECK(s.app().import_job().report()->patterns == src.project.pattern_count());
}

TEST_CASE("import job: a modal overlay holds the strip back but not the job") {
  Env env;
  const std::vector<std::uint8_t> wav = make_wav(30000);
  std::vector<std::int16_t> samples(40000, 0);
  sim::Simulator s;
  s.step();
  test::MemorySource source(wav);
  CHECK(s.app().start_import(wav_request(env, source, "SLOW.WAV", 0, samples)));
  CHECK(s.action(core::action_id("learn.toggle"), 0) == app::DispatchResult::Changed);
  CHECK(s.app().overlay() == app::OverlayId::Learn);
  s.wait_ms(500);  // the job finished meanwhile: Learn is up, the news waits in the queue
  CHECK(s.app().session().jobs.running() == 0u);
  CHECK(s.app().import_job().reader()->progress() == formats::Progress::Done);
  CHECK(s.app().overlay() == app::OverlayId::Learn);
  CHECK(s.action(core::action_id("learn.toggle"), 0) == app::DispatchResult::Changed);
  CHECK(head(s.app()) == "IMPORT SLOW.WAV DONE");  // shown now, with its whole time
  s.wait_ms(1900);
  CHECK(head(s.app()) == "IMPORT SLOW.WAV DONE");
  s.wait_ms(200);
  CHECK(head(s.app()).empty());
}

TEST_CASE("import job: a long file name is cut in the strip, never the percentage or the reason") {
  Env env;
  const std::vector<std::uint8_t> wav = make_wav(30000);
  std::vector<std::int16_t> samples(40000, 0);
  const char* long_name = "A-VERY-LONG-FILE-NAME-OF-A-SAMPLE-THAT-IS-NOT-SHORT.WAV";  // 55 characters
  sim::Simulator s;
  s.step();
  test::MemorySource source(wav);
  CHECK(s.app().start_import(wav_request(env, source, long_name, 0, samples)));
  s.step();
  const std::string progress = head(s.app());
  CHECK(progress.size() <= core::kNotifyTextChars);
  CHECK(progress.find("IMPORT A-VERY-LONG") == 0u);
  CHECK(progress.back() == '%');  // the percentage is whole
  CHECK(progress.find("DONE") == std::string::npos);
  s.wait_ms(100);
  const std::string done = head(s.app());
  CHECK(done.size() == 45u);  // the job's own name is cut at 40 characters ("IMPORT " and 33 of the file's): then " DONE"
  CHECK(done.substr(done.size() - 5) == " DONE");
  s.wait_ms(2100);

  std::vector<std::uint8_t> cut(wav.begin(), wav.begin() + 1000);
  test::MemorySource bad(cut);
  CHECK(s.app().start_import(wav_request(env, bad, long_name, 1, samples)));
  s.wait_ms(20);
  const std::string failed = head(s.app());
  CHECK(failed.size() == core::kNotifyTextChars);
  CHECK(failed.substr(failed.size() - 18) == " FAILED: TRUNCATED");  // the reason is whole
  CHECK(failed.find("IMPORT A-VERY-LONG") == 0u);
}

TEST_CASE("import job: the cost model turns a budget into a byte count, never less than one read nor more than 64 KiB") {
  Env env;
  app::ImportJob job;
  const std::vector<std::uint8_t> wav = make_wav(60000);  // 120 KB: more than one enormous step
  std::vector<std::int16_t> samples(70000, 0);
  test::MemorySource source(wav);
  formats::wav::SampleReader reader;
  formats::ReadParams params;
  params.pcm = samples.data();
  params.pcm_capacity = samples.size();
  params.size_hint = static_cast<std::uint32_t>(wav.size());
  job.begin(reader, source, env.arena(), "X.WAV", params);
  CHECK(std::string(job.name()) == "IMPORT X.WAV");
  CHECK(job.progress() == 0u);
  CHECK(job.cancellable());
  CHECK(job.step(0) == core::JobStatus::Running);  // a zero budget still moves a block
  CHECK(source.position() == app::ImportJob::kMinStepBytes);
  const std::size_t p0 = source.position();
  CHECK(job.step(100) == core::JobStatus::Running);  // 100 us: 400 bytes, below the minimum: the minimum
  CHECK(source.position() == p0 + app::ImportJob::kMinStepBytes);
  const std::size_t p1 = source.position();
  CHECK(job.step(2000) == core::JobStatus::Running);  // 2 ms: 8,000 bytes
  CHECK(source.position() == p1 + 8000u);
  const std::size_t p2 = source.position();
  CHECK(job.step(0xFFFFFFFFu) == core::JobStatus::Running);  // an enormous budget is bounded
  CHECK(source.position() - p2 == app::ImportJob::kMaxStepBytes);
  CHECK(job.progress() > 0u);
  CHECK(job.progress() < 1000u);
  job.cancel();
  CHECK(std::string(job.detail()) == "CANCELLED");

  // A name that is too long for the strip is cut, not overrun; no label is an empty one.
  test::MemorySource source2(wav);
  job.begin(reader, source2, env.arena(), "AN-EXTREMELY-LONG-FILE-NAME-THAT-DOES-NOT-FIT.WAV", params);
  CHECK(std::string(job.name()).size() == 40u);
  job.begin(reader, source2, env.arena(), nullptr, params);
  CHECK(std::string(job.name()) == "IMPORT ");

  // Run to the end: Done at 1000; and the job of a reader that cannot be abandoned says so.
  test::MemorySource source3(wav);
  job.begin(reader, source3, env.arena(), "Y.WAV", params);
  unsigned guard = 0;
  while (job.step(2000) == core::JobStatus::Running && ++guard < 100) {
  }
  CHECK(job.progress() == core::kJobProgressMax);
  CHECK(std::string(job.detail()).empty());
  app::ImportJob project_job;
  test::MemorySource source4(wav);
  project_job.begin(*env.registry.find_id("og-project")->reader, source4, env.arena(), "P.MT", formats::ReadParams{});
  CHECK_FALSE(project_job.cancellable());
}

TEST_CASE("peak job: a cooperative build of the cache in slices, with an honest progress") {
  std::vector<std::int16_t> samples(50000);
  for (std::size_t i = 0; i < samples.size(); ++i) samples[i] = static_cast<std::int16_t>((i * 97u) % 20000u) - 10000;
  const std::size_t elems = ui::plot::PeakCache::elements_required(50000, 1);
  std::vector<std::int16_t> storage(elems + 8);
  ui::plot::PeakCache cache;
  app::PeakJob job;
  CHECK(job.begin(cache, storage.data(), elems, samples.data(), 50000, 1, "W.WAV"));
  CHECK(std::string(job.name()) == "PEAKS W.WAV");
  CHECK(job.progress() == 0u);
  CHECK_FALSE(cache.built());
  unsigned steps = 0;
  unsigned last = 0;
  core::JobStatus st = core::JobStatus::Running;
  while (st == core::JobStatus::Running && steps < 1000) {
    st = job.step(1000);  // 8,000 reads
    ++steps;
    if (st == core::JobStatus::Running) {
      CHECK(job.progress() >= last);
      CHECK(job.progress() < 1000u);
      last = job.progress();
    }
  }
  CHECK(st == core::JobStatus::Done);
  CHECK(steps > 3u);
  CHECK(job.progress() == 1000u);
  CHECK(cache.built());
  int mn = 32767, mx = -32768;
  for (const std::int16_t v : samples) {
    mn = std::min<int>(mn, v);
    mx = std::max<int>(mx, v);
  }
  CHECK(cache.range(0, 0, 50000).min == mn);
  CHECK(cache.range(0, 0, 50000).max == mx);

  // Refused: storage too small, an empty sound, and a cancelled job does not go on.
  CHECK_FALSE(job.begin(cache, storage.data(), 8, samples.data(), 50000, 1, "W.WAV"));
  CHECK(job.step(100) == core::JobStatus::Failed);
  CHECK_FALSE(job.begin(cache, storage.data(), elems, samples.data(), 0, 1, "W.WAV"));
  CHECK(job.begin(cache, storage.data(), elems, samples.data(), 50000, 1, "W.WAV"));
  job.cancel();
  CHECK(job.step(100) == core::JobStatus::Failed);
}
