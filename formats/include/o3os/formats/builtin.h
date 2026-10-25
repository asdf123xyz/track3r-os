// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>
#include <o3os/formats/iff_sample.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/mod/import.h>
#include <o3os/formats/native.h>
#include <o3os/formats/og.h>
#include <o3os/formats/settings.h>
#include <o3os/formats/wav_sample.h>
#include <o3os/formats/wt_surge.h>

namespace o3os::formats {

namespace detail {

// The native project format (formats/native.h) as a strategy.
class NativeReaderStrategy final : public FormatReader {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget) noexcept override;
  Progress progress() const noexcept override { return reader_.progress(); }
  const FormatReport& report() const noexcept override { return report_; }

 private:
  native::Reader reader_;
  FormatReport report_;
};

class NativeWriterStrategy final : public FormatWriter {
 public:
  void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept override;
  Progress step(std::size_t byte_budget) noexcept override;
  Progress progress() const noexcept override { return writer_.progress(); }
  const FormatReport& report() const noexcept override { return report_; }

 private:
  native::Writer writer_;
  FormatReport report_;
};

// The device settings file (formats/settings.h, OS-002d) as strategies: the whole file is at most 1 KiB, so each holds it in a buffer of its
// own, reads (writes) it through the byte stream a piece at a time and parses (builds) it as a whole. They touch no project.
class SettingsReaderStrategy final : public FormatReader {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  Progress step(std::size_t byte_budget) noexcept override;
  Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }

 private:
  void fail(FormatError e, const char* detail) noexcept;

  core::ByteSource* source_ = nullptr;
  model::Settings* target_ = nullptr;
  Progress progress_ = Progress::Failed;
  FormatReport report_;
  std::size_t have_ = 0;
  std::uint8_t buf_[settings::kMaxFileBytes + 1];  // one byte more than a file may have: a longer one is seen as too big
};

class SettingsWriterStrategy final : public FormatWriter {
 public:
  void begin(const model::Project& project, core::ByteSink& sink, const WriteParams& params) noexcept override;
  Progress step(std::size_t byte_budget) noexcept override;
  Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }

 private:
  void fail(FormatError e, const char* detail) noexcept;

  core::ByteSink* sink_ = nullptr;
  Progress progress_ = Progress::Failed;
  FormatReport report_;
  std::size_t size_ = 0;
  std::size_t sent_ = 0;
  std::uint8_t buf_[settings::kMaxFileBytes];
};

}  // namespace detail

// The strategy objects of the built-in formats, in one piece of memory the caller owns (a static, a job slot):
// 30.4 KB on the MK66 (the WAV reader is 1.2 KB of it), of which the native reader and the OG project reader and writer are the
// largest, plus the MED importer (OS-008b, about 13 KB). Two jobs that
// need the same format at the same time need two of these.
struct BuiltinFormats {
  detail::NativeReaderStrategy native_reader;
  detail::NativeWriterStrategy native_writer;
  og::ProjectReader og_project_reader;
  og::PatternReader og_pattern_reader;
  og::InstrumentReader og_mti_reader{false};
  og::InstrumentReader og_pti_reader{true};
  og::PamdReader og_pamd_reader;
  og::ProjectWriter og_project_writer;
  og::PatternWriter og_pattern_writer;
  og::InstrumentWriter og_pti_writer;
  og::PamdWriter og_pamd_writer;
  wav::SampleReader wav_reader;  // OS-002c
  wt::SurgeReader wt_reader;     // OS-GRAN: Surge's .wt
  iff::SampleReader iff_reader;  // OS-SAMPFMT: AIFF / AIFF-C and IFF 8SVX
  detail::SettingsReaderStrategy settings_reader;  // OS-002d
  detail::SettingsWriterStrategy settings_writer;
  med::ImportReader med_reader;  // OS-008b
  mod::ImportReader mod_reader;  // OS-MODIMP: one object for the "mod" and the "s3m" descriptors (about 26 KB)
};

// Registers, in this order: "native" (.o3pj, read and write), then the OG files "og-project" (.mt), "og-pattern"
// (.mtp), "og-pti" (.pti), "og-mti" (.mti, read only: Export writes .pti), "og-pamd" (patternsMetadata), then "wav"
// (.wav, a sample: read only; OS-002c), then "surge-wt" (.wt, a Surge wavetable, a sample: read only; OS-GRAN), then "iff-sample" (.aif .aiff .aifc .8svx .svx .iff, AIFF / AIFF-C and IFF 8SVX, a sample: read only; OS-SAMPFMT), then "settings" (.o3st, the device settings file, read and written; OS-002d), then "med" (.med
// .mmd .mmd0 .. .mmd3, an OctaMED module, read only, FormatKind::Project; OS-008b: it needs ReadParams::random), then "mod" (.mod, a ProTracker module) and "s3m" (.s3m, a Scream Tracker 3 module), read
// only, FormatKind::Project, sniffed by their signatures (OS-MODIMP: they need ReadParams::random too, and the two descriptors share one reader). The order matters
// once: bytes of an instrument file of unknown size are taken for a .pti, never for an .mti. False when the registry has no room for all thirteen
// (some may then be missing). Later formats (MOD, XM, IT, MIDI) add their own descriptors to the same registry.
bool register_builtin_formats(FormatRegistry& registry, BuiltinFormats& storage) noexcept;

}  // namespace o3os::formats
