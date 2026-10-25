// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The send effects' controls on the Mixer's MASTER FX page (OS-SENDFX): the rows after the master section's parameters and the CUE LEVEL. The DELAY
// controls are ping-pong on/off, sync on/off, sync rate, time and feedback, the REVERB's size, damping, pre-delay and diffusion, with the ranges below (time 1..3500 ms,
// stepped by 10 below 500 and by 30 above; feedback 0..70; rate one of 19; reverb values 0..100 per cent); TONE, MODE and QUALITY are further rows.
//
// The board (mix_board.h) holds the values and each edit is a ParamChange of kind Master (audio/graph_params.h sendfx::), the same path as every other level of the page. The values are the
// project's since OS-PERSIST2 (the SNFX chunk; model/project_state.h holds the same ranges and defaults, app/project_state.cpp static_asserts that they are equal, and a row is never renumbered).
// Pure: the stepping and the text are functions of the row and the value, tested without a screen.

#include <o3os/audio/graph_params.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

enum class SendFxKind : std::uint8_t {
  Number,  // a whole number with a unit, one step a detent
  OnOff,   // 0 or 1: a turn up is ON, down is OFF
  Rate,    // the list of 19 sync rates, one a detent
  Time,    // milliseconds: 10 below 500 and 30 from there
  Choice,  // 0 or 1 named by `names`
  Ms,      // milliseconds over 1..1000 (OS-LIMIT's attack and release): a detent is 1 below 10, 5 below 50, 10 below 200, 25 below 500, 50 from there
};

struct SendFxRow {
  const char* name;  // at most 12 characters (core::kParamNameChars)
  unsigned field;    // the engine's ParamChange field (audio::graph_param::master)
  SendFxKind kind;
  std::uint16_t min;
  std::uint16_t max;  // where the screen stops (70 for the delay's feedback)
  std::uint16_t def;
  const char* unit;               // "", "%", "ms"
  const char* const* names;       // Choice: two names
};

inline constexpr const char* kDelayModeNames[2] = {"MONO", "STEREO"};
inline constexpr const char* kReverbQualityNames[2] = {"HALF", "FULL"};
// The sync rate labels (the lengths in steps: "4" is a quarter note, "1/4" a quarter of a step).
inline constexpr const char* kDelayRateNames[audio::sendfx::kDelayRateCount] = {"8",   "6",   "4",    "3",    "2",   "3/2", "1",   "3/4",  "1/2", "3/8",
                                                                                 "1/3", "1/4", "3/16", "1/6", "1/8", "1/12", "1/16", "1/24", "1/32"};

namespace gpm = audio::graph_param::master;
inline constexpr unsigned kSendFxRows = 12;
inline constexpr unsigned kSendFxDelayRows = 7;  // the first seven are the delay's, the rest the reverb's
inline constexpr SendFxRow kSendFxTable[kSendFxRows] = {
    {"DLY PINGPONG", gpm::DelayPingPong, SendFxKind::OnOff, 0, 1, 0, "", nullptr},
    {"DLY SYNC", gpm::DelaySync, SendFxKind::OnOff, 0, 1, 0, "", nullptr},
    {"DLY RATE", gpm::DelayRate, SendFxKind::Rate, 0, audio::sendfx::kDelayRateCount - 1u, 0, "", nullptr},
    {"DLY TIME", gpm::DelayTime, SendFxKind::Time, 1, 3500, 500, "ms", nullptr},
    {"DLY FEEDBACK", gpm::DelayFeedback, SendFxKind::Number, 0, 70, 50, "%", nullptr},
    {"DLY TONE", gpm::DelayTone, SendFxKind::Number, 0, 100, 100, "%", nullptr},
    {"DLY MODE", gpm::DelayMode, SendFxKind::Choice, 0, 1, 0, "", kDelayModeNames},
    {"RVB SIZE", gpm::ReverbSize, SendFxKind::Number, 0, 100, 50, "%", nullptr},
    {"RVB DAMPING", gpm::ReverbDamp, SendFxKind::Number, 0, 100, 50, "%", nullptr},
    {"RVB PREDELAY", gpm::ReverbPredelay, SendFxKind::Number, 0, 100, 50, "%", nullptr},
    {"RVB DIFFUSE", gpm::ReverbDiffusion, SendFxKind::Number, 0, 100, 80, "%", nullptr},
    {"RVB QUALITY", gpm::ReverbQuality, SendFxKind::Choice, 0, 1, 0, "", kReverbQualityNames},
};

// The table's defaults are the effects' own (graph_params.h sendfx::): a board that is never touched says what the engine does.
static_assert(kSendFxTable[3].def == static_cast<std::uint16_t>(audio::sendfx::kDelayTimeDefault) && kSendFxTable[4].def == static_cast<std::uint16_t>(audio::sendfx::kDelayFeedbackDefault) &&
                  kSendFxTable[4].max == static_cast<std::uint16_t>(audio::sendfx::kDelayFeedbackOgMax) && kSendFxTable[7].def == static_cast<std::uint16_t>(audio::sendfx::kReverbDefault) &&
                  kSendFxTable[10].def == static_cast<std::uint16_t>(audio::sendfx::kReverbDiffusionDefault),
              "the Master FX rows start where the send effects start");

// The value `detents` turns away (a coarse turn is ten of them, three for the rate), held to the row's range. A switch or a choice goes to its end the turn points to.
constexpr int send_fx_step(const SendFxRow& row, int value, int detents, bool coarse) noexcept {
  int next = value;
  switch (row.kind) {
    case SendFxKind::OnOff:
    case SendFxKind::Choice:
      next = detents > 0 ? 1 : (detents < 0 ? 0 : value);
      break;
    case SendFxKind::Rate:
      next = value + detents * (coarse ? 3 : 1);
      break;
    case SendFxKind::Time: {
      // a detent is 10 ms below 500 and 30 ms from there on, by the value the detent starts from
      const int per = value < 500 ? 10 : 30;
      next = value + detents * per * (coarse ? 10 : 1);
      break;
    }
    case SendFxKind::Number:
      next = value + detents * (coarse ? 10 : 1);
      break;
    case SendFxKind::Ms: {
      const int per = value < 10 ? 1 : (value < 50 ? 5 : (value < 200 ? 10 : (value < 500 ? 25 : 50)));  // by the value the detent starts from, as TIME's
      next = value + detents * per * (coarse ? 5 : 1);
      break;
    }
  }
  const int lo = static_cast<int>(row.min);
  const int hi = static_cast<int>(row.max);
  return next < lo ? lo : (next > hi ? hi : next);
}

// The text of a value (no unit): a number, ON / OFF, a rate label or a choice's name. Returns the length written (a NUL follows; `cap` at least 8).
inline std::size_t send_fx_text(const SendFxRow& row, int value, char* out, std::size_t cap) noexcept {
  const char* fixed = nullptr;
  switch (row.kind) {
    case SendFxKind::OnOff: fixed = value != 0 ? "ON" : "OFF"; break;
    case SendFxKind::Rate: fixed = kDelayRateNames[value < 0 ? 0 : (value >= static_cast<int>(audio::sendfx::kDelayRateCount) ? audio::sendfx::kDelayRateCount - 1u : static_cast<unsigned>(value))]; break;
    case SendFxKind::Choice: fixed = row.names[value != 0 ? 1 : 0]; break;
    default: break;
  }
  std::size_t n = 0;
  if (fixed != nullptr) {
    for (; fixed[n] != '\0' && n + 1 < cap; ++n) {
      out[n] = fixed[n];
    }
  } else {
    char digits[8];
    std::size_t d = 0;
    unsigned v = value < 0 ? 0u : static_cast<unsigned>(value);
    do {
      digits[d++] = static_cast<char>('0' + v % 10u);
      v /= 10u;
    } while (v != 0u && d < sizeof digits);
    while (d != 0u && n + 1 < cap) {
      out[n++] = digits[--d];
    }
  }
  out[n] = '\0';
  return n;
}

}  // namespace o3os::app
