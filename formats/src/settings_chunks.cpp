// SPDX-License-Identifier: GPL-3.0-or-later
#include "settings_chunks.h"

#include <o3os/model/params.h>

namespace o3os::formats::settings_chunks {

using native::codec::Cursor;
using native::codec::Enc;

std::size_t sets_payload_size(const model::Settings& s) noexcept { return kFixed + s.field_count() * kEntry; }

std::size_t lrnb_payload_size(const model::Settings& s) noexcept { return kFixed + s.learn_count() * kEntry; }

void encode_sets(Enc& e, const model::Settings& s) noexcept {
  e.u16(kVersion);
  e.u16(static_cast<std::uint16_t>(s.field_count()));
  for (unsigned f = 0; f < s.field_count(); ++f) {
    e.u32(static_cast<std::uint32_t>(core::param_id(s.kind(), 0, f)));
    e.f32(s.get(f));
  }
}

void encode_lrnb(Enc& e, const model::Settings& s) noexcept {
  e.u16(kVersion);
  e.u16(static_cast<std::uint16_t>(s.learn_count()));
  for (unsigned i = 0; i < core::kLearnSlots; ++i) {
    const core::LearnBinding& b = s.learn(i);
    if (!core::learn_empty(b)) {
      e.u32(static_cast<std::uint32_t>(b.param));
      e.u8(b.cc);
      e.u8(b.channel);
      e.u8(b.mode);
      e.u8(b.flags);
    }
  }
}

bool decode_sets(Cursor& c, bool newer_minor, model::Settings& out, Counts& counts) noexcept {
  (void)c.u16();  // the version: checked by the caller before the length is judged
  const unsigned count = c.u16();
  const unsigned take = count > kMaxSetsEntries ? kMaxSetsEntries : count;  // a newer minor may carry more than this version can name
  if (c.bad() || (count > kMaxSetsEntries && !newer_minor) || (newer_minor ? c.left() < take * kEntry : c.left() != count * kEntry)) {
    return false;
  }
  counts.skipped += count - take;
  for (unsigned i = 0; i < take; ++i) {
    const core::ParamId id = static_cast<core::ParamId>(c.u32());
    const float value = c.f32();
    if (core::param_kind(id) != out.kind() || core::param_index(id) != 0 ||
        out.set(core::param_field(id), value) != model::Status::Ok) {
      ++counts.skipped;  // not a parameter of this scope, or a value it cannot take: the default stays
    }
  }
  return !c.bad();
}

bool decode_lrnb(Cursor& c, bool newer_minor, model::Settings& out, Counts& counts) noexcept {
  (void)c.u16();
  const unsigned count = c.u16();
  const unsigned take = count > kMaxLrnbEntries ? kMaxLrnbEntries : count;
  if (c.bad() || (count > kMaxLrnbEntries && !newer_minor) || (newer_minor ? c.left() < take * kEntry : c.left() != count * kEntry)) {
    return false;
  }
  counts.skipped += count - take;
  for (unsigned i = 0; i < take; ++i) {
    core::LearnBinding b{};
    b.param = static_cast<core::ParamId>(c.u32());
    b.cc = c.u8();
    b.channel = c.u8();
    b.mode = c.u8();
    b.flags = c.u8();
    core::LearnPlan plan{0, 0};
    if (core::learn_empty(b) || !core::learn_plan(out.learn_slots(), core::kLearnSlots, b, plan) ||
        out.set_learn(static_cast<unsigned>(plan.write), b) != model::Status::Ok) {
      ++counts.skipped;  // invalid, of the other scope, or the store is full
      continue;
    }
    for (unsigned s = 0; s < core::kLearnSlots; ++s) {
      if ((plan.clear & (std::uint32_t{1} << s)) != 0) {
        (void)out.set_learn(s, core::LearnBinding{});
      }
    }
  }
  return !c.bad();
}

}  // namespace o3os::formats::settings_chunks
