// SPDX-License-Identifier: GPL-3.0-or-later
// Command-line helper for tests/e2e_open303_ref.py (OS-E2E-9): renders the ORIGINAL double-precision Open303 (third_party/open303, Robin Schmidt, MIT; tests/support/open303_ref.*), the oracle of the
// TB-303 module, for a list of notes, and writes what it sounds like. Not part of the OS.
//
//   o3ref303_tool render RATE SAMPLES OUT.f32 WAVE CUTOFF RESONANCE ENVMOD DECAY ACCENT SLIDE VOLUME [AT,NOTE,VELOCITY ...]
//       RATE the sample rate in Hz (a number: 44117.647...), SAMPLES the length, OUT.f32 little-endian float32 mono (the double sample rounded). The parameters are the module's
//       (modules/tb303/module.json): waveform 0..1, cutoff Hz, resonance %, env mod %, decay ms, accent %, slide ms, volume dB. Each event is `AT,NOTE,VELOCITY`: at sample AT the note NOTE
//       (a MIDI number) starts at VELOCITY (1..127, from 100 an accent), or, with velocity 0, the note that is held ends. A note that starts while another is held first ends it (the
//       sequencer's rule for the module: a new note releases the held one and starts again; Open303 would slide on overlapping notes).
//   Prints "render ok samples=<n>" on success; exit 1 and the reason otherwise.
#include "../support/open303_ref.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

struct Event {
  long at;
  int note;
  int velocity;
};

}  // namespace

int main(int argc, char** argv) {
  if (argc < 13 || std::strcmp(argv[1], "render") != 0) {
    std::fprintf(stderr, "usage: o3ref303_tool render RATE SAMPLES OUT.f32 WAVE CUTOFF RESONANCE ENVMOD DECAY ACCENT SLIDE VOLUME [AT,NOTE,VELOCITY ...]\n");
    return 2;
  }
  const double rate = std::atof(argv[2]);
  const long samples = std::atol(argv[3]);
  if (rate < 8000.0 || samples <= 0 || samples > 10000000) {
    std::fprintf(stderr, "render: a rate of 8 kHz or more and 1..10,000,000 samples\n");
    return 2;
  }
  open303_ref::Voice voice(rate);
  voice.set_waveform(std::atof(argv[5]));
  voice.set_cutoff(std::atof(argv[6]));
  voice.set_resonance(std::atof(argv[7]));
  voice.set_env_mod(std::atof(argv[8]));
  voice.set_decay(std::atof(argv[9]));
  voice.set_accent(std::atof(argv[10]));
  voice.set_slide_time(std::atof(argv[11]));
  voice.set_volume(std::atof(argv[12]));
  std::vector<Event> events;
  for (int a = 13; a < argc; ++a) {
    Event e{};
    if (std::sscanf(argv[a], "%ld,%d,%d", &e.at, &e.note, &e.velocity) != 3 || e.at < 0 || e.note < 0 || e.note > 127 || e.velocity < 0 || e.velocity > 127) {
      std::fprintf(stderr, "render: '%s' is not AT,NOTE,VELOCITY\n", argv[a]);
      return 2;
    }
    events.push_back(e);
  }
  std::vector<float> out(static_cast<std::size_t>(samples));
  std::size_t next = 0;
  int held = -1;
  for (long i = 0; i < samples; ++i) {
    while (next < events.size() && events[next].at <= i) {
      const Event& e = events[next++];
      if (e.velocity == 0) {
        if (held >= 0) {
          voice.note_on(held, 0);
          held = -1;
        }
      } else {
        if (held >= 0) {
          voice.note_on(held, 0);
        }
        voice.note_on(e.note, e.velocity);
        held = e.note;
      }
    }
    out[static_cast<std::size_t>(i)] = static_cast<float>(voice.sample());
  }
  std::FILE* f = std::fopen(argv[4], "wb");
  if (f == nullptr || std::fwrite(out.data(), sizeof(float), out.size(), f) != out.size() || std::fclose(f) != 0) {
    std::fprintf(stderr, "render: cannot write %s\n", argv[4]);
    return 1;
  }
  std::printf("render ok samples=%ld\n", samples);
  return 0;
}
