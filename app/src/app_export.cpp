// SPDX-License-Identifier: GPL-3.0-or-later
// Export writes for apps (OS-H4): the state machine of app/app_export.h. No path is built here and no card call is made: the names are checked by the save service (which hands them to the path guard
// again), the file is a job of the service, and the writer applies the guard to every call that changes the card.
#include <o3os/app/app_export.h>

#include <o3os/app/save_service.h>

#include <o3module.h>

#include <cstring>

namespace o3os::app {

static_assert(AppExport::kStemMax == SaveService::kAppNameMax, "the app export's name buffers are the guard's");
static_assert(AppExport::kRingBytes == O3M_EXPORT_RING_BYTES && AppExport::kMaxFileBytes == O3M_EXPORT_MAX_FILE_BYTES && AppExport::kMaxFiles == O3M_EXPORT_MAX_FILES, "the SDK's published numbers");

// ---- the ring ------------------------------------------------------------------------------------------------------------------------------------

bool AppFileSource::push(const void* data, std::size_t n) noexcept {
  if (!attached() || data == nullptr || n > space()) {
    return false;
  }
  const std::uint8_t* src = static_cast<const std::uint8_t*>(data);
  const std::size_t at = head_ % cap_;
  const std::size_t first = n < cap_ - at ? n : cap_ - at;
  std::memcpy(ring_ + at, src, first);
  if (first < n) {
    std::memcpy(ring_, src + first, n - first);
  }
  head_ += n;
  return true;
}

void AppFileSource::begin(core::ByteSink& sink) noexcept {
  if (begun_) {
    restart_ = true;  // the atomic save asked to start over (a retry): the bytes that were drained are gone, so the export fails instead
    return;
  }
  begun_ = true;
  sink_ = &sink;
}

formats::Progress AppFileSource::step(std::size_t byte_budget) noexcept {
  if (restart_) {
    failure_ = "CANNOT RESTART";
    return formats::Progress::Failed;
  }
  if (aborted_) {
    failure_ = "ABORTED";
    return formats::Progress::Failed;
  }
  if (sink_ == nullptr) {
    return formats::Progress::Running;
  }
  std::size_t left = used() < byte_budget ? used() : byte_budget;
  while (left != 0u) {
    const std::size_t at = tail_ % cap_;
    const std::size_t chunk = left < cap_ - at ? left : cap_ - at;
    const core::IoResult r = sink_->write(ring_ + at, chunk);
    if (r.status != core::IoStatus::Ok || r.count == 0u || r.count > chunk) {
      failure_ = "WRITE FAILED";
      return formats::Progress::Failed;  // (the job reads the sink's own status for the reason)
    }
    tail_ += r.count;
    left -= r.count;
  }
  return closed_ && used() == 0u ? formats::Progress::Done : formats::Progress::Running;
}

// ---- the service ---------------------------------------------------------------------------------------------------------------------------------

void AppExport::begin_app(const char* stem) noexcept {
  give_up();  // (a file of an app that is gone, if the host forgot to end it)
  app_ = stem != nullptr;
  std::size_t n = 0;
  for (; stem != nullptr && stem[n] != '\0' && n < kStemMax; ++n) {
    stem_[n] = stem[n];
  }
  stem_[n] = '\0';
  if (stem != nullptr && stem[n] != '\0') {
    stem_[0] = '\0';  // longer than the guard's 31: no folder name (open answers RANGE)
  }
  opens_ = 0;
  bytes_ = 0;
  result_ = O3M_EXPORT_IDLE;
  state_ = State::Idle;
}

void AppExport::tick(std::uint64_t now_ms) noexcept {
  now_ms_ = now_ms;
  refresh();
  if (state_ == State::Open && src_.drained() != drained_seen_) {  // the job is taking bytes to the card: a slow card with a full ring is progress
    drained_seen_ = src_.drained();
    progress_ms_ = now_ms;
  }
  if (state_ == State::Open && now_ms >= progress_ms_ && now_ms - progress_ms_ >= kIdleMs) {
    give_up();
  }
}

void AppExport::end_app() noexcept {
  give_up();
  app_ = false;
  result_ = O3M_EXPORT_IDLE;
}

// Takes in a job that ended on its own (done, or failed: a pulled card, no space, a read-back that differs, a cancel).
void AppExport::refresh() noexcept {
  if (state_ == State::Idle || saves_ == nullptr) {
    return;
  }
  const SaveService::AppFileState f = saves_->app_file();
  if (f == SaveService::AppFileState::Done) {
    result_ = O3M_EXPORT_DONE;
    state_ = State::Idle;
  } else if (f == SaveService::AppFileState::Failed || f == SaveService::AppFileState::None) {
    result_ = O3M_EXPORT_FAILED;  // (None while a file is open: the job is gone, e.g. the service was reset)
    state_ = State::Idle;
  }
}

void AppExport::give_up() noexcept {
  refresh();  // (a job that has just ended on its own is not given up: a finished file stays)
  if (state_ == State::Idle) {
    return;
  }
  src_.abort();
  if (saves_ != nullptr) {
    saves_->drop_app_file();
  }
  state_ = State::Idle;
  result_ = O3M_EXPORT_FAILED;
}

std::int32_t AppExport::open(const char* name, const char* ext) noexcept {
  if (!app_) {
    return O3M_EXPORT_STATE;
  }
  refresh();
  if (state_ != State::Idle) {
    return O3M_EXPORT_STATE;
  }
  if (name == nullptr || ext == nullptr || !SaveService::project_name_ok(name) || !hal::storage::save::app_ext_ok(ext) || !SaveService::project_name_ok(stem_)) {
    return O3M_EXPORT_RANGE;
  }
  if (opens_ >= kMaxFiles) {
    return O3M_EXPORT_LIMIT;
  }
  if (!attached()) {
    return O3M_EXPORT_NO_CARD;
  }
  src_.reset();
  bool no_suffix = false;
  char chosen[kNameCap];
  switch (saves_->start_app_file(stem_, name, ext, src_, chosen, sizeof chosen, no_suffix)) {
    case SaveService::Start::Started:
      break;
    case SaveService::Start::NoName:
      return no_suffix ? O3M_EXPORT_LIMIT : O3M_EXPORT_RANGE;
    default:  // no write layer, the gate (no card, a card that may not be written), a save or the firmware update has the card, no job slot
      return O3M_EXPORT_NO_CARD;
  }
  std::memcpy(final_, chosen, std::strlen(chosen) + 1u);
  ++opens_;
  bytes_ = 0;
  progress_ms_ = now_ms_;
  drained_seen_ = 0;
  state_ = State::Open;
  result_ = O3M_EXPORT_IDLE;  // (a result the app never read is dropped by the next file)
  return O3M_EXPORT_OK;
}

std::int32_t AppExport::write(const void* buf, std::uint32_t n) noexcept {
  if (!app_) {
    return O3M_EXPORT_STATE;
  }
  refresh();
  if (state_ != State::Open) {
    return O3M_EXPORT_STATE;
  }
  if (buf == nullptr || n == 0u || n > src_.capacity()) {
    return O3M_EXPORT_RANGE;
  }
  if (n > kMaxFileBytes - bytes_) {
    give_up();  // 4 MiB: the file is aborted, the Temp removed, the status reads FAILED
    return O3M_EXPORT_LIMIT;
  }
  if (!src_.push(buf, n)) {
    return O3M_EXPORT_BUSY;
  }
  bytes_ += n;
  progress_ms_ = now_ms_;
  return O3M_EXPORT_OK;
}

std::int32_t AppExport::close() noexcept {
  if (!app_) {
    return O3M_EXPORT_STATE;
  }
  refresh();
  if (state_ != State::Open) {
    return O3M_EXPORT_STATE;
  }
  src_.finish();
  state_ = State::Closing;
  return O3M_EXPORT_OK;
}

std::uint32_t AppExport::status(char* final_name, std::uint32_t cap) noexcept {
  if (!app_) {
    return O3M_EXPORT_IDLE;
  }
  refresh();
  if (state_ != State::Idle) {
    return O3M_EXPORT_WRITING;
  }
  const std::uint32_t r = result_;
  if (r == O3M_EXPORT_DONE || r == O3M_EXPORT_FAILED) {
    if (r == O3M_EXPORT_DONE && final_name != nullptr && cap != 0u) {
      const std::size_t n = std::strlen(final_);
      const std::size_t take = n < cap - 1u ? n : cap - 1u;
      std::memcpy(final_name, final_, take);
      final_name[take] = '\0';
    }
    result_ = O3M_EXPORT_IDLE;
  }
  return r;
}

}  // namespace o3os::app
