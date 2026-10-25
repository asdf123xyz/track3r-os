// SPDX-License-Identifier: GPL-3.0-or-later
// Command-line helper for the MED reader (OS-008a), for the ctest `med_python` (driven by tools/mmd_build.py --check) and for
// looking at a module by hand. Not part of the OS.
//
//   o3med_tool dump FILE              print everything the reader reads as text: the same canonical dump that mmd_build.py
//                                     writes from its description of a synthetic module (exit 1 and the error on a refusal)
//   o3med_tool flow FILE [structural] print the linearisation (or the structural fallback) as patterns, order and report
//   o3med_tool survey FILE...         one line per file: version, sizes, summary, flow outcome; a refusal is a line, not a stop
//   o3med_tool import FILE [pal|ntsc] [refuse|structural|truncate] [engine]
//                                     import the module (OS-008b) and print the project it makes (name, tempo, tracks, order,
//                                     every non-empty step, every instrument) and the import report as text; exit 1 and the
//                                     error on a refusal. tests/med_openmpt_oracle.py compares this with libopenmpt
#include <o3os/formats/crc32.h>
#include <o3os/formats/med/flow.h>
#include <o3os/formats/med/import.h>
#include <o3os/formats/med/map.h>
#include <o3os/formats/med/mmd.h>
#include <o3os/formats/med/tempo.h>
#include <o3os/model/project.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

using namespace o3os::formats::med;

bool read_file(const char* path, std::vector<std::uint8_t>& out) {
  std::FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    return false;
  }
  std::uint8_t buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) {
    out.insert(out.end(), buf, buf + n);
  }
  std::fclose(f);
  return true;
}

void print_status(const char* what, const Status& s) {
  std::fprintf(stderr, "%s failed: %s (%s) at offset %u\n", what, error_name(s.error), s.detail, static_cast<unsigned>(s.offset));
}

const char* kind_name(InstKind k) {
  switch (k) {
    case InstKind::Empty: return "empty";
    case InstKind::Sample: return "sample";
    case InstKind::Synth: return "synth";
    case InstKind::Hybrid: return "hybrid";
    case InstKind::Unsupported: return "unsupported";
  }
  return "?";
}

const char* play_name(PlayKind k) {
  switch (k) {
    case PlayKind::Block: return "block";
    case PlayKind::Skip: return "skip";
    case PlayKind::Bad: return "bad";
    case PlayKind::Stop: return "stop";
    case PlayKind::Jump: return "jump";
  }
  return "?";
}

// The canonical dump. mmd_build.py writes the same text from its description of the module; every field here is a fact
// of the file that needs no interpretation (no loop clamping, no octave arithmetic beyond the type table).
int dump(const char* path) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, bytes)) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  Reader r;
  Status s = r.open(src);
  if (!s.ok()) {
    print_status("open", s);
    return 1;
  }
  Summary sum;
  s = r.validate(sum);
  if (!s.ok()) {
    print_status("validate", s);
    return 1;
  }
  const SongInfo& g = r.song();
  std::printf("mmd v=%u size=%u\n", static_cast<unsigned>(g.version), static_cast<unsigned>(g.file_size));
  std::printf("song blocks=%u len=%u samples=%u tempo=%u spd=%u flags=%u flags2=%u transpose=%d master=%u pseqs=%u tracks=%u entries=%u\n",
              g.num_blocks, g.song_len, g.num_samples, g.def_tempo, g.tempo2, g.flags, g.flags2, static_cast<int>(g.play_transpose),
              g.master_volume, g.num_pseqs, g.num_tracks, static_cast<unsigned>(g.play_entries));
  std::printf("name %s\n", g.name);
  for (std::uint32_t p = 0; p < g.play_entries; ++p) {
    PlayEntry e;
    s = r.play_entry(p, e);
    if (!s.ok()) {
      print_status("play_entry", s);
      return 1;
    }
    std::printf("play %u %s %u\n", static_cast<unsigned>(p), play_name(e.kind),
                static_cast<unsigned>(e.kind == PlayKind::Jump ? e.target : e.block));
  }
  for (std::uint32_t i = 0; i < g.num_blocks; ++i) {
    BlockInfo b;
    s = r.block(i, b);
    if (!s.ok()) {
      print_status("block", s);
      return 1;
    }
    if (b.empty) {
      std::printf("block %u empty\n", static_cast<unsigned>(i));
      continue;
    }
    char name[caps::kBlockNameMax + 1];
    s = r.block_name(b, name, sizeof name);
    if (!s.ok()) {
      print_status("block_name", s);
      return 1;
    }
    std::printf("block %u tracks=%u lines=%u pages=%u ext=%u name=%s\n", static_cast<unsigned>(i), b.tracks, b.lines, b.pages,
                b.cmd_ext != 0 ? 1u : 0u, name);
    Cell cells[caps::kMaxTracks];
    for (std::uint32_t page = 0; page <= b.pages; ++page) {
      for (std::uint32_t line = 0; line < b.lines; ++line) {
        s = r.read_line(b, line, page, cells);
        if (!s.ok()) {
          print_status("read_line", s);
          return 1;
        }
        for (std::uint32_t t = 0; t < b.tracks; ++t) {
          const Cell& c = cells[t];
          if ((c.note | c.instrument | c.command | c.data | c.ext) != 0 || c.note_hi) {
            std::printf("cell %u %u %u %u n=%u i=%u c=%u d=%u x=%u%s\n", static_cast<unsigned>(i), static_cast<unsigned>(page),
                        static_cast<unsigned>(line), static_cast<unsigned>(t), c.note, c.instrument, c.command, c.data, c.ext,
                        c.note_hi ? " h=1" : "");
          }
        }
      }
    }
  }
  for (std::uint32_t slot = 0; slot < g.num_samples; ++slot) {
    InstrumentInfo i;
    s = r.instrument(slot, i);
    if (!s.ok()) {
      print_status("instrument", s);
      return 1;
    }
    if (i.kind == InstKind::Empty) {
      std::printf("instr %u empty\n", static_cast<unsigned>(slot));
      continue;
    }
    std::printf("instr %u kind=%s type=%d bits=%u stereo=%u bytes=%u octaves=%u vol=%u trans=%d midich=%u midipre=%u name=%s "
                "hold=%u decay=%u fine=%d pitch=%u eflags=%u waves=%u\n",
                static_cast<unsigned>(slot), kind_name(i.kind), static_cast<int>(i.raw_type), i.sixteen_bit ? 16u : 8u,
                i.stereo ? 1u : 0u, i.data_bytes, i.octaves, i.volume, static_cast<int>(i.transpose), i.midi_channel, i.midi_preset,
                i.name, i.hold, i.decay, static_cast<int>(i.finetune), i.default_pitch, i.ext_flags, i.waveforms);
    for (std::uint32_t w = 0; w < i.waveforms; ++w) {
      std::uint32_t h, d, n;
      s = r.synth_waveform(i, w, h, d, n);
      if (!s.ok()) {
        print_status("synth_waveform", s);
        return 1;
      }
      std::printf("wave %u %u %u\n", static_cast<unsigned>(slot), static_cast<unsigned>(w), n);
    }
  }
  const std::uint32_t ntracks = g.version <= 1 ? 16u : g.num_tracks;
  for (std::uint32_t t = 0; t < ntracks; ++t) {
    std::printf("trackvol %u %u\n", static_cast<unsigned>(t), r.track_volume(t));
  }
  if (g.version >= 2) {
    for (std::uint32_t t = 0; t < ntracks; ++t) {
      std::printf("trackpan %u %d\n", static_cast<unsigned>(t), static_cast<int>(r.track_pan(t)));
    }
  }
  return 0;
}

LinearSong g_song;  // 4.5 KB

int flow(const char* path, bool structural_mode) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, bytes)) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  Reader r;
  Status s = r.open(src);
  if (!s.ok()) {
    print_status("open", s);
    return 1;
  }
  FlowOptions opt;
  s = structural_mode ? structural(r, opt, g_song) : linearise(r, opt, g_song);
  const FlowReport& f = g_song.report;
  const TempoModel model = tempo_model(r.song());
  const Bpm start = tempo_to_bpm(model, g_song.initial_tempo, g_song.initial_spd);
  std::printf("flow %s: start tempo %u speed %u = %u.%02u BPM%s\n", structural_mode ? "structural" : "linear",
              g_song.initial_tempo, g_song.initial_spd, start.x100 / 100, start.x100 % 100,
              start.clamp != Clamp::None ? " (clamped)" : "");
  if (!s.ok()) {
    print_status("flow", s);
  }
  for (unsigned i = 0; i < g_song.pattern_count; ++i) {
    const PatternDef& d = g_song.patterns[i];
    std::printf("pattern %u: block %u lines %u..%u%s", i, d.block, d.first, d.last, (d.flags & kPatReplay) != 0 ? " replay" : "");
    if (d.tempo_dep != 0) std::printf(" entry tempo %u speed %u", d.entry_tempo, d.entry_spd);
    std::printf("\n");
  }
  std::printf("order:");
  for (unsigned i = 0; i < g_song.order_count; ++i) std::printf(" %u", g_song.order[i]);
  std::printf("\n");
  std::printf("report: visits %u runs %u patterns %u orders %u loop %s (order %u, position %u line %u%s) ended %d truncated %d\n",
              f.visits, f.runs, g_song.pattern_count, g_song.order_count, f.loop_found ? "yes" : "no", f.loop_order, f.loop_pos,
              f.loop_line, f.loop_state_differs ? ", tempo or speed differ" : "", f.ended ? 1 : 0, f.truncated ? 1 : 0);
  std::printf("        breaks %u jumps %u loops %u repeats %u speed %u (ignored %u) tempo %u (clamped %u, suspect %u) skipped %u\n",
              f.breaks, f.jumps, f.loops, f.repeats, f.speed_sets, f.speed_ignored, f.tempo_sets, f.tempo_clamps, f.tempo_suspect,
              f.skipped_entries);
  if (structural_mode) {
    std::printf("        dropped: jumps %u loops %u repeats %u, break cells %u, end cells %u\n", f.dropped_jumps, f.dropped_loops,
                f.dropped_repeats, f.break_cells, f.end_cells);
  }
  return s.ok() ? 0 : 1;
}

int survey(int argc, char** argv) {
  for (int a = 2; a < argc; ++a) {
    std::vector<std::uint8_t> bytes;
    if (!read_file(argv[a], bytes)) {
      std::printf("%s: cannot read\n", argv[a]);
      continue;
    }
    SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
    Reader r;
    Status s = r.open(src);
    if (!s.ok()) {
      std::printf("%s: refused at open: %s (%s) offset %u\n", argv[a], error_name(s.error), s.detail, static_cast<unsigned>(s.offset));
      continue;
    }
    Summary sum;
    s = r.validate(sum);
    if (!s.ok()) {
      std::printf("%s: MMD%u refused at validate: %s (%s) offset %u\n", argv[a], r.song().version, error_name(s.error), s.detail,
                  static_cast<unsigned>(s.offset));
      continue;
    }
    FlowOptions opt;
    const Status f = linearise(r, opt, g_song);
    std::printf("%s: MMD%u blocks %u (max %u tracks, %u lines) instruments %u (synth %u, unsupported %llx, oversize %llx) ignored %x"
                " | flow %s patterns %u orders %u loop %d\n",
                argv[a], r.song().version, sum.blocks_referenced, sum.max_tracks, sum.max_lines, sum.instruments, sum.synth,
                static_cast<unsigned long long>(sum.unsupported_mask), static_cast<unsigned long long>(sum.oversize_mask),
                static_cast<unsigned>(sum.ignored), f.ok() ? "ok" : error_name(f.error), g_song.pattern_count, g_song.order_count,
                g_song.report.loop_found ? 1 : 0);
  }
  return 0;
}

const char* result_name(InstResult r) {
  switch (r) {
    case InstResult::None: return "none";
    case InstResult::Sample: return "sample";
    case InstResult::Midi: return "midi";
    case InstResult::EmptyData: return "empty";
    case InstResult::Unsupported: return "unsupported";
    case InstResult::Oversize: return "oversize";
    case InstResult::ArenaFull: return "arena-full";
    case InstResult::NoWave: return "no-wave";
  }
  return "?";
}

int import_module(int argc, char** argv) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(argv[2], bytes)) {
    std::fprintf(stderr, "cannot read %s\n", argv[2]);
    return 2;
  }
  ImportOptions opt;
  for (int a = 3; a < argc; ++a) {
    if (std::strcmp(argv[a], "ntsc") == 0) opt.clock = Clock::Ntsc;
    else if (std::strcmp(argv[a], "pal") == 0) opt.clock = Clock::Pal;
    else if (std::strcmp(argv[a], "structural") == 0) opt.overflow = Overflow::Structural;
    else if (std::strcmp(argv[a], "truncate") == 0) opt.overflow = Overflow::Truncate;
    else if (std::strcmp(argv[a], "refuse") == 0) opt.overflow = Overflow::Refuse;
    else if (std::strcmp(argv[a], "engine") == 0) opt.engine_fx = true;
    else {
      std::fprintf(stderr, "unknown option %s\n", argv[a]);
      return 2;
    }
  }
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  auto arena = std::make_unique<o3os::model::ModelArena>();
  auto importer = std::make_unique<Importer>();
  importer->begin(src, *arena, opt);
  while (importer->step(4096) == o3os::formats::Progress::Running) {
  }
  const ImportReport& r = importer->report();
  if (importer->progress() != o3os::formats::Progress::Done) {
    const Status& s = importer->status();
    std::printf("import failed: %s (%s) at offset %u overflow=%d\n", error_name(s.error), s.detail, static_cast<unsigned>(s.offset),
                r.overflow ? 1 : 0);
    return 1;
  }
  const o3os::model::Project& p = arena->project;
  std::printf("import ok version=%u clock=%s structural=%d\n", r.version, opt.clock == Clock::Pal ? "pal" : "ntsc", r.structural ? 1 : 0);
  std::printf("project name=%s tempo_x100=%u active=%u patterns=%u order=%u\n", p.name(), p.tempo_x100(),
              static_cast<unsigned>(p.active_tracks()), p.pattern_count(), p.song_length());
  for (unsigned t = 0; t < caps::kMaxTracks; ++t) {
    const TrackReport& tr = r.track[t];
    if (tr.cells == 0) continue;
    if (tr.slot == kNoSlot) {
      std::printf("track %u dropped cells=%u notes=%u\n", t, tr.cells, tr.notes);
    } else {
      const o3os::model::TrackSlot* ts = p.track(tr.slot);
      std::printf("track %u slot=%u vol=%u pan=%u cells=%u notes=%u\n", t, static_cast<unsigned>(tr.slot), ts->volume, ts->pan,
                  tr.cells, tr.notes);
    }
  }
  for (unsigned i = 0; i < p.song_length(); ++i) std::printf("order %u %u\n", i, static_cast<unsigned>(p.order(i)));
  const LinearSong& lin = importer->linear();
  for (unsigned i = 0; i < lin.pattern_count; ++i) {  // pattern i + 1 is lines first..last of block `block`
    std::printf("piece %u block=%u first=%u last=%u replay=%u\n", i + 1, lin.patterns[i].block, lin.patterns[i].first,
                lin.patterns[i].last, (lin.patterns[i].flags & kPatReplay) != 0 ? 1u : 0u);
  }
  for (unsigned id = 1; id <= o3os::model::kMaxPatternId; ++id) {
    const o3os::model::Pattern* pat = p.pattern(id);
    if (pat == nullptr) continue;
    std::printf("pattern %u len=%u name=%s\n", id, p.pattern_length(id), pat->name);
    for (unsigned t = 0; t < o3os::model::kMaxTracks; ++t) {
      for (unsigned l = 0; l < pat->track_length[t]; ++l) {
        const o3os::model::Step& st = pat->steps[t][l];
        if (o3os::model::is_empty(st)) continue;
        std::printf("step %u %u %u n=%u i=%u v=%u f1=%u:%u f2=%u:%u\n", id, t, l, st.note, st.instr, st.vol, st.fx[0].type, st.fx[0].value,
                    st.fx[1].type, st.fx[1].value);
      }
    }
  }
  for (unsigned i = 0; i < caps::kMaxSamples; ++i) {
    const InstReport& ir = r.instrument[i];
    if (ir.result == InstResult::None) continue;
    std::printf("instr %u result=%s flags=0x%x frames=%u tune=%d fine=%d", i, result_name(ir.result), ir.flags, ir.frames,
                static_cast<int>(ir.tune), static_cast<int>(ir.fine_tune));
    if (ir.result == InstResult::Sample) {  // the CRC-32 of the PCM as the WAV would hold it (16-bit little-endian, mono)
      o3os::formats::Crc32 crc;
      std::int16_t pcm[256];
      for (std::uint32_t at = 0; at < ir.frames; at += 256) {
        const std::uint32_t n = ir.frames - at < 256 ? ir.frames - at : 256;
        if (!importer->read_pcm(i, at, n, pcm).ok()) {
          std::printf(" pcm_crc=failed");
          break;
        }
        std::uint8_t le[512];
        for (std::uint32_t k = 0; k < n; ++k) {
          const std::uint16_t v = static_cast<std::uint16_t>(pcm[k]);
          le[2 * k] = static_cast<std::uint8_t>(v & 0xFF);
          le[2 * k + 1] = static_cast<std::uint8_t>(v >> 8);
        }
        crc.update(le, 2u * n);
      }
      std::printf(" pcm_crc=%08x", static_cast<unsigned>(crc.value()));
    }
    if (const o3os::model::Instrument* in = p.instrument(i)) {
      std::printf(" kind=%u name=%s path=%s rate=%u vol=%u mode=%u loop=%u-%u chan=%u", static_cast<unsigned>(in->kind), in->name,
                  in->sample_path, in->sample_rate, in->sampler.volume, in->sampler.play_mode, in->sampler.loop_start,
                  in->sampler.loop_end, in->midi_channel);
    }
    std::printf("\n");
  }
  std::printf("report ignored=0x%x master_ignored=%d tracks used=%u kept=%u dropped=%u cells=%u\n", r.ignored,
              r.master_volume_ignored ? 1 : 0, r.tracks_used, r.tracks_kept, r.tracks_dropped, r.cells);
  std::printf("report notes_out_of_range=%u note_cuts=%u note_hi_ignored=%u fx_overflow=%u volume_clamped=%u volume_overwritten=%u volume_baked=%u timing_carried=%u timing_lost=%u\n",
              r.notes_out_of_range, r.note_cuts, r.note_hi_ignored, r.fx_overflow, r.volume_clamped, r.volume_overwritten,
              r.volume_baked, r.timing_carried, r.timing_lost);
  std::printf("report tempo_fx=%u clamped=%u fine=%u speed_fx=%u state_unknown=%u instruments=%u refused=%u pcm_bytes=%llu\n",
              r.tempo_fx, r.tempo_fx_clamped, r.tempo_fx_fine, r.speed_fx, r.state_unknown, r.instruments, r.instruments_refused,
              static_cast<unsigned long long>(r.pcm_bytes));
  for (unsigned c = 0; c < 256; ++c) {
    if (r.cmd_dropped[c] != 0) std::printf("cmd dropped %02X %u\n", c, r.cmd_dropped[c]);
    if (r.cmd_approx[c] != 0) std::printf("cmd approx %02X %u\n", c, r.cmd_approx[c]);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "import") == 0) return import_module(argc, argv);
  if (argc >= 3 && std::strcmp(argv[1], "dump") == 0) return dump(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "flow") == 0) return flow(argv[2], argc >= 4 && std::strcmp(argv[3], "structural") == 0);
  if (argc >= 3 && std::strcmp(argv[1], "survey") == 0) return survey(argc, argv);
  std::fprintf(stderr, "usage: o3med_tool dump FILE | flow FILE [structural] | survey FILE... | import FILE [options]\n");
  return 2;
}
