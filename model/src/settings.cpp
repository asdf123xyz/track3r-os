// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/model/settings.h>

#include <o3os/model/params.h>

#include <cstring>

namespace o3os::model {

namespace {

const core::LearnBinding kEmptyBinding = {core::ParamId::None, 0, 0, 0, 0};

core::LearnScope scope_of(core::ParamKind kind) noexcept {
  return kind == core::ParamKind::Device ? core::LearnScope::Device : core::LearnScope::Project;
}

bool find_field(core::ParamKind kind, unsigned field, ParamField& out) noexcept {
  return find_param(core::param_id(kind, 0, field), out);
}

}  // namespace

unsigned Settings::field_count() const noexcept { return param_slots_per_index(kind_); }

void Settings::init(core::ParamKind kind) noexcept {
  kind_ = kind;
  std::memset(raw_, 0, sizeof raw_);
  ParamField f;
  for (unsigned i = 0; i < field_count(); ++i) {
    if (find_field(kind_, i, f)) {
      (void)param_store(raw_, f, f.desc->def);
    }
  }
  std::memset(learn_, 0, sizeof learn_);
  ++epoch_;  // every generation moves, and none can repeat what it was before
}

float Settings::get(unsigned field) const noexcept {
  ParamField f;
  return field < field_count() && find_field(kind_, field, f) ? param_load(raw_, f) : 0.0f;
}

Status Settings::set(unsigned field, float value) noexcept {
  ParamField f;
  if (field >= field_count() || !find_field(kind_, field, f)) {
    return Status::BadIndex;
  }
  const float before = param_load(raw_, f);
  if (!param_store(raw_, f, value)) {
    return Status::BadValue;
  }
  if (param_load(raw_, f) != before) {
    ++gen_[field];
  }
  return Status::Ok;
}

std::uint32_t Settings::generation(unsigned field) const noexcept {
  return field < kMaxFields ? epoch_ + gen_[field] : 0u;
}

const core::LearnBinding& Settings::learn(unsigned slot) const noexcept {
  return slot < core::kLearnSlots ? learn_[slot] : kEmptyBinding;
}

Status Settings::set_learn(unsigned slot, const core::LearnBinding& b) noexcept {
  if (slot >= core::kLearnSlots) {
    return Status::BadIndex;
  }
  if (!core::learn_valid(b) || (!core::learn_empty(b) && core::learn_scope(core::param_kind(b.param)) != scope_of(kind_))) {
    return Status::BadValue;
  }
  if (std::memcmp(&learn_[slot], &b, sizeof b) != 0) {
    learn_[slot] = b;
    ++learn_gen_;
  }
  return Status::Ok;
}

unsigned Settings::learn_count() const noexcept {
  unsigned n = 0;
  for (const core::LearnBinding& b : learn_) {
    n += core::learn_empty(b) ? 0u : 1u;
  }
  return n;
}

bool Settings::values_default() const noexcept {
  ParamField f;
  for (unsigned i = 0; i < field_count(); ++i) {
    if (find_field(kind_, i, f) && param_load(raw_, f) != f.desc->def) {
      return false;
    }
  }
  return true;
}

bool Settings::check_invariants() const noexcept {
  if (kind_ != core::ParamKind::Project && kind_ != core::ParamKind::Device) {
    return false;
  }
  ParamField f;
  for (unsigned i = 0; i < field_count(); ++i) {
    if (!find_field(kind_, i, f) || !core::param_in_range(*f.desc, param_load(raw_, f))) {
      return false;
    }
  }
  for (unsigned i = field_count(); i < kValueBytes; ++i) {
    if (raw_[i] != 0) {
      return false;
    }
  }
  for (const core::LearnBinding& b : learn_) {
    if (!core::learn_empty(b) && core::learn_scope(core::param_kind(b.param)) != scope_of(kind_)) {
      return false;
    }
  }
  return core::learn_store_valid(learn_, core::kLearnSlots);
}

Status Settings::assign(const Settings& other) noexcept {
  if (other.kind_ != kind_ || !other.check_invariants()) {
    return Status::BadValue;
  }
  for (unsigned i = 0; i < field_count(); ++i) {
    if (raw_[i] != other.raw_[i]) {
      ++gen_[i];
    }
  }
  std::memcpy(raw_, other.raw_, sizeof raw_);
  if (std::memcmp(learn_, other.learn_, sizeof learn_) != 0) {
    std::memcpy(learn_, other.learn_, sizeof learn_);
    ++learn_gen_;
  }
  return Status::Ok;
}

}  // namespace o3os::model
