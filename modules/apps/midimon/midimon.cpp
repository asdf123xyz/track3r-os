// SPDX-License-Identifier: MIT
//
// "MIDI Monitor" (OS-H1, new Community OS behaviour, ABI 1.3 until OS-APPUX S3, now 1.10, capability midi_in): the MIDI messages that arrive on DIN and USB, as they come: the newest eight (fewer on a small page) with
// the port, the channel, the kind and the values, a strip of the 16 channels that lights for a moment when one is heard, and the counters of what was lost (messages that did not fit in the
// OS's ring of 64, SysEx messages, which the host drops and counts). It only listens: it makes no sound and sends nothing.
//
//   Insert, F1   pause / resume the list (messages keep being counted and the strip keeps moving)     F2   clear the list and the counters
//   Up / Down, the encoder, Shift + the encoder, Delete (the grammar of o3m_edit.h, OS-APPUX S3): the two rows at the bottom, kept with the project
//   CHANNEL 0 = all, 1..16;   TYPE 0 all, 1 notes, 2 controllers, 3 the rest (program change, pressure, pitch bend). Delete puts the row's default back (it was CLEAR: that is F2 now).
//
// The ring is drained from view_draw(), which the OS calls 30 times a second while the page is up (the natural once-a-frame place): 33 ms of a dense stream is far less than 64 messages on
// a DIN cable, and USB bursts are counted when they overflow. Real-time bytes (clock, start, stop) and system common are not delivered by the host; SysEx is only counted.
#include <o3module.h>

#include "midimon_gen.h"
#include "o3m_edit.h"

namespace {

constexpr uint32_t kRows = 8;         // the most lines kept and shown
constexpr uint32_t kRowPitch = 24;
constexpr uint32_t kRecentMs = 300;   // a channel's cell and the newest line stay lit this long
constexpr uint32_t kChunk = 16;       // events asked of the host in one call

struct Line {
  uint8_t port, status, d1, d2;
};

struct State {
  const o3m_io_api* io;  // NULL: the host lent no io table (an OS older than ABI 1.3)
  Line log[kRows];       // circular: the newest is log[(head + kRows - 1) % kRows]
  uint32_t head;         // the next slot to write
  uint32_t shown;        // lines held, 0..kRows
  uint32_t total;        // messages heard since the last clear (all of them, filtered out or not)
  uint32_t lost;         // the host's overflow count
  uint32_t sysex;        // the host's SysEx count
  uint32_t now;          // io->now_ms() at the last poll
  uint32_t last_ms;      // when the newest line arrived
  uint32_t paused;
  uint32_t channel;      // CHANNEL: 0 all, else 1..16
  uint32_t type;         // TYPE: 0 all, 1 notes, 2 controllers, 3 the rest
  uint32_t row;          // the row of the two the cursor is on
  uint32_t ch_ms[16];    // when each channel was last heard
  uint8_t ch_seen[16];
};
static_assert(sizeof(State) <= MIDIMON_STATE_BYTES, "module.json: state_bytes is too small for State");

void clear(State* s) {
  s->head = 0;
  s->shown = 0;
  s->total = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    s->ch_seen[i] = 0;
    s->ch_ms[i] = 0;
  }
}

uint32_t category(uint8_t status) {
  const uint32_t hi = status >> 4;
  return hi == 8u || hi == 9u ? 1u : (hi == 0xBu ? 2u : 3u);
}

void accept(State* s, const o3m_midi_event& e) {
  if ((e.status & 0x80u) == 0u || e.status >= 0xF0u) {
    return;  // the host delivers channel messages only; anything else is not ours
  }
  ++s->total;
  const uint32_t ch = e.status & 15u;
  s->ch_ms[ch] = s->now;
  s->ch_seen[ch] = 1;
  if ((s->channel != 0u && s->channel != ch + 1u) || (s->type != 0u && s->type != category(e.status)) || s->paused != 0u) {
    return;
  }
  s->log[s->head] = Line{e.port, e.status, static_cast<uint8_t>(e.data1 & 0x7Fu), static_cast<uint8_t>(e.data2 & 0x7Fu)};
  s->head = (s->head + 1u) % kRows;
  if (s->shown < kRows) {
    ++s->shown;
  }
  s->last_ms = s->now;
}

// Once a frame: the clock, the ring and the counters.
void poll(State* s) {
  if (s->io == nullptr) {
    return;
  }
  if (s->io->now_ms != nullptr) {
    s->now = s->io->now_ms();
  }
  if (s->io->midi_read == nullptr) {
    return;
  }
  o3m_midi_event buf[kChunk];
  for (uint32_t round = 0; round < 8u; ++round) {  // 8 x 16 is more than the host's ring holds
    uint32_t n = s->io->midi_read(buf, kChunk);
    if (n > kChunk) {
      n = kChunk;
    }
    for (uint32_t i = 0; i < n; ++i) {
      accept(s, buf[i]);
    }
    if (n < kChunk) {
      break;
    }
  }
  if (s->io->midi_lost != nullptr) {
    s->io->midi_lost(&s->lost, &s->sysex);
  }
}

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  return host->draw != nullptr ? 0 : 1;  // an OS of ABI 1.0 lends no draw API
}

uint32_t clamp_int(float v, float lo, float hi) {
  const float x = v >= lo ? (v <= hi ? v : hi) : lo;  // a NaN falls to the lower bound
  return static_cast<uint32_t>(x + 0.5f);
}

void set_param(void* raw, uint32_t id, float value) {
  State* s = static_cast<State*>(raw);
  if (id == MIDIMON_PARAM_CHANNEL) {
    s->channel = clamp_int(value, 0.0f, 16.0f);
  } else if (id == MIDIMON_PARAM_TYPE) {
    s->type = clamp_int(value, 0.0f, 3.0f);
  }
}

// The two rows are the OS's to hand back (set_param, before enter(): the project's kept values or the manifest's defaults) and the project's to keep (set_own_param). The list, the counters and PAUSED are
// transient: they start empty at every launch (accepted, OS-APPUX 3.4).
void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  clear(s);
  s->paused = 0;
  s->row = 0;
}

enum Row : uint32_t { kChannel, kType, kRowCount };
constexpr int32_t kCoarse[kRowCount] = {4, 1};  // Shift + the encoder: four channels a detent; the TYPE list has no coarse step

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const o3edit::Input in = o3edit::grammar(ev, &s->row, kRowCount, nullptr, 1);
  if (in.move == o3edit::kMoveValue || in.move == o3edit::kMoveDefault) {
    const int32_t d = in.move == o3edit::kMoveValue ? o3edit::steps(in, kCoarse[s->row]) : 0;
    const bool reset = in.move == o3edit::kMoveDefault;
    if (s->row == kChannel) {
      s->channel = reset ? 0u : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->channel) + d, 0, 16));
    } else {
      s->type = reset ? 0u : static_cast<uint32_t>(o3edit::clamp_i(static_cast<int32_t>(s->type) + d, 0, 3));
    }
    o3edit::own_set(s->io, s->row, static_cast<float>(s->row == kChannel ? s->channel : s->type));
    return;
  }
  if (ev->kind != O3M_EV_BUTTON || ev->value == 0) {  // (Shift + F1 and Shift + F2 are F1 and F2: the Shift row says the same)
    return;
  }
  if (ev->id == O3M_BTN_INSERT || ev->id == O3M_BTN_F1) {  // (F1 PAUSE and F2 CLEAR: the page's key row says what Insert and F2 do)
    s->paused ^= 1u;
  } else if (ev->id == O3M_BTN_F2) {
    clear(s);
  }
}

// ---- text, with no libc -----------------------------------------------------------------------------------------------------------------

uint32_t put_text(char* out, uint32_t at, const char* text) {
  while (*text != '\0') {
    out[at++] = *text++;
  }
  return at;
}

// `v` right-aligned in `width` columns (wider numbers are not cut).
uint32_t put_num(char* out, uint32_t at, uint32_t v, uint32_t width) {
  char digits[10];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + v % 10u);
    v /= 10u;
  } while (v != 0u && n < 10u);
  for (uint32_t i = n; i < width; ++i) {
    out[at++] = ' ';
  }
  while (n != 0u) {
    out[at++] = digits[--n];
  }
  return at;
}

// "<text>" padded with spaces to `width` columns.
uint32_t put_pad(char* out, uint32_t at, const char* text, uint32_t width) {
  const uint32_t start = at;
  at = put_text(out, at, text);
  while (at - start < width) {
    out[at++] = ' ';
  }
  return at;
}

// A note as "C#4" (MIDI 60 is C4, 0 is C-1), padded to 4 columns.
uint32_t put_note(char* out, uint32_t at, uint32_t note) {
  static const char* const kNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
  const uint32_t start = at;
  at = put_text(out, at, kNames[note % 12u]);
  const uint32_t octave = note / 12u;  // 0 is octave -1
  if (octave == 0u) {
    out[at++] = '-';
    out[at++] = '1';
  } else {
    out[at++] = static_cast<char>('0' + (octave - 1u));
  }
  while (at - start < 4u) {
    out[at++] = ' ';
  }
  return at;
}

// One line: "DIN 01 NOTE ON  C#4  60 V127" (at most 29 characters).
uint32_t format_line(char* out, const Line& l) {
  uint32_t n = put_text(out, 0, l.port == 0u ? "DIN " : "USB ");
  n = put_num(out, n, (l.status & 15u) + 1u, 2);
  if (out[n - 2] == ' ') {
    out[n - 2] = '0';
  }
  out[n++] = ' ';
  switch (l.status >> 4) {
    case 0x8:
    case 0x9: {
      const bool on = (l.status >> 4) == 0x9 && l.d2 != 0u;
      n = put_pad(out, n, on ? "NOTE ON" : "NOTE OFF", 9);
      n = put_note(out, n, l.d1);
      n = put_num(out, n, l.d1, 3);
      n = put_text(out, n, " V");
      n = put_num(out, n, l.d2, 3);
      break;
    }
    case 0xA:
      n = put_pad(out, n, "POLY AT", 9);
      n = put_note(out, n, l.d1);
      n = put_num(out, n, l.d1, 3);
      n = put_text(out, n, " P");
      n = put_num(out, n, l.d2, 3);
      break;
    case 0xB:
      n = put_pad(out, n, "CC", 9);
      n = put_num(out, n, l.d1, 3);
      n = put_text(out, n, " = ");
      n = put_num(out, n, l.d2, 3);
      break;
    case 0xC:
      n = put_pad(out, n, "PROGRAM", 9);
      n = put_num(out, n, l.d1, 3);
      break;
    case 0xD:
      n = put_pad(out, n, "CH AT", 9);
      n = put_num(out, n, l.d1, 3);
      break;
    default: {  // 0xE: pitch bend, 14 bits, centre 8192
      n = put_pad(out, n, "PITCH", 9);
      const int32_t bend = static_cast<int32_t>((static_cast<uint32_t>(l.d2) << 7) | l.d1) - 8192;
      out[n++] = bend < 0 ? '-' : '+';
      n = put_num(out, n, static_cast<uint32_t>(bend < 0 ? -bend : bend), 1);
      break;
    }
  }
  out[n] = '\0';
  return n;
}

uint32_t line_color(const Line& l) {
  switch (l.status >> 4) {
    case 0x9: return l.d2 != 0u ? O3M_COLOR_INSTR : O3M_COLOR_TEXT_DIM;
    case 0x8: return O3M_COLOR_TEXT_DIM;
    case 0xB: return O3M_COLOR_FX;
    default: return O3M_COLOR_TEXT;
  }
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  poll(s);
  const o3m_draw_api* api = c->draw;
  const int32_t w = c->width;
  api->rect(c, 0, 0, w, c->height, O3M_COLOR_PANE);
  api->text(c, 8, 4, "MIDI MONITOR", O3M_FACE_DISPLAY, O3M_COLOR_TEXT);

  char text[48];
  uint32_t n = put_text(text, 0, "IN ");
  n = put_num(text, n, s->total, 1);
  n = put_text(text, n, "  LOST ");
  n = put_num(text, n, s->lost, 1);
  n = put_text(text, n, "  SYSEX ");
  n = put_num(text, n, s->sysex, 1);
  text[n] = '\0';
  api->text(c, 8, 44, text, O3M_FACE_DATA, s->lost != 0u ? O3M_COLOR_ACCENT : O3M_COLOR_TEXT_DIM);
  if (s->paused != 0u) {
    api->text(c, w - 8 - api->text_width(c, "PAUSED", O3M_FACE_DATA), 4 + 6, "PAUSED", O3M_FACE_DATA, O3M_COLOR_ACCENT);
  }

  // the 16 channels: lit while heard, then dark
  const int32_t cell = (w - 16) / 16;
  for (uint32_t ch = 0; ch < 16u; ++ch) {
    const bool lit = s->ch_seen[ch] != 0u && s->now - s->ch_ms[ch] < kRecentMs;
    api->rect(c, 8 + static_cast<int32_t>(ch) * cell + 1, 72, cell - 2, 10, lit ? O3M_COLOR_PLAY : O3M_COLOR_EMPTY);
  }

  const int32_t top = 92;
  uint32_t rows = static_cast<uint32_t>((c->height - top - 2 * o3edit::kRowPitch - 8) / static_cast<int32_t>(kRowPitch));  // (the two rows of the filter are at the bottom)
  rows = rows < kRows ? rows : kRows;
  if (s->io == nullptr || s->io->midi_read == nullptr) {
    api->text(c, 8, top, "NO MIDI INPUT FOR THIS APP", O3M_FACE_DATA, O3M_COLOR_ACCENT);
  } else if (s->shown == 0u) {
    api->text(c, 8, top, s->paused != 0u ? "PAUSED" : "WAITING FOR MIDI ON DIN OR USB", O3M_FACE_DATA, O3M_COLOR_TEXT_DIM);
  }
  for (uint32_t i = 0; i < rows && i < s->shown; ++i) {
    const Line& l = s->log[(s->head + kRows - 1u - i) % kRows];
    format_line(text, l);
    const uint32_t color = i == 0u && s->now - s->last_ms < kRecentMs ? static_cast<uint32_t>(O3M_COLOR_ACCENT) : line_color(l);
    api->text(c, 8, top + static_cast<int32_t>(i * kRowPitch), text, O3M_FACE_DATA, color);
  }
  // the filter: CHANNEL and TYPE, the page's two rows (the grammar of o3m_edit.h)
  static const char* const kTypes[4] = {"ALL", "NOTES", "CONTROL", "OTHER"};
  o3edit::PageRow filter[kRowCount];
  filter[kChannel].name = "CHANNEL";
  filter[kChannel].dim = false;
  n = s->channel == 0u ? put_text(filter[kChannel].value, 0, "ALL") : put_num(filter[kChannel].value, 0, s->channel, 1);
  filter[kChannel].value[n] = '\0';
  filter[kType].name = "TYPE";
  filter[kType].dim = false;
  n = put_text(filter[kType].value, 0, kTypes[s->type < 4u ? s->type : 0u]);
  filter[kType].value[n] = '\0';
  o3edit::draw_rows(c, 0, c->height - 2 * o3edit::kRowPitch - 4, w / 2, filter, kRowCount, s->row);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"PAUSE", "CLEAR"}, {"PAUSE", "CLEAR"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(MIDIMON_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    MIDIMON_ENTRY_HEAD, sizeof(State), MIDIMON_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, MIDIMON_ENTRY_VIEWS(&kUi),
};
