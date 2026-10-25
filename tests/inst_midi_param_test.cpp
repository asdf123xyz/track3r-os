// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-INSTUI S8b + S9: the twelve parameter rows of INST 1.3 (model/params.h kMidiFirst..): VELOCITY, PROGRAM, OUT, PB RANGE, OFFSET, CC A..F, VEL>. The descriptors (ranges, defaults, names, units),
// the ids and the generation slots, the pages they are listed on, the displays of the Program and CC-slot units (format and parse), the edit through the Project and the Editor (one undo unit each,
// a generation per row), INIT, and that the rows stay out of the engine's table and Learn. Host and Cortex-M4F.
#include <doctest/doctest.h>

#include <o3os/core/param.h>
#include <o3os/model/editor.h>
#include <o3os/model/instrument.h>
#include <o3os/model/params.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstring>
#include <set>
#include <string>

#include "support/need.h"

using namespace o3os;
using core::ParamId;
using core::ParamKind;
namespace ip = model::instrument_param;

namespace {

model::ModelArena g_arena;
alignas(8) std::uint8_t g_ring[16 * 1024];

model::Project& project() { return g_arena.project; }
ParamId inst(unsigned slot, unsigned field) { return core::param_id(ParamKind::Instrument, slot, field); }

void two_instruments() {
  project().reset();
  model::Instrument s;
  model::init_instrument(s, model::InstrumentKind::Sample);
  CHECK(project().set_instrument(0, s) == model::Status::Ok);
  model::Instrument m;
  model::init_instrument(m, model::InstrumentKind::Midi);
  m.midi_channel = 4;
  CHECK(project().set_instrument(1, m) == model::Status::Ok);
}

std::string text(const core::ParamDesc& d, float v) {
  char buf[core::kParamTextChars + 1] = {};
  (void)core::param_format(d, v, buf, sizeof buf);
  return buf;
}

}  // namespace

TEST_CASE("midi params: the twelve rows exist, are valid descriptors, and have the defaults a fresh instrument has") {
  model::Instrument fresh;
  model::init_instrument(fresh, model::InstrumentKind::Midi);
  const float defaults[ip::kMidiRows] = {static_cast<float>(fresh.midi_velocity), static_cast<float>(fresh.midi_program), static_cast<float>(fresh.midi_out), static_cast<float>(fresh.midi_pb_range),
                                         static_cast<float>(fresh.midi_offset), static_cast<float>(fresh.midi_cc[0]), static_cast<float>(fresh.midi_cc[1]), static_cast<float>(fresh.midi_cc[2]),
                                         static_cast<float>(fresh.midi_cc[3]), static_cast<float>(fresh.midi_cc[4]), static_cast<float>(fresh.midi_cc[5]), static_cast<float>(fresh.vel_sens)};
  const char* const names[ip::kMidiRows] = {"VELOCITY", "PROGRAM", "OUT", "PB RANGE", "OFFSET", "CC A", "CC B", "CC C", "CC D", "CC E", "CC F", "VEL>"};
  const float lo[ip::kMidiRows] = {1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  const float hi[ip::kMidiRows] = {127, 128, 3, 24, 100, 131, 131, 131, 131, 131, 131, 100};
  for (unsigned r = 0; r < ip::kMidiRows; ++r) {
    const core::ParamDesc* d = model::param_desc(inst(5, ip::kMidiFirst + r));
    NEED(d != nullptr);
    CHECK_MESSAGE(core::param_desc_valid(*d), names[r]);
    CHECK(std::string(d->name) == names[r]);
    CHECK_MESSAGE(d->def == defaults[r], names[r]);
    CHECK_MESSAGE((d->min == lo[r] && d->max == hi[r]), names[r]);
    CHECK_MESSAGE((d->flags & (core::kParamLearnable | core::kParamModulatable | core::kParamApply)) == core::kParamLearnable, names[r]);  // every parameter is a Learn target (product-ux decision 4)
    CHECK_MESSAGE(d->group == (r == 11 ? ip::GroupVoice : (r >= 5 ? ip::GroupCc : ip::GroupMidi)), names[r]);
  }
  CHECK(model::param_desc(inst(5, ip::kMidiFirst + ip::kMidiRows)) == nullptr);
  CHECK(model::param_desc(inst(64, ip::MidiVelocity)) == nullptr);
  // the ids keep clear of the engine's live ids and of everything that existed
  CHECK((ip::kMidiFirst == 0x80 && ip::MidiVelocity == 0x80 && ip::MidiProgram == 0x81 && ip::MidiOut == 0x82 && ip::MidiPbRange == 0x83 && ip::MidiOffset == 0x84 && ip::kMidiCcFirst == 0x85 &&
         ip::VelSens == 0x8B));
  CHECK(ip::midi_cc(5) == 0x8A);
  CHECK(ip::kModFirst + ip::kEnvelopes * ip::kModStride <= ip::kMidiFirst);
}

TEST_CASE("midi params: the generation slots cover every row once, in both directions") {
  std::set<unsigned> seen;
  for (unsigned slot = 0; slot < ip::kCount; ++slot) {
    const unsigned field = model::param_field_of_slot(ParamKind::Instrument, slot);
    NEED(field != 0xFFFFu);
    CHECK(model::param_slot(inst(0, field)) == slot);
    seen.insert(field);
  }
  CHECK(seen.size() == ip::kCount);
  CHECK(model::param_slots_per_index(ParamKind::Instrument) == 95u);
  for (unsigned r = 0; r < ip::kMidiRows; ++r) {
    CHECK(model::param_slot(inst(7, ip::kMidiFirst + r)) == 83u + r);
  }
  CHECK(model::param_field_of_slot(ParamKind::Instrument, 95) == 0xFFFFu);
}

TEST_CASE("midi params: the pages list their rows: MIDI five (the screen puts the virtual CHANNEL before them), CC six, VEL> closes VOICE") {
  std::uint16_t f[16] = {};
  NEED(model::param_group_fields(ParamKind::Instrument, ip::GroupMidi, 0, f, 16) == 5u);
  CHECK((f[0] == ip::MidiVelocity && f[1] == ip::MidiProgram && f[2] == ip::MidiOut && f[3] == ip::MidiPbRange && f[4] == ip::MidiOffset));
  NEED(model::param_group_fields(ParamKind::Instrument, ip::GroupCc, 0, f, 16) == 6u);
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(f[i] == ip::midi_cc(i));
  }
  const unsigned n = model::param_group_fields(ParamKind::Instrument, ip::GroupVoice, 0, f, 16);
  NEED(n == 6u);
  CHECK((f[0] == ip::Volume && f[n - 1] == ip::VelSens));
  CHECK(std::string(model::param_group_name(ParamKind::Instrument, ip::GroupMidi)) == "MIDI");
  CHECK(std::string(model::param_group_name(ParamKind::Instrument, ip::GroupCc)) == "CC");
  CHECK(ip::kGroups == 5u);  // a sample instrument's pages are what they were
}

TEST_CASE("midi params: PROGRAM shows NONE or the program 0..127; a CC slot shows CONFIG, CC n, PROG, CH AT, POLY AT; both parse back") {
  const core::ParamDesc& prog = *model::param_desc(inst(0, ip::MidiProgram));
  CHECK(text(prog, 0) == "NONE");
  CHECK(text(prog, 1) == "0");
  CHECK(text(prog, 128) == "127");
  const core::ParamDesc& cc = *model::param_desc(inst(0, ip::midi_cc(0)));
  CHECK(text(cc, 0) == "CONFIG");
  CHECK(text(cc, 1) == "CC 0");
  CHECK(text(cc, 75) == "CC 74");
  CHECK(text(cc, 128) == "CC 127");
  CHECK(text(cc, 129) == "PROG");
  CHECK(text(cc, 130) == "CH AT");
  CHECK(text(cc, 131) == "POLY AT");
  const core::ParamDesc& out = *model::param_desc(inst(0, ip::MidiOut));
  CHECK(text(out, 0) == "Config");
  CHECK(text(out, 3) == "DIN + USB");
  float v = -1.0f;
  CHECK((core::param_parse(prog, "none", v) && v == 0.0f));
  CHECK((core::param_parse(prog, "0", v) && v == 1.0f));
  CHECK((core::param_parse(prog, "127", v) && v == 128.0f));
  CHECK_FALSE(core::param_parse(prog, "128", v));
  CHECK_FALSE(core::param_parse(prog, "-1", v));
  CHECK_FALSE(core::param_parse(prog, "1.5", v));
  CHECK((core::param_parse(cc, "config", v) && v == 0.0f));
  CHECK((core::param_parse(cc, "CC 74", v) && v == 75.0f));
  CHECK((core::param_parse(cc, "cc74", v) && v == 75.0f));
  CHECK((core::param_parse(cc, "127", v) && v == 128.0f));
  CHECK((core::param_parse(cc, "PROG", v) && v == 129.0f));
  CHECK((core::param_parse(cc, "ch at", v) && v == 130.0f));
  CHECK((core::param_parse(cc, "POLY AT", v) && v == 131.0f));
  CHECK_FALSE(core::param_parse(cc, "CC 128", v));
  CHECK_FALSE(core::param_parse(cc, "banana", v));
  for (float x = 0.0f; x <= 131.0f; x += 1.0f) {  // every value parses back from its own text
    float back = -1.0f;
    CHECK(core::param_parse(cc, text(cc, x).c_str(), back));
    CHECK(back == x);
  }
  for (float x = 0.0f; x <= 128.0f; x += 1.0f) {
    float back = -1.0f;
    CHECK(core::param_parse(prog, text(prog, x).c_str(), back));
    CHECK(back == x);
  }
}

TEST_CASE("midi params: set_param writes the byte of the instrument, counts one generation per change, and refuses a value outside the range; undo and redo take it back") {
  two_instruments();
  model::UndoJournal journal;
  journal.begin(g_ring, sizeof g_ring);
  model::Editor e(project(), journal);
  const struct {
    unsigned field;
    float value;
    std::uint8_t model::Instrument::*member;
  } rows[] = {{ip::MidiVelocity, 90, &model::Instrument::midi_velocity}, {ip::MidiProgram, 12, &model::Instrument::midi_program},     {ip::MidiOut, 2, &model::Instrument::midi_out},
              {ip::MidiPbRange, 7, &model::Instrument::midi_pb_range},  {ip::MidiOffset, 33, &model::Instrument::midi_offset},       {ip::VelSens, 25, &model::Instrument::vel_sens}};
  for (const auto& r : rows) {
    const std::uint32_t g0 = project().param_generation(inst(1, r.field));
    const std::uint32_t ig = project().instrument_generation(1);
    CHECK(e.set_param(inst(1, r.field), r.value) == model::Status::Ok);
    CHECK(static_cast<float>(project().instrument(1)->*r.member) == r.value);
    CHECK(project().get_param(inst(1, r.field)) == r.value);
    CHECK(project().param_generation(inst(1, r.field)) == g0 + 1u);
    CHECK(project().instrument_generation(1) == ig + 1u);
    CHECK(e.set_param(inst(1, r.field), r.value) == model::Status::Ok);  // the same value: no change, no generation
    CHECK(project().param_generation(inst(1, r.field)) == g0 + 1u);
    CHECK(model::validate_instrument(*project().instrument(1), 0) == nullptr);
  }
  // each CC slot is its own byte
  for (unsigned i = 0; i < 6; ++i) {
    CHECK(e.set_param(inst(1, ip::midi_cc(i)), static_cast<float>(10 + i * 20)) == model::Status::Ok);
    CHECK(project().instrument(1)->midi_cc[i] == 10u + i * 20u);
  }
  // out of range: refused, nothing written
  CHECK(e.set_param(inst(1, ip::MidiVelocity), 0) != model::Status::Ok);
  CHECK(e.set_param(inst(1, ip::MidiVelocity), 128) != model::Status::Ok);
  CHECK(e.set_param(inst(1, ip::MidiOut), 4) != model::Status::Ok);
  CHECK(e.set_param(inst(1, ip::midi_cc(0)), 132) != model::Status::Ok);
  CHECK(project().instrument(1)->midi_velocity == 90);
  // the fields are the instrument's, whatever its kind: a sample instrument takes VEL> (and keeps the rest at their defaults)
  CHECK(e.set_param(inst(0, ip::VelSens), 40) == model::Status::Ok);
  CHECK(project().instrument(0)->vel_sens == 40);
  CHECK(e.set_param(inst(30, ip::VelSens), 40) == model::Status::NotFound);  // a free slot
  // undo and redo, one unit each
  CHECK(e.undo() == model::UndoResult::Ok);
  CHECK(project().instrument(0)->vel_sens == 100);
  CHECK(e.redo() == model::UndoResult::Ok);
  CHECK(project().instrument(0)->vel_sens == 40);
  for (int i = 0; i < 7; ++i) {
    CHECK(e.undo() == model::UndoResult::Ok);  // the VEL> of slot 0 (redone above), then CC F .. CC A, each its own unit
  }
  CHECK(project().instrument(1)->midi_cc[5] == 0);
  CHECK(project().instrument(1)->midi_cc[0] == 0);
  CHECK(project().instrument(1)->midi_offset == 33);
}

TEST_CASE("midi params: INIT (reset_instrument_params) puts every row back to its default and leaves the channel") {
  two_instruments();
  model::Instrument m = *project().instrument(1);
  m.midi_velocity = 50;
  m.midi_program = 9;
  m.midi_out = 3;
  m.midi_pb_range = 24;
  m.midi_offset = 100;
  for (std::uint8_t& c : m.midi_cc) {
    c = 131;
  }
  m.vel_sens = 3;
  model::reset_instrument_params(m);
  CHECK(model::inst_v13_default(m));
  CHECK(m.midi_channel == 4);
  CHECK(m.kind == model::InstrumentKind::Midi);
}
