// SPDX-License-Identifier: GPL-3.0-or-later
// Command-line helper for the MOD / S3M reader and importer (OS-MODIMP), for the ctest `mod_python` (driven by tools/mod_build.py --check), for tests/modimp_openmpt_oracle.py and for
// looking at a module by hand. Not part of the OS.
//
//   o3mod_tool dump FILE              print everything the reader reads as text: the canonical dump that mod_build.py writes from its description of a synthetic module
//                                     (exit 1 and the error on a refusal)
//   o3mod_tool import FILE [pal|ntsc] [refuse|structural|truncate] [engine|noengine]
//                                     import the module (OS-MODIMP) and print the project it makes (name, tempo, tracks, order, every non-empty step, every instrument) and
//                                     the import report as text, in the shape of `o3med_tool import` (tests/med_openmpt_oracle.py reads both); exit 1 and the error on a
//                                     refusal. tests/modimp_openmpt_oracle.py compares this with libopenmpt
#include <o3os/formats/crc32.h>
#include <o3os/formats/mod/import.h>
#include <o3os/formats/mod/map.h>
#include <o3os/formats/mod/module.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

namespace {

using namespace o3os::formats::mod;

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
  std::fprintf(stderr, "%s failed: %s (%s) at offset %u\n", what, o3os::formats::med::error_name(s.error), s.detail, static_cast<unsigned>(s.offset));
}

int dump(const char* path) {
  std::vector<std::uint8_t> bytes;
  if (!read_file(path, bytes)) {
    std::fprintf(stderr, "cannot read %s\n", path);
    return 2;
  }
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  auto m = std::make_unique<Module>();
  Status s = m->open(src);
  if (!s.ok()) {
    print_status("open", s);
    return 1;
  }
  const ModuleInfo& g = m->info();
  const bool is_mod = g.kind == Kind::Mod;
  if (is_mod) {
    std::printf("mod tag=%s channels=%u orders=%u patterns=%u restart=%u size=%u\n", g.tag, g.channels, g.order_count, g.pattern_count, g.restart,
                static_cast<unsigned>(g.file_size));
  } else {
    std::printf("s3m size=%u channels=%u orders=%u patterns=%u instruments=%u speed=%u tempo=%u gv=%u mv=%u stereo=%d signed=%d fast=%d skipped=%u bad=%u\n",
                static_cast<unsigned>(g.file_size), g.channels, g.order_count, g.pattern_count, g.sample_count, g.speed, g.tempo, g.global_volume,
                g.master_volume, g.stereo ? 1 : 0, g.signed_samples ? 1 : 0, g.fast_slides ? 1 : 0, g.skipped_orders, g.bad_orders);
  }
  std::printf("name %s\n", g.name);
  if (!is_mod) {
    for (unsigned c = 0; c < caps::kMaxChannels; ++c) {
      if (g.chan_used[c]) {
        std::printf("chan %u pan=%u muted=%d adlib=%d\n", c, g.chan_pan[c], g.chan_muted[c] ? 1 : 0, g.chan_adlib[c] ? 1 : 0);
      }
    }
  }
  for (unsigned i = 0; i < g.order_count; ++i) {
    std::printf("order %u %u\n", i, m->order(i));
  }
  for (unsigned i = 0; i < g.sample_count; ++i) {
    SampleInfo si;
    s = m->sample(i, si);
    if (!s.ok()) {
      print_status("sample", s);
      return 1;
    }
    if (is_mod) {
      std::printf("sample %u frames=%u vol=%u fine=%d loop=%u+%u notes=%u name=%s\n", i, si.frames, si.volume, static_cast<int>(si.finetune),
                  si.has_loop ? si.loop_start : 0u, si.has_loop ? si.loop_len : 0u, si.notes, si.name);
    } else {
      const char* kind = si.kind == SampleKind::Pcm ? "pcm" : si.kind == SampleKind::AdLib ? "adlib" : si.kind == SampleKind::Unsupported ? "packed" : "empty";
      if (si.kind == SampleKind::Pcm || (si.kind == SampleKind::Empty && si.s3m_type == 1)) {
        std::printf("inst %u kind=%s frames=%u bits=%u vol=%u c2spd=%u loop=%u+%u notes=%u name=%s\n", i, kind, si.frames, si.sixteen_bit ? 16u : 8u, si.volume,
                    si.c2spd, si.has_loop ? si.loop_start : 0u, si.has_loop ? si.loop_len : 0u, si.notes, si.name);
      } else if (si.kind == SampleKind::Unsupported) {
        std::printf("inst %u kind=packed frames=0 bits=8 vol=%u c2spd=%u loop=0+0 notes=0 name=%s\n", i, si.volume, si.c2spd, si.name);
      } else {
        std::printf("inst %u kind=%s frames=0 bits=8 vol=0 c2spd=0 loop=0+0 notes=0 name=%s\n", i, kind, si.name);
      }
    }
  }
  std::vector<Cell> cells(caps::kMaxChannels);
  for (unsigned p = 0; p < g.pattern_count; ++p) {
    for (unsigned row = 0; row < caps::kRows; ++row) {
      s = m->read_line(p, row, cells.data());
      if (!s.ok()) {
        print_status("read_line", s);
        return 1;
      }
      for (unsigned c = 0; c < g.channels; ++c) {
        const Cell& x = cells[c];
        if (is_mod) {
          if (x.period != 0 || x.instrument != 0 || x.effect != 0 || x.param != 0) {
            std::printf("cell %u %u %u period=%u note=%u inst=%u fx=%u param=%u off=%d\n", p, row, c, x.period, x.note, x.instrument, x.effect, x.param,
                        x.off_table ? 1 : 0);
          }
        } else if (x.note != 0 || x.cut || x.bad_note || x.instrument != 0 || x.volume != 255 || x.effect != 0 || x.param != 0) {
          std::printf("cell %u %u %u note=%u cut=%d inst=%u vol=%u fx=%u param=%u bad=%d\n", p, row, c, x.note, x.cut ? 1 : 0, x.instrument, x.volume,
                      x.effect, x.param, x.bad_note ? 1 : 0);
        }
      }
    }
  }
  return 0;
}

const char* result_name(o3os::formats::med::InstResult r) {
  using R = o3os::formats::med::InstResult;
  switch (r) {
    case R::None: return "none";
    case R::Sample: return "sample";
    case R::Midi: return "midi";
    case R::EmptyData: return "empty";
    case R::Unsupported: return "unsupported";
    case R::Oversize: return "oversize";
    case R::ArenaFull: return "arena-full";
    case R::NoWave: return "no-wave";
  }
  return "?";
}

int import_module(int argc, char** argv) {
  using o3os::formats::Progress;
  std::vector<std::uint8_t> bytes;
  if (!read_file(argv[2], bytes)) {
    std::fprintf(stderr, "cannot read %s\n", argv[2]);
    return 2;
  }
  ImportOptions opt;
  for (int a = 3; a < argc; ++a) {
    if (std::strcmp(argv[a], "ntsc") == 0) opt.clock = o3os::formats::med::Clock::Ntsc;
    else if (std::strcmp(argv[a], "pal") == 0) opt.clock = o3os::formats::med::Clock::Pal;
    else if (std::strcmp(argv[a], "structural") == 0) opt.overflow = o3os::formats::med::Overflow::Structural;
    else if (std::strcmp(argv[a], "truncate") == 0) opt.overflow = o3os::formats::med::Overflow::Truncate;
    else if (std::strcmp(argv[a], "refuse") == 0) opt.overflow = o3os::formats::med::Overflow::Refuse;
    else if (std::strcmp(argv[a], "engine") == 0) opt.engine_fx = EngineFx::On;
    else if (std::strcmp(argv[a], "noengine") == 0) opt.engine_fx = EngineFx::Off;
    else {
      std::fprintf(stderr, "unknown option %s\n", argv[a]);
      return 2;
    }
  }
  SpanSource src(bytes.data(), static_cast<std::uint32_t>(bytes.size()));
  auto arena = std::make_unique<o3os::model::ModelArena>();
  auto importer = std::make_unique<Importer>();
  importer->begin(src, *arena, opt);
  while (importer->step(4096) == Progress::Running) {
  }
  const ImportReport& rep = importer->report();
  const o3os::formats::med::ImportReport& r = rep.med;
  if (importer->progress() != Progress::Done) {
    const Status& s = importer->status();
    std::printf("import failed: %s (%s) at offset %u overflow=%d\n", o3os::formats::med::error_name(s.error), s.detail, static_cast<unsigned>(s.offset), rep.overflow ? 1 : 0);
    return 1;
  }
  const o3os::model::Project& p = arena->project;
  std::printf("import ok kind=%s tag=%s channels=%u engine=%d structural=%d\n", rep.kind == Kind::Mod ? "mod" : "s3m", importer->module().info().tag, rep.channels,
              rep.engine_fx ? 1 : 0, rep.structural ? 1 : 0);
  std::printf("project name=%s tempo_x100=%u active=%u patterns=%u order=%u\n", p.name(), p.tempo_x100(), static_cast<unsigned>(p.active_tracks()), p.pattern_count(),
              p.song_length());
  for (unsigned t = 0; t < caps::kMaxChannels; ++t) {
    const o3os::formats::med::TrackReport& tr = r.track[t];
    if (tr.cells == 0) continue;
    if (tr.slot == o3os::formats::med::kNoSlot) {
      std::printf("track %u dropped cells=%u notes=%u\n", t, tr.cells, tr.notes);
    } else {
      const o3os::model::TrackSlot* ts = p.track(tr.slot);
      std::printf("track %u slot=%u vol=%u pan=%u flags=%u cells=%u notes=%u\n", t, static_cast<unsigned>(tr.slot), ts->volume, ts->pan, ts->flags, tr.cells, tr.notes);
    }
  }
  for (unsigned i = 0; i < p.song_length(); ++i) std::printf("order %u %u\n", i, static_cast<unsigned>(p.order(i)));
  const o3os::formats::med::LinearSong& lin = importer->linear();
  for (unsigned i = 0; i < lin.pattern_count; ++i) {  // pattern i + 1 is rows first..last of source pattern `block`
    std::printf("piece %u block=%u first=%u last=%u replay=%u\n", i + 1, lin.patterns[i].block, lin.patterns[i].first, lin.patterns[i].last,
                (lin.patterns[i].flags & o3os::formats::med::kPatReplay) != 0 ? 1u : 0u);
  }
  for (unsigned id = 1; id <= o3os::model::kMaxPatternId; ++id) {
    const o3os::model::Pattern* pat = p.pattern(id);
    if (pat == nullptr) continue;
    std::printf("pattern %u len=%u name=%s\n", id, p.pattern_length(id), pat->name);
    for (unsigned t = 0; t < o3os::model::kMaxTracks; ++t) {
      for (unsigned l = 0; l < pat->track_length[t]; ++l) {
        const o3os::model::Step& st = pat->steps[t][l];
        if (o3os::model::is_empty(st)) continue;
        std::printf("step %u %u %u n=%u i=%u v=%u f1=%u:%u f2=%u:%u\n", id, t, l, st.note, st.instr, st.vol, st.fx[0].type, st.fx[0].value, st.fx[1].type, st.fx[1].value);
      }
    }
  }
  for (unsigned i = 0; i < caps::kSlots; ++i) {
    const o3os::formats::med::InstReport& ir = r.instrument[i];
    if (ir.result == o3os::formats::med::InstResult::None) continue;
    std::printf("instr %u result=%s flags=0x%x frames=%u tune=%d fine=%d", i, result_name(ir.result), ir.flags, ir.frames, static_cast<int>(ir.tune), static_cast<int>(ir.fine_tune));
    if (ir.result == o3os::formats::med::InstResult::Sample) {  // the CRC-32 of the PCM as the WAV would hold it (16-bit little-endian, mono)
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
      std::printf(" pcm_crc=%08x pcm_rate=%u", static_cast<unsigned>(crc.value()), importer->pcm_rate(i));
    }
    if (const o3os::model::Instrument* in = p.instrument(i)) {
      std::printf(" kind=%u name=%s path=%s rate=%u vol=%u mode=%u loop=%u-%u chan=%u engine=%u", static_cast<unsigned>(in->kind), in->name, in->sample_path, in->sample_rate,
                  in->sampler.volume, in->sampler.play_mode, in->sampler.loop_start, in->sampler.loop_end, in->midi_channel, static_cast<unsigned>(in->engine));
    }
    std::printf("\n");
  }
  std::printf("report tracks used=%u kept=%u dropped=%u cells=%u\n", r.tracks_used, r.tracks_kept, r.tracks_dropped, r.cells);
  std::printf("report notes_out_of_range=%u fx_overflow=%u volume_clamped=%u volume_overwritten=%u volume_baked=%u timing_carried=%u timing_lost=%u\n", r.notes_out_of_range,
              r.fx_overflow, r.volume_clamped, r.volume_overwritten, r.volume_baked, r.timing_carried, r.timing_lost);
  std::printf("report tempo_fx=%u clamped=%u fine=%u speed_fx=%u state_unknown=%u instruments=%u refused=%u pcm_bytes=%llu\n", r.tempo_fx, r.tempo_fx_clamped, r.tempo_fx_fine,
              r.speed_fx, r.state_unknown, r.instruments, r.instruments_refused, static_cast<unsigned long long>(r.pcm_bytes));
  std::printf("report flow visits=%u runs=%u loop=%d loop_order=%u ended=%d truncated=%d tempo_clamps=%u speed_sets=%u tempo_sets=%u jumps=%u breaks=%u loops=%u repeats=%u\n",
              r.flow.visits, r.flow.runs, r.flow.loop_found ? 1 : 0, r.flow.loop_order, r.flow.ended ? 1 : 0, r.flow.truncated ? 1 : 0, r.flow.tempo_clamps, r.flow.speed_sets,
              r.flow.tempo_sets, r.flow.jumps, r.flow.breaks, r.flow.loops, r.flow.repeats);
  std::printf("report module periods_off_table=%u bad_notes=%u instrument_refs_dropped=%u volume_invalid=%u adlib_cells=%u unused_cells=%u tick0_slide_cells=%u instruments_beyond=%u "
              "samples_trimmed=%u loops_clamped=%u rates_clamped=%u rates_defaulted=%u stereo=%u adlib=%u packed=%u\n",
              rep.periods_off_table, rep.bad_notes, rep.instrument_refs_dropped, rep.volume_invalid, rep.adlib_cells, rep.unused_cells, rep.tick0_slide_cells, rep.instruments_beyond,
              rep.samples_trimmed, rep.loops_clamped, rep.rates_clamped, rep.rates_defaulted, rep.stereo_samples, rep.adlib_instruments, rep.packed_samples);
  for (unsigned id = 0; id < kFxIds; ++id) {
    const FxTally& f = rep.fx[id];
    if (f.seen == 0) continue;
    std::printf("fx %u seen=%u approx=%u dropped=%u class=%d name=%s\n", id, f.seen, f.approx, f.dropped, static_cast<int>(fx_class(id)), fx_name(id));
  }
  for (unsigned c = 0; c < 256; ++c) {
    if (r.cmd_dropped[c] != 0) std::printf("cmd dropped %02X %u\n", c, r.cmd_dropped[c]);
    if (r.cmd_approx[c] != 0) std::printf("cmd approx %02X %u\n", c, r.cmd_approx[c]);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 3 && std::strcmp(argv[1], "dump") == 0) return dump(argv[2]);
  if (argc >= 3 && std::strcmp(argv[1], "import") == 0) return import_module(argc, argv);
  std::fprintf(stderr, "usage: o3mod_tool dump FILE | import FILE [options]\n");
  return 2;
}
