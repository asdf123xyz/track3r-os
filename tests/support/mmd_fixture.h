// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Synthetic OctaMED / MED files (MMD0 to MMD3) for the tests of formats/med (OS-008a), written from the layouts of
// Kinnunen's Rev 6 document (docs/formats/med.md section 2) and deliberately sharing no code with the reader under test
// (the independent Python writer tools/mmd_build.py is the second opinion, run by the med_python ctest). A small
// description (ModuleSpec) goes in, bytes and the offsets of what was written (Layout) come out, so a test can patch one
// field. No real module is ever committed: everything here is made up. Header-only so the QEMU suite can use it.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace o3os::test::mmd {

using Bytes = std::vector<std::uint8_t>;

struct CellSpec {
  std::uint8_t note = 0, inst = 0, cmd = 0, data = 0;
};
struct PageCell {
  std::uint8_t cmd = 0, data = 0;
};

struct BlockSpec {
  unsigned tracks = 4;
  unsigned lines = 8;                  // >= 1
  std::vector<CellSpec> cells;         // line-major, resized to tracks x lines when written
  std::vector<std::vector<PageCell>> pages;  // extra command pages (MMD1+), each tracks x lines
  bool share_pages = false;            // every page pointer names the first page's data (a small file with many pages)
  std::vector<std::uint8_t> cmd_ext;   // MMD1+: tracks x lines x (1 + pages) bytes, empty = no table
  std::string name;                    // MMD1+
  bool empty = false;                  // the block table holds 0 for this number
  CellSpec& at(unsigned line, unsigned track) {
    if (cells.size() < static_cast<std::size_t>(tracks) * lines) cells.resize(static_cast<std::size_t>(tracks) * lines);
    return cells[static_cast<std::size_t>(line) * tracks + track];
  }
  BlockSpec& set(unsigned line, unsigned track, unsigned note, unsigned inst, unsigned cmd, unsigned data) {
    CellSpec& c = at(line, track);
    c.note = static_cast<std::uint8_t>(note);
    c.inst = static_cast<std::uint8_t>(inst);
    c.cmd = static_cast<std::uint8_t>(cmd);
    c.data = static_cast<std::uint8_t>(data);
    return *this;
  }
  BlockSpec& page_cell(unsigned page, unsigned line, unsigned track, unsigned cmd, unsigned data) {
    while (pages.size() <= page) pages.emplace_back();
    std::vector<PageCell>& p = pages[page];
    if (p.size() < static_cast<std::size_t>(tracks) * lines) p.resize(static_cast<std::size_t>(tracks) * lines);
    p[static_cast<std::size_t>(line) * tracks + track] = {static_cast<std::uint8_t>(cmd), static_cast<std::uint8_t>(data)};
    return *this;
  }
};

struct SynthSpec {
  std::uint8_t defaultdecay = 0;
  std::uint16_t rep = 0, replen = 0;
  std::uint16_t voltbllen = 4, wftbllen = 4;
  std::uint8_t volspeed = 1, wfspeed = 1;
  std::uint16_t wforms = 0xFFFF;             // written as is; 0xFFFF = none
  std::uint8_t voltbl[128] = {};
  std::uint8_t wftbl[128] = {};
  std::vector<std::vector<std::uint8_t>> waves;  // waveform data (even sizes); wave 0 of a hybrid is a whole sample
  bool hybrid = false;
};

struct InstrSpec {
  bool present = true;
  int type = 0;                              // InstrHdr.type as stored (i16)
  std::vector<std::uint8_t> data;            // sample data: for stereo the left channel then the right
  long length = -1;                          // InstrHdr.length when it should differ from the data (-1: from the data)
  bool has_synth = false;
  SynthSpec synth;
  // The song record (MMD0sample)
  std::uint16_t rep = 0, replen = 0;
  std::uint8_t midich = 0, midipreset = 0, svol = 64;
  std::int8_t strans = 0;
  // InstrExt entry bytes (padded or cut to the module's stride) and the 40-byte name
  std::vector<std::uint8_t> ext;
  std::string name;
};

struct PlayCmdSpec {
  std::uint16_t pos = 0;
  std::uint8_t cmd = 0;                      // 1 stop, 2 jump
  std::vector<std::uint8_t> extra;           // a jump: the u16 target
};
struct PlaySeqSpec {
  std::string name;
  std::vector<std::uint16_t> entries;
  std::vector<PlayCmdSpec> cmds;
};

struct ModuleSpec {
  unsigned version = 1;                      // 0..3
  std::uint16_t deftempo = 125;
  std::int8_t playtransp = 0;
  std::uint8_t flags = 0, flags2 = 0, tempo2 = 6, mastervol = 64;
  std::uint8_t trkvol[16] = {64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64, 64};
  std::vector<BlockSpec> blocks;
  std::vector<InstrSpec> instruments;
  std::vector<std::uint8_t> playseq;         // MMD0/1: block numbers
  // MMD2 / MMD3
  std::vector<PlaySeqSpec> pseqs;
  std::vector<std::uint16_t> sections;       // each: a play sequence number
  std::vector<std::uint8_t> trackvols;
  std::vector<std::int8_t> trackpans;
  std::uint32_t flags3 = 0;
  std::uint16_t voladj = 0, channels = 0, echolen = 0;
  std::uint8_t echotype = 0, echodepth = 0;
  std::int8_t stereosep = 0;
  // expansion data
  bool exp = false;                          // written even when nothing needs it
  std::uint16_t s_ext_entrsz = 0;            // InstrExt stride, 0 = no InstrExt array
  std::uint16_t i_ext_entrsz = 0;            // instrument-name stride (>= 40 for names), 0 = no array
  std::string songname;
  std::string annotation;
  unsigned extra_songs = 0;                  // chained MCNx headers after the file
  unsigned extra_flags = 0;
};

// Where things went, for the tests that patch a field.
struct Layout {
  std::uint32_t song = 0, blockarr = 0, smplarr = 0, expdata = 0;
  std::uint32_t playseqtable = 0, sectiontable = 0, trackvols = 0, trackpans = 0;
  std::vector<std::uint32_t> block;          // block header offsets (0 = empty)
  std::vector<std::uint32_t> block_info;     // BlockInfo offsets (0 = none)
  std::vector<std::uint32_t> pagetable;      // BlockCmdPageTable offsets (0 = none)
  std::vector<std::uint32_t> instr;          // InstrHdr offsets (0 = none)
  std::vector<std::uint32_t> pseq;           // PlaySeq offsets
  std::uint32_t exp_smp = 0, iinfo = 0, songname = 0;
  std::vector<std::uint32_t> extra_song;     // chained header offsets
};

namespace detail {

struct W {
  Bytes b;
  std::size_t pos() const { return b.size(); }
  void align() { while (b.size() & 1) b.push_back(0); }
  void u8(unsigned v) { b.push_back(static_cast<std::uint8_t>(v)); }
  void u16(unsigned v) { u8(v >> 8); u8(v); }
  void u32(std::uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
  void zeros(std::size_t n) { b.insert(b.end(), n, 0); }
  void bytes(const Bytes& v) { b.insert(b.end(), v.begin(), v.end()); }
  void text(const std::string& s, std::size_t field) {
    for (std::size_t i = 0; i < field; ++i) u8(i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0);
  }
  void patch16(std::size_t at, unsigned v) {
    b[at] = static_cast<std::uint8_t>(v >> 8);
    b[at + 1] = static_cast<std::uint8_t>(v);
  }
  void patch32(std::size_t at, std::uint32_t v) {
    patch16(at, v >> 16);
    patch16(at + 2, v & 0xFFFF);
  }
};

}  // namespace detail

inline Bytes build(const ModuleSpec& m, Layout* layout = nullptr) {
  using detail::W;
  Layout lay;
  W w;
  const unsigned ver = m.version;
  const std::size_t nblocks = m.blocks.size();
  const std::size_t ninstr = m.instruments.size();

  w.zeros(52);  // header, filled last
  w.b[0] = 'M'; w.b[1] = 'M'; w.b[2] = 'D'; w.b[3] = static_cast<std::uint8_t>('0' + ver);
  lay.song = static_cast<std::uint32_t>(w.pos());
  w.zeros(788);

  // ---- MMD2+: play sequences, sections, track tables
  if (ver >= 2) {
    for (const PlaySeqSpec& ps : m.pseqs) {
      w.align();
      lay.pseq.push_back(static_cast<std::uint32_t>(w.pos()));
      const std::size_t at = w.pos();
      w.text(ps.name, 32);
      w.u32(0);  // commandTableOffset
      w.u32(0);
      w.u16(static_cast<unsigned>(ps.entries.size()));
      for (std::uint16_t e : ps.entries) w.u16(e);
      if (!ps.cmds.empty()) {
        w.align();
        w.patch32(at + 32, static_cast<std::uint32_t>(w.pos()));
        for (const PlayCmdSpec& c : ps.cmds) {
          w.u16(c.pos);
          w.u8(c.cmd);
          w.u8(static_cast<unsigned>(c.extra.size()));
          w.bytes(c.extra);
        }
        w.u16(0xFFFF);
        w.u8(0);
        w.u8(0);
      }
    }
    w.align();
    lay.playseqtable = static_cast<std::uint32_t>(w.pos());
    for (std::uint32_t p : lay.pseq) w.u32(p);
    lay.sectiontable = static_cast<std::uint32_t>(w.pos());
    for (std::uint16_t s : m.sections) w.u16(s);
    w.align();
    if (!m.trackvols.empty()) {
      lay.trackvols = static_cast<std::uint32_t>(w.pos());
      w.bytes(m.trackvols);
    }
    w.align();
    if (!m.trackpans.empty()) {
      lay.trackpans = static_cast<std::uint32_t>(w.pos());
      for (std::int8_t v : m.trackpans) w.u8(static_cast<std::uint8_t>(v));
    }
    w.align();
  }

  // ---- the two tables
  lay.blockarr = static_cast<std::uint32_t>(w.pos());
  w.zeros(4 * nblocks);
  lay.smplarr = static_cast<std::uint32_t>(w.pos());
  w.zeros(4 * ninstr);

  // ---- blocks
  for (std::size_t i = 0; i < nblocks; ++i) {
    const BlockSpec& b = m.blocks[i];
    lay.block.push_back(0);
    lay.block_info.push_back(0);
    lay.pagetable.push_back(0);
    if (b.empty) continue;
    w.align();
    const std::size_t at = w.pos();
    lay.block[i] = static_cast<std::uint32_t>(at);
    if (ver == 0) {
      w.u8(b.tracks);
      w.u8(b.lines - 1);
    } else {
      w.u16(b.tracks);
      w.u16(b.lines - 1);
      w.u32(0);  // info
    }
    for (unsigned l = 0; l < b.lines; ++l) {
      for (unsigned t = 0; t < b.tracks; ++t) {
        CellSpec c;
        const std::size_t idx = static_cast<std::size_t>(l) * b.tracks + t;
        if (idx < b.cells.size()) c = b.cells[idx];
        if (ver == 0) {  // xynnnnnn iiiicccc dddddddd
          w.u8((c.note & 0x3F) | ((c.inst & 0x10) ? 0x80 : 0) | ((c.inst & 0x20) ? 0x40 : 0));
          w.u8(((c.inst & 0x0F) << 4) | (c.cmd & 0x0F));
          w.u8(c.data);
        } else {  // xnnnnnnn xxiiiiii cccccccc dddddddd
          w.u8(c.note);  // the whole byte: 0x80 is the reserved bit the reader passes on
          w.u8(c.inst & 0x3F);
          w.u8(c.cmd);
          w.u8(c.data);
        }
      }
    }
    if (ver >= 1 && (!b.name.empty() || !b.pages.empty() || !b.cmd_ext.empty())) {
      w.align();
      const std::size_t info = w.pos();
      lay.block_info[i] = static_cast<std::uint32_t>(info);
      w.patch32(at + 4, static_cast<std::uint32_t>(info));
      w.zeros(36);
      if (!b.name.empty()) {
        const std::size_t name = w.pos();
        for (char ch : b.name) w.u8(static_cast<std::uint8_t>(ch));
        w.u8(0);
        w.patch32(info + 4, static_cast<std::uint32_t>(name));
        w.patch32(info + 8, static_cast<std::uint32_t>(b.name.size() + 1));
      }
      if (!b.pages.empty()) {
        w.align();
        const std::size_t pt = w.pos();
        lay.pagetable[i] = static_cast<std::uint32_t>(pt);
        w.patch32(info + 12, static_cast<std::uint32_t>(pt));
        w.u16(static_cast<unsigned>(b.pages.size()));
        w.u16(0);
        w.zeros(4 * b.pages.size());
        for (std::size_t p = 0; p < b.pages.size(); ++p) {
          if (b.share_pages && p > 0) {
            std::uint32_t first = 0;
            for (unsigned k = 0; k < 4; ++k) first = (first << 8) | w.b[pt + 4 + k];
            w.patch32(pt + 4 + 4 * p, first);
            continue;
          }
          w.align();
          w.patch32(pt + 4 + 4 * p, static_cast<std::uint32_t>(w.pos()));
          for (unsigned l = 0; l < b.lines; ++l) {
            for (unsigned t = 0; t < b.tracks; ++t) {
              PageCell c;
              const std::size_t idx = static_cast<std::size_t>(l) * b.tracks + t;
              if (idx < b.pages[p].size()) c = b.pages[p][idx];
              w.u8(c.cmd);
              w.u8(c.data);
            }
          }
        }
      }
      if (!b.cmd_ext.empty()) {
        w.align();
        const std::size_t ptr = w.pos();
        w.u32(0);
        w.patch32(info + 16, static_cast<std::uint32_t>(ptr));
        w.patch32(ptr, static_cast<std::uint32_t>(w.pos()));
        w.bytes(b.cmd_ext);
      }
    }
    w.patch32(lay.blockarr + 4 * i, static_cast<std::uint32_t>(at));
  }

  // ---- instruments
  for (std::size_t i = 0; i < ninstr; ++i) {
    const InstrSpec& s = m.instruments[i];
    lay.instr.push_back(0);
    if (!s.present) continue;
    w.align();
    const std::size_t at = w.pos();
    lay.instr[i] = static_cast<std::uint32_t>(at);
    w.patch32(lay.smplarr + 4 * i, static_cast<std::uint32_t>(at));
    if (s.has_synth) {
      const SynthSpec& y = s.synth;
      w.u32(534);
      w.u16(static_cast<unsigned>(y.hybrid ? -2 : -1));
      w.u8(y.defaultdecay);
      w.zeros(3);
      w.u16(y.rep);
      w.u16(y.replen);
      w.u16(y.voltbllen);
      w.u16(y.wftbllen);
      w.u8(y.volspeed);
      w.u8(y.wfspeed);
      w.u16(y.wforms);
      for (unsigned k = 0; k < 128; ++k) w.u8(y.voltbl[k]);
      for (unsigned k = 0; k < 128; ++k) w.u8(y.wftbl[k]);
      const std::size_t wf_at = w.pos();
      w.zeros(256);  // wf[64]
      for (std::size_t k = 0; k < y.waves.size() && k < 64; ++k) {
        w.align();
        w.patch32(wf_at + 4 * k, static_cast<std::uint32_t>(w.pos() - at));
        if (y.hybrid && k == 0) {
          w.u32(static_cast<std::uint32_t>(y.waves[0].size()));
          w.u16(0);
          w.bytes(y.waves[0]);
        } else {
          w.u16(static_cast<unsigned>(y.waves[k].size() / 2));
          w.bytes(y.waves[k]);
        }
      }
    } else {
      w.u32(s.length >= 0 ? static_cast<std::uint32_t>(s.length) : static_cast<std::uint32_t>(s.data.size() / ((s.type & 0x20) ? 2 : 1)));
      w.u16(static_cast<unsigned>(s.type));
      w.bytes(s.data);
    }
  }

  // ---- expansion data
  const bool have_ext = m.s_ext_entrsz != 0;
  const bool have_info = m.i_ext_entrsz != 0;
  const bool want_exp = m.exp || have_ext || have_info || !m.songname.empty() || !m.annotation.empty() || m.extra_songs != 0;
  if (want_exp) {
    w.align();
    lay.expdata = static_cast<std::uint32_t>(w.pos());
    const std::size_t e = w.pos();
    w.zeros(80);
    if (have_ext) {
      w.align();
      lay.exp_smp = static_cast<std::uint32_t>(w.pos());
      for (std::size_t i = 0; i < ninstr; ++i) {
        Bytes ent = m.instruments[i].ext;
        ent.resize(m.s_ext_entrsz, 0);
        w.bytes(ent);
      }
      w.patch32(e + 4, lay.exp_smp);
      w.patch16(e + 8, static_cast<unsigned>(ninstr));
      w.patch16(e + 10, m.s_ext_entrsz);
    }
    if (have_info) {
      w.align();
      lay.iinfo = static_cast<std::uint32_t>(w.pos());
      for (std::size_t i = 0; i < ninstr; ++i) w.text(m.instruments[i].name, m.i_ext_entrsz);
      w.patch32(e + 20, lay.iinfo);
      w.patch16(e + 24, static_cast<unsigned>(ninstr));
      w.patch16(e + 26, m.i_ext_entrsz);
    }
    if (!m.songname.empty()) {
      lay.songname = static_cast<std::uint32_t>(w.pos());
      for (char ch : m.songname) w.u8(static_cast<std::uint8_t>(ch));
      w.u8(0);
      w.patch32(e + 44, lay.songname);
      w.patch32(e + 48, static_cast<std::uint32_t>(m.songname.size() + 1));
    }
    if (!m.annotation.empty()) {
      const std::size_t a = w.pos();
      for (char ch : m.annotation) w.u8(static_cast<std::uint8_t>(ch));
      w.u8(0);
      w.patch32(e + 12, static_cast<std::uint32_t>(a));
      w.patch32(e + 16, static_cast<std::uint32_t>(m.annotation.size() + 1));
    }
    w.patch32(32, static_cast<std::uint32_t>(e));
    // chained songs: MCNx headers, each with an expansion record whose nextmod continues the chain
    std::size_t prev_exp = e;
    for (unsigned k = 0; k < m.extra_songs; ++k) {
      w.align();
      const std::size_t h = w.pos();
      lay.extra_song.push_back(static_cast<std::uint32_t>(h));
      w.zeros(52);
      w.b[h] = 'M'; w.b[h + 1] = 'C'; w.b[h + 2] = 'N'; w.b[h + 3] = '1';
      w.patch32(prev_exp, static_cast<std::uint32_t>(h));
      w.align();
      const std::size_t xe = w.pos();
      w.zeros(80);
      w.patch32(h + 32, static_cast<std::uint32_t>(xe));
      prev_exp = xe;
    }
  }

  // ---- the song record
  const std::size_t s = lay.song;
  for (std::size_t i = 0; i < 63 && i < ninstr; ++i) {
    const InstrSpec& is = m.instruments[i];
    w.patch16(s + 8 * i, is.rep);
    w.patch16(s + 8 * i + 2, is.replen);
    w.b[s + 8 * i + 4] = is.midich;
    w.b[s + 8 * i + 5] = is.midipreset;
    w.b[s + 8 * i + 6] = is.svol;
    w.b[s + 8 * i + 7] = static_cast<std::uint8_t>(is.strans);
  }
  w.patch16(s + 504, static_cast<unsigned>(nblocks));
  if (ver <= 1) {
    w.patch16(s + 506, static_cast<unsigned>(m.playseq.size()));
    for (std::size_t i = 0; i < m.playseq.size() && i < 256; ++i) w.b[s + 508 + i] = m.playseq[i];
    for (unsigned i = 0; i < 16; ++i) w.b[s + 770 + i] = m.trkvol[i];
  } else {
    w.patch16(s + 506, static_cast<unsigned>(m.sections.size()));
    w.patch32(s + 508, lay.playseqtable);
    w.patch32(s + 512, lay.sectiontable);
    w.patch32(s + 516, lay.trackvols);
    unsigned ntr = 0;
    for (const BlockSpec& b : m.blocks) ntr = b.tracks > ntr ? b.tracks : ntr;
    if (!m.trackvols.empty()) ntr = static_cast<unsigned>(m.trackvols.size());
    w.patch16(s + 520, ntr);
    w.patch16(s + 522, static_cast<unsigned>(m.pseqs.size()));
    w.patch32(s + 524, lay.trackpans);
    w.patch32(s + 528, m.flags3);
    w.patch16(s + 532, m.voladj);
    w.patch16(s + 534, m.channels);
    w.b[s + 536] = m.echotype;
    w.b[s + 537] = m.echodepth;
    w.patch16(s + 538, m.echolen);
    w.b[s + 540] = static_cast<std::uint8_t>(m.stereosep);
  }
  w.patch16(s + 764, m.deftempo);
  w.b[s + 766] = static_cast<std::uint8_t>(m.playtransp);
  w.b[s + 767] = m.flags;
  w.b[s + 768] = m.flags2;
  w.b[s + 769] = m.tempo2;
  w.b[s + 786] = m.mastervol;
  w.b[s + 787] = static_cast<std::uint8_t>(ninstr);

  // ---- the header
  w.patch32(4, static_cast<std::uint32_t>(w.pos()));
  w.patch32(8, lay.song);
  w.patch32(16, nblocks != 0 ? lay.blockarr : 0);
  w.patch32(24, ninstr != 0 ? lay.smplarr : 0);
  w.b[51] = static_cast<std::uint8_t>(m.extra_songs);
  w.patch16(48, 0xFFFF);
  if (layout != nullptr) *layout = lay;
  return w.b;
}

// ---- ready-made modules ----------------------------------------------------------------------------------------------

// A small module of every version with a bit of everything the reader reads: three blocks (one with a name, command pages
// and an extension table from MMD1), a play sequence that uses two of them, a sample with a loop, a 16-bit stereo sample,
// a three-octave sample, a synth and a hybrid instrument, names, a song name and (MMD2+) two sections.
inline ModuleSpec sample_module(unsigned version) {
  ModuleSpec m;
  m.version = version;
  m.deftempo = 33;
  m.tempo2 = 6;
  m.flags = 0x10;
  m.exp = true;
  m.s_ext_entrsz = 18;
  m.i_ext_entrsz = 40;
  m.songname = "Synthetic Song";
  m.annotation = "made up";

  BlockSpec a;
  a.tracks = 4;
  a.lines = 8;
  a.set(0, 0, 25, 1, 0x0C, 0x40).set(0, 1, 30, 2, 0, 0).set(2, 0, 0, 0, 0x0F, 0x00).set(4, 3, 40, 63, 0x04, 0x35);
  a.set(7, 2, 63, 31, 0x0F, 0xF3);
  BlockSpec b;
  b.tracks = 4;
  b.lines = 4;
  b.set(0, 0, 13, 5, 0x09, 0x03).set(3, 1, 20, 0, 0x0B, 0x00);
  BlockSpec c;
  c.tracks = 4;
  c.lines = 2;
  c.set(1, 2, 33, 3, 0x0C, 0x20);
  if (version >= 1) {
    c.set(0, 0, 0x80, 0, 0, 0).set(0, 1, 0x81, 0, 0, 0);  // a note byte of exactly 0x80 (OpenMPT: a cut) and one with note 1
    a.name = "intro";
    a.page_cell(0, 1, 0, 0x1D, 0x02).page_cell(0, 3, 2, 0x16, 0x00);
    a.cmd_ext.assign(static_cast<std::size_t>(4) * 8 * 2, 0);
    a.cmd_ext[5] = 0x7F;
    b.name = "second";
    b.set(1, 0, 100, 7, 0x1F, 0x23);  // note 100: only an MMD1+ cell holds it
  }
  m.blocks = {a, b, c};
  m.playseq = {0, 1, 0};

  InstrSpec s1;  // one-octave sample with a loop
  s1.type = 0;
  for (unsigned i = 0; i < 64; ++i) s1.data.push_back(static_cast<std::uint8_t>(i * 4));
  s1.rep = 8;
  s1.replen = 16;
  s1.svol = 48;
  s1.strans = -3;
  s1.name = "square";
  s1.ext = {2, 3, 0, 0xFF, 25, 1, 0, 0, 0, 0, 0, 0, 0, 16, 0, 0, 0, 32};  // hold, decay, -, finetune -1, pitch, loop flag, V7 long loop 16 / 32
  InstrSpec s2;  // 16-bit stereo
  s2.type = 0x30;
  for (unsigned i = 0; i < 32; ++i) {
    s2.data.push_back(static_cast<std::uint8_t>(i));
    s2.data.push_back(0);
  }
  s2.name = "stereo16";
  InstrSpec s3;  // three octaves: 1 + 2 + 4 = 7 base units of 5 bytes
  s3.type = 2;
  s3.data.assign(35, 0x11);
  s3.rep = 1;
  s3.replen = 2;
  s3.ext = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 4};  // loop flag, V7 long loop 2 / 4 (the same loop)
  s3.name = "iff3";
  InstrSpec s4;  // synth
  s4.has_synth = true;
  s4.synth.wforms = 2;
  s4.synth.waves = {Bytes(8, 0x10), Bytes(16, 0x20)};
  s4.name = "synth";
  InstrSpec s5;  // hybrid
  s5.has_synth = true;
  s5.synth.hybrid = true;
  s5.synth.wforms = 2;
  s5.synth.waves = {Bytes(12, 0x30), Bytes(8, 0x40)};
  s5.name = "hybrid";
  InstrSpec s6;  // a slot with no instrument
  s6.present = false;
  m.instruments = {s1, s2, s3, s4, s5, s6};

  if (version >= 2) {
    PlaySeqSpec p0, p1;
    p0.name = "main";
    p0.entries = {0, 1};
    p1.name = "coda";
    p1.entries = {2, 0x8000, 0};
    p1.cmds.push_back({1, 2, {0, 0}});
    m.pseqs = {p0, p1};
    m.sections = {0, 1};
    m.playseq.clear();
    m.trackvols = {64, 50, 32, 16};
    m.trackpans = {-16, 0, 0, 16};
    m.flags3 = 1;
    m.voladj = 150;
    m.channels = 4;
    m.echotype = 1;
    m.echodepth = 2;
    m.echolen = 300;
    m.stereosep = 2;
  }
  return m;
}

}  // namespace o3os::test::mmd
