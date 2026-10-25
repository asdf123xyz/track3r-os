// SPDX-License-Identifier: MIT
//
// "SMF Export" (OS-H4, new Community OS behaviour, ABI 1.7: capabilities read_project and export_files): the pattern as a Standard MIDI File on the card, in AppExport/smfexport/. It reads the project and
// writes nothing to it; the file is the OS's own atomic export (a Temp, a read-back, a rename that never replaces): this app only feeds it bytes.
//
//   RANGE    opened from Pattern F7 TOOLS with a block: the block's tracks and rows; without a block, or from the MODULES list: every track of the pattern, each over its own length. PATTERN (the
//            encoder) names the pattern; from TOOLS it starts as the pattern in view.
//   FILE     PAT<nnn>.mid (PAT<nnn>_SEL.mid for a block); a name that is taken becomes ..._001, the OS's rule. Type 1, 96 PPQ, 24 ticks to a row, tempo and 4/4 in the first track, one track per
//            track that has a note (smfexport_math.h has the rest: Micro-move, velocity, the channel, what is not exported).
//   WRITING  built a track at a time (the writer has no seek, so a track is whole in a buffer before its length is written) and fed to export_write in chunks of at most 1 KiB; a BUSY answer (the OS's
//            ring is full) is retried on the next frame. When the last byte is in, export_close, then export_status is read every frame until DONE or FAILED. F8 leaves; the OS aborts a file that is
//            still open then.
//   BUDGET   F1 only starts the job: every call into the app has 4 ms (O3M_VIEW_MAX_US, input() too), and the app's code and state run from SDRAM, uncached, so the work is spread over the frames of view_draw
//            in small units (scan one track for notes, open the file, build 32 rows of a track, write one chunk, close; smfexport_math.h kRowsPerSlice, kUnitsPerFrame), and a frame stops taking units
//            once the host's millisecond clock has moved (checked before every unit, the first too). A 16-track pattern takes about a second.
//   KEYS     the grammar of o3m_edit.h on its one row: the encoder the pattern (Shift + the encoder by ten), Delete the pattern it opened on; both ignored while a file is written. F1 EXPORT. F8 leaves.
//
// No OS parameters, no heap, no libc: every loop is written out.
#include <o3module.h>

#include "o3m_edit.h"
#include "smfexport_gen.h"
#include "smfexport_math.h"

namespace {

using namespace o3edit;
using namespace smfexport;

constexpr uint32_t kGuardMs = 1;        // a frame takes no unit once the clock has moved this much since it began
constexpr const char* kFolder = "APPEXPORT/SMFEXPORT/";

// kScanning: F1 was pressed, the tracks are being looked at (the header says how many hold a note); kWriting: the file is open.
enum Phase : uint32_t { kIdle, kScanning, kWriting, kClosing, kDone, kFailed };
// kMsg*: the answer under the page's row.
enum Msg : uint32_t { kMsgNone, kMsgNoHost, kMsgNoPattern, kMsgNoNotes, kMsgOpen, kMsgWrite, kMsgClose, kMsgCard };

struct State {
  const o3m_read_api* read;
  const o3m_io_api* io;
  uint32_t ready;       // both tables there with every member this app calls
  uint32_t pattern;     // 1..255
  uint32_t pat_kept;    // PATTERN as the project kept it (set_param, before enter()): 0 = the pattern the page opened on (AUTO), else 1..255; used when the app is opened from MODULES
  // the launch context
  uint32_t from_tools, ctx_pattern, ctx_track, ctx_instr, ctx_row0, ctx_rows, ctx_tracks;
  // the pattern and the range it makes (refresh)
  uint32_t have_pattern;
  o3m_pattern_info info;
  uint32_t block;       // the range is the launch block
  uint32_t first_track, track_count, first_row, rows_asked;
  // the export in progress
  uint32_t phase, msg, msg_code, finished;
  uint32_t lead_instr;  // the instrument a track's first step without one plays: the last one named above the range, else the page's (0: none)
  uint32_t next_track, out_len, out_pos, total_bytes, tracks_written, tracks_with_notes;
  uint32_t scan_track;  // the next track the scan looks at
  uint32_t building;    // a track is being built in `tw` (its steps are in `steps`)
  TrackWriter tw;
  Counts counts;
  char final_name[40];
  // per instrument: 0xFF = not read yet, else the MIDI channel 1..16 (0: none, the track's own) and the default velocity
  uint8_t chan[O3M_INSTR_MAX + 1];
  uint8_t defvel[O3M_INSTR_MAX + 1];
  o3m_step steps[kMaxPatRows];
  uint8_t out[kTrackBytesMax];
};
static_assert(sizeof(State) <= SMFEXPORT_STATE_BYTES, "module.json: state_bytes is too small for State");

// ---- the instruments (channel and default velocity) ----------------------------------------------------------------------------------------------------

void load_instrument(State* s, uint32_t n) {
  s->chan[n] = 0;
  s->defvel[n] = 127;
  if (!O3M_TABLE_HAS(s->read, instrument_info) || s->read->instrument_info == nullptr) {
    return;
  }
  o3m_instrument_info info;
  info.size = sizeof info;
  info.kind = O3M_INSTRUMENT_EMPTY;
  info.engine = info.param_count = info.midi_channel = 0;
  if (s->read->instrument_info(static_cast<uint8_t>(n), &info) != O3M_EDIT_OK) {
    return;
  }
  if (info.kind == O3M_INSTRUMENT_MIDI && info.size >= sizeof info && info.midi_channel >= 1u && info.midi_channel <= 16u) {
    s->chan[n] = static_cast<uint8_t>(info.midi_channel);
  } else if (info.kind == O3M_INSTRUMENT_SAMPLE && O3M_TABLE_HAS(s->read, get_instrument_param) && s->read->get_instrument_param != nullptr) {
    // a note with no volume plays at the sample instrument's own: the sequencer's default, the VOLUME parameter (id 0, 0..100) as 0..127 (sequencer.cpp default_velocity)
    float v = 0.0f;
    if (s->read->get_instrument_param(static_cast<uint8_t>(n), 0, &v) == O3M_EDIT_OK && v >= 0.0f && v <= 100.0f) {
      const uint32_t vel = static_cast<uint32_t>(v) * 127u / 100u;
      s->defvel[n] = static_cast<uint8_t>(vel < 1u ? 1u : vel);
    }
  }
}

struct Res {
  State* s;
  uint32_t first_instrument() const { return s->lead_instr; }
  uint32_t channel(uint32_t track, uint32_t instrument) const {
    if (instrument >= 1u && instrument <= O3M_INSTR_MAX) {
      if (s->chan[instrument] == 0xFFu) {
        load_instrument(s, instrument);
      }
      if (s->chan[instrument] != 0u) {
        return static_cast<uint32_t>(s->chan[instrument]) - 1u;  // the instrument's CHANNEL, 1..16
      }
    }
    return track % 16u;  // the track's number mod 16, plus 1
  }
  uint32_t default_velocity(uint32_t instrument) const {
    if (instrument >= 1u && instrument <= O3M_INSTR_MAX) {
      if (s->chan[instrument] == 0xFFu) {
        load_instrument(s, instrument);
      }
      return s->defvel[instrument];
    }
    return 127;
  }
};

// ---- the plan ----------------------------------------------------------------------------------------------------------------------------------------

uint32_t rows_of(const State* s, uint32_t track) {
  if (track >= s->info.tracks || track >= O3M_PATTERN_TRACKS_MAX) {
    return 0;
  }
  const uint32_t len = s->info.track_length[track];
  if (s->first_row >= len) {
    return 0;
  }
  const uint32_t room = len - s->first_row;
  const uint32_t n = s->rows_asked < room ? s->rows_asked : room;
  return n < kMaxPatRows ? n : kMaxPatRows;
}

// Reads the pattern in view of the page and works out the range (the launch block when it is for this pattern, else the whole pattern).
void refresh(State* s) {
  s->have_pattern = 0;
  s->info.size = sizeof s->info;
  if (s->read->pattern_info(static_cast<uint16_t>(s->pattern), &s->info) != O3M_EDIT_OK) {
    return;
  }
  s->have_pattern = 1;
  s->block = s->from_tools != 0u && s->ctx_rows != 0u && s->ctx_tracks != 0u && s->ctx_pattern == s->pattern ? 1u : 0u;
  if (s->block != 0u) {
    s->first_track = s->ctx_track;
    s->track_count = s->first_track < s->info.tracks ? (s->ctx_tracks < s->info.tracks - s->first_track ? s->ctx_tracks : s->info.tracks - s->first_track) : 0u;
    s->first_row = s->ctx_row0;
    s->rows_asked = s->ctx_rows;
  } else {
    s->first_track = 0;
    s->track_count = s->info.tracks < kMaxTracks ? s->info.tracks : kMaxTracks;
    s->first_row = 0;
    s->rows_asked = kMaxPatRows;
  }
}

// The steps of track `t` of the range into s->steps; the rows it has there (0: none).
uint32_t load_track(State* s, uint32_t t) {
  const uint32_t n = rows_of(s, t);
  if (n == 0u || s->read->get_steps(static_cast<uint16_t>(s->pattern), static_cast<uint8_t>(t), static_cast<uint8_t>(s->first_row), s->steps, static_cast<uint8_t>(n)) != O3M_EDIT_OK) {
    return 0;
  }
  return n;
}

// ---- exporting ---------------------------------------------------------------------------------------------------------------------------------------

void fail(State* s, uint32_t msg, int32_t code) {
  s->phase = kFailed;
  s->msg = msg;
  s->msg_code = static_cast<uint32_t>(code);
}

// The instrument in effect at the first row of the range on track `t`: the last one a step above the range names (the sequencer keeps an instrument until another is named), else the one the page was
// opened on (the cursor's instrument), else none. A step above the range is read only from this pattern: what an earlier pattern left on the track is not known to an app.
uint32_t lead_instrument(State* s, uint32_t t) {
  const uint32_t above = s->first_row < kMaxPatRows ? s->first_row : kMaxPatRows;
  if (above != 0u && s->read->get_steps(static_cast<uint16_t>(s->pattern), static_cast<uint8_t>(t), 0, s->steps, static_cast<uint8_t>(above)) == O3M_EDIT_OK) {
    for (uint32_t j = above; j != 0u; --j) {
      const uint32_t in = s->steps[j - 1u].instr;
      if (in >= 1u && in <= O3M_INSTR_MAX) {
        return in;
      }
    }
  }
  return s->from_tools != 0u ? s->ctx_instr + 1u : 0u;
}

// One unit of building: the next track that has a note, or the next slice of the one being built (false: there is no track left, the file is complete). The header already says how many tracks follow
// (the scan counted them), so exactly that many are written: a track that has lost its notes since (the project changed under the export) is written empty, and one that gained notes is left out,
// which keeps the file well-formed. A track with no note costs one call (it comes back with nothing built).
bool build_step(State* s) {
  Res res{s};
  if (s->building != 0u) {
    s->tw.run(s->steps, kRowsPerSlice, res, &s->counts);
    if (s->tw.complete()) {
      const uint32_t bytes = s->tw.finish();
      s->building = 0;
      if (bytes != 0u) {  // (cannot be 0: kTrackBytesMax holds 128 rows)
        s->out_len = bytes;
        s->out_pos = 0;
        ++s->tracks_written;
      }
    }
    return true;
  }
  if (s->tracks_written >= s->tracks_with_notes) {
    return false;
  }
  if (s->next_track < s->first_track + s->track_count) {
    const uint32_t t = s->next_track++;
    s->lead_instr = lead_instrument(s, t);
    const uint32_t n = load_track(s, t);
    if (n != 0u && has_note(s->steps, n)) {
      s->tw.begin(s->out, sizeof s->out, n, t, res);
      s->building = 1;
    }
    return true;
  }
  // no track left with a note: the ones that lost theirs are written empty
  s->lead_instr = 0;
  s->tw.begin(s->out, sizeof s->out, 0, s->first_track + s->tracks_written, res);
  const uint32_t bytes = s->tw.finish();
  if (bytes == 0u) {
    return false;
  }
  s->out_len = bytes;
  s->out_pos = 0;
  ++s->tracks_written;
  return true;
}

// The scan: the header says how many MTrk follow, so each track of the range is looked at (one unit a track) before the file is opened.
void scan_step(State* s) {
  if (s->scan_track < s->first_track + s->track_count) {
    const uint32_t n = load_track(s, s->scan_track++);
    s->tracks_with_notes += n != 0u && has_note(s->steps, n) ? 1u : 0u;
    return;
  }
  if (s->tracks_with_notes == 0u) {
    s->phase = kIdle;
    s->msg = kMsgNoNotes;
    return;
  }
  // the file name: PAT<nnn> (PAT<nnn>_SEL for a block)
  char stem[16];
  uint32_t n = put_text(stem, 0, "PAT");
  n = put_uint(stem, n, s->pattern, 3);
  if (s->block != 0u) {
    n = put_text(stem, n, "_SEL");
  }
  stem[n] = '\0';
  // the header and the tempo track go first
  const uint32_t h = build_header(s->out, sizeof s->out, 1u + s->tracks_with_notes);
  const uint32_t tt = build_tempo_track(s->out + h, static_cast<uint32_t>(sizeof s->out) - h, s->read->get_tempo());
  if (h == 0u || tt == 0u) {
    fail(s, kMsgWrite, 0);
    return;
  }
  s->out_len = h + tt;
  s->out_pos = 0;
  const int32_t r = s->io->export_open(stem, "mid");
  if (r != O3M_EXPORT_OK) {
    fail(s, kMsgOpen, r);
    return;
  }
  s->phase = kWriting;
}

// One unit of the writing phase: a chunk of the bytes in hand, else the next unit of building, else the close. False: nothing more this frame (BUSY, closed, failed).
bool write_step(State* s) {
  if (s->out_pos >= s->out_len) {
    if (s->finished == 0u) {
      if (!build_step(s)) {
        s->finished = 1;
      }
      return true;
    }
    const int32_t r = s->io->export_close();  // every byte has been taken: close
    if (r != O3M_EXPORT_OK) {
      fail(s, kMsgClose, r);
    } else {
      s->phase = kClosing;
    }
    return false;
  }
  const uint32_t left = s->out_len - s->out_pos;
  const uint32_t put = left < kChunk ? left : kChunk;
  const int32_t r = s->io->export_write(s->out + s->out_pos, put);
  if (r == O3M_EXPORT_BUSY) {
    return false;  // the ring is full: the same bytes again on the next frame
  }
  if (r != O3M_EXPORT_OK) {
    fail(s, kMsgWrite, r);
    return false;
  }
  s->out_pos += put;
  s->total_bytes += put;
  return true;
}

// One frame of work: units while the clock has not moved (checked before every unit) and at most kUnitsPerFrame, then, once closed, the status poll.
void pump(State* s) {
  if (s->phase == kScanning || s->phase == kWriting) {
    const uint32_t t0 = s->io->now_ms();
    for (uint32_t u = 0; u < kUnitsPerFrame && (s->phase == kScanning || s->phase == kWriting) && s->io->now_ms() - t0 < kGuardMs; ++u) {
      if (s->phase == kScanning) {
        scan_step(s);
      } else if (!write_step(s)) {
        break;
      }
    }
  } else if (s->phase == kClosing) {
    const uint32_t st = s->io->export_status(s->final_name, sizeof s->final_name);
    if (st == O3M_EXPORT_DONE) {
      s->phase = kDone;
      s->msg = kMsgNone;
    } else if (st == O3M_EXPORT_FAILED || st == O3M_EXPORT_IDLE) {
      fail(s, kMsgCard, 0);
    }
  }
}

// F1: checks what is cheap and starts the job; the work is done by the frames (pump), never in this call.
void start(State* s) {
  if (s->phase == kScanning || s->phase == kWriting || s->phase == kClosing) {
    return;
  }
  s->phase = kIdle;
  s->msg = kMsgNone;
  s->msg_code = 0;
  s->final_name[0] = '\0';
  if (s->ready == 0u) {
    s->msg = kMsgNoHost;
    return;
  }
  refresh(s);
  if (s->have_pattern == 0u) {
    s->msg = kMsgNoPattern;
    return;
  }
  for (uint32_t i = 0; i < kFxSlots; ++i) {
    s->counts.skipped[i] = 0;
  }
  s->counts.notes = 0;
  for (uint32_t i = 0; i <= O3M_INSTR_MAX; ++i) {
    s->chan[i] = 0xFF;
    s->defvel[i] = 127;
  }
  s->tracks_with_notes = 0;
  s->scan_track = s->first_track;
  s->next_track = s->first_track;
  s->tracks_written = s->total_bytes = s->finished = s->building = 0;
  s->out_len = s->out_pos = 0;
  s->phase = kScanning;
}

// ---- the host ----------------------------------------------------------------------------------------------------------------------------------------

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  if (host->draw == nullptr || !O3M_HOST_HAS(host, read) || host->read == nullptr || !O3M_TABLE_HAS(host->read, get_steps) || host->read->get_steps == nullptr || host->read->pattern_info == nullptr ||
      host->read->launch_ctx == nullptr || host->read->get_tempo == nullptr) {
    return 1;  // an OS below 1.5, or no read_project: nothing to export
  }
  s->read = host->read;
  s->io = O3M_HOST_HAS(host, io) ? host->io : nullptr;
  s->ready = s->io != nullptr && O3M_TABLE_HAS(s->io, export_status) && s->io->export_open != nullptr && s->io->export_write != nullptr && s->io->export_close != nullptr &&
                     s->io->export_status != nullptr && s->io->now_ms != nullptr
                 ? 1u
                 : 0u;  // (an OS below 1.7 or an app without export_files: the page says so)
  return 0;
}

// The one declared row, PATTERN (0 = AUTO: the pattern the page opened on, 1..255 a pattern): the OS hands back what the project kept before enter(). Opened from Pattern F7 TOOLS the target is the cursor's
// (a relaunch from TOOLS starts at the cursor), so the kept pattern is the start of a launch from MODULES.
void set_param(void* raw, uint32_t index, float v) {
  State* s = static_cast<State*>(raw);
  if (index == 0u) {
    const uint32_t n = v > 0.0f ? static_cast<uint32_t>(v + 0.5f) : 0u;
    s->pat_kept = n < 255u ? n : 255u;
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  o3m_launch_ctx lc;
  lc.size = sizeof lc;
  lc.flags = lc.pattern = lc.track = lc.instrument = lc.block_start = lc.block_length = 0;
  lc.block_tracks = 0;
  lc.reserved[0] = lc.reserved[1] = lc.reserved[2] = 0;
  s->from_tools = s->read->launch_ctx(&lc);
  s->ctx_pattern = lc.pattern;
  s->ctx_track = lc.track;
  s->ctx_instr = lc.instrument < O3M_INSTR_MAX ? lc.instrument : O3M_INSTR_MAX - 1u;
  s->ctx_row0 = lc.block_start;
  s->ctx_rows = lc.block_length;
  s->ctx_tracks = lc.block_tracks;
  const uint32_t home = s->from_tools != 0u && lc.pattern >= 1u && lc.pattern <= 255u ? lc.pattern : 1u;
  s->pattern = s->from_tools == 0u && s->pat_kept != 0u ? s->pat_kept : home;
  s->phase = kIdle;
  s->msg = kMsgNone;
  refresh(s);
}

// The one row is PATTERN: the encoder moves it (Shift + the encoder by ten) and Delete is the pattern the page opened on, while no file is being written (OS-APPUX S2).
void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  const bool busy = s->phase == kScanning || s->phase == kWriting || s->phase == kClosing;
  uint32_t row = 0;
  const Input in = grammar(ev, &row, 1, nullptr, 1);
  if (in.move == kMoveValue || in.move == kMoveDefault) {
    if (!busy) {
      const uint32_t home = s->from_tools != 0u && s->ctx_pattern >= 1u && s->ctx_pattern <= 255u ? s->ctx_pattern : 1u;
      s->pattern = in.move == kMoveDefault ? home : static_cast<uint32_t>(clamp_i(static_cast<int32_t>(s->pattern) + steps(in, 10), 1, 255));
      own_set(s->io, 0, in.move == kMoveDefault ? 0.0f : static_cast<float>(s->pattern));  // (kept with the project: the default as AUTO)
      s->phase = kIdle;
      s->msg = kMsgNone;
      refresh(s);
    }
    return;
  }
  if (ev->kind == O3M_EV_BUTTON && ev->value != 0 && ev->id == O3M_BTN_F1) {
    start(s);
  }
}

// ---- the page ----------------------------------------------------------------------------------------------------------------------------------------

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  State* s = static_cast<State*>(raw);
  pump(s);  // (the frame is the app's clock: the writes and the status poll go on here)
  char facts[64];
  facts[0] = '\0';
  if (s->have_pattern != 0u) {
    uint32_t n = 0;
    if (s->block != 0u) {
      n = put_text(facts, n, "BLOCK  TRACK ");
      n = put_uint(facts, n, s->first_track + 1u, 1);
      if (s->track_count > 1u) {
        n = put_text(facts, n, "-");
        n = put_uint(facts, n, s->first_track + s->track_count, 1);
      }
      n = put_text(facts, n, "  ROWS ");
      n = put_uint(facts, n, s->first_row + 1u, 1);
      n = put_text(facts, n, "-");
      n = put_uint(facts, n, s->first_row + (s->ctx_rows != 0u ? s->ctx_rows : 1u), 1);
    } else {
      n = put_text(facts, n, "WHOLE PATTERN  TRACKS ");
      n = put_uint(facts, n, s->track_count, 1);
      n = put_text(facts, n, "  ROWS ");
      n = put_uint(facts, n, s->info.length, 1);
    }
    facts[n] = '\0';
  }
  PageRow rows[1];
  rows[0].name = "PATTERN";
  rows[0].dim = false;
  {
    uint32_t n = put_uint(rows[0].value, 0, s->pattern, 1);
    if (s->have_pattern == 0u) {
      n = put_text(rows[0].value, n, " (EMPTY)");
    }
    rows[0].value[n] = '\0';
  }
  char answer[72], extra[200];
  uint32_t n = 0, x = 0;
  uint32_t color = O3M_COLOR_ACCENT;
  switch (s->phase) {
    case kScanning:
    case kWriting:
    case kClosing: n = put_text(answer, n, "EXPORTING..."); break;
    case kDone:
      n = put_text(answer, n, "DONE");
      color = O3M_COLOR_PLAY;
      break;
    case kFailed:
      color = O3M_COLOR_ACCENT;
      switch (s->msg) {
        case kMsgOpen:
          n = put_text(answer, n, s->msg_code == O3M_EXPORT_NO_CARD ? "NO CARD, OR IT CANNOT BE WRITTEN NOW" : (s->msg_code == O3M_EXPORT_LIMIT ? "16 FILES THIS TIME: LEAVE AND OPEN AGAIN" : "NOT EXPORTED"));
          n = put_text(answer, n, "  (CODE ");
          n = put_uint(answer, n, s->msg_code, 1);
          n = put_text(answer, n, ")");
          break;
        case kMsgCard: n = put_text(answer, n, "NOT WRITTEN: THE CARD FAILED OR THE SAVE WAS GIVEN UP"); break;
        default:
          n = put_text(answer, n, "NOT EXPORTED  (CODE ");
          n = put_uint(answer, n, s->msg_code, 1);
          n = put_text(answer, n, ")");
          break;
      }
      break;
    default:
      color = O3M_COLOR_ACCENT;
      switch (s->msg) {
        case kMsgNoHost: n = put_text(answer, n, "THIS HOST CANNOT EXPORT FILES"); break;
        case kMsgNoPattern: n = put_text(answer, n, "THE PATTERN DOES NOT EXIST YET: ENTER A NOTE FIRST"); break;
        case kMsgNoNotes: n = put_text(answer, n, "NO NOTES IN THE RANGE"); break;
        default: break;
      }
      break;
  }
  answer[n] = '\0';
  if (s->phase == kDone || s->phase == kScanning || s->phase == kWriting || s->phase == kClosing) {
    if (s->phase == kDone) {
      x = put_text(extra, x, "WROTE ");
      x = put_text(extra, x, kFolder);
      x = put_text(extra, x, s->final_name);
      x = put_text(extra, x, ". ");
    }
    x = put_uint(extra, x, s->counts.notes, 1);
    x = put_text(extra, x, s->counts.notes == 1u ? " NOTE  " : " NOTES  ");
    x = put_uint(extra, x, s->tracks_written, 1);
    x = put_text(extra, x, s->tracks_written == 1u ? " TRACK  " : " TRACKS  ");
    x = put_uint(extra, x, s->total_bytes, 1);
    x = put_text(extra, x, " BYTES. ");
    bool any = false;
    for (uint32_t t = 1; t < kFxSlots && x + 20u < sizeof extra; ++t) {
      if (s->counts.skipped[t] != 0u) {
        x = put_text(extra, x, any ? "  " : "FX NOT EXPORTED: ");
        if (t == 43u) {
          x = put_text(extra, x, "ENGINE");
        } else {
          x = put_text(extra, x, "FX");
          x = put_uint(extra, x, t, 1);
        }
        x = put_text(extra, x, " X");
        x = put_uint(extra, x, s->counts.skipped[t], 1);
        any = true;
      }
    }
  }
  extra[x] = '\0';
  draw_page(c, "SMF EXPORT", facts, rows, 1, 0, extra, answer, color);
}

void leave(void*) {}
void shutdown(void*) {}

// OS-KEYROWDLG S5 (ABI 1.9): what the keys do, for the page's key row (labels only: the OS delivers the keys to input() whatever they say; F8 is the OS's BACK).
const o3m_keys kKeys = {{"EXPORT"}, {"EXPORT"}};  // (Shift + Fn runs Fn: no app reads `mods`, so the Shift row says the same)

const o3m_keys* ui_keys(void*) { return &kKeys; }

const o3m_ui kUi = {O3M_UI_SIZE(SMFEXPORT_ABI_MINOR), enter, input, view_draw, leave, ui_keys};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    SMFEXPORT_ENTRY_HEAD, sizeof(State), SMFEXPORT_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, SMFEXPORT_ENTRY_VIEWS(&kUi),
};
