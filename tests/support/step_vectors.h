// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Helpers of the pattern-edit vector tests (pattern_edit_rules_test.cpp, pattern_edit_app_test.cpp, pad_board_test.cpp).
//
// The expected values of those tests are the output of this project's own reference model (maintainer, 2026-10-18), kept once as test vectors
// (tests/data/pattern_edit_vectors.h): a sweep over many inputs keeps one 64-bit FNV-1a digest of all its results per group of inputs, a
// few cases are also written out in full. Nothing here computes what the reference model does; this header only
//   * maps between the vectors' step encoding (a signed note: -1 empty, -2 fade, -3 cut, -4 off, 0..127; an instrument as a slot
//     0..63) and the model's (step.h: note byte, instrument slot + 1), and
//   * reduces a result (a step and the session's "last used" values) to the same eight bytes on the recording side and on this one.
//
// A result is compared in a canonical form so that two things the OS does differently on purpose do not show: the model's instrument 0
// ("names none") against the vectors' slot 0 on a step without a note, and an FX value left behind under a None type (the OS never
// writes one, the vectors sometimes hold one).

#include <o3os/model/step.h>
#include <o3os/model/step_edit.h>

#include <cstddef>
#include <cstdint>

namespace o3os::test::vec {

enum : int { kNoteEmpty = -1, kNoteFade = -2, kNoteCut = -3, kNoteOff = -4 };

// The model's note byte of a vectors' note (-4..127).
inline std::uint8_t to_model_note(int og_note) {
  switch (og_note) {
    case kNoteEmpty:
      return model::kNoteEmpty;
    case kNoteFade:
      return model::kNoteFade;
    case kNoteCut:
      return model::kNoteCut;
    case kNoteOff:
      return model::kNoteOff;
    default:
      return static_cast<std::uint8_t>(og_note + 1);
  }
}

// The vectors' note of a model note byte (the reserved bytes 129..252 are not comparable and give 0x7F).
inline int from_model_note(std::uint8_t m) {
  if (m == model::kNoteEmpty) return kNoteEmpty;
  if (m == model::kNoteFade) return kNoteFade;
  if (m == model::kNoteCut) return kNoteCut;
  if (m == model::kNoteOff) return kNoteOff;
  return m <= 128 ? m - 1 : 0x7F;
}

// One result, canonical: the eight bytes a digest takes.
struct Out {
  std::uint8_t note;   // the vectors' note as a byte (-1 = 0xFF ...)
  std::uint8_t instr;  // the vectors' slot; 0 for "none"; 0xFF for a model step that has a note and names no instrument
  std::uint8_t type[2];
  std::uint8_t value[2];  // 0 where the type is None
  std::uint8_t last_slot;
  std::uint8_t last_fx;
};

inline Out out_of(const model::Step& m, const model::edit::Last& last) {
  Out o{};
  o.note = static_cast<std::uint8_t>(static_cast<std::int8_t>(from_model_note(m.note)));
  o.instr = m.instr != 0 ? static_cast<std::uint8_t>(m.instr - 1) : (from_model_note(m.note) >= 0 ? std::uint8_t{0xFF} : std::uint8_t{0});
  for (int k = 0; k < 2; ++k) {
    o.type[k] = m.fx[k].type;
    o.value[k] = m.fx[k].type != 0 ? m.fx[k].value : std::uint8_t{0};
  }
  o.last_slot = static_cast<std::uint8_t>(last.instrument - 1);
  o.last_fx = last.fx;
  return o;
}

// A session only (the "last used" values of a sweep's end): the bytes of a step that has nothing.
inline Out session_out(unsigned slot, unsigned name) {
  Out o{};
  o.last_slot = static_cast<std::uint8_t>(slot);
  o.last_fx = static_cast<std::uint8_t>(name);
  return o;
}

// The result of an operation that has no session (a step only, an instrument walk ...): `last` is the default.
inline Out out_of(const model::Step& m) { return out_of(m, model::edit::Last{}); }

// FNV-1a, 64 bits, over a sequence of results.
class Digest {
 public:
  void add(const Out& o) {
    const std::uint8_t b[8] = {o.note, o.instr, o.type[0], o.value[0], o.type[1], o.value[1], o.last_slot, o.last_fx};
    add_bytes(b, sizeof b);
  }
  void add_bytes(const std::uint8_t* p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
      h_ = (h_ ^ p[i]) * 0x100000001B3ull;
    }
  }
  void add_u32(std::uint32_t v) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
                               static_cast<std::uint8_t>(v >> 24)};
    add_bytes(b, sizeof b);
  }
  std::uint64_t value() const { return h_; }
  void reset() { h_ = 0xCBF29CE484222325ull; }

 private:
  std::uint64_t h_ = 0xCBF29CE484222325ull;
};

// The eight bytes of a result in the order of a digest and of vectors::StepCase::want.
inline void bytes_of(const Out& o, std::uint8_t (&b)[8]) {
  const std::uint8_t v[8] = {o.note, o.instr, o.type[0], o.value[0], o.type[1], o.value[1], o.last_slot, o.last_fx};
  for (std::size_t i = 0; i < 8; ++i) b[i] = v[i];
}

}  // namespace o3os::test::vec
