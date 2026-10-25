// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/song_tools_view.h>

#include <o3os/model/step_edit.h>

namespace o3os::app {

namespace {

constexpr const char* kToolNames[4] = {"TRANSPOSE", "INSTRUMENT", "CLEAR", "EXPORT WAV"};
constexpr const char* kWhatNames[5] = {"NOTES", "VOLUME", "FX 1", "FX 2", "ALL"};

using core::ParamCurve;
using core::ParamDesc;
using core::ParamUnit;

// name, unit text, names, knots, min, max, def, detents, coarse, list_count, unit, curve, flags, group
constexpr ParamDesc kOp = {"OP", "", kToolNames, nullptr, 0.0f, 3.0f, 0.0f, 3, 1, 4, ParamUnit::Enum, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kAmount = {"AMOUNT", "st", nullptr, nullptr, -24.0f, 24.0f, 0.0f, 48, 12, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kTo = {"TO", "", nullptr, nullptr, 1.0f, static_cast<float>(model::edit::kInstrumentSlots), 1.0f, model::edit::kInstrumentSlots - 1, 8, 0, ParamUnit::Int, ParamCurve::Linear, 0, 0};
constexpr ParamDesc kWhat = {"WHAT", "", kWhatNames, nullptr, 0.0f, 4.0f, 4.0f, 4, 1, 5, ParamUnit::Enum, ParamCurve::Linear, 0, 0};

bool enumerated(const SongToolsView& v, SongToolsRow row) noexcept { return row == SongToolsRow::Op || (row == SongToolsRow::Value && v.tool == SongTool::Clear); }

}  // namespace

unsigned song_tools_rows(const SongToolsView& v) noexcept { return v.tool == SongTool::ExportWav ? 1u : kSongToolsRows; }

const ParamDesc& song_tools_desc(const SongToolsView& v, SongToolsRow row) noexcept {
  if (row == SongToolsRow::Op) {
    return kOp;
  }
  return v.tool == SongTool::Transpose ? kAmount : (v.tool == SongTool::Instrument ? kTo : kWhat);
}

const char* song_tools_caption(const SongToolsView& v, SongToolsRow row) noexcept { return song_tools_desc(v, row).name; }

float song_tools_get(const SongToolsView& v, SongToolsRow row) noexcept {
  if (row == SongToolsRow::Op) {
    return static_cast<float>(v.tool);
  }
  switch (v.tool) {
    case SongTool::Transpose: return static_cast<float>(v.amount);
    case SongTool::Instrument: return static_cast<float>(v.instrument);
    case SongTool::Clear: return static_cast<float>(v.what);
    case SongTool::ExportWav: break;
  }
  return 0.0f;
}

void song_tools_set(SongToolsView& v, SongToolsRow row, float value) noexcept {
  const float clamped = core::param_clamp(song_tools_desc(v, row), value);
  const int n = static_cast<int>(clamped + (clamped < 0.0f ? -0.5f : 0.5f));
  if (row == SongToolsRow::Op) {
    v.tool = static_cast<SongTool>(n);
    return;
  }
  switch (v.tool) {
    case SongTool::Transpose: v.amount = static_cast<std::int8_t>(n); break;
    case SongTool::Instrument: v.instrument = static_cast<std::uint8_t>(n); break;
    case SongTool::Clear: v.what = static_cast<SongWhat>(n); break;
    case SongTool::ExportWav: break;
  }
}

bool song_tools_step(SongToolsView& v, SongToolsRow row, int detents) noexcept {
  if (detents == 0) {
    return false;
  }
  const ParamDesc& d = song_tools_desc(v, row);
  const float now = song_tools_get(v, row);
  float to;
  if (enumerated(v, row)) {
    const int count = static_cast<int>(d.max - d.min) + 1;
    int k = (static_cast<int>(now - d.min) + detents) % count;
    k = k < 0 ? k + count : k;
    to = d.min + static_cast<float>(k);
  } else {
    to = core::param_step(d, now, detents);
  }
  song_tools_set(v, row, to);
  return song_tools_get(v, row) != now;
}

unsigned song_tools_text(const SongToolsView& v, SongToolsRow row, char* out, unsigned cap) noexcept {
  if (cap == 0u) {
    return 0u;
  }
  char tmp[core::kParamTextChars + 4];
  const char* text = tmp;
  if (row == SongToolsRow::Value && v.tool == SongTool::Transpose) {
    unsigned n = 0;
    const int a = v.amount;
    if (a != 0) {
      tmp[n++] = a < 0 ? '-' : '+';
    }
    const unsigned mag = static_cast<unsigned>(a < 0 ? -a : a);
    if (mag >= 10u) {
      tmp[n++] = static_cast<char>('0' + mag / 10u);
    }
    tmp[n++] = static_cast<char>('0' + mag % 10u);
    tmp[n] = '\0';
  } else {
    (void)core::param_format(song_tools_desc(v, row), song_tools_get(v, row), tmp, sizeof tmp);
  }
  unsigned n = 0;
  while (text[n] != '\0' && n + 1u < cap) {
    out[n] = text[n];
    ++n;
  }
  out[n] = '\0';
  return n;
}

std::uint8_t song_tools_elements(SongWhat what) noexcept {
  switch (what) {
    case SongWhat::Notes: return static_cast<std::uint8_t>(model::edit::kElNote);
    case SongWhat::Volume: return static_cast<std::uint8_t>(model::edit::kElVol);
    case SongWhat::Fx1: return static_cast<std::uint8_t>(model::edit::kElFx1);
    case SongWhat::Fx2: return static_cast<std::uint8_t>(model::edit::kElFx2);
    case SongWhat::All: return static_cast<std::uint8_t>(model::edit::kElAll);
  }
  return static_cast<std::uint8_t>(model::edit::kElAll);
}

}  // namespace o3os::app
