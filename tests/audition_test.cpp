// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-FU2: the Sample load audition. F5 held plays the first seconds of the WAV under the cursor on a descriptor that is no instrument's (audio::kPreviewSlot), read as a job into the
// platform's buffer, through the live notes (PreviewSink::audition); the key's release, a cursor move and leaving the screen end it; the buffer and the descriptor are written only in
// the quiet window (no live note queued or held, no voice reading the descriptor). Also the importer's clip mode and the tuning that plays a 22 kHz file as it sounds.
#include <doctest/doctest.h>

#include "support/need.h"

#include <o3os/app/actions.h>
#include <o3os/app/app.h>
#include <o3os/app/preview_job.h>
#include <o3os/app/sample_bank.h>
#include <o3os/app/screens/sample_load_screen.h>
#include <o3os/audio/voices.h>
#include <o3os/core/action.h>
#include <o3os/core/input_event.h>
#include <o3os/formats/wav.h>
#include <o3os/model/editor.h>
#include <o3os/platform/host/dir_storage.h>
#include <o3os/platform/host/fakes.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

using namespace o3os;
using app::Button;
using app::ScreenId;
namespace fs = std::filesystem;

namespace {

alignas(8) std::uint8_t g_journal_ring[64 * 1024];

void put16(std::vector<std::uint8_t>& v, unsigned x) {
  v.push_back(static_cast<std::uint8_t>(x & 0xFF));
  v.push_back(static_cast<std::uint8_t>(x >> 8));
}
void put32(std::vector<std::uint8_t>& v, std::uint32_t x) {
  put16(v, x & 0xFFFF);
  put16(v, x >> 16);
}
void tag(std::vector<std::uint8_t>& v, const char* t) {
  for (int i = 0; i < 4; ++i) {
    v.push_back(static_cast<std::uint8_t>(t[i]));
  }
}
std::int16_t ramp(std::uint32_t i) { return static_cast<std::int16_t>(static_cast<int>((i * 7u) % 20000u) - 10000); }

// A 16-bit WAV of `frames` frames at `rate`; stereo repeats the ramp in both channels. A `smpl`-free file: just fmt and data.
std::vector<std::uint8_t> wav_bytes(std::uint32_t frames, std::uint32_t rate = 44100, unsigned channels = 1) {
  std::vector<std::uint8_t> v;
  tag(v, "RIFF");
  put32(v, 36 + frames * 2 * channels);
  tag(v, "WAVE");
  tag(v, "fmt ");
  put32(v, 16);
  put16(v, 1);
  put16(v, channels);
  put32(v, rate);
  put32(v, rate * 2 * channels);
  put16(v, 2 * channels);
  put16(v, 16);
  tag(v, "data");
  put32(v, frames * 2 * channels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      put16(v, static_cast<std::uint16_t>(ramp(i)));
    }
  }
  return v;
}

struct VectorSource final : core::ByteSource {
  explicit VectorSource(const std::vector<std::uint8_t>& b) : bytes(b) {}
  core::IoResult read(void* dst, std::size_t max) noexcept override {
    const std::size_t n = std::min(max, bytes.size() - pos);
    if (n == 0) {
      return {0, core::IoStatus::End};
    }
    std::memcpy(dst, bytes.data() + pos, n);
    pos += n;
    return {n, core::IoStatus::Ok};
  }
  const std::vector<std::uint8_t>& bytes;
  std::size_t pos = 0;
};

void write_file(const fs::path& p, const std::vector<std::uint8_t>& bytes) {
  fs::create_directories(p.parent_path());
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

// What the engine would be told: the audition's notes, and whether the live path is idle.
struct Sink final : app::PreviewSink {
  void preview(unsigned, unsigned) noexcept override {}
  void audition(bool on, unsigned track) noexcept override {
    log.push_back(on ? 1 : 0);
    last_track = track;
  }
  bool live_idle() const noexcept override { return idle; }
  std::vector<int> log;
  unsigned last_track = 99;
  bool idle = true;
};

struct Rig {
  fs::path root;
  platform::host::FakeInput input;
  platform::host::FakeClock clock;
  platform::host::FakeDisplay display;
  std::unique_ptr<platform::host::DirStorage> card;
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<model::ModelArena> arena = std::make_unique<model::ModelArena>();
  std::vector<std::int16_t> memory;
  std::vector<std::int16_t> peaks;
  std::vector<std::int16_t> preview;
  model::UndoJournal journal;
  std::unique_ptr<model::Editor> editor;
  std::unique_ptr<app::SampleBankMem> mem = std::make_unique<app::SampleBankMem>();
  app::SampleBank bank;
  Sink sink;
  std::unique_ptr<app::App> app = std::make_unique<app::App>(input, clock, display);

  explicit Rig(std::uint32_t preview_samples = 4000, bool with_preview = true) : memory(200008), peaks(40000), preview(preview_samples + 8) {
    static unsigned n = 0;
    root = fs::temp_directory_path() / ("o3os-audition-" + std::to_string(::getpid()) + "-" + std::to_string(n++));
    fs::remove_all(root);
    fs::create_directories(root);
    card = std::make_unique<platform::host::DirStorage>(root.string());
    NEED(card->mount() == hal::storage::Status::Ok);
    arena->project.reset();
    bank.init(*voices, *arena, *mem, memory.data(), 200000, peaks.data(), peaks.size());
    if (with_preview) {
      bank.set_preview_buffer(preview.data(), preview_samples);
    }
    journal.begin(g_journal_ring, sizeof g_journal_ring);
    editor = std::make_unique<model::Editor>(arena->project, journal);
    app->attach_document(editor.get());
    app->attach_storage(card.get());
    app->attach_samples(&bank);
    app->attach_preview(&sink);
  }
  ~Rig() { fs::remove_all(root); }
  void pass(unsigned ms = 16) {
    clock.advance(ms * core::kTicksPerMs);
    app->run_once();
  }
  void passes(unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      pass();
    }
  }
  void settle() {
    for (unsigned i = 0; i < 2000; ++i) {
      pass();
      const app::DirScan::State s = app->model().files->state();
      if (s != app::DirScan::State::Scanning && s != app::DirScan::State::Idle) {
        return;
      }
    }
    FAIL_CHECK("the scan did not end");
  }
  void go(ScreenId id) {
    (void)app->invoke(core::action_id("nav.screen"), static_cast<core::ActionArg>(id));
    pass();
  }
  void key(Button b, bool down) {
    input.push(core::make_button_event(clock.now(), static_cast<std::uint8_t>(b), down));
    pass();
  }
  void select(unsigned i) {
    app::ViewState& v = const_cast<app::ViewState&>(app->state());
    (void)app::kSampleLoadScreen.set_position(app->model(), v, static_cast<int>(i));
  }
  // Passes until the audition's note is on (the read is a job: a few passes).
  bool until_on(unsigned max = 100) {
    const std::size_t n = sink.log.size();
    for (unsigned i = 0; i < max && sink.log.size() == n; ++i) {
      pass();
    }
    return sink.log.size() > n && sink.log.back() == 1;
  }
};

}  // namespace

TEST_CASE("audition: the importer's clip mode reads the first frames that fit and ends there, Done; without it a long file is refused") {
  const std::vector<std::uint8_t> file = wav_bytes(1000);
  std::int16_t dst[100];
  formats::wav::Importer imp;
  {
    VectorSource src(file);
    imp.begin(src, dst, 100, false, true);
    while (imp.step() == formats::wav::Progress::Running) {
    }
    CHECK(imp.progress() == formats::wav::Progress::Done);
    CHECK(imp.info().frames == 100u);
    CHECK(imp.report().trailing_ignored);
    for (std::uint32_t i = 0; i < 100; ++i) {
      CHECK(dst[i] == ramp(i));
    }
  }
  {
    VectorSource src(file);
    imp.begin(src, dst, 100, false, false);
    while (imp.step() == formats::wav::Progress::Running) {
    }
    CHECK(imp.progress() == formats::wav::Progress::Failed);
    CHECK(imp.report().error == formats::wav::Error::LimitExceeded);
  }
  {
    const std::vector<std::uint8_t> st = wav_bytes(1000, 44100, 2);  // stereo: downmixed to mono, so 100 samples are 100 frames
    VectorSource src(st);
    imp.begin(src, dst, 100, false, true);
    while (imp.step() == formats::wav::Progress::Running) {
    }
    CHECK(imp.progress() == formats::wav::Progress::Done);
    CHECK(imp.info().frames == 100u);
    CHECK(imp.info().channels == 1u);
    for (std::uint32_t i = 0; i < 100; ++i) {
      CHECK(dst[i] == ramp(i));  // L = R passes bit-exact
    }
  }
  {
    const std::vector<std::uint8_t> small = wav_bytes(50);  // a file that fits is read whole, clip or not
    VectorSource src(small);
    imp.begin(src, dst, 100, false, true);
    while (imp.step() == formats::wav::Progress::Running) {
    }
    CHECK(imp.progress() == formats::wav::Progress::Done);
    CHECK(imp.info().frames == 50u);
    CHECK_FALSE(imp.report().trailing_ignored);
  }
}

TEST_CASE("audition: the tuning plays a file at its own rate as it sounds, whatever the rate") {
  const std::uint32_t rates[] = {8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000};
  for (const std::uint32_t rate : rates) {
    std::int8_t tune = 0;
    std::int8_t fine = 0;
    app::preview_tuning(rate, tune, fine);
    const double want = static_cast<double>(rate) / 44100.0 * 4294967296.0;
    const double got = static_cast<double>(audio::tuned_inc_q32(audio::kRootNote, tune, fine));
    CHECK(got / want == doctest::Approx(1.0).epsilon(0.003));
  }
  std::int8_t tune = 5;
  std::int8_t fine = 5;
  app::preview_tuning(0, tune, fine);
  CHECK(tune == 0);
  CHECK(fine == 0);
}

TEST_CASE("audition: F5 held reads the first seconds of the file and plays them; the release ends the note") {
  Rig rig;  // a buffer of 4000 samples
  write_file(rig.root / "long.wav", wav_bytes(30000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 1u);
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == 1u);
  const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
  CHECK(d.length == 4000u);  // clipped to the buffer
  CHECK(d.tune == 0);
  NEED(d.data == rig.preview.data());
  for (std::uint32_t i = 0; i < 4000; ++i) {
    NEED(d.data[i] == ramp(i));
  }
  CHECK(rig.voices->sample(0).length == 0u);  // no instrument was touched
  CHECK(rig.bank.pcm(0).frames == 0u);
  CHECK(rig.arena->project.instrument(0) == nullptr);
  CHECK(std::string(rig.app->message()) != "PREVIEW DONE");  // the read is quiet
  rig.passes(5);
  CHECK(rig.sink.log.size() == 1u);  // still the one note: held
  rig.key(Button::F5, false);
  NEED(rig.sink.log.size() == 2u);
  CHECK(rig.sink.log[1] == 0);
  rig.passes(5);
  CHECK(rig.sink.log.size() == 2u);
}

TEST_CASE("audition: F5 on an AIFF or 8SVX says to load it (the audition reads WAV files)") {
  Rig rig;
  write_file(rig.root / "a.aif", wav_bytes(2000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 1u);
  rig.key(Button::F5, true);
  rig.passes(5);
  CHECK(std::string(rig.app->message()) == "NO PREVIEW OF THIS TYPE: LOAD IT");
  CHECK(rig.sink.log.empty());
  rig.key(Button::F5, false);
}

TEST_CASE("audition: a file recorded at 22,050 Hz is tuned an octave up, a stereo file is downmixed") {
  Rig rig;
  write_file(rig.root / "a.wav", wav_bytes(2000, 22050, 2));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
  CHECK(d.length == 2000u);
  CHECK(d.tune == -12);
  CHECK(d.fine_tune == 0);
  CHECK(d.data[10] == ramp(10));
  // The pool plays it: instrument 65 is slot 64, a note on it sounds.
  CHECK(rig.voices->note_on(0, audio::kRootNote, audio::kPreviewSlot, audio::kVelocityDefault));
  CHECK(rig.voices->slot_in_use(audio::kPreviewSlot));
  rig.key(Button::F5, false);
}

TEST_CASE("audition: F8 CHANNEL on a stereo file makes the audition play that channel (OS-LOADCH); a mono file is not affected") {
  Rig rig;
  std::vector<std::uint8_t> st = wav_bytes(2000, 44100, 2);  // L = R = ramp: turn the right channel into -ramp
  for (std::uint32_t i = 0; i < 2000; ++i) {
    const std::uint32_t at = 44 + i * 4 + 2;
    const std::int16_t r = static_cast<std::int16_t>(-ramp(i));
    st[at] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(r) & 0xFF);
    st[at + 1] = static_cast<std::uint8_t>(static_cast<std::uint16_t>(r) >> 8);
  }
  write_file(rig.root / "a.wav", st);
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select(0);
  rig.key(Button::F8, true);  // MIX -> LEFT
  rig.key(Button::F8, false);
  rig.key(Button::F8, true);  // -> RIGHT
  rig.key(Button::F8, false);
  CHECK(rig.app->state().wav.channel == 2u);
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
  CHECK(d.length == 2000u);
  CHECK(d.data[10] == static_cast<std::int16_t>(-ramp(10)));
  CHECK(d.data[1999] == static_cast<std::int16_t>(-ramp(1999)));
  rig.key(Button::F5, false);
}

TEST_CASE("audition: nothing is read or bound while a live note is held or a voice reads the descriptor, and it goes on when the window opens") {
  Rig rig;
  write_file(rig.root / "a.wav", wav_bytes(2000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.sink.idle = false;  // a pad is held
  rig.key(Button::F5, true);
  rig.passes(20);
  CHECK(rig.sink.log.empty());
  CHECK(rig.voices->sample(audio::kPreviewSlot).length == 0u);
  CHECK(std::all_of(rig.preview.begin(), rig.preview.end(), [](std::int16_t v) { return v == 0; }));  // the buffer was not written
  rig.sink.idle = true;
  NEED(rig.until_on());
  rig.key(Button::F5, false);
  // A voice still reads the descriptor (the previous note's release): a second audition waits for it.
  CHECK(rig.voices->note_on(0, audio::kRootNote, audio::kPreviewSlot, audio::kVelocityDefault));
  const std::size_t before = rig.sink.log.size();
  rig.key(Button::F5, true);
  rig.passes(30);
  CHECK(rig.sink.log.size() == before);
  rig.voices->cut(0);
  std::int16_t block[audio::VoicePool::kBlock];
  (void)block;
  for (unsigned i = 0; i < 4; ++i) {  // render the declick tail out
    rig.voices->begin_block();
    rig.voices->render(0, audio::VoicePool::kBlock);
    rig.voices->end_block();
  }
  CHECK_FALSE(rig.voices->slot_in_use(audio::kPreviewSlot));
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == before + 1u);
  rig.key(Button::F5, false);
}

TEST_CASE("audition: a move of the cursor restarts it on the new file; leaving the screen and a lost input end it; a folder or an empty list plays nothing") {
  Rig rig;
  write_file(rig.root / "a.wav", wav_bytes(2000));
  write_file(rig.root / "b.wav", wav_bytes(2000));
  fs::create_directories(rig.root / "dir");
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 3u);  // dir, a, b
  rig.key(Button::F5, true);  // the cursor is on the folder
  rig.passes(5);
  CHECK(rig.sink.log.empty());
  CHECK(std::string(rig.app->message()) == "A FOLDER: PRESS ENTER");
  rig.key(Button::F5, false);
  rig.select(1);
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  rig.select(2);  // the next file: the first one's note ends and the next file's starts, F5 still down (OS-BUG-PREVLAT: the preview follows the cursor)
  rig.passes(10);
  NEED(rig.sink.log.size() == 3u);
  CHECK(rig.sink.log[1] == 0);
  CHECK(rig.sink.log[2] == 1);
  rig.passes(5);
  CHECK(rig.sink.log.size() == 3u);  // and then it plays on, held
  rig.key(Button::F5, false);
  NEED(rig.sink.log.size() == 4u);
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == 5u);
  rig.go(ScreenId::Pattern);  // leaving the screen
  rig.passes(2);
  NEED(rig.sink.log.size() == 6u);
  CHECK(rig.sink.log[5] == 0);
  rig.key(Button::F5, false);
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.select(1);
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  rig.app->reset_input();  // the input was lost
  CHECK(rig.sink.log.back() == 0);
}

TEST_CASE("audition: a file that is not a WAV says so and plays nothing; no preview memory says so") {
  {
    Rig rig;
    write_file(rig.root / "bad.wav", std::vector<std::uint8_t>(200, 7));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.key(Button::F5, true);
    rig.passes(20);
    CHECK(rig.sink.log.empty());
    CHECK(std::string(rig.app->message()).find("PREVIEW FAILED") == 0u);
    rig.key(Button::F5, false);
  }
  {
    Rig rig(4000, false);
    write_file(rig.root / "a.wav", wav_bytes(100));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.key(Button::F5, true);
    CHECK(std::string(rig.app->message()) == "NO PREVIEW MEMORY");
  }
}

// ---- OS-BUG-PREVLAT: the streamed start and the cursor that is followed ----------------------------------------------------------------------------------------------

namespace {

// A 24-bit WAV (interleaved channels, the ramp in the top 16 bits).
std::vector<std::uint8_t> wav24_bytes(std::uint32_t frames, std::uint32_t rate, unsigned channels) {
  std::vector<std::uint8_t> v;
  tag(v, "RIFF");
  put32(v, 36 + frames * 3 * channels);
  tag(v, "WAVE");
  tag(v, "fmt ");
  put32(v, 16);
  put16(v, 1);
  put16(v, channels);
  put32(v, rate);
  put32(v, rate * 3 * channels);
  put16(v, 3 * channels);
  put16(v, 24);
  tag(v, "data");
  put32(v, frames * 3 * channels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    for (unsigned c = 0; c < channels; ++c) {
      v.push_back(0);
      put16(v, static_cast<std::uint16_t>(ramp(i)));
    }
  }
  return v;
}

constexpr std::int16_t kStale = 0x1111;  // what the last preview left in the buffer

bool all_zero(const std::int16_t* p, std::uint32_t from, std::uint32_t to) {
  for (std::uint32_t i = from; i < to; ++i) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

}  // namespace

TEST_CASE("audition: a long file starts before the read is done; the unread tail is silent, never the last preview's audio, and the file is whole at the end") {
  Rig rig(40000);
  std::fill(rig.preview.begin(), rig.preview.end(), kStale);
  write_file(rig.root / "long.wav", wav_bytes(30000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  app::PreviewJob& job = rig.bank.preview_job();
  const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
  CHECK(d.length == 30000u);  // the frames planned, not the frames read
  CHECK(job.written() < 30000u);  // the read is still going
  CHECK(job.written() >= 4096u);
  NEED(d.data == rig.preview.data());
  const std::uint32_t w = job.written();
  CHECK(all_zero(d.data, w, w + 4000u));  // zero ahead of the read, in the first stretch
  for (std::uint32_t i = 0; i < w; ++i) {
    NEED(d.data[i] == ramp(i));
  }
  CHECK(rig.sink.log.size() == 1u);
  for (unsigned i = 0; i < 40 && job.written() < 30000u; ++i) {
    // The next 4096 samples the read will write are zero or already read at every pass, never the stale audio: the head (under 3k samples a pass) cannot reach the stale part.
    const std::uint32_t at = job.written();
    CHECK(all_zero(d.data, at, std::min<std::uint32_t>(at + 4096u, 30000u)));
    rig.pass();
  }
  rig.passes(3);
  for (std::uint32_t i = 0; i < 30000u; ++i) {
    NEED(d.data[i] == ramp(i));
  }
  CHECK(rig.sink.log.size() == 1u);  // one note, held
  rig.key(Button::F5, false);
  CHECK(rig.sink.log.size() == 2u);
}

TEST_CASE("audition: a clip under 4096 frames, and a read too slow for the head, start at Done") {
  {
    Rig rig(40000);
    write_file(rig.root / "short.wav", wav_bytes(3000));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.key(Button::F5, true);
    NEED(rig.until_on());
    CHECK(rig.bank.preview_job().written() == 3000u);  // whole before the note
    CHECK(rig.voices->sample(audio::kPreviewSlot).length == 3000u);
    rig.key(Button::F5, false);
  }
  {
    // 96 kHz 24-bit stereo: 6 bytes a frame, so a pass of 8 KB reads 1.3k frames and the head wants 1.5k: the start waits for rate / 8 frames and for the projection to hold.
    Rig rig(40000);
    write_file(rig.root / "hi.wav", wav24_bytes(40000, 96000, 2));
    rig.go(ScreenId::SampleLoad);
    rig.settle();
    rig.key(Button::F5, true);
    NEED(rig.until_on());
    const std::uint32_t w = rig.bank.preview_job().written();
    CHECK(w >= 12000u);  // rate / 8
    CHECK(w < 40000u);   // still a start before the end of the read
    const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
    CHECK(d.length == 40000u);
    rig.key(Button::F5, false);
  }
}

TEST_CASE("audition: a read that fails or is truncated while the note plays does not drop the note; the rest of the clip is silent and the strip says why") {
  Rig rig(40000);
  std::fill(rig.preview.begin(), rig.preview.end(), kStale);
  std::vector<std::uint8_t> bytes = wav_bytes(30000);
  bytes.resize(44 + 12000u * 2u);  // the header says 30000 frames, the file ends at 12000
  write_file(rig.root / "cut.wav", bytes);
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == 1u);
  rig.passes(10);  // the read reaches the end of the file: Truncated
  CHECK(std::string(rig.app->message()).find("PREVIEW FAILED") == 0u);
  CHECK(rig.sink.log.size() == 1u);  // the note was not let go
  const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
  CHECK(d.length == 30000u);
  CHECK(d.data[0] == ramp(0));
  CHECK(d.data[11999] == ramp(11999));
  CHECK(all_zero(d.data, 12000u, 30000u));  // no stale audio behind what was read
  rig.key(Button::F5, false);
  CHECK(rig.sink.log.size() == 2u);
  CHECK(rig.sink.log[1] == 0);
}

TEST_CASE("audition: with F5 held the preview follows the cursor, one read at a time; a folder stops it and the next file starts it") {
  Rig rig(40000);
  fs::create_directories(rig.root / "dir");
  write_file(rig.root / "a.wav", wav_bytes(30000));
  write_file(rig.root / "b.wav", wav_bytes(25000));
  rig.go(ScreenId::SampleLoad);
  rig.settle();
  NEED(rig.app->model().files->index().count() == 3u);  // dir, a, b
  rig.select(1);
  rig.key(Button::F5, true);
  NEED(rig.until_on());
  CHECK(rig.voices->sample(audio::kPreviewSlot).length == 30000u);
  std::fill(rig.preview.begin(), rig.preview.end(), kStale);  // a and b hold the same ramp: only a marker in the reused buffer tells a stale read from b's own data
  rig.select(2);  // b, while a is still being read
  rig.passes(3);
  NEED(rig.sink.log.size() >= 2u);
  CHECK(rig.sink.log[1] == 0);  // a's note let go
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == 3u);
  CHECK(rig.voices->sample(audio::kPreviewSlot).length == 25000u);
  {
    app::PreviewJob& job = rig.bank.preview_job();
    const audio::SampleDesc& d = rig.voices->sample(audio::kPreviewSlot);
    const std::uint32_t w = job.written();
    CHECK(w < 25000u);  // b is still being read: the zero tail is what the head would meet
    CHECK(all_zero(d.data, w, std::min<std::uint32_t>(w + 4000u, 25000u)));  // zeros right after the restart, not a's or the marker's audio
  }
  rig.passes(15);
  for (std::uint32_t i = 0; i < 25000u; ++i) {
    NEED(rig.voices->sample(audio::kPreviewSlot).data[i] == ramp(i));
  }
  rig.select(0);  // the folder
  rig.passes(5);
  NEED(rig.sink.log.size() == 4u);
  CHECK(rig.sink.log[3] == 0);
  CHECK(rig.app->state().wav.audition_held);
  rig.select(1);  // a file again: it plays
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == 5u);
  CHECK(rig.voices->sample(audio::kPreviewSlot).length == 30000u);
  rig.key(Button::F5, false);
  CHECK(rig.sink.log.back() == 0);
  // A cursor that runs through the files before the first read began (the window is closed): only the last file is read.
  rig.sink.idle = false;
  const std::size_t before = rig.sink.log.size();
  rig.key(Button::F5, true);
  rig.select(2);
  rig.select(1);
  rig.select(2);
  rig.passes(5);
  rig.sink.idle = true;
  NEED(rig.until_on());
  CHECK(rig.sink.log.size() == before + 1u);
  CHECK(rig.voices->sample(audio::kPreviewSlot).length == 25000u);
  rig.key(Button::F5, false);
}
