// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>
#include <o3os/formats/wav_sample.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::wt {

// The Surge wavetable (.wt) as a registered sample format (OS-GRAN, BASE-04, FormatKind::Sample), read as the format's own header says (the Surge XT source, src/common/dsp/Wavetable.h:
// `wt_header`, packed, little-endian):
//
//   0   'vawt'               the tag
//   4   u32 n_samples        the frame size: samples a table (window)
//   8   u16 n_tables         the number of tables (windows)
//   10  u16 flags            1 is_sample, 2 loop_sample, 4 int16, 8 int16_is_16, 0x10 has_metadata, 0x20 user_modified
//   12  the data             n_tables x n_samples values, table after table: float32, or int16 with flag 4
//   ..  metadata             flag 0x10: a NUL-terminated XML string at the end of the file (not read)
//
// The values become 16-bit mono PCM in ReadParams::pcm: float32 x 32767 rounded and held to +-32767 (NaN is 0); int16 with flag 8 as it is; int16 WITHOUT flag 8 is Surge's "15-bit" form
// (its readers are i152float / i162float): ASSUMED here to be full scale at +-16384, so it is doubled and saturated (the constant is not in the header file: OPEN QUESTION, to be held against
// a real file; a wrong guess is 6 dB). The instrument is the wavetable of those windows (wav_sample.h WAVETABLE FRAMES: sample type 1, play mode 6, window size n_samples, window 0, rate
// 44,100). The engine has pitch tables for window sizes 32..2048, a power of two only: any other n_samples (Surge allows 4096) is refused Unsupported, nothing written. Flags 1, 2 and 0x20 are
// ignored and counted in FormatReport::sanitised. The audio must fit ReadParams::pcm (LimitExceeded, nothing written) and the file must hold all of it (Truncated); bytes after it (the metadata)
// are not read. Nothing is written to the model before the file has been read and checked: a failed or cancelled import leaves the project as it was. Memory: the object, about 100 bytes.
class SurgeReader final : public FormatReader {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  formats::Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }
  unsigned permille() const noexcept override;
  bool can_cancel() const noexcept override { return true; }
  void cancel() noexcept override;

 private:
  enum class State : std::uint8_t { Header, Data, Done, Failed };

  void fail(FormatError e, const char* detail) noexcept;
  void on_header() noexcept;
  void convert(std::size_t bytes) noexcept;
  void finish() noexcept;

  core::ByteSource* source_ = nullptr;
  model::Project* project_ = nullptr;
  std::int16_t* pcm_ = nullptr;
  std::size_t capacity_ = 0;
  std::uint16_t slot_ = 0;
  State state_ = State::Failed;
  formats::Progress progress_ = formats::Progress::Failed;
  FormatReport report_;
  std::uint32_t n_samples_ = 0;
  std::uint32_t n_tables_ = 0;
  std::uint16_t flags_ = 0;
  std::uint32_t total_ = 0;      // samples to read
  std::uint32_t out_ = 0;        // samples written
  std::uint8_t width_ = 4;       // bytes a sample in the file: 2 or 4
  std::uint8_t have_ = 0;        // bytes of buf_ held (Header: of the 12; Data: a sample split across two reads)
  std::uint8_t buf_[248] = {};  // (a multiple of 2 and 4; have_ is a byte)
  char name_[64] = {};
};

}  // namespace o3os::formats::wt
