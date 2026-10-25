// SPDX-License-Identifier: GPL-3.0-or-later
#include <doctest/doctest.h>

#include "support/mmd_fixture.h"
#include "support/need.h"

#include <o3os/formats/med/mmd.h>

#include <cstdint>
#include <cstring>
#include <string>

using namespace o3os;
using namespace o3os::formats::med;
using o3os::test::mmd::Bytes;
using o3os::test::mmd::Layout;
using o3os::test::mmd::ModuleSpec;

// The MMD0 to MMD3 reader of OS-008a (slice M0) over synthetic files from support/mmd_fixture.h: header, song, blocks and
// cells (3- and 4-byte cells, command pages, the extension table), instruments of every type, the play list of every
// version, track tables, the summary of validate(). The hostile-input side is med_hostile_test.cpp.

namespace {

struct Opened {
  Layout lay;  // before bytes: build() fills it
  Bytes bytes;
  SpanSource src;
  Reader rd;
  Status status;
  explicit Opened(const ModuleSpec& spec) : bytes(test::mmd::build(spec, &lay)), src(bytes.data(), static_cast<std::uint32_t>(bytes.size())) {
    status = rd.open(src);
  }
};

}  // namespace

TEST_CASE("med reader: the header and the song record of every version") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    Opened o(test::mmd::sample_module(v));
    NEED(o.status.ok());
    const SongInfo& s = o.rd.song();
    CHECK(s.version == v);
    CHECK(s.num_blocks == 3);
    CHECK(s.num_samples == 6);
    CHECK(s.def_tempo == 33);
    CHECK(s.tempo2 == 6);
    CHECK(s.vol_hex());
    CHECK(!s.bpm_mode());
    CHECK(s.lines_per_beat() == 4);
    CHECK(s.master_volume == 64);
    CHECK(s.has_exp);
    CHECK(s.s_ext_entrsz == 18);
    CHECK(s.i_ext_entrsz == 40);
    CHECK(std::string(s.name) == "Synthetic Song");
    CHECK(s.annotation_len == 8);
    CHECK(s.file_size == o.bytes.size());
    if (v <= 1) {
      CHECK(s.song_len == 3);
      CHECK(s.play_entries == 3);
    } else {
      CHECK(s.song_len == 2);  // sections
      CHECK(s.play_entries == 5);
      CHECK(s.num_pseqs == 2);
      CHECK(s.num_tracks == 4);
      CHECK(s.flags3 == 1);
      CHECK(s.vol_adjust == 150);
      CHECK(s.channels == 4);
      CHECK(s.echo_type == 1);
      CHECK(s.echo_depth == 2);
      CHECK(s.echo_len == 300);
      CHECK(s.stereo_sep == 2);
    }
  }
}

TEST_CASE("med reader: flags2 gives the BPM mode, the beat length and mixing") {
  ModuleSpec m = test::mmd::sample_module(1);
  m.flags2 = 0x20 | 5;
  Opened a(m);
  NEED(a.status.ok());
  CHECK(a.rd.song().bpm_mode());
  CHECK(a.rd.song().lines_per_beat() == 6);
  CHECK(!a.rd.song().mixing());
  m.flags2 = 0x80;
  m.flags = 0x40 | 0x20;
  Opened b(m);
  NEED(b.status.ok());
  CHECK(b.rd.song().mixing());
  CHECK(b.rd.song().eight_channel());
  CHECK(b.rd.song().st_slide());
  CHECK(b.rd.song().lines_per_beat() == 4);
}

TEST_CASE("med reader: cells of every version, including the bits of MMD0 and the 7-bit notes of MMD1") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    Opened o(test::mmd::sample_module(v));
    NEED(o.status.ok());
    BlockInfo b;
    NEED(o.rd.block(0, b).ok());
    CHECK(!b.empty);
    CHECK(b.tracks == 4);
    CHECK(b.lines == 8);
    CHECK(b.cell_bytes == (v == 0 ? 3 : 4));
    Cell cells[64];
    NEED(o.rd.read_line(b, 0, 0, cells).ok());
    CHECK(cells[0].note == 25);
    CHECK(cells[0].instrument == 1);
    CHECK(cells[0].command == 0x0C);
    CHECK(cells[0].data == 0x40);
    CHECK(cells[1].note == 30);
    CHECK(cells[1].instrument == 2);
    CHECK(cells[2].note == 0);
    NEED(o.rd.read_line(b, 2, 0, cells).ok());
    CHECK(cells[0].command == 0x0F);
    CHECK(cells[0].data == 0x00);
    NEED(o.rd.read_line(b, 4, 0, cells).ok());
    CHECK(cells[3].note == 40);
    CHECK(cells[3].instrument == 63);  // iiii + x as bit 4 + y as bit 5 in MMD0
    CHECK(cells[3].command == 0x04);
    CHECK(cells[3].data == 0x35);
    NEED(o.rd.read_line(b, 7, 0, cells).ok());
    CHECK(cells[2].note == 63);
    CHECK(cells[2].instrument == 31);
    CHECK(cells[2].command == 0x0F);
    CHECK(cells[2].data == 0xF3);
    NEED(o.rd.read_line(b, 3, 0, cells).ok());
    CHECK(cells[0].note == 0);

    NEED(o.rd.block(1, b).ok());
    CHECK(b.lines == 4);
    NEED(o.rd.read_line(b, 0, 0, cells).ok());
    CHECK(cells[0].command == 0x09);
    CHECK(cells[0].data == 0x03);
    CHECK(cells[0].instrument == 5);
    if (v >= 1) {
      NEED(o.rd.read_line(b, 1, 0, cells).ok());
      CHECK(cells[0].note == 100);  // above 63: MMD1 notes are 7 bits
      CHECK(cells[0].instrument == 7);
      CHECK(cells[0].command == 0x1F);
      CHECK(cells[0].data == 0x23);
    }
  }
}

TEST_CASE("med reader: bit 7 of an MMD1 note byte is passed on in Cell::note_hi, not masked away") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    Opened o(test::mmd::sample_module(v));
    NEED(o.status.ok());
    BlockInfo b;
    NEED(o.rd.block(2, b).ok());
    Cell cells[64];
    NEED(o.rd.read_line(b, 0, 0, cells).ok());
    if (v == 0) {  // MMD0 cells have no such bit: the x and y bits are instrument bits
      for (unsigned t = 0; t < 4; ++t) CHECK(!cells[t].note_hi);
      continue;
    }
    CHECK(cells[0].note == 0);   // the note byte 0x80: OpenMPT calls it a note cut, the reader leaves the call to the mapping
    CHECK(cells[0].note_hi);
    CHECK(cells[1].note == 1);   // 0x81
    CHECK(cells[1].note_hi);
    CHECK(!cells[2].note_hi);
    NEED(o.rd.read_line(b, 1, 0, cells).ok());
    CHECK(cells[2].note == 33);
    CHECK(!cells[2].note_hi);
    NEED(o.rd.block(0, b).ok());
    NEED(o.rd.read_line(b, 0, 0, cells).ok());
    CHECK(cells[0].note == 25);
    CHECK(!cells[0].note_hi);
    NEED(o.rd.read_line(b, 0, 1, cells).ok());  // extra pages never carry it
    CHECK(!cells[0].note_hi);
  }
}

TEST_CASE("med reader: block names, command pages and the command-extension table (MMD1+)") {
  for (unsigned v = 1; v <= 3; ++v) {
    CAPTURE(v);
    Opened o(test::mmd::sample_module(v));
    NEED(o.status.ok());
    BlockInfo b;
    NEED(o.rd.block(0, b).ok());
    CHECK(b.pages == 1);
    CHECK(b.cmd_ext != 0);
    char name[32];
    NEED(o.rd.block_name(b, name, sizeof name).ok());
    CHECK(std::string(name) == "intro");
    Cell cells[64];
    NEED(o.rd.read_line(b, 1, 1, cells).ok());  // extra page: command and data only
    CHECK(cells[0].command == 0x1D);
    CHECK(cells[0].data == 0x02);
    CHECK(cells[0].note == 0);
    CHECK(cells[0].instrument == 0);
    NEED(o.rd.read_line(b, 3, 1, cells).ok());
    CHECK(cells[2].command == 0x16);
    CHECK(cells[2].data == 0x00);
    // the extension byte of line 1, track 1 of page 0 (offset 5 of the table)
    NEED(o.rd.read_line(b, 1, 0, cells).ok());
    CHECK(cells[1].ext == 0x7F);
    CHECK(cells[0].ext == 0);
    NEED(o.rd.read_line(b, 0, 0, cells, false).ok());
    CHECK(cells[0].ext == 0);
    CHECK(!o.rd.read_line(b, 8, 0, cells).ok());   // line past the block
    CHECK(!o.rd.read_line(b, 0, 2, cells).ok());   // page past the block
    NEED(o.rd.block(2, b).ok());
    NEED(o.rd.block_name(b, name, sizeof name).ok());
    CHECK(std::string(name).empty());
    CHECK(b.pages == 0);
    CHECK(b.cmd_ext == 0);
  }
  Opened o0(test::mmd::sample_module(0));
  NEED(o0.status.ok());
  BlockInfo b;
  NEED(o0.rd.block(0, b).ok());
  CHECK(b.pages == 0);
  CHECK(b.name == 0);
}

TEST_CASE("med reader: an empty block table entry and a block number past the table") {
  ModuleSpec m = test::mmd::sample_module(1);
  m.blocks[2].empty = true;
  Opened o(m);
  NEED(o.status.ok());
  BlockInfo b;
  NEED(o.rd.block(2, b).ok());
  CHECK(b.empty);
  CHECK(b.tracks == 0);
  CHECK(!o.rd.block(3, b).ok());
}

TEST_CASE("med reader: instruments of every type") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    Opened o(test::mmd::sample_module(v));
    NEED(o.status.ok());
    InstrumentInfo i;

    NEED(o.rd.instrument(0, i).ok());  // an 8-bit sample with a loop
    CHECK(i.kind == InstKind::Sample);
    CHECK(i.octaves == 1);
    CHECK(!i.sixteen_bit);
    CHECK(!i.stereo);
    CHECK(i.frames == 64);
    CHECK(i.base_frames == 64);
    CHECK(i.data_bytes == 64);
    CHECK(i.pcm_bytes == 128);
    CHECK(i.volume == 48);
    CHECK(i.transpose == -3);
    CHECK(i.has_loop);
    CHECK(i.loop_start == 16);  // rep and replen are in units of two frames
    CHECK(i.loop_len == 32);
    CHECK((i.notes & ~kNoteLongLoop) == 0);
    CHECK(std::string(i.name) == "square");
    CHECK(i.ext_bytes == 18);
    CHECK(i.hold == 2);
    CHECK(i.decay == 3);
    CHECK(i.finetune == -1);
    CHECK(i.default_pitch == 25);
    CHECK(i.ext_flags == 1);
    CHECK(!i.disabled);
    const std::uint8_t first = o.bytes[i.data + 1];
    CHECK(first == 4);

    NEED(o.rd.instrument(1, i).ok());  // 16-bit stereo: length is one channel, in bytes
    CHECK(i.kind == InstKind::Sample);
    CHECK(i.sixteen_bit);
    CHECK(i.stereo);
    CHECK(i.data_bytes == 32);
    CHECK(i.frames == 16);
    CHECK(i.pcm_bytes == 64);
    CHECK(!i.has_loop);
    CHECK(std::string(i.name) == "stereo16");

    NEED(o.rd.instrument(2, i).ok());  // IFF3OCT: 35 frames = 5 + 10 + 20
    CHECK(i.raw_type == 2);
    CHECK(i.octaves == 3);
    CHECK(i.frames == 35);
    CHECK(i.base_frames == 5);
    CHECK(i.has_loop);
    CHECK(i.loop_start == 2);
    CHECK(i.loop_len == 3);  // 4 asked, clamped into the 5 frames of the shortest octave
    CHECK((i.notes & kNoteLoopClamped) != 0);

    NEED(o.rd.instrument(3, i).ok());  // synth
    CHECK(i.kind == InstKind::Synth);
    CHECK(i.waveforms == 2);
    CHECK(i.voltbl_len == 4);
    CHECK(i.wftbl_len == 4);
    std::uint32_t hdr = 0, data = 0, bytes = 0;
    NEED(o.rd.synth_waveform(i, 0, hdr, data, bytes).ok());
    CHECK(bytes == 8);
    CHECK(o.bytes[data] == 0x10);
    NEED(o.rd.synth_waveform(i, 1, hdr, data, bytes).ok());
    CHECK(bytes == 16);
    CHECK(o.bytes[data] == 0x20);
    CHECK(!o.rd.synth_waveform(i, 2, hdr, data, bytes).ok());

    NEED(o.rd.instrument(4, i).ok());  // hybrid: wf[0] is a sample of its own
    CHECK(i.kind == InstKind::Hybrid);
    CHECK(i.waveforms == 2);
    CHECK(i.frames == 12);
    NEED(o.rd.synth_waveform(i, 0, hdr, data, bytes).ok());
    CHECK(bytes == 12);
    CHECK(data == hdr + 6);
    NEED(o.rd.synth_waveform(i, 1, hdr, data, bytes).ok());
    CHECK(bytes == 8);

    NEED(o.rd.instrument(5, i).ok());  // no instrument in the slot
    CHECK(i.kind == InstKind::Empty);
    CHECK(!o.rd.instrument(6, i).ok());
  }
}

TEST_CASE("med reader: sample types, flags and the rules for the loop") {
  ModuleSpec m = test::mmd::sample_module(2);
  m.instruments.clear();
  auto add = [&](int type, std::size_t n) {
    test::mmd::InstrSpec s;
    s.type = type;
    s.data.assign(n, 0x22);
    m.instruments.push_back(s);
  };
  add(0x10, 40);   // 16-bit
  add(0x18, 40);   // obsolete Aura 16-bit
  add(7, 20);      // EXTSAMPLE
  add(1, 62);      // IFF5OCT: 31 parts of 2
  add(3, 9);       // IFF2OCT: 3 parts of 3
  add(4, 30);      // IFF4OCT
  add(5, 63);      // IFF6OCT
  add(6, 127);     // IFF7OCT
  add(0x40, 8);    // delta: not read
  add(0x80, 8);    // packed: not read
  add(0x08, 8);    // a type nobody has described
  add(-3, 8);      // HighLife
  Opened o(m);
  NEED(o.status.ok());
  InstrumentInfo i;
  NEED(o.rd.instrument(0, i).ok());
  CHECK(i.sixteen_bit);
  CHECK(i.frames == 20);
  NEED(o.rd.instrument(1, i).ok());
  CHECK(i.kind == InstKind::Sample);
  CHECK(i.sixteen_bit);
  CHECK(i.frames == 20);
  NEED(o.rd.instrument(2, i).ok());
  CHECK(i.ext_sample);
  CHECK(i.octaves == 1);
  NEED(o.rd.instrument(3, i).ok());
  CHECK(i.octaves == 5);
  CHECK(i.base_frames == 2);
  NEED(o.rd.instrument(4, i).ok());
  CHECK(i.octaves == 2);
  CHECK(i.base_frames == 3);
  NEED(o.rd.instrument(5, i).ok());
  CHECK(i.octaves == 4);
  CHECK(i.base_frames == 2);
  NEED(o.rd.instrument(6, i).ok());
  CHECK(i.octaves == 6);
  CHECK(i.base_frames == 1);
  NEED(o.rd.instrument(7, i).ok());
  CHECK(i.octaves == 7);
  CHECK(i.base_frames == 1);
  for (unsigned k = 8; k < 12; ++k) {
    CAPTURE(k);
    NEED(o.rd.instrument(k, i).ok());
    CHECK(i.kind == InstKind::Unsupported);
  }
  Summary sum;
  NEED(o.rd.validate(sum).ok());
  CHECK(sum.unsupported_mask == (0xFull << 8));
  CHECK(sum.instruments == 12);
}

TEST_CASE("med reader: the loop follows the loop flag of MMD2+ and the long loop of MMD3") {
  ModuleSpec m = test::mmd::sample_module(2);
  m.instruments.clear();
  test::mmd::InstrSpec s;
  s.type = 0;
  s.data.assign(100, 0x22);
  s.rep = 5;
  s.replen = 10;
  s.ext = {0, 0, 0, 0, 0, 0};  // flags 0: the loop flag is off
  m.instruments = {s};
  m.s_ext_entrsz = 6;
  {
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(!i.has_loop);  // MMD2+: the flag decides, replen alone does not
  }
  m.instruments[0].ext = {0, 0, 0, 0, 0, 1 | 8};
  {
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(i.has_loop);
    CHECK(i.ping_pong);
    CHECK(i.loop_start == 10);
    CHECK(i.loop_len == 20);
  }
  // MMD3 with the V7 InstrExt (stride 18): long_repeat 30, long_replen 40 override the song record
  m.version = 3;
  m.s_ext_entrsz = 18;
  m.instruments[0].ext = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 30, 0, 0, 0, 40};
  {
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(i.has_loop);
    CHECK(i.loop_start == 30);
    CHECK(i.loop_len == 40);
    CHECK((i.notes & kNoteLongLoop) != 0);
  }
  // the same stride in an MMD2 file does not override (libxmp's rule)
  m.version = 2;
  {
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(i.loop_start == 10);
    CHECK((i.notes & kNoteLongLoop) == 0);
  }
  // MMD0/1: replen <= 1 is no loop whatever rep says
  m.version = 1;
  m.s_ext_entrsz = 0;
  m.instruments[0].replen = 1;
  {
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(!i.has_loop);
  }
  // a loop that starts past the end is cut to nothing
  m.instruments[0].replen = 20;
  m.instruments[0].rep = 90;  // 180 frames in
  {
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(!i.has_loop);
    CHECK((i.notes & kNoteLoopClamped) != 0);
  }
}

TEST_CASE("med reader: the play list of MMD0/1 and of sections with play-sequence commands") {
  Opened o1(test::mmd::sample_module(1));
  NEED(o1.status.ok());
  PlayEntry e;
  const std::uint16_t want[3] = {0, 1, 0};
  for (std::uint32_t p = 0; p < 3; ++p) {
    NEED(o1.rd.play_entry(p, e).ok());
    CHECK(e.kind == PlayKind::Block);
    CHECK(e.block == want[p]);
    CHECK(e.section_len == 3);
  }
  CHECK(!o1.rd.play_entry(3, e).ok());

  Opened o2(test::mmd::sample_module(2));
  NEED(o2.status.ok());
  // section 0 (play sequence 0): blocks 0, 1; section 1 (play sequence 1): block 2, a reserved number turned into a
  // Jump to position 0 of the section by a command, block 0
  NEED(o2.rd.play_entry(0, e).ok());
  CHECK(e.kind == PlayKind::Block);
  CHECK(e.block == 0);
  CHECK(e.section == 0);
  CHECK(e.section_first == 0);
  CHECK(e.section_len == 2);
  NEED(o2.rd.play_entry(1, e).ok());
  CHECK(e.block == 1);
  NEED(o2.rd.play_entry(2, e).ok());
  CHECK(e.kind == PlayKind::Block);
  CHECK(e.block == 2);
  CHECK(e.section == 1);
  CHECK(e.section_first == 2);
  CHECK(e.section_len == 3);
  NEED(o2.rd.play_entry(3, e).ok());
  CHECK(e.kind == PlayKind::Jump);
  CHECK(e.target == 0);
  NEED(o2.rd.play_entry(4, e).ok());
  CHECK(e.kind == PlayKind::Block);
  CHECK(e.block == 0);
  CHECK(!o2.rd.play_entry(5, e).ok());

  // a Stop command, a reserved number and an entry past the block table
  ModuleSpec m = test::mmd::sample_module(2);
  m.pseqs[1].entries = {2, 0x8000, 9, 1};
  m.pseqs[1].cmds = {{3, 1, {}}};
  Opened o3(m);
  NEED(o3.status.ok());
  NEED(o3.rd.play_entry(3, e).ok());
  CHECK(e.kind == PlayKind::Skip);
  NEED(o3.rd.play_entry(4, e).ok());
  CHECK(e.kind == PlayKind::Bad);
  NEED(o3.rd.play_entry(5, e).ok());
  CHECK(e.kind == PlayKind::Stop);
  Summary sum;
  NEED(o3.rd.validate(sum).ok());
  CHECK(sum.skip_play_entries == 1);
  CHECK(sum.bad_play_entries == 1);
  CHECK(sum.play_commands == 1);
}

TEST_CASE("med reader: a section that names a play sequence that does not exist plays nothing") {
  ModuleSpec m = test::mmd::sample_module(2);
  m.sections = {0, 7, 1};
  Opened o(m);
  NEED(o.status.ok());
  CHECK(o.rd.song().play_entries == 5);
  Summary sum;
  NEED(o.rd.validate(sum).ok());
  CHECK(sum.bad_sections == 1);
  PlayEntry e;
  NEED(o.rd.play_entry(2, e).ok());
  CHECK(e.section == 2);
  CHECK(e.block == 2);
}

TEST_CASE("med reader: track volumes and pans") {
  Opened o1(test::mmd::sample_module(1));
  NEED(o1.status.ok());
  CHECK(o1.rd.track_volume(0) == 64);
  CHECK(o1.rd.track_volume(15) == 64);
  CHECK(o1.rd.track_volume(16) == 64);  // MMD0/1 hold 16 volumes; beyond them the default
  CHECK(o1.rd.track_pan(0) == 0);
  ModuleSpec m = test::mmd::sample_module(1);
  m.trkvol[3] = 20;
  m.trkvol[4] = 99;  // above 64: clamped
  Opened o1b(m);
  NEED(o1b.status.ok());
  CHECK(o1b.rd.track_volume(3) == 20);
  CHECK(o1b.rd.track_volume(4) == 64);

  Opened o2(test::mmd::sample_module(3));
  NEED(o2.status.ok());
  CHECK(o2.rd.track_volume(0) == 64);
  CHECK(o2.rd.track_volume(1) == 50);
  CHECK(o2.rd.track_volume(3) == 16);
  CHECK(o2.rd.track_volume(4) == 64);  // past numtracks
  CHECK(o2.rd.track_pan(0) == -16);
  CHECK(o2.rd.track_pan(1) == 0);
  CHECK(o2.rd.track_pan(3) == 16);
  CHECK(o2.rd.track_pan(60) == 0);
  m = test::mmd::sample_module(3);
  m.trackpans = {-100, 100, 5, 0};  // out of range: clamped
  Opened o2b(m);
  NEED(o2b.status.ok());
  CHECK(o2b.rd.track_pan(0) == -16);
  CHECK(o2b.rd.track_pan(1) == 16);
  CHECK(o2b.rd.track_pan(2) == 5);
}

TEST_CASE("med reader: validate() summarises a whole file") {
  for (unsigned v = 0; v <= 3; ++v) {
    CAPTURE(v);
    Opened o(test::mmd::sample_module(v));
    NEED(o.status.ok());
    Summary s;
    const Status st = o.rd.validate(s);
    CHECK_MESSAGE(st.ok(), st.detail);
    NEED(st.ok());
    CHECK(s.songs == 1);
    CHECK(s.blocks_referenced == (v <= 1 ? 2u : 3u));
    CHECK(s.max_tracks == 4);
    CHECK(s.max_lines == 8);
    CHECK(s.instruments == 5);
    CHECK(s.synth == 2);
    CHECK(s.unsupported_mask == 0);
    CHECK(s.oversize_mask == 0);
    CHECK(s.pages_max == (v >= 1 ? 1u : 0u));
    CHECK(s.pcm_bytes == 128 + 64 + 70 + 24);
    CHECK((s.ignored & kIgnoreAnnotation) != 0);
    CHECK(((s.ignored & kIgnoreEcho) != 0) == (v >= 2));
    CHECK(((s.ignored & kIgnoreVolAdjust) != 0) == (v >= 2));
    CHECK(((s.ignored & kIgnoreStereoSep) != 0) == (v >= 2));
    CHECK((s.ignored & kIgnoreExtraSongs) == 0);
    CHECK(s.cells == (v <= 1 ? 4u * 8 + 4u * 4 : 4u * 8 + 4u * 4 + 4u * 2));
  }
}

TEST_CASE("med reader: chained songs are counted and listed as ignored") {
  ModuleSpec m = test::mmd::sample_module(1);
  m.extra_songs = 3;
  Opened o(m);
  NEED(o.status.ok());
  Summary s;
  NEED(o.rd.validate(s).ok());
  CHECK(s.songs == 4);
  CHECK((s.ignored & kIgnoreExtraSongs) != 0);
  CHECK(o.rd.song().extra_songs == 3);
}

TEST_CASE("med reader: names are sanitised and cut to our limits") {
  ModuleSpec m = test::mmd::sample_module(1);
  m.instruments[0].name = std::string("a\x01" "b\x7F" "c\xE9") + std::string(60, 'x');
  m.songname = std::string(50, 'S');
  Opened o(m);
  NEED(o.status.ok());
  InstrumentInfo i;
  NEED(o.rd.instrument(0, i).ok());
  CHECK(std::string(i.name).substr(0, 6) == "a?b?c?");
  CHECK(std::strlen(i.name) == caps::kInstrumentNameMax);
  CHECK(std::strlen(o.rd.song().name) == caps::kSongNameMax);
}

TEST_CASE("med reader: the InstrExt stride decides which fields exist") {
  for (unsigned stride : {1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u, 10u, 14u, 17u, 18u, 24u}) {
    CAPTURE(stride);
    ModuleSpec m = test::mmd::sample_module(3);
    m.s_ext_entrsz = static_cast<std::uint16_t>(stride);
    m.instruments[0].ext = {2, 3, 0, 0xFF, 25, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 7, 7, 7, 7, 7, 7};
    Opened o(m);
    NEED(o.status.ok());
    InstrumentInfo i;
    NEED(o.rd.instrument(0, i).ok());
    CHECK(i.ext_bytes == (stride > 18 ? 18u : stride));
    CHECK(i.hold == (stride >= 1 ? 2 : 0));
    CHECK(i.decay == (stride >= 2 ? 3 : 0));
    CHECK(i.finetune == (stride >= 4 ? -1 : 0));
    CHECK(i.default_pitch == (stride >= 5 ? 25 : 0));
    CHECK(i.ext_flags == (stride >= 6 ? 1 : 0));
    CHECK(((i.notes & kNoteLongLoop) != 0) == (stride >= 18));
  }
}

TEST_CASE("med reader: errors have names and map onto the format errors") {
  CHECK(std::string(error_name(Error::BadPointer)) == "bad pointer");
  CHECK(std::string(error_name(Error::None)) == "none");
  CHECK(to_format_error(Error::None) == formats::FormatError::None);
  CHECK(to_format_error(Error::Truncated) == formats::FormatError::Truncated);
  CHECK(to_format_error(Error::BadMagic) == formats::FormatError::BadMagic);
  CHECK(to_format_error(Error::BadPointer) == formats::FormatError::Malformed);
  CHECK(to_format_error(Error::LimitExceeded) == formats::FormatError::LimitExceeded);
  CHECK(to_format_error(Error::Malformed) == formats::FormatError::Malformed);
  CHECK(to_format_error(Error::Unsupported) == formats::FormatError::Unsupported);
  CHECK(to_format_error(Error::SourceFailed) == formats::FormatError::SourceFailed);
}

TEST_CASE("med reader: a Reader that failed to open, or never opened, is blank and answers Malformed, never crashes") {
  Bytes junk(100, 0x55);
  SpanSource src(junk.data(), static_cast<std::uint32_t>(junk.size()));
  Reader r;
  Status s = r.open(src);
  CHECK(s.error == Error::BadMagic);
  CHECK(r.song().version == 0);
  CHECK(r.song().file_size == 0);
  CHECK(r.song().num_blocks == 0);
  BlockInfo b;
  InstrumentInfo i;
  PlayEntry e;
  Summary sum;
  Cell cells[4];
  CHECK(!r.block(0, b).ok());
  CHECK(!r.instrument(0, i).ok());
  CHECK(!r.play_entry(0, e).ok());
  CHECK(r.validate(sum).error == Error::Malformed);
  CHECK(!r.read_line(BlockInfo(), 0, 0, cells).ok());
  CHECK(r.track_volume(0) == 64);
  CHECK(r.track_pan(0) == 0);
  // a BlockInfo that block() did not make (too many tracks) is refused instead of overrunning the line buffer
  Opened o(test::mmd::sample_module(1));
  NEED(o.status.ok());
  NEED(o.rd.block(0, b).ok());
  b.tracks = 200;
  Cell big[256];
  CHECK(!o.rd.read_line(b, 0, 0, big).ok());
  // reopening a good Reader on a bad file blanks it
  SpanSource good(o.bytes.data(), static_cast<std::uint32_t>(o.bytes.size()));
  Reader again;
  NEED(again.open(good).ok());
  CHECK(again.song().num_blocks == 3);
  CHECK(again.open(src).error == Error::BadMagic);
  CHECK(again.song().num_blocks == 0);
}
