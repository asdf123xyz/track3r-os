// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <o3os/formats/format.h>

#include <cstddef>
#include <cstdint>

namespace o3os::formats::med {

// OctaMED / MED module files, MMD0 to MMD3 (OS-008a slice M0, docs/formats/med.md sections 2 and 3). A module is a graph of
// structures joined by file offsets, so this reader works on a random-access source and never trusts a number: every pointer
// is range-checked in 64-bit arithmetic before it is followed, every count is capped by the table below, nothing recurses,
// nothing allocates and a file of any content ends in a typed Status. Integers are decoded by hand (big-endian), never by
// struct punning. Nothing here knows the project model: the mapping of cells and instruments into it is slice M2.
//
// What is read: MMD0 (3-byte cells), MMD1 (4-byte cells, BlockInfo, command pages), MMD2 and MMD3 (play sequences in
// sections, track volumes and pans, mixing fields; MMD3 is read like MMD2, its note offset is the importer's business), the
// song's 63 sample records, instruments of every type of Rev 6 (sample, 5 multi-octave types, EXTSAMPLE, 16-bit, stereo,
// synth, hybrid), the expansion data (song name, InstrExt, instrument names, chained songs) and the play-sequence commands
// of MMD2+. Not read, only noted in Summary::ignored: annotation, notation, MIDI dumps, MMDInfo, ARexx, MIDI 3x settings,
// echo and stereo mixing fields. 'MMDC' (packed MMD0) and instrument types -3, -4, delta and packed data are Unsupported.
//
// Contexts: main loop only (a job). A Reader is 0.7 KB and holds only the song record and the section table; blocks, cells
// and instruments are read on demand, a line at a time.

// ---- caps: every limit of section 2.6 in one place -------------------------------------------------------------------
namespace caps {
inline constexpr std::uint32_t kMinPointer = 52;           // the header is 52 bytes: no structure starts before it
inline constexpr std::uint32_t kHeaderBytes = 52;
inline constexpr std::uint32_t kSongBytes = 788;
inline constexpr std::uint32_t kMaxSongs = 256;            // nextmod chain, strictly increasing offsets
inline constexpr std::uint32_t kMaxBlocksMmd01 = 255;      // the play sequence of MMD0/1 holds 8-bit block numbers
inline constexpr std::uint32_t kMaxBlocksMmd23 = 0x7FFF;
inline constexpr std::uint32_t kMaxSamples = 63;
inline constexpr std::uint32_t kMaxSongLenMmd01 = 256;
inline constexpr std::uint32_t kMaxSections = 256;         // MMD2+: songlen counts sections
inline constexpr std::uint32_t kMaxPlaySeqs = 256;
inline constexpr std::uint32_t kMaxPlayEntries = 4096;     // all sections together
inline constexpr std::uint32_t kMaxPlayCommands = 256;     // entries of one play sequence's command table that are followed
inline constexpr std::uint32_t kMinTracks = 1;
inline constexpr std::uint32_t kMaxTracks = 64;
inline constexpr std::uint32_t kMaxLines = 9999;           // libxmp's SoundStudio limit; OctaMED's own is 3,200
inline constexpr std::uint32_t kMaxPages = 16;             // extra command pages of one block
inline constexpr std::uint32_t kMaxSynthTable = 128;
inline constexpr std::uint32_t kMaxWaveforms = 64;
inline constexpr std::uint32_t kInstrumentNameMax = 32;    // our instrument name limit (the file field is 40)
inline constexpr std::uint32_t kSongNameMax = 32;
inline constexpr std::uint32_t kBlockNameMax = 30;         // the pattern name limit of the model
inline constexpr std::uint64_t kSampleArenaBytes = 10816ull * 1024ull;  // ADR 0013 amendment 1: the PCM an instrument may need
inline constexpr std::uint32_t kExpPrefix = 68;            // bytes of MMD0exp that are read (fields up to mmdcmd3x)
inline constexpr std::uint32_t kSynthBytes = 534;          // SynthInstr
}  // namespace caps

// ---- errors ----------------------------------------------------------------------------------------------------------
enum class Error : std::uint8_t {
  None,
  Truncated,       // the file ends inside a structure or inside data a count promises
  BadMagic,        // not an MMD0 to MMD3 file
  BadPointer,      // a pointer before the header, past the end, or whose table does not fit
  LimitExceeded,   // a count above its cap in caps::
  Malformed,       // a value out of range or inconsistent (0 tracks, an octave split of a too short sample...)
  Unsupported,     // a valid file with something this reader does not take (MMDC, instruments outside the file)
  SourceFailed,    // the byte source reported a failure
};

const char* error_name(Error e) noexcept;
FormatError to_format_error(Error e) noexcept;  // BadPointer becomes Malformed: FormatError has no pointer class

// The first problem found. `offset` is the file offset of the offending field (a pointer, a count) or of the structure
// that is cut short; `detail` is static text.
struct Status {
  Error error = Error::None;
  std::uint32_t offset = 0;
  const char* detail = "";
  bool ok() const noexcept { return error == Error::None; }
};

// ---- the byte source ---------------------------------------------------------------------------------------------------
// Random access, because the file is a pointer graph (core::ByteSource is forward-only). read_at moves exactly `n` bytes
// or fails; it is never asked for bytes past size(). Non-blocking by contract, like the other byte contracts: the platform
// wraps an SD file behind it (a sector cache makes the line-by-line reads cheap).
class RandomSource {
 public:
  virtual std::uint32_t size() const noexcept = 0;
  virtual bool read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept = 0;

 protected:
  RandomSource() = default;
  ~RandomSource() = default;  // not deleted through the interface: no heap, so no virtual destructor slot
  RandomSource(const RandomSource&) = delete;
  RandomSource& operator=(const RandomSource&) = delete;
};

// A file in memory.
class SpanSource final : public RandomSource {
 public:
  SpanSource(const std::uint8_t* data, std::uint32_t n) noexcept : data_(data), size_(n) {}
  std::uint32_t size() const noexcept override { return size_; }
  bool read_at(std::uint32_t offset, void* dst, std::uint32_t n) noexcept override;

 private:
  const std::uint8_t* data_;
  std::uint32_t size_;
};

// ---- the song --------------------------------------------------------------------------------------------------------
inline constexpr std::uint8_t kFlagFilterOn = 0x01;
inline constexpr std::uint8_t kFlagVolHex = 0x10;
inline constexpr std::uint8_t kFlagStSlide = 0x20;
inline constexpr std::uint8_t kFlag8Channel = 0x40;
inline constexpr std::uint8_t kFlag2BeatMask = 0x1F;
inline constexpr std::uint8_t kFlag2Bpm = 0x20;
inline constexpr std::uint8_t kFlag2Mix = 0x80;

// Things the file holds that no slice before M2 uses; Summary::ignored says which are present, the import report lists them.
enum Ignored : std::uint32_t {
  kIgnoreAnnotation = 1u << 0,
  kIgnoreNotation = 1u << 1,
  kIgnoreMidiDumps = 1u << 2,
  kIgnoreMmdInfo = 1u << 3,
  kIgnoreArexx = 1u << 4,
  kIgnoreMidiCmd3x = 1u << 5,
  kIgnoreEcho = 1u << 6,
  kIgnoreVolAdjust = 1u << 7,
  kIgnoreStereoSep = 1u << 8,
  kIgnoreExtraSongs = 1u << 9,
  kIgnoreChannelSplit = 1u << 10,
};

struct SongInfo {
  std::uint8_t version = 0;           // 0..3: MMD0..MMD3
  std::uint8_t extra_songs = 0;       // the header's count of songs minus one
  std::uint32_t file_size = 0;
  std::uint16_t num_blocks = 0;
  std::uint16_t song_len = 0;         // MMD0/1: play-sequence entries; MMD2+: sections
  std::uint8_t num_samples = 0;       // instruments in smplarr, at most 63
  std::uint16_t def_tempo = 0;
  std::int8_t play_transpose = 0;
  std::uint8_t flags = 0;
  std::uint8_t flags2 = 0;
  std::uint8_t tempo2 = 0;            // ticks per line (SPD)
  std::uint8_t master_volume = 0;
  // MMD2 / MMD3
  std::uint16_t num_tracks = 0;       // numtracks of the song (0 for MMD0/1: the blocks carry it)
  std::uint16_t num_pseqs = 0;
  std::uint32_t flags3 = 0;           // 1 stereo, 2 free pan
  std::uint16_t vol_adjust = 0;       // percent, 0 = 100
  std::uint16_t channels = 0;         // 0 = 4
  std::uint8_t echo_type = 0;
  std::uint8_t echo_depth = 0;
  std::uint16_t echo_len = 0;
  std::int8_t stereo_sep = 0;
  // expansion data (0 / false when absent)
  bool has_exp = false;
  std::uint16_t s_ext_entries = 0;    // InstrExt entries and stride
  std::uint16_t s_ext_entrsz = 0;
  std::uint16_t i_ext_entries = 0;    // MMDInstrInfo entries and stride
  std::uint16_t i_ext_entrsz = 0;
  std::uint32_t annotation_len = 0;
  std::uint8_t channel_split[4] = {0, 0, 0, 0};
  char name[caps::kSongNameMax + 1] = {};  // sanitised: bytes outside 0x20..0x7E are '?'
  // derived
  std::uint32_t play_entries = 0;     // length of the flattened play list (see Reader::play_entry)

  bool vol_hex() const noexcept { return (flags & kFlagVolHex) != 0; }
  bool st_slide() const noexcept { return (flags & kFlagStSlide) != 0; }
  bool eight_channel() const noexcept { return (flags & kFlag8Channel) != 0; }
  bool filter_on() const noexcept { return (flags & kFlagFilterOn) != 0; }
  bool bpm_mode() const noexcept { return (flags2 & kFlag2Bpm) != 0; }
  bool mixing() const noexcept { return (flags2 & kFlag2Mix) != 0; }
  // Lines per beat: the BPM beat length when BPM mode is on, else 4.
  std::uint32_t lines_per_beat() const noexcept { return bpm_mode() ? static_cast<std::uint32_t>(flags2 & kFlag2BeatMask) + 1u : 4u; }
};

// ---- blocks and cells ------------------------------------------------------------------------------------------------
// One cell of a line. note: 0 = none, 1 = C-1 (MMD0 0..63, MMD1+ 0..127). note_hi: MMD1+, bit 7 of the note byte, which the
// layout document calls reserved ("mask it out when reading") and OpenMPT reads as a note cut when the byte is exactly 0x80;
// it is passed on, not interpreted, so that the mapping decides (always false for MMD0, whose x and y bits are instrument
// bits, and on extra pages). instrument: 0 = none, 1..63. command and data: the MED command byte (MMD0: 0..15) and its data.
// ext: the command-extension byte of SoundStudio files (0 when the block has none), read by commands 20 and 29. On an extra
// command page only command, data and ext are set.
struct Cell {
  bool note_hi = false;
  std::uint8_t note = 0;
  std::uint8_t instrument = 0;
  std::uint8_t command = 0;
  std::uint8_t data = 0;
  std::uint8_t ext = 0;
};

struct BlockInfo {
  std::uint32_t offset = 0;           // the block header
  std::uint32_t cells = 0;            // the first cell of the default page
  std::uint16_t tracks = 0;           // 1..64 (0 for an empty block)
  std::uint16_t lines = 0;            // 1..9999 (0 for an empty block); the file stores lines - 1
  std::uint8_t cell_bytes = 0;        // 3 (MMD0) or 4
  std::uint8_t pages = 0;             // extra command pages, 0..16
  bool empty = true;                  // the block table holds 0 for this number: no data at all
  std::uint32_t cmd_ext = 0;          // the command-extension bytes (tracks x lines x (1 + pages)), 0 = none
  std::uint32_t page[caps::kMaxPages] = {};  // each extra page: tracks x lines two-byte cells; 0 = a page of empty cells
  std::uint32_t name = 0;             // the block name, 0 = none
  std::uint32_t name_len = 0;         // with the terminator, as the file says
};

// ---- instruments -----------------------------------------------------------------------------------------------------
enum class InstKind : std::uint8_t { Empty, Sample, Synth, Hybrid, Unsupported };

// Anomalies of one instrument that the import report counts (never silent).
enum InstNote : std::uint8_t {
  kNoteLoopClamped = 1u << 0,    // the loop was cut to lie inside the sample
  kNoteTrimmed = 1u << 1,        // bytes that belong to no whole frame or octave were ignored
  kNoteOversize = 1u << 2,       // the PCM would not fit the sample arena: refuse this instrument
  kNoteLongLoop = 1u << 3,       // the loop comes from InstrExt long_repeat / long_replen
};

struct InstrumentInfo {
  InstKind kind = InstKind::Empty;
  std::int16_t raw_type = 0;          // InstrHdr.type as stored
  std::uint8_t octaves = 1;           // sample data holds this many octaves, shortest first (types 1..6: 5, 3, 2, 4, 6, 7)
  bool sixteen_bit = false;
  bool stereo = false;
  bool ext_sample = false;            // type 7: two extra-low octaves are synthesised by the player
  std::uint8_t notes = 0;             // InstNote bits
  std::uint32_t offset = 0;           // InstrHdr
  std::uint32_t data = 0;             // the first byte of sample data (left channel of a stereo sample)
  std::uint32_t data_bytes = 0;       // one channel, all octaves
  std::uint32_t frames = 0;           // data_bytes in frames of one channel, all octaves
  std::uint32_t base_frames = 0;      // frames of the shortest octave (= frames when octaves == 1)
  std::uint64_t pcm_bytes = 0;        // 16-bit PCM the arena would hold: frames x channels x 2
  // The song record of the instrument.
  std::uint8_t midi_channel = 0;      // 0 = not a MIDI instrument
  std::uint8_t midi_preset = 0;
  std::uint8_t volume = 0;            // svol 0..64
  std::int8_t transpose = 0;          // strans, semitones
  // Loop in frames of the shortest octave (each longer octave doubles both), clamped into the sample.
  bool has_loop = false;
  bool ping_pong = false;
  std::uint32_t loop_start = 0;
  std::uint32_t loop_len = 0;
  // InstrExt (fields past the file's stride read as 0; ext_bytes says how many bytes of the 18 the file holds)
  std::uint8_t ext_bytes = 0;
  std::uint8_t hold = 0;
  std::uint8_t decay = 0;
  std::int8_t finetune = 0;           // -8..7
  std::uint8_t default_pitch = 0;     // 0 = none, 1 = C-1
  std::uint8_t ext_flags = 0;         // 1 loop, 2 ext preset, 4 disabled, 8 ping-pong
  bool disabled = false;
  std::uint16_t long_midi_preset = 0;
  std::uint8_t output_device = 0;
  std::uint32_t long_repeat = 0;      // V7 loop in frames (valid when ext_bytes >= 18); loop_start / loop_len already use it when
  std::uint32_t long_replen = 0;      // the rules of med.md 3.3 say so (kNoteLongLoop)
  // Synth and hybrid (SynthInstr)
  std::uint16_t voltbl_len = 0;
  std::uint16_t wftbl_len = 0;
  std::uint8_t vol_speed = 0;
  std::uint8_t wf_speed = 0;
  std::uint16_t waveforms = 0;        // 0..64 (0xFFFF in the file = none)
  char name[caps::kInstrumentNameMax + 1] = {};
};

// ---- the play list -----------------------------------------------------------------------------------------------------
// MMD0/1 hold one play sequence of block numbers. MMD2+ hold sections, each naming a play sequence; the flattened play list
// is the sequences of the sections one after the other (libxmp plays only the first sequence, OpenMPT appends them all;
// docs/formats/med.md 3.2). `0B xx` is a position within the section being played.
enum class PlayKind : std::uint8_t {
  Block,   // plays `block` (< song().num_blocks; its table entry may still be 0: Reader::block says empty)
  Skip,    // a number >= 0x8000 (reserved: players skip it)
  Bad,     // names a block number >= song().num_blocks: a reader skips it and the report counts it
  Stop,    // MMD2+ play-sequence command: the song ends here
  Jump,    // MMD2+ play-sequence command: continue at `target`, a position within this section
};

struct PlayEntry {
  PlayKind kind = PlayKind::Skip;
  std::uint16_t block = 0;
  std::uint16_t target = 0;           // Jump: position within the section
  std::uint16_t section = 0;
  std::uint32_t section_first = 0;    // flat position of the section's first entry
  std::uint32_t section_len = 0;      // entries in the section
};

// What validate() found. The importer decides what to refuse; nothing here is dropped silently.
struct Summary {
  std::uint32_t songs = 1;                 // the chain of nextmod songs, this one included
  std::uint32_t blocks_referenced = 0;     // playable blocks named by the play list (distinct below block number 4096)
  std::uint32_t empty_blocks = 0;          // referenced numbers whose block table entry is 0
  std::uint32_t bad_play_entries = 0;      // entries naming a block that does not exist or is empty
  std::uint32_t skip_play_entries = 0;     // entries >= 0x8000
  std::uint32_t bad_sections = 0;          // MMD2+: sections that name a play sequence that does not exist (they play nothing)
  std::uint32_t play_commands = 0;         // stop / jump entries
  std::uint16_t max_tracks = 0;
  std::uint16_t max_lines = 0;
  std::uint32_t pages_max = 0;
  std::uint64_t cells = 0;                 // cells of the referenced blocks (default page only)
  std::uint32_t instruments = 0;           // present (non-empty) instruments
  std::uint32_t synth = 0;                 // synth and hybrid instruments
  std::uint64_t unsupported_mask = 0;      // bit i: instrument i (slot, number - 1) has a type this reader does not take
  std::uint64_t oversize_mask = 0;         // bit i: its PCM alone would not fit the sample arena
  std::uint64_t pcm_bytes = 0;             // all instruments together (compare with caps::kSampleArenaBytes)
  std::uint32_t ignored = 0;               // Ignored bits
};

// validate() as a resumable job (OS-008b: the importer runs it in steps): the cursor of Reader::validate_step. A unit of work is
// one song of the chain, one section, one play-list entry (with the block it names, the first time), or one instrument.
struct ValidateState {
  enum class Phase : std::uint8_t { Chain, Sections, Play, Instruments, Done };
  Phase phase = Phase::Chain;
  std::uint32_t index = 0;
  std::uint8_t seen[512] = {};   // a 4096-bit set of the block numbers the play list has named
};

class Reader {
 public:
  // Reads the header, the song record, the expansion prefix and the section table of `source`, which must outlive the
  // Reader. Everything else is read on demand. A failed open leaves the Reader blank: song() is empty and every other call
  // answers Malformed.
  Status open(RandomSource& source) noexcept;

  const SongInfo& song() const noexcept { return song_; }
  RandomSource& source() const noexcept { return *src_; }

  // Walks every structure the song reaches (the referenced blocks, all instruments, the play list, the chain of songs) once
  // and returns the first problem, or fills `out`. Cells are not read: only their extent is checked.
  Status validate(Summary& out) noexcept;
  // The same walk in pieces: `out` and `st` start from their defaults (Summary(), ValidateState()); each call does at least one unit
  // and about `budget` of them, and returns the first problem (the job is then over) or sets st.phase to Done with `out` complete.
  // validate() is this run to the end, so the two cannot disagree.
  Status validate_step(ValidateState& st, Summary& out, std::size_t budget) const noexcept;

  // Blocks. `index` < song().num_blocks; an empty table entry gives out.empty = true.
  Status block(std::uint32_t index, BlockInfo& out) const noexcept;
  // Line `line` of the default page (page 0, notes and commands) or of extra page 1..b.pages (commands only) into
  // out[0 .. b.tracks).
  // `with_ext` false skips the command-extension read when the caller has no use for it (the flow simulation).
  Status read_line(const BlockInfo& b, std::uint32_t line, std::uint32_t page, Cell* out, bool with_ext = true) const noexcept;
  // The block name into out (cap >= 1), sanitised, at most min(cap - 1, name_len - 1) characters; "" when there is none.
  Status block_name(const BlockInfo& b, char* out, std::size_t cap) const noexcept;

  // Instruments: `slot` 0..num_samples-1 is MED instrument number slot + 1.
  Status instrument(std::uint32_t slot, InstrumentInfo& out) const noexcept;
  // Synth and hybrid: waveform `w` < info.waveforms. For a synth waveform `data` is the first byte and `bytes` the length;
  // for hybrid waveform 0 the instrument is a sample with its own InstrHdr at `header`, and `data`/`bytes` are its data.
  Status synth_waveform(const InstrumentInfo& info, std::uint32_t w, std::uint32_t& header, std::uint32_t& data,
                        std::uint32_t& bytes) const noexcept;

  // The flattened play list, positions 0 .. song().play_entries - 1.
  Status play_entry(std::uint32_t pos, PlayEntry& out) const noexcept;

  // Track volume 0..64 (64 when the file has none) and pan -16..16 (0 = centre) of track `t` < 64.
  std::uint8_t track_volume(std::uint32_t t) const noexcept;
  std::int8_t track_pan(std::uint32_t t) const noexcept;
  // The file has a pan table (MMD2+ with numtracks > 0 and a non-null trackpans); without one track_pan() says 0, centre, and the
  // importer picks the hardware panning of the song instead (OS-008b).
  bool has_track_pans() const noexcept { return song_.version >= 2 && trackpans_ != 0 && song_.num_tracks > 0; }

 private:
  Status open_impl(RandomSource& source) noexcept;
  Status get(std::uint32_t offset, void* dst, std::uint32_t n) const noexcept;
  Status table(std::uint32_t at, std::uint32_t ptr, std::uint64_t bytes, const char* what) const noexcept;
  Status section_seq(std::uint32_t section, std::uint32_t& seq_ptr, std::uint32_t& length) const noexcept;
  Status copy_text(std::uint32_t offset, std::uint32_t field_len, char* out, std::size_t cap) const noexcept;
  Status read_instrument_ext(std::uint32_t slot, InstrumentInfo& out) const noexcept;
  Status chain(std::uint32_t& songs) const noexcept;

  RandomSource* src_ = nullptr;
  SongInfo song_;
  std::uint32_t size_ = 0;
  std::uint32_t song_ptr_ = 0;
  std::uint32_t blockarr_ = 0;
  std::uint32_t smplarr_ = 0;
  std::uint32_t exp_ = 0;
  std::uint32_t exp_smp_ = 0;
  std::uint32_t iinfo_ = 0;
  // MMD2+
  std::uint32_t playseqtable_ = 0;
  std::uint32_t sectiontable_ = 0;
  std::uint32_t trackvols_ = 0;
  std::uint32_t trackpans_ = 0;
  std::uint32_t ignored_ = 0;     // Ignored bits of the expansion data, noted by open()
  std::uint16_t sec_start_[caps::kMaxSections + 1] = {};  // flat position of each section's first entry; [n] = total
};

}  // namespace o3os::formats::med
