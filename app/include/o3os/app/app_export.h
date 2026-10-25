// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Export writes for apps (OS-H4, ADR 0014 amendment A, docs/specs/OS-H4.md): the four calls an SD app module with O3M_CAP_EXPORT_FILES is lent (o3module.h: export_open, export_write, export_close,
// export_status) and the staging ring behind them. One object per device, owned by the SaveService (app/save_service.h: it owns the card's one save job, its gates and its busy state, and a file an app
// writes is one more job of it), reached by the module host (app/module_ui.h) through ModuleUi::set_export.
//
// WHAT AN APP CAN DO TO THE CARD. It can make a NEW file `AppExport/<app>/<stem>.<ext>` and nothing else: <app> is the app's own catalogue stem (never a string the app supplies), <stem> a name the
// path guard takes (hal/save_guard.h valid_name), <ext> one of mid, syx, txt, csv, json. The folder is made if need be; a taken name becomes <stem>_001 .. _999; the file is written to the folder's
// Temp, synced, read back and checked, and only then renamed to its final name by the NewOnly commit of the atomic save (app/atomic_save.h), which never replaces. Nothing the app passes becomes a
// path other than that, and the guard (applied to every path by the writer, whatever this file does) refuses any other create, rename or remove: nothing outside AppExport/<app>/ is touched, a file
// that is finished is never opened for writing, renamed or removed, and only the Temp is ever removed (an abort or a failure).
//
// NEVER BLOCKS. export_write copies into a ring of kRingBytes and answers BUSY when it is full (nothing taken: the app calls again on a later frame with the same bytes); the save job, stepped by the main
// loop's job pass, drains the ring to the card WHILE THE APP IS STILL WRITING, so a file of kMaxFileBytes needs no more RAM than the ring. export_close only asks for the commit; export_status says when it
// is DONE or FAILED. Everything runs on the main loop (the app's calls and the job's steps): the ring has one producer and one consumer on the same thread, so its counters need no atomics.
//
// ABORT. A file still open or committing when the app exits, is stopped for its time budget or faults (ModuleUi::release_host), or that would pass kMaxFileBytes, is given up: the job is cancelled at once,
// its Temp removed, and the status reads FAILED. Files already DONE stay. A user's save, export or DELETE while a file is committing cancels it the same way (SaveService::yield_recent): the core's own
// saves are never refused because of an app. A pulled or full card fails the job (FAILED); its stale Temp is removed by the next export's heal (the atomic save's step 2, allowed in AppExport/<app>/).
//
// IDLE. An open file holds the card's one job slot (the save service is busy for loads, a firmware update and the autosaves), so a file the app leaves open with no write for kIdleMs is aborted like one
// it left at exit: FAILED, the Temp removed. A closed file is not timed: the job ends by itself.
//
// The ring cannot restart: a step of the atomic save that would call begin() a second time (a retry) fails the export instead.

#include <o3os/app/atomic_save.h>

#include <cstddef>
#include <cstdint>

namespace o3os::app {

class SaveService;

// The bytes of one file, as the app wrote them. A SaveSource over a ring the platform provides.
class AppFileSource final : public SaveSource {
 public:
  void attach(std::uint8_t* ring, std::size_t bytes) noexcept {
    ring_ = ring;
    cap_ = bytes;
    reset();
  }
  bool attached() const noexcept { return ring_ != nullptr && cap_ != 0u; }
  std::size_t capacity() const noexcept { return cap_; }
  // A new file: empty ring, nothing begun.
  void reset() noexcept {
    head_ = tail_ = 0;
    closed_ = aborted_ = begun_ = restart_ = false;
    sink_ = nullptr;
  }
  std::size_t used() const noexcept { return head_ - tail_; }
  std::size_t drained() const noexcept { return tail_; }  // bytes taken out in all (the consumer's counter)
  std::size_t space() const noexcept { return cap_ - used(); }
  // The app's bytes. False (nothing taken) when they do not fit.
  bool push(const void* data, std::size_t n) noexcept;
  void finish() noexcept { closed_ = true; }  // the app closed the file: the job ends when the ring is empty
  void abort() noexcept { aborted_ = true; }  // the file is given up: the job's next step fails (the job is cancelled too, this is for a step already queued)

  void begin(core::ByteSink& sink) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  const char* failure() const noexcept override { return failure_; }
  void end() noexcept override {}

 private:
  std::uint8_t* ring_ = nullptr;
  std::size_t cap_ = 0;
  std::size_t head_ = 0;  // bytes pushed in all (the producer's counter); the byte at head_ % cap_ is written next
  std::size_t tail_ = 0;  // bytes drained in all (the consumer's); a file is at most 4 MiB, so neither counter wraps
  bool closed_ = false;
  bool aborted_ = false;
  bool begun_ = false;
  bool restart_ = false;  // begin() was called a second time: a retry the ring cannot do
  core::ByteSink* sink_ = nullptr;
  const char* failure_ = "";
};

class AppExport {
 public:
  static constexpr std::size_t kRingBytes = 16u * 1024u;       // the staging ring: the platform provides this much (SDRAM_STATIC); a write of more than the ring is RANGE
  static constexpr std::uint32_t kMaxFileBytes = 4u * 1024u * 1024u;
  static constexpr unsigned kMaxFiles = 16;                    // opens per launch of an app
  static constexpr unsigned kStemMax = 31;                     // the guard's kNameMax: a file's stem and an app's folder name
  static constexpr unsigned kNameCap = kStemMax + 8;           // "<stem>_001.json" with its NUL
  static constexpr std::uint32_t kIdleMs = 20000;              // an open file the app neither writes to nor closes for this long is aborted (the app reads FAILED): an open file holds the card's one job slot

  void attach(SaveService& saves, std::uint8_t* ring, std::size_t bytes) noexcept {
    saves_ = &saves;
    src_.attach(ring, bytes);
  }
  bool attached() const noexcept { return saves_ != nullptr && src_.attached(); }

  // ---- the app's life (ModuleUi::launch_app / release_host)
  // An app that holds export_files is launched: `stem` is its catalogue stem (its folder's name). The file count starts again.
  void begin_app(const char* stem) noexcept;
  // The app will not run again (exit, stopped, faulted, the project changed under it): a file open or committing is aborted. Files DONE stay.
  void end_app() noexcept;

  // The save service's poll, once a pass: the clock for kIdleMs.
  void tick(std::uint64_t now_ms) noexcept;

  // ---- the host calls (O3M_EXPORT_* codes of o3module.h)
  std::int32_t open(const char* name, const char* ext) noexcept;
  std::int32_t write(const void* buf, std::uint32_t n) noexcept;
  std::int32_t close() noexcept;
  std::uint32_t status(char* final_name, std::uint32_t cap) noexcept;

  // For the tests and `diag`.
  unsigned opens() const noexcept { return opens_; }
  bool file_open() const noexcept { return state_ == State::Open || state_ == State::Closing; }
  std::uint32_t file_bytes() const noexcept { return bytes_; }
  const AppFileSource& source() const noexcept { return src_; }

 private:
  enum class State : std::uint8_t { Idle, Open, Closing };
  void refresh() noexcept;     // takes in a job that ended
  void give_up() noexcept;     // the file is aborted: its job cancelled, the Temp removed

  SaveService* saves_ = nullptr;
  AppFileSource src_;
  State state_ = State::Idle;
  bool app_ = false;           // an app that holds export_files runs
  std::uint32_t result_ = 0;   // O3M_EXPORT_IDLE, or DONE / FAILED not yet read by the app
  unsigned opens_ = 0;
  std::uint64_t now_ms_ = 0;      // the clock of the last tick()
  std::uint64_t progress_ms_ = 0; // when the open file was opened, last written to, or last drained to the card
  std::size_t drained_seen_ = 0;
  std::uint32_t bytes_ = 0;    // bytes accepted into the open file
  char stem_[kStemMax + 1] = {};  // the app's folder name
  char final_[kNameCap] = {};  // "<stem>.<ext>" as the open chose it
};

}  // namespace o3os::app
