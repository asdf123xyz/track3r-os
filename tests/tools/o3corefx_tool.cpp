// SPDX-License-Identifier: GPL-3.0-or-later
// Command-line helper for tests/corefx_openmpt_oracle.py (OS-COREFX CF1): plays a MOD or S3M through the whole chain of the Modern engine, as the OS does, and writes what the engine sounds like.
// Not part of the OS.
//
//   o3corefx_tool render FILE OUT.raw TICKS [noengine]
//       imports FILE (the importer of OS-MODIMP; an S3M for the Modern engine, a MOD with `noengine`), gives every instrument its PCM, plays pattern 1 (the song's first piece) with the sequencer and the
//       engine (Tick events on, the tracker FX processor in the dispatch) for TICKS ticks of the module's own (speed x rows, a tick of 2.5 / BPM seconds at the module's tempo, the project's BPM mode),
//       and writes the engine's output as raw little-endian 16-bit stereo to OUT.raw. Prints "render ok rate=<Hz> blocks=<n> tempo_x100=<n>" on success; exit 1 and the reason otherwise.
#include <o3os/audio/engine.h>
#include <o3os/audio/voices.h>
#include <o3os/formats/mod/import.h>
#include <o3os/hal/audio.h>
#include <o3os/model/project.h>
#include <o3os/seq/engine_link.h>
#include <o3os/seq/sequencer.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

namespace {

namespace haudio = o3os::hal::audio;
using namespace o3os;

std::uint32_t fake_cycles_counter = 0;
std::uint32_t fake_cycles() noexcept { return fake_cycles_counter += 11u; }

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  std::uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  std::fclose(f);
  return true;
}

int render(int argc, char** argv) {
  using formats::Progress;
  if (argc < 5) {
    std::fprintf(stderr, "usage: o3corefx_tool render FILE OUT.raw TICKS [noengine]\n");
    return 2;
  }
  std::vector<std::uint8_t> bytes;
  if (!read_file(argv[2], bytes)) {
    std::fprintf(stderr, "cannot read %s\n", argv[2]);
    return 2;
  }
  const unsigned ticks = static_cast<unsigned>(std::atoi(argv[4]));
  formats::mod::ImportOptions opt;
  for (int a = 5; a < argc; ++a) {
    if (std::strcmp(argv[a], "noengine") == 0) {
      opt.engine_fx = formats::mod::EngineFx::Off;
    } else {
      std::fprintf(stderr, "unknown option %s\n", argv[a]);
      return 2;
    }
  }
  formats::med::SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  auto arena = std::make_unique<model::ModelArena>();
  auto importer = std::make_unique<formats::mod::Importer>();
  importer->begin(src, *arena, opt);
  while (importer->step(4096) == Progress::Running) {
  }
  if (importer->progress() != Progress::Done) {
    std::printf("import failed: %s\n", importer->status().detail);
    return 1;
  }
  model::Project& project = arena->project;

  // the instruments: the importer's PCM, looped as the instrument says, at the unity gain of a velocity (the oracle's notes name theirs)
  std::vector<std::int16_t> pcm;
  struct Slot {
    std::size_t at;
    std::uint32_t frames;
  };
  Slot slots[formats::mod::caps::kSlots] = {};
  for (unsigned i = 0; i < formats::mod::caps::kSlots; ++i) {
    const formats::med::InstReport& ir = importer->report().med.instrument[i];
    if (ir.result != formats::med::InstResult::Sample || ir.frames == 0) {
      continue;
    }
    slots[i].at = pcm.size();
    slots[i].frames = ir.frames;
    pcm.resize(pcm.size() + ir.frames);
    if (!importer->read_pcm(i, 0, ir.frames, pcm.data() + slots[i].at).ok()) {
      std::printf("pcm failed\n");
      return 1;
    }
  }
  static audio::TapMem taps;
  static audio::SeqEvent events[audio::kMaxEventsPerBlock];
  auto pool = std::make_unique<audio::VoicePool>();
  auto engine = std::make_unique<audio::Engine>(&fake_cycles, taps);
  auto sequencer = std::make_unique<seq::Sequencer>(project);
  for (unsigned i = 0; i < formats::mod::caps::kSlots; ++i) {
    const model::Instrument* in = project.instrument(i);
    if (slots[i].frames == 0 || in == nullptr) {
      continue;
    }
    audio::SampleDesc d;
    d.data = pcm.data() + slots[i].at;
    d.length = slots[i].frames;
    if (in->sampler.play_mode != 0 && in->sampler.loop_end > in->sampler.loop_start) {
      d.loop_start = static_cast<std::uint32_t>((static_cast<std::uint64_t>(in->sampler.loop_start) * slots[i].frames + 32767u) / 65535u);
      d.loop_end = static_cast<std::uint32_t>((static_cast<std::uint64_t>(in->sampler.loop_end) * slots[i].frames + 32767u) / 65535u);
    }
    pool->set_sample(i, d);
  }
  engine->attach_voices(pool.get());
  engine->attach_events(seq::event_source(*sequencer, events, audio::kMaxEventsPerBlock));
  seq::Sequencer::Config cfg = sequencer->config();
  cfg.emit_ticks = true;
  sequencer->configure(cfg);

  alignas(8) std::int16_t in[haudio::kBlockSamples] = {};
  alignas(8) std::int16_t out[haudio::kBlockSamples] = {};
  std::FILE* f = std::fopen(argv[3], "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "cannot write %s\n", argv[3]);
    return 2;
  }
  if (!sequencer->commands().push(seq::Command{seq::Command::Kind::PlayPattern, 1, 0, 0})) {
    return 1;
  }
  const std::uint32_t rate = (haudio::sample_rate_mhz() + 500u) / 1000u;
  // the oracle's modules keep the start tempo: a tick is 2.5 / BPM seconds, 20 ms at the 125 BPM and speed 6 of both formats
  const std::uint32_t blocks = static_cast<std::uint32_t>(static_cast<double>(ticks) * 0.02 * static_cast<double>(haudio::sample_rate_mhz()) / 1000.0 / static_cast<double>(haudio::kBlock)) + 2u;
  for (std::uint32_t b = 0; b < blocks; ++b) {
    haudio::BlockReport report{};
    const haudio::BlockIo io{in, out, 0, &report};
    engine->render_block(io);
    std::fwrite(out, sizeof out[0], haudio::kBlockSamples, f);
  }
  std::fclose(f);
  std::printf("render ok rate=%u blocks=%u tempo_x100=%u\n", rate, blocks, project.tempo_x100());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::strcmp(argv[1], "render") == 0) {
    return render(argc, argv);
  }
  std::fprintf(stderr, "usage: o3corefx_tool render FILE OUT.raw TICKS [noengine]\n");
  return 2;
}
