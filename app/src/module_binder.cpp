// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/app/module_binder.h>

#include <o3os/audio/diag.h>

#include <cstring>

namespace o3os::app {

namespace {

using audio::ModuleVoices;
using Kind = ModuleProblem::Kind;

constexpr std::uint8_t kModuleProblem = 0xFF;

const char* bind_name(ModuleVoices::Bind b) noexcept {
  switch (b) {
    case ModuleVoices::Bind::Ok: return "ok";
    case ModuleVoices::Bind::NoScratch: return "no output blocks";
    case ModuleVoices::Bind::BadSlot: return "bad slot";
    case ModuleVoices::Bind::SlotTaken: return "slot already bound";
    case ModuleVoices::Bind::NoInstance: return "too many module instruments";
    case ModuleVoices::Bind::BadState: return "bad state block";
    case ModuleVoices::Bind::InitFailed: return "init() refused";
  }
  return "?";
}

void copy_name(char* dst, const char* src) noexcept {
  std::size_t n = 0;
  while (src[n] != '\0' && n < module::kStemMax) {
    dst[n] = src[n];
    ++n;
  }
  dst[n] = '\0';
}

}  // namespace

void ModuleBinder::note(Kind kind, unsigned slot, const char* name, module::Status status, ModuleVoices::Bind bind) noexcept {
  if (report_.problem_count >= ModuleReport::kMaxProblems) {
    ++report_.problems_dropped;
    return;
  }
  ModuleProblem& p = report_.problems[report_.problem_count++];
  p.kind = kind;
  p.slot = static_cast<std::uint8_t>(slot);
  p.status = status;
  p.bind = bind;
  copy_name(p.name, name);
}

bool ModuleBinder::begin(hal::storage::Storage& storage, const model::Project& project, module::ModuleSet& set, module::ModuleLoader& loader,
                         audio::ModuleVoices& voices, module::ModuleLoader::SyncFn sync) noexcept {
  if (state_ == State::Running) {
    return false;
  }
  storage_ = &storage;
  project_ = &project;
  set_ = &set;
  loader_ = &loader;
  voices_ = &voices;
  sync_ = sync;
  report_ = ModuleReport{};
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project.instrument(i);
    report_.instruments += (in != nullptr && in->kind == model::InstrumentKind::Module) ? 1u : 0u;
  }
  for (std::uint32_t m = 0; m < project.module_count(); ++m) {
    report_.modules += wanted(m) ? 1u : 0u;
  }
  cursor_ = 0;
  loading_ = false;
  phase_ = Phase::Load;
  state_ = State::Running;
  return true;
}

// The module table's entry `module_index` is named by a Module instrument.
bool ModuleBinder::wanted(unsigned module_index) const noexcept {
  for (std::uint32_t i = 0; i < model::kMaxInstruments; ++i) {
    const model::Instrument* in = project_->instrument(i);
    if (in != nullptr && in->kind == model::InstrumentKind::Module && in->module_index == module_index) {
      return true;
    }
  }
  return false;
}

void ModuleBinder::load_next(std::size_t budget) noexcept {
  if (cursor_ >= project_->module_count()) {
    phase_ = Phase::Bind;
    cursor_ = 0;
    return;
  }
  const model::ModuleRef& ref = *project_->module(cursor_);
  if (!loading_) {
    if (!wanted(cursor_)) {
      ++cursor_;
      return;
    }
    if (set_->find(ref.name) >= 0) {
      ++report_.already;
      ++cursor_;
      return;
    }
    if (!loader_->begin(*storage_, *set_, ref.name, sync_)) {
      note(loader_->status() == module::Status::Quarantined ? Kind::Quarantined : Kind::Refused, kModuleProblem, ref.name, loader_->status(),
           ModuleVoices::Bind::Ok);
      ++cursor_;
      return;
    }
    loading_ = true;
  }
  const module::ModuleLoader::State s = loader_->step(budget);
  if (s == module::ModuleLoader::State::Running) {
    return;
  }
  loading_ = false;
  if (s == module::ModuleLoader::State::Done) {
    ++report_.loaded;
    const module::LoadedModule& m = loader_->module();
    report_.abi_differs += (ref.abi_major != 0u && ref.abi_major != m.header.abi_major) ? 1u : 0u;
  } else {
    const module::Status why = loader_->status();
    Kind k = Kind::Refused;
    if (why == module::Status::NotFound) {
      k = Kind::NotFound;
    } else if (why == module::Status::NoMemory) {
      k = Kind::NoRoom;
    } else if (why == module::Status::Io) {
      k = Kind::CardError;
    }
    note(k, kModuleProblem, ref.name, why, ModuleVoices::Bind::Ok);
  }
  ++cursor_;
}

void ModuleBinder::bind_next() noexcept {
  while (cursor_ < model::kMaxInstruments) {
    const unsigned slot = cursor_++;
    const model::Instrument* in = project_->instrument(slot);
    if (in == nullptr || in->kind != model::InstrumentKind::Module) {
      continue;
    }
    const model::ModuleRef* ref = project_->module(in->module_index);
    const int idx = ref != nullptr ? set_->find(ref->name) : -1;
    if (idx < 0) {
      ++report_.muted;  // the module's own problem is in the list already (NotFound, Refused ...)
      continue;
    }
    const module::LoadedModule& m = *set_->at(static_cast<unsigned>(idx));
    if (m.header.kind != O3M_KIND_INSTRUMENT) {
      note(Kind::WrongKind, slot, ref->name, module::Status::BadKind, ModuleVoices::Bind::Ok);
      ++report_.muted;
      return;
    }
    if (m.entry == nullptr) {
      note(Kind::NotRunnable, slot, ref->name, module::Status::Ok, ModuleVoices::Bind::Ok);
      ++report_.muted;
      return;
    }
    const module::ModuleSet::StateMark mark = set_->state_mark();
    std::uint8_t* mem = nullptr;
    std::uint32_t addr = 0;
    if (!set_->take_state(m.entry->state_bytes, mem, addr)) {
      note(Kind::NoRoom, slot, ref->name, module::Status::NoMemory, ModuleVoices::Bind::Ok);
      ++report_.muted;
      return;
    }
    std::memset(mem, 0, m.entry->state_bytes);
    const ModuleVoices::Bind b = voices_->bind(slot, *m.entry, module::fault_id(ref->name), mem, in->params, in->param_len);
    if (b != ModuleVoices::Bind::Ok) {
      set_->state_rollback(mark);
      note(Kind::BindFailed, slot, ref->name, module::Status::Ok, b);
      ++report_.muted;
      return;
    }
    ++report_.bound;
    return;  // one instrument a step: its init() may take a while
  }
  state_ = State::Done;
}

ModuleBinder::State ModuleBinder::step(std::size_t budget) noexcept {
  if (state_ != State::Running) {
    return state_;
  }
  if (phase_ == Phase::Load) {
    load_next(budget);
  } else {
    bind_next();
  }
  return state_;
}

std::size_t describe_modules(const ModuleReport& r, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("modules: ").dec(r.instruments).str(" instruments, ").dec(r.loaded).str(" loaded");
  if (r.already != 0u) {
    t.str(", ").dec(r.already).str(" already there");
  }
  t.str(", ").dec(r.bound).str(" bound, ").dec(r.muted).str(" muted");
  if (r.abi_differs != 0u) {
    t.str(", ").dec(r.abi_differs).str(" saved with another ABI");
  }
  return t.size();
}

std::size_t describe_module_problem(const ModuleProblem& p, char* out, std::size_t cap) noexcept {
  audio::diag::Text t(out, cap);
  t.str("mod: ");
  if (p.slot != kModuleProblem) {
    t.str("instrument ").dec(static_cast<std::uint32_t>(p.slot) + 1u).str(" muted: ");
  }
  t.str(p.name).chr(' ');
  switch (p.kind) {
    case Kind::NotFound: t.str("not found (modules/instruments/").str(p.name).str(".tmod)"); break;
    case Kind::Refused: t.str("refused: ").str(module::status_name(p.status)).str(", quarantined until reboot"); break;
    case Kind::Quarantined: t.str("quarantined until reboot"); break;
    case Kind::NoRoom: t.str("does not fit the module memory"); break;
    case Kind::CardError: t.str("could not be read from the card"); break;
    case Kind::WrongKind: t.str("is not an instrument"); break;
    case Kind::NotRunnable: t.str("cannot run in this build"); break;
    case Kind::BindFailed: t.str("did not start: ").str(bind_name(p.bind)); break;
  }
  return t.size();
}

}  // namespace o3os::app
