// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>
#include <o3os/formats/wav_sample.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::iff {

// AIFF / AIFF-C and IFF 8SVX as one registered sample format (OS-SAMPFMT, FormatKind::Sample, docs/specs/OS-SAMPFMT.md). Both are big-endian IFF containers (a 'FORM' of chunks, each an id, a
// 32-bit size and a payload padded to an even length), so one resumable reader walks them; the form type at offset 8 decides which. Like the WAV reader it streams the audio into
// ReadParams::pcm (the sample arena slot), converted to int16, and makes the sample instrument `params.index` through wav::make_sample_instrument (name, path, rate, loop, wavetable rule): the result
// is an ordinary sample instrument and nothing in the project format changes. The file's rate is kept (nothing is resampled). Nothing is written to the model before the file has been read and checked.
//
//   AIFF  'FORM' 'AIFF' (and 'AIFC'). COMM must come before SSND (a streamed read cannot go back). PCM of 1..8, 9..16 and 17..24 bits, in 1, 2 or 3 bytes, left-justified as the format says; 24-bit goes to 16 with
//         the WAV reader's TPDF dither (exact where the low byte is zero); mono or stereo, stereo mixed to mono (L + R) / 2 exactly as the WAV reader does (ReadParams::keep_stereo keeps it stereo).
//         AIFF-C: compression 'NONE' and 'sowt' (little-endian PCM) are read; every other compression, and 32-bit PCM, is refused Unsupported with a message. The rate is the 80-bit IEEE extended
//         `sampleRate`, decoded by integer arithmetic (decode_extended_rate: exact for every integral rate, rounded to nearest otherwise). The audio is min(numSampleFrames, what SSND holds); an SSND
//         shorter than COMM says is read as far as it goes and counted in FormatReport::sanitised. INST's sustain loop (play mode 1 forward, 2 forward-backward = ping-pong) with the MARK positions
//         it names becomes the loop [begin, end) in frames (end exclusive: the marker is the position the loop returns from); a loop whose markers are missing, out of range or not begin < end is dropped and
//         counted. At most 24 markers are kept (a loop that names a later one is dropped). INST and MARK may come before or after SSND.
//   8SVX  'FORM' '8SVX'. VHDR: oneShotHiSamples, repeatHiSamples, samplesPerSec (kept as the rate; 0 is refused Malformed), ctOctave, sCompression, volume (not used). BODY: 8-bit signed PCM, only the
//         FIRST octave (oneShotHiSamples + repeatHiSamples samples; a multi-octave file is counted in sanitised). A repeat part makes the loop [oneShot, oneShot + repeat) played forward, otherwise the sample is a
//         one-shot. sCompression 1 is Fibonacci-delta: per octave a pad byte, the initial value, then two 4-bit deltas a byte (high nibble first) from the table -34 -21 -13 -8 -5 -3 -2 -1 0 1 2 3 5 8 13 21 as the EA
//         reference unpacker does (D1Unpack; the sum is held to -128..127, where the reference stores a wrapped byte: the two agree on every well-formed file); any other compression is refused Unsupported.
//         NAME names the instrument (printable ASCII, 32 characters). CHAN saying stereo (both channels in BODY, left first) reads the left channel and is counted.
//
// Bounded and defensive (PAR-011): every chunk size is checked against the FORM's, the file's (ReadParams::size_hint) and the room given before a byte is written; a chunk that runs past the end of the form
// is Malformed, one past the end of the file Truncated; sizes are 64-bit sums, no multiplication can overflow; the audio must fit `pcm` (LimitExceeded, nothing written); a duplicate COMM / VHDR is Malformed;
// trouble in a chunk AFTER the audio ends the read there, successfully, counted in sanitised. No heap, nothing blocks (a source that would block ends the step). A failed or cancelled read leaves the
// project as it was. Memory: the object, about 0.5 KB, wherever the caller puts it (SampleBankMem, SDRAM). Contexts: main loop only (a job).

inline constexpr unsigned kMaxMarkers = 24;

// OS-LOADCH: the report's detail when SSND comes before COMM (a streamed read cannot go back; a known limit, OS-SAMPFMT). The Sample load strip words it for the user (app/import_job.cpp).
inline constexpr const char* kSsndBeforeComm = "the sound data comes before the COMM chunk";

// 'FORM' followed by AIFF, AIFC or 8SVX. A head of fewer than 12 bytes is not claimed (it cannot show the form type, and the native project is a FORM too).
bool sniff(const std::uint8_t* head, std::size_t n, std::uint32_t file_size) noexcept;

// The 80-bit IEEE 754 extended number at `ext` (sign + 15-bit exponent, then the 64-bit mantissa with its explicit integer bit, big-endian: AIFF's sampleRate) as whole hertz, rounded to nearest, by integer
// arithmetic only. False for a negative, infinite, NaN or zero value, one below 1 Hz, or one that does not fit 32 bits (2^32 - 0.5 and over); `hz` is then unchanged.
bool decode_extended_rate(const std::uint8_t ext[10], std::uint32_t& hz) noexcept;

class SampleReader final : public FormatReader {
 public:
  void begin(core::ByteSource& source, model::ModelArena& arena, const ReadParams& params) noexcept override;
  formats::Progress step(std::size_t byte_budget) noexcept override;
  formats::Progress progress() const noexcept override { return progress_; }
  const FormatReport& report() const noexcept override { return report_; }
  unsigned permille() const noexcept override;
  bool can_cancel() const noexcept override { return true; }
  void cancel() noexcept override;

 private:
  enum class State : std::uint8_t { FormHead, ChunkHead, Collect, Mark, DataBegin, Data, Skip, Pad, Done, Failed };
  enum class Form : std::uint8_t { None, Aiff, AiffC, Svx };

  static constexpr std::uint64_t kUnknown = ~std::uint64_t{0};

  struct Parse {
    Form form = Form::None;
    State resume = State::Pad;  // where Skip goes next
    std::uint64_t pos = 0;      // bytes consumed from the source
    std::uint64_t form_end = kUnknown;
    std::uint64_t file_end = kUnknown;
    std::uint32_t chunk_id = 0;
    std::uint32_t chunk_left = 0;  // payload bytes of the chunk not consumed yet
    std::uint32_t skip_left = 0;
    std::uint32_t data_left = 0;   // source bytes of audio still to read
    std::uint32_t total = 0;       // samples the audio will make (frames x output channels)
    std::uint32_t out = 0;         // samples written
    std::uint32_t rate = 0;
    std::uint32_t frames = 0;      // frames the audio makes (valid once it began)
    std::uint32_t comm_frames = 0;
    std::uint32_t one_shot = 0;
    std::uint32_t repeat = 0;
    std::uint32_t rng = 0x2545F491u;  // fixed seed, as the WAV reader: the output is a pure function of the input
    std::uint32_t sanitised = 0;
    std::uint32_t mark_pos[kMaxMarkers] = {};
    std::uint16_t mark_id[kMaxMarkers] = {};
    std::uint16_t mark_left = 0;
    std::uint16_t loop_begin_id = 0;
    std::uint16_t loop_end_id = 0;
    std::int16_t fib = 0;         // the running value of the Fibonacci-delta decoder
    std::uint8_t have = 0;        // bytes of buf_ held
    std::uint8_t want = 0;        // bytes the Collect state waits for
    std::uint8_t channels = 0;    // of the file: 1 or 2
    std::uint8_t width = 0;       // bytes a sample in the file: 1, 2 or 3
    std::uint8_t unit = 1;        // bytes of one conversion unit: a frame
    std::uint8_t out_channels = 0;
    std::uint8_t mark_n = 0;
    std::uint8_t mark_step = 0;
    std::uint8_t loop_mode = 0;   // INST sustain loop: 0 none, 1 forward, 2 forward-backward
    std::uint8_t octaves = 1;
    std::uint8_t compression = 0;
    std::uint8_t fib_head = 0;    // Fibonacci-delta header bytes (pad, initial) still to read
    std::uint8_t name_len = 0;
    bool odd = false;             // the chunk has a pad byte
    bool little = false;          // 'sowt'
    bool header_seen = false;     // COMM or VHDR
    bool inst_seen = false;
    bool mark_seen = false;
    bool name_seen = false;
    bool chan_seen = false;
    bool data_seen = false;
    bool data_done = false;
    bool loop_dropped = false;
  };

  void fail(FormatError e, const char* detail) noexcept;
  int take(std::size_t want) noexcept;  // 1: buf_ holds `want` bytes, 0: later, -1: ended or failed
  int read_some(std::uint8_t* where, std::size_t ask, std::size_t& got) noexcept;
  void on_end() noexcept;
  void on_form_head() noexcept;
  void on_chunk_head() noexcept;
  void on_collected() noexcept;
  void on_comm() noexcept;
  void on_vhdr() noexcept;
  void on_inst() noexcept;
  void on_ssnd_head() noexcept;
  void step_mark() noexcept;
  void data_begin() noexcept;
  void step_data() noexcept;
  void convert_aiff(std::size_t units) noexcept;
  void convert_svx(std::size_t bytes) noexcept;
  void end_chunk() noexcept;
  void next_chunk() noexcept;
  void finish() noexcept;
  void collect(std::size_t n) noexcept;
  void skip(std::uint32_t n, State then) noexcept;
  bool past_end(std::uint32_t size) noexcept;

  core::ByteSource* source_ = nullptr;
  model::Project* project_ = nullptr;
  std::int16_t* pcm_ = nullptr;
  std::size_t capacity_ = 0;
  std::size_t budget_ = 0;
  std::uint16_t slot_ = 0;
  std::uint16_t wavetable_ = kWtAuto;
  bool keep_stereo_ = false;
  std::uint8_t channel_ = 0;  // OS-LOADCH: a stereo file reduced to mono: 0 the mean, 1 left, 2 right (ReadParams::channel)
  bool blocked_ = false;
  State state_ = State::Failed;
  formats::Progress progress_ = formats::Progress::Failed;
  FormatReport report_;
  Parse p_;
  char name_[64] = {};   // the file's name (ReadParams::name)
  char title_[33] = {};  // 8SVX NAME
  std::uint8_t buf_[252] = {};  // a multiple of 1, 2, 3, 4 and 6: whole frames of every layout
};

}  // namespace o3os::formats::iff
