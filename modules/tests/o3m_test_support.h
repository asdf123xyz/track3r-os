// SPDX-License-Identifier: GPL-3.0-or-later
//
// Test support for the modules in modules (OS-012c): a module's host shared object (CMake o3m_<name>_host, the same source as the .tmod) is loaded with dlopen and driven through
// the ABI exactly as the OS drives it (o3m_host, init, set_param, note_on, render, note_off), and its views through module::ViewHost, the OS's own draw-API recorder. Host only;
// each module's test executable (o3m_<name>_test) includes this once.
#pragma once

#include <doctest/doctest.h>

#include <o3os/module/loader.h>
#include <o3os/module/view.h>

#include <dlfcn.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// This tree builds without exceptions, so doctest's REQUIRE is unavailable: NEED checks once and leaves the test case when false.
#define NEED(...)                                           \
  do {                                                      \
    const bool o3_need_ok = static_cast<bool>(__VA_ARGS__); \
    CHECK_MESSAGE(o3_need_ok, #__VA_ARGS__);                \
    if (!o3_need_ok) {                                      \
      return;                                               \
    }                                                       \
  } while (0)

namespace o3m_test {

constexpr float kRate = 44100.0f;
constexpr double kPi = 3.14159265358979323846;

inline float x_exp2(float x) { return std::exp2(x); }
inline float x_log2(float x) { return std::log2(x); }
inline float x_sin(float x) { return std::sin(x); }
inline float x_cos(float x) { return std::cos(x); }
inline void x_log(const char*) {}

inline o3m_host make_host(float rate = kRate) {
  o3m_host h{};
  h.abi_major = O3M_ABI_MAJOR;
  h.abi_minor = O3M_ABI_MINOR;
  h.size = sizeof h;
  h.sample_rate = rate;
  h.log = x_log;
  h.exp2_f = x_exp2;
  h.log2_f = x_log2;
  h.sin_f = x_sin;
  h.cos_f = x_cos;
  h.draw = o3os::module::draw_api();
  return h;
}

// OS-H1 (ABI 1.3): a fake of the app host tables, for the tests of apps. The test sets the clock and the tempo, queues MIDI events, reads back what the app asked (set_tempo calls), and
// chooses which tables an app is lent, as the OS does from the capabilities in the file (make_app_host). One instance at a time: the table functions have no context, like the real ones.
struct FakeApi {
  std::uint32_t now = 0;
  std::uint32_t tempo = 12000;
  int set_tempo_result = O3M_EDIT_OK;
  std::vector<std::uint32_t> tempo_calls;
  std::deque<o3m_midi_event> midi;
  std::uint32_t overflow = 0, sysex = 0;
  o3m_launch_ctx launch{sizeof(o3m_launch_ctx), 0, 0, 0, 0, 0, 0, 0, {0, 0, 0}};
  std::uint32_t reads = 0;  // midi_read calls
  // OS-H2a (ABI 1.4): the audio tap. The test sets what tap_levels answers next, and reads which point the app chose.
  std::uint32_t tap_source = 0;
  std::vector<std::uint32_t> tap_selects;
  std::deque<o3m_tap_levels> tap_levels;  // answers, oldest first; none left = frames 0
  // OS-H2b: the samples. tap_latest answers with the newest frames of `tap_window` (zeros in front when it is shorter, as the real one does right after a select; 0 when it is empty or the
  // arguments break the contract: frames odd or outside 2..2048). tap_stream answers one chunk of `tap_chunks` a call (the oldest dropped and counted in `lost` when it does not fit).
  // `tap_overreturn` makes both claim that many frames without writing (a hostile host).
  struct TapChunk {
    std::vector<std::int16_t> s;
    std::uint32_t lost = 0;
  };
  std::vector<std::int16_t> tap_window;
  std::deque<TapChunk> tap_chunks;
  std::uint32_t tap_overreturn = 0;
  std::uint32_t latest_calls = 0, stream_calls = 0;
  // OS-H2b: a clock that runs while the app works, to test the apps' time guards: every `tick_every`-th call of now_ms advances it by one millisecond (0: it only moves when the test moves it).
  std::uint32_t tick_every = 0, now_calls = 0;
  // OS-H3A (ABI 1.5): a small project for the tests of the apps that edit it. ONE pattern (id `pattern_id`) of `pat_tracks` active tracks, each `pat_len[t]` rows long; the steps are the test's to
  // fill. edit_begin / set_step(s) / edit_end behave like the OS's: a write outside a group is NO_GROUP, bad arguments are RANGE and write nothing, the writes of a group are checked when
  // called and APPLIED at edit_end (the masked fields only), and `end_result` can make edit_end answer something else (then nothing is applied). `groups` lists every group that ended OK, as
  // the writes it held (pattern, track, first row, count, fields), so a test can check one group per APPLY.
  static constexpr std::uint32_t kTracks = 16, kRows = 128;
  std::uint32_t pattern_id = 1;
  std::uint32_t pat_tracks = 4;
  std::uint8_t pat_len[kTracks] = {16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16};
  o3m_step steps[kTracks][kRows] = {};
  struct Write {
    std::uint16_t pattern;
    std::uint8_t track, first, count, fields;
  };
  bool group_open = false;
  std::vector<Write> pending;
  std::vector<o3m_step> pending_steps;
  std::vector<std::vector<Write>> groups;  // the groups that ended OK
  // OS-H3B (ABI 1.6): the instruments 1..64 and their parameters. The test fills `instr[n]` (kind, engine, params); get_param_desc lists them in order, get_instrument_param reads `value`, and
  // set_instrument_param behaves like the OS's: NO_GROUP outside a group, RANGE (nothing written) for an empty or MIDI instrument, an unknown id, a parameter without O3M_PD_APP_WRITABLE, NaN or a
  // value outside min..max; the writes of a group are APPLIED at edit_end (a whole-number parameter, flagged STEPPED or ENUM, stores the value rounded). `param_groups` lists every group that ended OK
  // as its parameter writes, in call order.
  struct FakeParam {
    std::uint16_t id = 0;
    float min = 0, max = 1, def = 0, value = 0;
    std::uint16_t flags = O3M_PD_APP_WRITABLE;
    std::uint8_t group = 0;
    char name[O3M_PARAM_DESC_NAME] = {};
  };
  struct FakeInstr {
    std::uint32_t kind = O3M_INSTRUMENT_EMPTY, engine = 0;
    std::uint32_t midi_channel = 0;  // ABI 1.7: 1..16 for a MIDI instrument, else 0
    std::vector<FakeParam> params;
  };
  struct PWrite {
    std::uint8_t instrument;
    std::uint16_t id;
    float value;
  };
  FakeInstr instr[O3M_INSTR_MAX + 1];
  std::vector<PWrite> pending_p;
  std::vector<std::vector<PWrite>> param_groups;
  int info_reads = 0, desc_reads = 0, param_reads = 0;
  FakeParam* find_param(unsigned n, unsigned id) {
    if (n < 1u || n > O3M_INSTR_MAX) {
      return nullptr;
    }
    for (FakeParam& p : instr[n].params) {
      if (p.id == id) {
        return &p;
      }
    }
    return nullptr;
  }
  // A SAMPLE instrument with a representative slice of the real parameter table (ids, ranges, groups and flags as the OS lists them): VOLUME 0..100 (voice), TUNE -24..24 STEPPED (voice), FILTER and FILTER TYPE ENUM
  // (filter), CUTOFF 0..1 and RESONANCE 0..4.3 (filter), DELAY SEND 0..100 (sends), an envelope AMOUNT 0..1 and RELEASE 0..10000 ms (env), an LFO SHAPE ENUM 0..4 (mod), and two parameters that are NOT writable:
  // PLAY ENGINE (id 13, ENUM, voice) and PLAY MODE (id 0x14, ENUM, source). `engine` is the play engine it reports.
  void add_sample(unsigned n, std::uint32_t engine = 0, float k = 0.0f) {
    FakeInstr& in = instr[n];
    in.kind = O3M_INSTRUMENT_SAMPLE;
    in.engine = engine;
    in.params.clear();
    const auto add = [&](std::uint16_t id, const char* name, float mn, float mx, float def, float value, std::uint16_t flags, std::uint8_t group) {
      FakeParam p;
      p.id = id;
      p.min = mn;
      p.max = mx;
      p.def = def;
      p.value = value;
      p.flags = flags;
      p.group = group;
      for (unsigned i = 0; name[i] != '\0' && i + 1 < sizeof p.name; ++i) {
        p.name[i] = name[i];
      }
      in.params.push_back(p);
    };
    const std::uint16_t w = O3M_PD_APP_WRITABLE;
    add(0, "VOLUME", 0, 100, 50, 50 + 10 * k, w, O3M_PGROUP_VOICE);
    add(2, "TUNE", -24, 24, 0, -4 * k, w | O3M_PD_STEPPED, O3M_PGROUP_VOICE);
    add(5, "FILTER", 0, 1, 0, k > 1 ? 1.0f : 0.0f, w | O3M_PD_ENUM, O3M_PGROUP_FILTER);
    add(6, "FILTER TYPE", 0, 2, 0, k > 2 ? 2.0f : 0.0f, w | O3M_PD_ENUM, O3M_PGROUP_FILTER);
    add(7, "CUTOFF", 0, 1, 1, 1.0f - 0.2f * k, w, O3M_PGROUP_FILTER);
    add(8, "RESONANCE", 0, 4.3f, 0, 0.5f * k, w, O3M_PGROUP_FILTER);
    add(9, "DELAY SEND", 0, 100, 0, 5 * k, w, O3M_PGROUP_SENDS);
    add(13, "PLAY ENGINE", 0, 4, 0, static_cast<float>(engine), O3M_PD_ENUM, O3M_PGROUP_SOURCE);
    add(0x14, "PLAY MODE", 0, 7, 0, 1, O3M_PD_ENUM, O3M_PGROUP_SOURCE);
    add(0x21, "AMOUNT", 0, 1, 1, 1.0f - 0.1f * k, w, O3M_PGROUP_ENV);
    add(0x25, "RELEASE", 0, 10000, 1000, 1000 + 500 * k, w, O3M_PGROUP_ENV);
    add(0x61, "LFO SHAPE", 0, 4, 2, k > 3 ? 4.0f : 2.0f, w | O3M_PD_ENUM, O3M_PGROUP_MOD);
  }
  // A MODULE instrument of `count` parameters 0x100 + i, 0..1 (INT 0..12 STEPPED for i % 4 == 3), all writable, page i / 8; `hash` is the module's engine value.
  void add_module(unsigned n, std::uint32_t hash, unsigned count, float k = 0.0f) {
    FakeInstr& in = instr[n];
    in.kind = O3M_INSTRUMENT_MODULE;
    in.engine = hash;
    in.params.clear();
    for (unsigned i = 0; i < count; ++i) {
      FakeParam p;
      p.id = static_cast<std::uint16_t>(0x100 + i);
      p.name[0] = 'P';
      p.name[1] = static_cast<char>('0' + i / 10);
      p.name[2] = static_cast<char>('0' + i % 10);
      p.group = static_cast<std::uint8_t>(i / 8);
      if (i % 4 == 3) {
        p.min = 0;
        p.max = 12;
        p.def = 6;
        p.value = static_cast<float>(static_cast<int>(2.0f + 3.0f * k + static_cast<float>(i)) % 13);
        p.flags = O3M_PD_APP_WRITABLE | O3M_PD_STEPPED;
      } else {
        p.min = 0;
        p.max = 1;
        p.def = 0.5f;
        p.value = 0.1f * static_cast<float>(i % 7) + 0.05f * k;
        p.value = p.value > 1.0f ? 1.0f : p.value;
      }
      in.params.push_back(p);
    }
  }
  // A host of ABI 1.5: the tables end before the 1.6 members.
  void make_abi15() {
    read.size = static_cast<std::uint32_t>(offsetof(o3m_read_api, instrument_info));
    edit.size = static_cast<std::uint32_t>(offsetof(o3m_edit_api, set_instrument_param));
  }
  // OS-H4 (ABI 1.7): the export calls. A file the app writes is kept whole in `exports` (name, ext, bytes, whether it was closed). The test chooses what open answers (`open_result`), makes every
  // `busy_every`-th write answer BUSY without taking anything (0: never), what a write answers once the file is open (`write_result`), what close answers, and how many status polls say WRITING before the
  // final answer (`status_delay`, then `final_status`: DONE writes the name chosen, `final_name`, into the app's buffer).
  struct ExportFile {
    std::string name, ext;
    std::vector<std::uint8_t> data;
    bool closed = false;
  };
  std::vector<ExportFile> exports;
  int open_result = O3M_EXPORT_OK, write_result = O3M_EXPORT_OK, close_result = O3M_EXPORT_OK;
  unsigned busy_every = 0, status_delay = 0;
  std::uint32_t final_status = O3M_EXPORT_DONE;
  std::string final_name;  // "" = "<name>.<ext>" as the app gave them
  bool export_open_now = false;
  unsigned write_calls = 0, status_polls = 0, busy_answers = 0, chunk_max = 0;  // chunk_max: the largest n any write was given
  int end_result = O3M_EDIT_OK;
  int begins = 0, ends = 0, steps_reads = 0;
  o3m_read_api read{};
  o3m_edit_api edit{};
  o3m_io_api io{};
  // OS-APPUX S3 (ABI 1.10): the transport the app reads, the rows it stored (set_own_param: index, value, in call order) and the notes it asked for (play_note / stop_note), with the answers they get.
  o3m_transport transport = {sizeof(o3m_transport), 0, 0, 0xFFFF, 0, 0, 12000, 0, 0xFFFF};
  struct OwnParam {
    std::uint32_t index;
    float value;
  };
  std::vector<OwnParam> own_params;
  struct PlayCall {
    bool on;
    std::uint32_t instrument, note, velocity;
  };
  std::vector<PlayCall> plays;
  int play_result = O3M_PLAY_OK;

  static FakeApi*& current() {
    static FakeApi* p = nullptr;
    return p;
  }
  FakeApi() {
    current() = this;
    read = {O3M_READ_API_VERSION, sizeof(o3m_read_api), [] { return current()->tempo; },
            [](o3m_launch_ctx* out) -> std::uint32_t {
              *out = current()->launch;
              return (current()->launch.flags & O3M_LAUNCH_FROM_TOOLS) != 0u ? 1u : 0u;
            },
            [](std::uint16_t pattern, o3m_pattern_info* out) -> std::int32_t {
              FakeApi* f = current();
              if (pattern != f->pattern_id || out == nullptr) {
                return O3M_EDIT_RANGE;
              }
              out->length = 0;
              for (std::uint32_t t = 0; t < f->pat_tracks; ++t) {
                out->length = f->pat_len[t] > out->length ? f->pat_len[t] : out->length;
              }
              out->tracks = f->pat_tracks;
              for (std::uint32_t t = 0; t < kTracks; ++t) {
                out->track_length[t] = t < f->pat_tracks ? f->pat_len[t] : 0;
              }
              out->size = sizeof(o3m_pattern_info);
              return O3M_EDIT_OK;
            },
            [](std::uint16_t pattern, std::uint8_t track, std::uint8_t first, o3m_step* out, std::uint8_t count) -> std::int32_t {
              FakeApi* f = current();
              ++f->steps_reads;
              if (!f->in_range(pattern, track, first, count) || out == nullptr) {
                return O3M_EDIT_RANGE;
              }
              for (unsigned k = 0; k < count; ++k) {
                out[k] = f->steps[track][first + k];
              }
              return O3M_EDIT_OK;
            },
            [](std::uint8_t n, o3m_instrument_info* out) -> std::int32_t {
              FakeApi* f = current();
              ++f->info_reads;
              if (n < 1u || n > O3M_INSTR_MAX || out == nullptr) {
                return O3M_EDIT_RANGE;
              }
              out->kind = f->instr[n].kind;
              out->engine = f->instr[n].engine;
              out->param_count = static_cast<std::uint32_t>(f->instr[n].params.size());
              if (out->size >= sizeof(o3m_instrument_info)) {  // (a struct built for 1.6 is 16 bytes and gets the first four members only)
                out->midi_channel = f->instr[n].kind == O3M_INSTRUMENT_MIDI ? f->instr[n].midi_channel : 0u;
                out->size = sizeof(o3m_instrument_info);
              } else {
                out->size = 16;
              }
              return O3M_EDIT_OK;
            },
            [](std::uint8_t n, std::uint16_t index, o3m_param_desc* out) -> std::int32_t {
              FakeApi* f = current();
              ++f->desc_reads;
              if (n < 1u || n > O3M_INSTR_MAX || out == nullptr || index >= f->instr[n].params.size()) {
                return O3M_EDIT_RANGE;
              }
              const FakeParam& p = f->instr[n].params[index];
              *out = o3m_param_desc{};
              out->size = sizeof(o3m_param_desc);
              out->min = p.min;
              out->max = p.max;
              out->def = p.def;
              out->id = p.id;
              out->flags = p.flags;
              out->group = p.group;
              for (unsigned i = 0; i + 1 < sizeof out->name; ++i) {
                out->name[i] = p.name[i];
              }
              return O3M_EDIT_OK;
            },
            [](std::uint8_t n, std::uint16_t id, float* out) -> std::int32_t {
              FakeApi* f = current();
              ++f->param_reads;
              const FakeParam* p = f->find_param(n, id);
              if (p == nullptr || out == nullptr) {
                return O3M_EDIT_RANGE;
              }
              *out = p->value;
              return O3M_EDIT_OK;
            }};
    edit = {O3M_EDIT_API_VERSION, sizeof(o3m_edit_api),
            [](std::uint32_t bpm_x100) -> int {
              FakeApi* f = current();
              f->tempo_calls.push_back(bpm_x100);
              if (f->set_tempo_result == O3M_EDIT_OK) {
                f->tempo = bpm_x100;
              }
              return f->set_tempo_result;
            },
            [](void) -> std::int32_t {
              FakeApi* f = current();
              ++f->begins;
              if (f->group_open) {
                return O3M_EDIT_GROUP_OPEN;
              }
              f->group_open = true;
              f->pending.clear();
              f->pending_steps.clear();
              f->pending_p.clear();
              return O3M_EDIT_OK;
            },
            [](void) -> std::int32_t {
              FakeApi* f = current();
              ++f->ends;
              if (!f->group_open) {
                return O3M_EDIT_NO_GROUP;
              }
              f->group_open = false;
              if (f->end_result != O3M_EDIT_OK) {
                return f->end_result;
              }
              std::size_t at = 0;
              for (const Write& w : f->pending) {
                for (unsigned k = 0; k < w.count; ++k) {
                  const o3m_step& src = f->pending_steps[at + k];
                  o3m_step& d = f->steps[w.track][w.first + k];
                  if ((w.fields & O3M_STEP_NOTE) != 0u) d.note = src.note;
                  if ((w.fields & O3M_STEP_INSTR) != 0u) d.instr = src.instr;
                  if ((w.fields & O3M_STEP_VOL) != 0u) d.vol = src.vol;
                  if ((w.fields & O3M_STEP_FX1) != 0u) { d.fx1_type = src.fx1_type; d.fx1_value = src.fx1_value; }
                  if ((w.fields & O3M_STEP_FX2) != 0u) { d.fx2_type = src.fx2_type; d.fx2_value = src.fx2_value; }
                }
                at += w.count;
              }
              f->groups.push_back(f->pending);
              for (const PWrite& w : f->pending_p) {
                FakeParam* p = f->find_param(w.instrument, w.id);
                if (p != nullptr) {
                  const bool whole = (p->flags & (O3M_PD_STEPPED | O3M_PD_ENUM)) != 0u;
                  p->value = whole ? static_cast<float>(static_cast<std::int32_t>(w.value + (w.value < 0.0f ? -0.5f : 0.5f))) : w.value;
                }
              }
              f->param_groups.push_back(f->pending_p);
              return O3M_EDIT_OK;
            },
            [](std::uint16_t pattern, std::uint8_t track, std::uint8_t row, const o3m_step* s, std::uint8_t fields) -> std::int32_t {
              return current()->write(pattern, track, row, s, 1, fields);
            },
            [](std::uint16_t pattern, std::uint8_t track, std::uint8_t first, const o3m_step* s, std::uint8_t count, std::uint8_t fields) -> std::int32_t {
              return current()->write(pattern, track, first, s, count, fields);
            },
            [](std::uint8_t n, std::uint16_t id, float value) -> std::int32_t {
              FakeApi* f = current();
              if (!f->group_open) {
                return O3M_EDIT_NO_GROUP;
              }
              const FakeParam* p = f->find_param(n, id);
              if (p == nullptr || (f->instr[n].kind != O3M_INSTRUMENT_SAMPLE && f->instr[n].kind != O3M_INSTRUMENT_MODULE) || (p->flags & O3M_PD_APP_WRITABLE) == 0u ||
                  !(value >= p->min && value <= p->max)) {
                return O3M_EDIT_RANGE;
              }
              f->pending_p.push_back(PWrite{n, id, value});
              return O3M_EDIT_OK;
            }};
    io = {O3M_IO_API_VERSION, sizeof(o3m_io_api),
          [] {
            FakeApi* f = current();
            const std::uint32_t v = f->now;
            ++f->now_calls;
            if (f->tick_every != 0u && f->now_calls % f->tick_every == 0u) {
              ++f->now;
            }
            return v;
          },
          [](o3m_midi_event* out, std::uint32_t max) -> std::uint32_t {
            FakeApi* f = current();
            ++f->reads;
            std::uint32_t n = 0;
            while (n < max && !f->midi.empty()) {
              out[n++] = f->midi.front();
              f->midi.pop_front();
            }
            return n;
          },
          [](std::uint32_t* over, std::uint32_t* sx) {
            if (over != nullptr) {
              *over = current()->overflow;
            }
            if (sx != nullptr) {
              *sx = current()->sysex;
            }
          },
          [](std::uint32_t source) -> int {
            FakeApi* f = current();
            if (source > O3M_TAP_MASTER) {
              return 1;
            }
            f->tap_source = source;
            f->tap_selects.push_back(source);
            return 0;
          },
          [](std::int16_t* out, std::uint32_t frames) -> std::uint32_t {
            FakeApi* f = current();
            ++f->latest_calls;
            if (f->tap_overreturn != 0u) {
              return f->tap_overreturn;
            }
            if (f->tap_window.empty() || (frames & 1u) != 0u || frames < 2u || frames > O3M_TAP_LATEST_MAX || (reinterpret_cast<std::uintptr_t>(out) & 3u) != 0u) {
              return 0;
            }
            const std::size_t have = f->tap_window.size() < frames ? f->tap_window.size() : frames;
            for (std::uint32_t i = 0; i < frames - have; ++i) {
              out[i] = 0;
            }
            for (std::size_t i = 0; i < have; ++i) {
              out[frames - have + i] = f->tap_window[f->tap_window.size() - have + i];
            }
            return static_cast<std::uint32_t>(have);
          },
          [](std::int16_t* out, std::uint32_t max_frames, std::uint32_t* lost) -> std::uint32_t {
            FakeApi* f = current();
            ++f->stream_calls;
            if (lost != nullptr) {
              *lost = 0;
            }
            if (f->tap_overreturn != 0u) {
              return f->tap_overreturn;
            }
            if (f->tap_chunks.empty() || (reinterpret_cast<std::uintptr_t>(out) & 3u) != 0u) {
              return 0;
            }
            TapChunk c = std::move(f->tap_chunks.front());
            f->tap_chunks.pop_front();
            max_frames = max_frames > O3M_TAP_STREAM_MAX ? O3M_TAP_STREAM_MAX : (max_frames & ~1u);
            std::size_t drop = c.s.size() > max_frames ? c.s.size() - max_frames : 0u;
            if (lost != nullptr) {
              *lost = c.lost + static_cast<std::uint32_t>(drop);
            }
            const std::size_t n = c.s.size() - drop;
            for (std::size_t i = 0; i < n; ++i) {
              out[i] = c.s[drop + i];
            }
            return static_cast<std::uint32_t>(n);
          },
          [](o3m_tap_levels* out) {
            FakeApi* f = current();
            o3m_tap_levels r{};
            r.size = sizeof r;
            if (!f->tap_levels.empty()) {
              r = f->tap_levels.front();
              f->tap_levels.pop_front();
            }
            *out = r;
          },
          [](const char* name, const char* ext) -> std::int32_t {
            FakeApi* f = current();
            if (f->export_open_now) {
              return O3M_EXPORT_STATE;
            }
            if (f->open_result != O3M_EXPORT_OK) {
              return f->open_result;
            }
            f->exports.push_back(ExportFile{name, ext, {}, false});
            f->export_open_now = true;
            f->status_polls = 0;
            return O3M_EXPORT_OK;
          },
          [](const void* buf, std::uint32_t n) -> std::int32_t {
            FakeApi* f = current();
            ++f->write_calls;
            if (!f->export_open_now) {
              return O3M_EXPORT_STATE;
            }
            if (buf == nullptr || n == 0u || n > O3M_EXPORT_RING_BYTES) {
              return O3M_EXPORT_RANGE;
            }
            f->chunk_max = n > f->chunk_max ? n : f->chunk_max;
            if (f->busy_every != 0u && f->write_calls % f->busy_every == 0u) {
              ++f->busy_answers;
              return O3M_EXPORT_BUSY;
            }
            if (f->write_result != O3M_EXPORT_OK) {
              return f->write_result;
            }
            const std::uint8_t* b = static_cast<const std::uint8_t*>(buf);
            f->exports.back().data.insert(f->exports.back().data.end(), b, b + n);
            return O3M_EXPORT_OK;
          },
          [](void) -> std::int32_t {
            FakeApi* f = current();
            if (!f->export_open_now) {
              return O3M_EXPORT_STATE;
            }
            if (f->close_result != O3M_EXPORT_OK) {
              return f->close_result;
            }
            f->exports.back().closed = true;
            return O3M_EXPORT_OK;
          },
          [](char* out, std::uint32_t cap) -> std::uint32_t {
            FakeApi* f = current();
            if (!f->export_open_now) {
              return O3M_EXPORT_IDLE;
            }
            if (!f->exports.back().closed || f->status_polls++ < f->status_delay) {
              return O3M_EXPORT_WRITING;
            }
            f->export_open_now = false;
            if (f->final_status == O3M_EXPORT_DONE && out != nullptr && cap != 0u) {
              const std::string n = f->final_name.empty() ? f->exports.back().name + "." + f->exports.back().ext : f->final_name;
              const std::size_t take = n.size() < cap - 1u ? n.size() : cap - 1u;
              std::memcpy(out, n.data(), take);
              out[take] = '\0';
            }
            return f->final_status;
          },
          [](o3m_transport* out) -> std::int32_t {
            *out = current()->transport;
            return 0;
          },
          [](std::uint32_t index, float value) -> std::int32_t {
            current()->own_params.push_back({index, value});
            return O3M_OWN_OK;
          },
          [](std::uint32_t instrument, std::uint32_t note, std::uint32_t velocity) -> std::int32_t {
            FakeApi* f = current();
            if (f->play_result == O3M_PLAY_OK) {
              f->plays.push_back({true, instrument, note, velocity});
            }
            return f->play_result;
          },
          [](std::uint32_t instrument) -> std::int32_t {
            FakeApi* f = current();
            f->plays.push_back({false, instrument, 0, 0});
            return O3M_PLAY_OK;
          }};
  }
  ~FakeApi() {
    if (current() == this) {
      current() = nullptr;
    }
  }
  bool in_range(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, std::uint8_t count) const {
    return pattern == pattern_id && track < pat_tracks && count != 0u && first < pat_len[track] && count <= pat_len[track] - first;
  }
  std::int32_t write(std::uint16_t pattern, std::uint8_t track, std::uint8_t first, const o3m_step* s, std::uint8_t count, std::uint8_t fields) {
    if (!group_open) {
      return O3M_EDIT_NO_GROUP;
    }
    if (!in_range(pattern, track, first, count) || s == nullptr || fields == 0u || (fields & ~O3M_STEP_ALL) != 0u) {
      return O3M_EDIT_RANGE;
    }
    pending.push_back(Write{pattern, track, first, count, fields});
    pending_steps.insert(pending_steps.end(), s, s + count);
    return O3M_EDIT_OK;
  }
  FakeApi(const FakeApi&) = delete;
  FakeApi& operator=(const FakeApi&) = delete;
  void push(std::uint8_t port, std::uint8_t status, std::uint8_t d1, std::uint8_t d2 = 0) { midi.push_back(o3m_midi_event{port, status, d1, d2}); }
};

// The host an app of ABI 1.3 is given when its file asks for `caps` (O3M_CAP_*): the read table for read_project, the edit table for edit_project, the io table always (its MIDI calls only for
// midi_in, its tap members only for audio_tap), NULL for the rest.
inline o3m_host make_app_host(FakeApi& api, std::uint32_t caps) {
  o3m_host h = make_host();
  h.read = (caps & O3M_CAP_READ_PROJECT) != 0u ? &api.read : nullptr;
  h.edit = (caps & O3M_CAP_EDIT_PROJECT) != 0u ? &api.edit : nullptr;
  static o3m_io_api lent;
  lent = api.io;
  if ((caps & O3M_CAP_MIDI_IN) == 0u) {
    lent.midi_read = nullptr;
    lent.midi_lost = nullptr;
  }
  if ((caps & O3M_CAP_AUDIO_TAP) == 0u) {
    lent.tap_select = nullptr;
    lent.tap_latest = nullptr;
    lent.tap_stream = nullptr;
    lent.tap_levels = nullptr;
  }
  if ((caps & O3M_CAP_EXPORT_FILES) == 0u) {
    lent.export_open = nullptr;
    lent.export_write = nullptr;
    lent.export_close = nullptr;
    lent.export_status = nullptr;
  }
  if ((caps & O3M_CAP_PLAY_NOTES) == 0u) {  // (get_transport and set_own_param go to every app of 1.10)
    lent.play_note = nullptr;
    lent.stop_note = nullptr;
  }
  h.io = &lent;
  return h;
}

// A module's host shared object.
struct Lib {
  void* handle = nullptr;
  const o3m_module* entry = nullptr;
  Lib() = default;
  Lib(const Lib&) = delete;
  Lib& operator=(const Lib&) = delete;
  ~Lib() {
    if (handle != nullptr) {
      dlclose(handle);
    }
  }
  bool open(const char* path) {
    handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      return false;
    }
    entry = static_cast<const o3m_module*>(dlsym(handle, "o3m_entry"));
    return entry != nullptr;
  }
};

// One instance of a module, driven the way the OS drives it. Every parameter starts at its default (the OS does the same when it binds a slot: it sets each one).
struct Instance {
  static constexpr std::size_t kStateMax = 65536;  // (OS-H2b: the Tuner keeps 34 KB: its window, the FFT work arrays and the tables)
  alignas(16) unsigned char state[kStateMax] = {};
  o3m_host host = make_host();
  const o3m_module* m = nullptr;
  float values[O3M_PARAMS_MAX] = {};

  bool start(const o3m_module& mod, float rate = kRate) { return start_with(mod, make_host(rate)); }
  // OS-H1: with the host table the test built (make_app_host: the tables an app of ABI 1.3 is lent for its capabilities).
  bool start_with(const o3m_module& mod, const o3m_host& h) {
    m = &mod;
    host = h;
    if (mod.state_bytes > kStateMax || mod.init(state, &host) != 0) {
      return false;
    }
    for (std::uint32_t i = 0; i < mod.param_count; ++i) {
      set(i, mod.params[i].def);
    }
    return true;
  }
  void set(std::uint32_t id, float v) {
    if (id < O3M_PARAMS_MAX) {
      values[id] = v;
    }
    m->set_param(state, id, v);
  }
  void on(std::uint32_t note, std::uint32_t vel = 127, std::uint32_t flags = 0) { m->note_on(state, note, vel, flags); }
  void off(std::uint32_t note) { m->note_off(state, note); }
  // frames samples in blocks of `block` (the last one shorter); `busy` is the last render's return value
  std::vector<float> render(std::uint32_t frames, std::uint32_t block = 64) {
    std::vector<float> out(frames, 0.0f);
    for (std::uint32_t at = 0; at < frames;) {
      const std::uint32_t n = frames - at < block ? frames - at : block;
      busy = m->render(state, nullptr, out.data() + at, n);
      at += n;
    }
    return out;
  }
  std::uint32_t busy = 0;
};

// Frequency of a (roughly periodic) signal from its rising zero crossings, with the crossing placed by linear interpolation: crossings - 1 periods over the time between the first and the last.
inline double frequency(const float* s, std::size_t n, double rate = kRate) {
  double first = -1.0, last = 0.0;
  std::size_t count = 0;
  for (std::size_t i = 1; i < n; ++i) {
    if (s[i - 1] < 0.0f && s[i] >= 0.0f) {
      const double t = static_cast<double>(i - 1) + static_cast<double>(-s[i - 1]) / static_cast<double>(s[i] - s[i - 1]);
      if (first < 0.0) {
        first = t;
      }
      last = t;
      ++count;
    }
  }
  return count >= 2 ? static_cast<double>(count - 1) * rate / (last - first) : 0.0;
}

inline double hz_of_note(double note) { return 440.0 * std::pow(2.0, (note - 69.0) / 12.0); }

// What a view drew, as the sink of ViewHost::replay sees it.
struct Drawing final : o3os::module::ViewSink {
  struct Rect {
    int x, y, w, h;
    std::uint8_t color;
  };
  struct Text {
    int x, y;
    std::string s;
    std::uint8_t face, color;
  };
  struct Poly {
    std::vector<std::int16_t> xy;
    std::uint8_t color;
  };
  std::vector<Rect> rects;
  std::vector<Text> texts;
  std::vector<Poly> polys;
  std::vector<o3os::module::CurveCall> curves;
  std::vector<std::vector<float>> curve_y;  // the series of each curve (the call's pointers die with the replay)
  void fill(int x, int y, int w, int h, std::uint8_t color) noexcept override { rects.push_back({x, y, w, h, color}); }
  void text(int x, int y, const char* s, std::uint8_t face, std::uint8_t color) noexcept override { texts.push_back({x, y, std::string(s), face, color}); }
  void polyline(const std::int16_t* xy, std::uint32_t points, std::uint8_t color) noexcept override {
    Poly p;
    p.xy.assign(xy, xy + 2u * points);
    p.color = color;
    polys.push_back(p);
  }
  void curve(const o3os::module::CurveCall& c) noexcept override {
    curves.push_back(c);
    curve_y.emplace_back(c.y, c.y + c.n);
    curves.back().y = curve_y.back().data();
    curves.back().x = nullptr;
  }
};

// The packed .tmod of a module as the OS's loader reads it: the header checks, the CRC, the relocation of the image to the addresses of the MK66's SRAM slab (hot part) and SDRAM
// (cold part), and the descriptor read back from the placed image. `present` is false when the cross compiler did not build the file (the test then says so and passes, unless O3_CI=1).
// OS-H3B: a rig for the editing apps (HUMANIZE, CHORDS, MUTATE, MORPH, all of the shape of RHYTHM GEN): the module's host shared object and an instance of it on a FakeApi, a ViewHost with the theme's
// faces, and the input and draw calls as the OS makes them (under the budgets). The test fills `api` (the project, the instruments, the launch context) and then calls start(); `launch_tools` sets
// the context Pattern F7 TOOLS would give.
inline constexpr std::uint32_t kReadEdit = O3M_CAP_READ_PROJECT | O3M_CAP_EDIT_PROJECT;
struct EditRig {
  Lib lib;
  Instance inst;
  FakeApi api;
  o3os::module::ViewHost views;
  bool ok = false;
  EditRig() {
    o3os::module::ViewHost::Metrics m;  // the theme's faces: Display 16 x 32, Data 10 x 20, Label 8 x 16, Micro 6 x 12
    const std::int16_t cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    for (unsigned f = 0; f < O3M_FACE_COUNT; ++f) {
      m.cell_w[f] = cw[f];
      m.cell_h[f] = ch[f];
    }
    views.set_metrics(m);
    for (auto& t : api.steps) {
      for (auto& st : t) {
        st = o3m_step{0, 0, 255, 0, 0, 0, 0, 0};
      }
    }
  }
  // The launch context of F7 TOOLS: pattern 1, `track`, the 0-based instrument slot, the block's first row and rows, the tracks it covers (0 rows = no block).
  void launch_tools(std::uint32_t track, std::uint32_t instrument0, std::uint32_t row0, std::uint32_t rows, std::uint32_t block_tracks) {
    api.launch = {sizeof(o3m_launch_ctx), O3M_LAUNCH_FROM_TOOLS, 1, track, instrument0, row0, rows, static_cast<std::uint8_t>(block_tracks), {0, 0, 0}};
  }
  bool start(const char* so, std::uint32_t caps = kReadEdit) {
    ok = lib.open(so) && inst.start_with(*lib.entry, make_app_host(api, caps));
    if (ok) {
      inst.m->ui->enter(inst.state);
    }
    return ok;
  }
  void enter() { inst.m->ui->enter(inst.state); }
  void event(std::uint32_t kind, std::uint32_t id, std::int32_t value) {
    const o3m_ui_event ev = {kind, id, value, 0};
    CHECK(views.call_input(*lib.entry, inst.state, 1, ev) == o3os::module::ViewStatus::Ok);
  }
  void press(std::uint32_t id) { event(O3M_EV_BUTTON, id, 1); }
  void turn(int n) { event(O3M_EV_ENCODER, 0, n); }
  void down(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_DOWN); }
  void up(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_UP); }
  void left(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_LEFT); }
  void right(unsigned n = 1) { for (unsigned i = 0; i < n; ++i) press(O3M_BTN_RIGHT); }
  Drawing draw(int w = 800, int h = 330) {
    Drawing d;
    CHECK(views.draw(*lib.entry, inst.state, 1, 0, w, h, nullptr, 0, o3os::module::kViewMaxWords) == o3os::module::ViewStatus::Ok);
    views.replay(d);
    return d;
  }
  static bool shows(const Drawing& d, const char* s) {
    for (const auto& t : d.texts) {
      if (t.s.find(s) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
  // Nothing the page draws leaves the canvas or overlaps another text (the faces of the theme).
  void check_layout(int w, int h, const char* what) {
    const int cw[O3M_FACE_COUNT] = {16, 10, 8, 6}, ch[O3M_FACE_COUNT] = {32, 20, 16, 12};
    const Drawing d = draw(w, h);
    for (std::size_t i = 0; i < d.texts.size(); ++i) {
      const auto& t = d.texts[i];
      const int tw = static_cast<int>(t.s.size()) * cw[t.face], th = ch[t.face];
      CHECK_MESSAGE((t.x >= 0 && t.y >= 0 && t.x + tw <= w && t.y + th <= h), what << " '" << t.s << "' at " << t.x << "," << t.y << " " << tw << "x" << th << " in " << w << "x" << h);
      for (std::size_t j = i + 1; j < d.texts.size(); ++j) {
        const auto& u = d.texts[j];
        const int uw = static_cast<int>(u.s.size()) * cw[u.face], uh = ch[u.face];
        CHECK_MESSAGE(!(t.x < u.x + uw && u.x < t.x + tw && t.y < u.y + uh && u.y < t.y + th), what << " '" << t.s << "' overlaps '" << u.s << "'");
      }
    }
  }
};

struct Tmod {
  bool present = false;
  o3os::module::Status header = o3os::module::Status::TooShort, relocation = o3os::module::Status::TooShort, descriptor = o3os::module::Status::TooShort;
  bool crc_ok = false;
  o3m_file_header h{};
  o3os::module::Desc32 desc{};
  std::uint32_t file_bytes = 0;
};

inline Tmod load_tmod(const char* path) {
  using namespace o3os::module;
  Tmod t;
  std::ifstream f(path, std::ios::binary);
  const std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (file.size() < sizeof(o3m_file_header)) {
    const char* ci = std::getenv("O3_CI");
    CHECK_MESSAGE(!(ci != nullptr && ci[0] == '1'), "the .tmod was not built (no arm-none-eabi-g++) and O3_CI=1");
    MESSAGE("the .tmod was not built (no arm-none-eabi-g++): skipped " << path);
    return t;
  }
  t.present = true;
  t.file_bytes = static_cast<std::uint32_t>(file.size());
  std::memcpy(&t.h, file.data(), sizeof t.h);
  t.header = check_header(t.h, t.file_bytes);
  if (t.header != Status::Ok) {
    return t;
  }
  std::vector<std::uint8_t> copy = file;
  std::memset(copy.data() + offsetof(o3m_file_header, image_crc), 0, 4);
  t.crc_ok = crc32_update(0, copy.data(), copy.size()) == t.h.image_crc;
  std::vector<std::uint8_t> hot(file.begin() + sizeof(o3m_file_header), file.begin() + sizeof(o3m_file_header) + t.h.hot_size);
  std::vector<std::uint8_t> cold(file.begin() + sizeof(o3m_file_header) + t.h.hot_size, file.begin() + sizeof(o3m_file_header) + t.h.image_size);
  hot.resize(hot.size() + 4);   // (a placement may not be empty)
  cold.resize(cold.size() + 4);
  Placement place;
  place.hot_mem = hot.data();
  place.hot_addr = 0x1FFFC000u;
  place.cold_mem = cold.data();
  place.cold_addr = 0x70000000u;
  std::vector<std::uint32_t> table(t.h.reloc_count);
  if (t.h.reloc_count != 0u) {
    std::memcpy(table.data(), file.data() + sizeof(o3m_file_header) + t.h.image_size, table.size() * sizeof(std::uint32_t));
  }
  Relocator r(t.h, place);
  t.relocation = r.apply(table.data(), t.h.reloc_count);
  if (t.relocation == Status::Ok && r.complete()) {
    t.descriptor = read_desc(t.h, place, t.desc);
  }
  return t;
}

}  // namespace o3m_test
