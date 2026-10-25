// SPDX-License-Identifier: GPL-3.0-or-later
// The Record screen's gallery and budget states (OS-011c): a real recorder over a real graph, taught a take by rendering a few seconds of a synthetic signal through it, in each
// state the screen has (idle, recording, a take under review, the RADIO page with a chip and without, RESAMPLE, no recorder) and the worst case of the widest text. Everything is a
// plain object read by the same render the device runs.
#include "screens_app.h"

#include <o3os/app/mix_board.h>
#include <o3os/app/record_view.h>
#include <o3os/app/recorder.h>
#include <o3os/app/screens/scene.h>
#include <o3os/audio/graph.h>
#include <o3os/audio/tap_mem.h>
#include <o3os/audio/voices.h>
#include <o3os/hal/capture.h>
#include <o3os/model/project.h>

#include <cmath>
#include <memory>
#include <vector>

namespace o3os::sim {

using app::Model;
using app::Recorder;
using app::RecMode;
using app::ScreenId;
using app::ViewState;

namespace {

namespace cap = hal::capture;
namespace ha = hal::audio;

std::uint32_t g_cycles = 0;
std::uint32_t fake_cycles() noexcept { return ++g_cycles; }

// A codec and a radio that say what the state wants shown.
class FakeHw final : public cap::CaptureHw {
 public:
  bool select_source(cap::Source, std::uint8_t) noexcept override { return true; }
  bool set_gain(cap::Source, std::uint8_t) noexcept override { return true; }
  void radio_power(bool) noexcept override {}
  void radio_region(cap::Region) noexcept override {}
  void radio_tune(std::uint16_t) noexcept override {}
  void radio_seek(bool) noexcept override {}
  void service(std::uint64_t) noexcept override {}
  cap::RadioStatus radio_status() const noexcept override { return status; }
  cap::RadioStatus status;
};

// What each world records and shows.
enum class Kind : std::uint8_t { Idle, Recording, Review, Radio, RadioAbsent, Resample, Worst, LoopRow, LoopReview, LoopMuted, LoopOverdub, LevelRows, LevelWaiting };

struct World {
  std::unique_ptr<audio::TapMem> taps = std::make_unique<audio::TapMem>();
  std::unique_ptr<audio::Graph> graph = std::make_unique<audio::Graph>(&fake_cycles, *taps);
  std::unique_ptr<model::ModelArena> project = std::make_unique<model::ModelArena>();
  std::unique_ptr<audio::VoicePool> voices = std::make_unique<audio::VoicePool>();
  std::unique_ptr<app::SampleBankMem> bank_mem = std::make_unique<app::SampleBankMem>();
  std::vector<std::int16_t> memory = std::vector<std::int16_t>(400000);  // about 9 s of room
  app::SampleBank bank;
  app::MixBoard mix;
  FakeHw hw;
  std::unique_ptr<Recorder> rec = std::make_unique<Recorder>();
  alignas(8) std::int16_t in[ha::kBlockSamples] = {};
  alignas(8) std::int16_t out[ha::kBlockSamples] = {};
  ha::BlockReport report = {};
  std::uint64_t now = 1000;
  std::uint32_t block = 0;
  // OS-LOOPER: a transport that plays, for a loop take: a Step event every kStepFrames samples, index 0..63 (a bar is 16 of them). The engine hands these to the REC tap and the loop player.
  static constexpr std::uint32_t kStepFrames = 2000;
  bool transport = false;
  std::uint64_t t_samples = 0;
  std::uint64_t next_step_at = 0;
  std::uint32_t step_index = 0;

  // A music-like burst train: a decaying 220 Hz tone struck every half second, with a little of its octave and a slow tremolo on the right.
  bool quiet = false;  // OS-SREC-LEVEL: digital silence (the take that listens has heard nothing yet)
  void make_block() {
    if (quiet) {
      for (std::int16_t& s : in) {
        s = 0;
      }
      return;
    }
    for (std::uint32_t i = 0; i < ha::kBlock; ++i) {
      const double t = static_cast<double>(block * ha::kBlock + i) / 44100.0;
      const double since = std::fmod(t, 0.5);
      const double env = std::exp(-6.0 * since);
      const double v = env * (std::sin(6.2831853 * 220.0 * t) + 0.3 * std::sin(6.2831853 * 440.0 * t));
      in[2u * i] = static_cast<std::int16_t>(15000.0 * v);
      in[2u * i + 1u] = static_cast<std::int16_t>(11000.0 * v * (0.8 + 0.2 * std::sin(6.2831853 * 3.0 * t)));
    }
  }
  void run(unsigned blocks, RecMode mode, bool in_view = true) {
    for (unsigned b = 0; b < blocks; ++b) {
      make_block();
      ++block;
      if (transport) {
        while (next_step_at < t_samples + ha::kBlock) {
          const std::uint32_t offset = static_cast<std::uint32_t>(next_step_at - t_samples);
          graph->rec().on_step(step_index % 64u, offset);
          graph->loop().on_step(step_index % 64u, offset);
          ++step_index;
          next_step_at += kStepFrames;
        }
      }
      t_samples += ha::kBlock;
      report = ha::BlockReport{};
      const ha::BlockIo io{in, out, 0, &report};
      graph->render(io);
      if (b % 8u == 7u) {
        now += 12;
        rec->poll(now, in_view, mode, transport);
      }
    }
    now += 12;
    rec->poll(now, in_view, mode, transport);
  }

  explicit World(Kind kind) {
    project->project.reset();
    bank.init(*voices, *project, *bank_mem, memory.data(), static_cast<std::uint32_t>(memory.size()));
    Recorder::Wiring w;
    w.hw = &hw;
    w.graph = graph.get();
    w.params = &graph->params();
    w.bank = &bank;
    w.mix = &mix;
    rec->attach(w);
    const RecMode mode = kind == Kind::Radio || kind == Kind::RadioAbsent ? RecMode::Radio : (kind == Kind::Resample ? RecMode::Resample : RecMode::Sources);
    rec->cfg.source = cap::Source::LineMix;
    rec->cfg.gain[0] = 40;
    rec->cfg.monitor = true;
    if (kind == Kind::Resample) {
      rec->cfg.tap = app::RecTapKind::Track;
      rec->cfg.track = 2;
    }
    if (kind == Kind::Worst) {
      rec->cfg.stop = app::RecStop::Bars;  // the widest STOP AT text (OS-REC2)
      rec->cfg.stop_bars = 16;
    }
    if (kind == Kind::LoopRow) {
      rec->cfg.loop_bars = 4;  // OS-LOOPER: the LOOP row set, ALIGN and STOP AT dim
    }
    if (kind == Kind::LevelRows || kind == Kind::LevelWaiting) {
      rec->cfg.align = app::RecAlign::Level;  // OS-SREC-LEVEL: THRESHOLD and PRE-ROLL follow START ON
      rec->cfg.level_db = -24;
      rec->cfg.preroll = 2;
    }
    if (kind == Kind::Radio || kind == Kind::RadioAbsent || kind == Kind::Worst) {
      rec->cfg.region = cap::Region::Japan;
      rec->cfg.radio_freq = 9910;
      hw.status.state = kind == Kind::RadioAbsent ? cap::RadioStatus::State::Absent : cap::RadioStatus::State::Ready;
      hw.status.region = cap::Region::Japan;
      hw.status.freq = 9910;
      hw.status.rssi = kind == Kind::RadioAbsent ? 0 : 42;
      hw.status.stereo = kind != Kind::RadioAbsent;
      hw.status.generation = 3;
    }
    run(40, mode);  // 0.1 s: the source settles (200 ms: 140 blocks more)
    run(160, mode);
    if (kind == Kind::Recording || kind == Kind::Review || kind == Kind::Worst) {
      (void)rec->arm(false);
      run(kind == Kind::Recording ? 2000u : 3000u, mode);  // 2.9 s or 4.3 s
      if (kind != Kind::Recording) {
        rec->stop();
        run(2, mode);
        rec->set_trim_start(rec->frames() / 8u);
        rec->set_trim_end(rec->frames() - rec->frames() / 5u);
      }
    }
    if (kind == Kind::LevelWaiting) {  // armed and listening, on a silent input, the transport stopped
      quiet = true;
      (void)rec->arm(false);
      run(40, mode);
    }
    if (kind == Kind::LoopReview || kind == Kind::LoopMuted || kind == Kind::LoopOverdub) {
      // OS-LOOPER: a take of two bars on a transport that plays, ended by itself on its bar; the loop plays on (a few passes), muted in the second state.
      rec->cfg.loop_bars = 2;
      rec->cfg.loop_vol = 80;
      transport = true;
      next_step_at = t_samples + 1000u;
      step_index = 3;  // (the take waits for the first bar)
      (void)rec->arm(true, 12000);  // (the tempo the loop is made at)
      run(1800, mode);
      run(400, mode);
      if (kind == Kind::LoopMuted) {
        rec->toggle_loop_mute();
        run(10, mode);
      }
      if (kind == Kind::LoopOverdub) {  // OS-LOOPER L2c: a session is on: the header says OVERDUB, F1 is in force, F5 UNDO LYR is lit once the first block was written
        (void)rec->overdub_toggle();
        run(40, mode);
      }
    }
    // The meters: what the REC bus carried over the last windows, as the app would have fed them.
    for (unsigned i = 0; i < 6; ++i) {
      mix.meter[audio::meter_ch::kRecL].update(ui::plot::MeterReading{0.46f, 0.21f});
      mix.meter[audio::meter_ch::kRecR].update(ui::plot::MeterReading{0.34f, 0.15f});
      mix.meter[audio::meter_ch::kMainL].update(ui::plot::MeterReading{0.7f, 0.3f});
      mix.meter[audio::meter_ch::kMainR].update(ui::plot::MeterReading{0.6f, 0.28f});
    }
    if (kind == Kind::Worst) {
      for (unsigned i = 0; i < 6; ++i) {
        mix.meter[audio::meter_ch::kRecL].update(ui::plot::MeterReading{1.0f, 0.9f});  // the clip lamp
        mix.meter[audio::meter_ch::kRecR].update(ui::plot::MeterReading{1.0f, 0.9f});
      }
    }
  }

  Model model() const {
    Model m{};
    m.project = &project->project;
    m.mix = &mix;
    m.rec = rec.get();
    m.samples = &bank;
    m.device.init(core::ParamKind::Device);
    return m;
  }
};

World& world(Kind kind) {
  static std::unique_ptr<World> worlds[13];
  std::unique_ptr<World>& w = worlds[static_cast<unsigned>(kind)];
  if (!w) {
    w = std::make_unique<World>(kind);
  }
  return *w;
}

ViewState record_view(RecMode mode, int row) {
  ViewState v;
  v.screen = ScreenId::Record;
  v.page[app::screen_index(ScreenId::Record)] = static_cast<std::uint8_t>(mode);
  v.record.row[static_cast<unsigned>(mode)] = static_cast<std::uint8_t>(row);
  return v;
}

void draw(ui::Renderer& r, const Model& m, const ViewState& v) { app::render_scene(m, v, r); }

void draw_idle(ui::Renderer& r, unsigned) { draw(r, world(Kind::Idle).model(), record_view(RecMode::Sources, 1)); }
void draw_recording(ui::Renderer& r, unsigned) {
  ViewState v = record_view(RecMode::Sources, 1);
  v.playing = true;
  v.play.playing = true;
  draw(r, world(Kind::Recording).model(), v);
}
void draw_review(ui::Renderer& r, unsigned) { draw(r, world(Kind::Review).model(), record_view(RecMode::Sources, 4)); }
// OS-LOOPER: the LOOP row set to 4 BARS (row under the cursor), and the review of a loop: LOOP VOL in place of the trim rows, the header naming the loop, F1 OVERDUB dim and F8 LOOP ON / MUTED.
void draw_loop_row(ui::Renderer& r, unsigned) { draw(r, world(Kind::LoopRow).model(), record_view(RecMode::Sources, 5)); }
void draw_loop_review(ui::Renderer& r, unsigned) {
  ViewState v = record_view(RecMode::Sources, 6);
  v.playing = true;
  v.play.playing = true;
  v.play.tempo_x100 = 12000;
  draw(r, world(Kind::LoopReview).model(), v);
}
void draw_loop_muted(ui::Renderer& r, unsigned) {
  ViewState v = record_view(RecMode::Sources, 6);
  v.playing = true;
  v.play.playing = true;
  v.play.tempo_x100 = 14000;  // the tempo moved since the loop was made
  draw(r, world(Kind::LoopMuted).model(), v);
}
void draw_loop_overdub(ui::Renderer& r, unsigned) {
  ViewState v = record_view(RecMode::Sources, 7);  // FEEDBACK under the cursor
  v.playing = true;
  v.play.playing = true;
  v.play.tempo_x100 = 12000;
  draw(r, world(Kind::LoopOverdub).model(), v);
}
// OS-SREC-LEVEL: START ON = LEVEL with THRESHOLD under the cursor (row 4), and the take armed and listening (WAITING FOR LEVEL).
void draw_level_rows(ui::Renderer& r, unsigned) { draw(r, world(Kind::LevelRows).model(), record_view(RecMode::Sources, 4)); }
void draw_level_waiting(ui::Renderer& r, unsigned) { draw(r, world(Kind::LevelWaiting).model(), record_view(RecMode::Sources, 3)); }
void draw_resample(ui::Renderer& r, unsigned) { draw(r, world(Kind::Resample).model(), record_view(RecMode::Resample, 0)); }
void draw_radio(ui::Renderer& r, unsigned) { draw(r, world(Kind::Radio).model(), record_view(RecMode::Radio, 0)); }
void draw_radio_absent(ui::Renderer& r, unsigned) { draw(r, world(Kind::RadioAbsent).model(), record_view(RecMode::Radio, 0)); }
void draw_none(ui::Renderer& r, unsigned) {
  Model m{};
  m.device.init(core::ParamKind::Device);
  draw(r, m, record_view(RecMode::Sources, 0));
}

// The dearest it gets: a take under review on the RADIO page with the widest region name, both trim rows, every meter hot, swept over the rows of every page.
void draw_worst(ui::Renderer& r, unsigned variant) {
  const RecMode mode = static_cast<RecMode>(variant % 3u);
  World& w = world(Kind::Worst);
  ViewState v = record_view(mode, static_cast<int>(variant / 3u));
  v.page[app::screen_index(ScreenId::Record)] = static_cast<std::uint8_t>(mode);
  v.playing = true;
  v.play.playing = true;
  Model m = w.model();
  draw(r, m, v);
}

}  // namespace

const ScreenState kRecordStates[14] = {
    {"idle", true, 1, kBudgetWords, draw_idle},
    {"recording", true, 1, kBudgetWords, draw_recording},
    {"review", true, 1, kBudgetWords, draw_review},
    {"resample", true, 1, kBudgetWords, draw_resample},
    {"radio", true, 1, kBudgetWords, draw_radio},
    {"radio_absent", true, 1, kBudgetWords, draw_radio_absent},
    {"none", true, 1, kBudgetWords, draw_none},
    {"worst", true, 3u * app::kRecMaxRows, kBudgetWords, draw_worst},
    {"loop_row", true, 1, kBudgetWords, draw_loop_row},
    {"loop_review", true, 1, kBudgetWords, draw_loop_review},
    {"loop_muted", true, 1, kBudgetWords, draw_loop_muted},
    {"loop_overdub", true, 1, kBudgetWords, draw_loop_overdub},
    {"level_rows", true, 1, kBudgetWords, draw_level_rows},
    {"level_waiting", true, 1, kBudgetWords, draw_level_waiting},
};

}  // namespace o3os::sim
