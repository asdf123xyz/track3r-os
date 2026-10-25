// SPDX-License-Identifier: GPL-3.0-or-later
//
// OS-E2E-5: the legibility sweep (sim/legibility.h) finds what it is for: a label cut by the screen edge or by a scissor, text drawn over
// text, and a colour pair that the theme's contrast table does not hold, and it passes clean text; the allow list names what it lets
// through; and recording the glyphs changes no pixel.
#include <doctest/doctest.h>

#include <o3os/eve/display_list_builder.h>
#include <o3os/eve/dl.h>
#include <o3os/sim/eve_raster.h>
#include <o3os/sim/gallery.h>
#include <o3os/sim/legibility.h>
#include <o3os/sim/raster_display.h>
#include <o3os/ui/assets.h>
#include <o3os/ui/renderer.h>
#include <o3os/ui/theme.h>

#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace o3os;
using sim::LegibilityKind;
using ui::Color;
using ui::Face;

namespace {

// One frame drawn into the simulator's rasteriser with the bitmap log on (or off), in one of the built-in themes.
struct Frame {
  std::unique_ptr<sim::EveRaster> raster = std::make_unique<sim::EveRaster>();
  sim::RasterDisplay display{*raster};
  const ui::Theme& theme;

  explicit Frame(unsigned theme_index = 0, bool record = true) : theme(ui::theme(ui::theme_id_at(theme_index))) {
    CHECK(sim::load_assets(display) == hal::UploadResult::Ok);
    if (record) {
      raster->record_glyphs(ui::kRamGEnd);
    }
  }

  template <typename Draw>
  void draw(Draw draw_content) {
    eve::DisplayListBuffer storage;
    eve::DisplayListBuilder builder(display, storage.words, eve::kMaxDisplayListWords);
    ui::Renderer r(builder, theme);
    CHECK(r.begin(Color::Pane));
    draw_content(r);
    CHECK(r.end());
    CHECK(display.render_failures() == 0u);
  }

  std::vector<sim::LegibilityFinding> check(const std::vector<sim::LegibilityAllow>& allow = {}) const {
    return sim::check_legibility(*raster, theme, "gallery/test_frame", allow);
  }
};

unsigned count(const std::vector<sim::LegibilityFinding>& findings, LegibilityKind kind) {
  unsigned n = 0;
  for (const sim::LegibilityFinding& f : findings) {
    n += f.kind == kind && f.allowed == nullptr ? 1u : 0u;
  }
  return n;
}

std::string all_text(const std::vector<sim::LegibilityFinding>& findings) {
  std::string out;
  for (const sim::LegibilityFinding& f : findings) {
    out += std::string(sim::legibility_kind_name(f.kind)) + ": " + f.what + "\n";
  }
  return out;
}

}  // namespace

TEST_CASE("legibility: clean text, tiles and an icon pass in every theme") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    Frame f(t);
    f.draw([](ui::Renderer& r) {
      r.text(20, 20, "PATTERN 01", Color::Text, Face::Data);          // a row of glyphs touching each other
      r.text(20, 60, "LABEL TEXT", Color::TextDim, Face::Label);
      r.panel({20, 100, 300, 24}, Color::Sel);
      r.text(24, 102, "SELECTED ROW", Color::SelText, Face::Data);
      r.panel({20, 140, 300, 24}, Color::Chrome);
      r.text(24, 142, "CHROME", Color::ChromeText, Face::Data);
      r.hex_tile(20, 200, 0x3F, Color::Instr);
      r.note_tile(60, 200, 14, Color::Text);
      r.icon(120, 200, ui::Icon::Play, Color::Text);
    });
    CHECK_FALSE(f.raster->glyph_log().empty());
    const auto findings = f.check();
    CHECK_MESSAGE(findings.empty(), ui::theme(ui::theme_id_at(t)).name << ":\n" << all_text(findings));
  }
}

TEST_CASE("legibility: a label cut by the screen edge fails") {
  Frame f;
  f.draw([](ui::Renderer& r) { r.text(776, 20, "CUT OFF", Color::Text, Face::Data); });  // 10 px glyphs: the third one is half out, the rest are out
  const auto findings = f.check();
  CHECK(count(findings, LegibilityKind::Clipped) >= 1u);
  CHECK(all_text(findings).find("the screen edge") != std::string::npos);
  CHECK(all_text(findings).find("10x20 'C'") == std::string::npos);  // the first, whole letters are not named
}

TEST_CASE("legibility: a label cut by a scissor fails, and says the scissor") {
  Frame f;
  f.draw([](ui::Renderer& r) {
    r.begin_raw();
    r.raw(eve::dl::scissor_xy(0, 0));
    r.raw(eve::dl::scissor_size(45, 480));  // five 10 px glyphs from x = 10 would reach 60
    r.text(10, 20, "ABCDE", Color::Text, Face::Data);
    r.end_raw();
  });
  const auto findings = f.check();
  CHECK(count(findings, LegibilityKind::Clipped) == 2u);  // the 4th is cut, the 5th is out: a label cut away whole is a finding too
  CHECK(all_text(findings).find("the scissor (0, 0, 45 x 480)") != std::string::npos);
}

TEST_CASE("legibility: text drawn over text fails; text beside text and a glyph redrawn elsewhere do not") {
  {
    Frame f;
    f.draw([](ui::Renderer& r) {
      r.text(100, 100, "HELLO", Color::Text, Face::Data);
      r.text(104, 100, "WORLD", Color::Text, Face::Data);  // four pixels to the right: ink on ink
    });
    const auto findings = f.check();
    CHECK(count(findings, LegibilityKind::Overlap) >= 1u);
    CHECK(all_text(findings).find("share") != std::string::npos);
  }
  {
    Frame f;  // the same glyph twice at one place is a double draw: it is ink on its own ink
    f.draw([](ui::Renderer& r) {
      r.text(100, 100, "H", Color::Text, Face::Data);
      r.text(100, 100, "H", Color::Text, Face::Data);
    });
    CHECK(count(f.check(), LegibilityKind::Overlap) == 1u);
  }
  {
    Frame f;
    f.draw([](ui::Renderer& r) {
      r.text(100, 100, "HELLO", Color::Text, Face::Data);
      r.text(150, 100, "WORLD", Color::Text, Face::Data);  // the next cell: touching, not overlapping
      r.text(100, 120, "AGAIN", Color::Text, Face::Data);  // the next line
    });
    CHECK(f.check().empty());
  }
}

TEST_CASE("legibility: a faux bold is the one overlap that is allowed, and the run says why") {
  Frame bold;
  bold.draw([](ui::Renderer& r) {
    r.text(100, 100, "M", Color::Text, Face::Label);
    r.text(101, 100, "M", Color::Text, Face::Label);  // the same glyph, a pixel to the right: the pattern header's bold mark
  });
  const auto findings = bold.check();
  CHECK(findings.size() == 1u);
  if (findings.size() == 1u) {
    CHECK(findings[0].kind == LegibilityKind::Overlap);
    CHECK(findings[0].allowed != nullptr);
    CHECK(std::string(findings[0].allowed != nullptr ? findings[0].allowed : "").find("faux bold") != std::string::npos);
  }

  Frame two_apart;  // two pixels is a collision, not a bold; so is another colour or another glyph
  two_apart.draw([](ui::Renderer& r) {
    r.text(100, 100, "M", Color::Text, Face::Label);
    r.text(102, 100, "M", Color::Text, Face::Label);
  });
  CHECK(count(two_apart.check(), LegibilityKind::Overlap) == 1u);
  Frame other_glyph;
  other_glyph.draw([](ui::Renderer& r) {
    r.text(100, 100, "M", Color::Text, Face::Label);
    r.text(101, 100, "N", Color::Text, Face::Label);
  });
  CHECK(count(other_glyph.check(), LegibilityKind::Overlap) == 1u);

  // The allowance is the Label face's M and S marks of the pattern header, nothing else that happens to be drawn twice a pixel apart.
  Frame solo(0);
  solo.draw([](ui::Renderer& r) {
    r.text(100, 100, "S", Color::Text, Face::Label);
    r.text(101, 100, "S", Color::Text, Face::Label);
  });
  CHECK(solo.check().size() == 1u);
  CHECK(count(solo.check(), LegibilityKind::Overlap) == 0u);
  Frame data_face(0);
  data_face.draw([](ui::Renderer& r) {
    r.text(100, 100, "M", Color::Text, Face::Data);
    r.text(101, 100, "M", Color::Text, Face::Data);
  });
  CHECK(count(data_face.check(), LegibilityKind::Overlap) == 1u);
  Frame other_label(0);
  other_label.draw([](ui::Renderer& r) {
    r.text(100, 100, "W", Color::Text, Face::Label);
    r.text(101, 100, "W", Color::Text, Face::Label);
  });
  CHECK(count(other_label.check(), LegibilityKind::Overlap) == 1u);
}

TEST_CASE("legibility: ink that a later panel covers is not on the picture, even when the panel has the ink's colour") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    Frame f(t);
    f.draw([](ui::Renderer& r) {
      r.text(100, 100, "EEEE", Color::ChromeText, Face::Data);       // the base screen's text, ...
      r.begin_raw();                                                  // ... a label cut by a scissor, ...
      r.raw(eve::dl::scissor_xy(0, 0));
      r.raw(eve::dl::scissor_size(45, 480));
      r.text(10, 140, "ABCD", Color::Text, Face::Data);
      r.end_raw();
      r.panel({20, 290, 100, 24}, Color::Accent);                    // ... and a pair outside the table (Rec on Accent), all under the dialog:
      r.text(24, 292, "REC", Color::Rec, Face::Data);
      r.panel({10, 90, 790, 240}, Color::ChromeText);                // a panel as dark as the first text (in the White theme ChromeText is black, as the data is)
      r.text(100, 100, "EEEE", Color::Chrome, Face::Data);           // the dialog's own text on its title bar: Chrome on ChromeText is a pair of the table
    });
    const auto findings = f.check();
    CHECK_MESSAGE(findings.empty(), ui::theme(ui::theme_id_at(t)).name << ":\n" << all_text(findings));
    CHECK(f.raster->glyph_log().size() >= 4u + 4u + 3u + 4u);  // the covered bitmaps were drawn and logged: they are left out of the findings, not of the log
  }
}

TEST_CASE("legibility: a bitmap that a later opaque panel hides in part is counted, not a finding; a translucent tint hides nothing") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    Frame part(t);
    part.draw([](ui::Renderer& r) {
      r.text(100, 100, "HHH", Color::Text, Face::Data);  // three 10 px glyphs from x = 100
      r.panel({125, 90, 200, 60}, Color::Chrome);        // the right bar of the third H lies under the panel, its left bar and the other two do not
    });
    const auto findings = part.check();
    CHECK_MESSAGE(findings.size() == 1u, ui::theme(ui::theme_id_at(t)).name << ":\n" << all_text(findings));
    if (findings.size() == 1u) {
      CHECK(findings[0].kind == LegibilityKind::Clipped);
      CHECK(std::string(findings[0].allowed != nullptr ? findings[0].allowed : "").find("covered in part") != std::string::npos);
      CHECK(findings[0].what.find("ink pixels under a later panel") != std::string::npos);
    }

    const auto rec_on_accent = [](ui::Renderer& r, bool tint) {
      r.panel({20, 20, 200, 24}, Color::Accent);
      r.text(24, 22, "REC", Color::Rec, Face::Data);  // a pair outside the table, in any theme
      if (tint) {
        r.begin_raw();
        r.raw(eve::dl::color_a(128));
      }
      r.panel({0, 0, 400, 100}, Color::Pane);
      if (tint) {
        r.end_raw();
      }
    };
    Frame covered(t);
    covered.draw([&rec_on_accent](ui::Renderer& r) { rec_on_accent(r, false); });
    CHECK(covered.check().empty());  // an opaque panel over it: the text is not on the picture
    Frame tinted(t);
    tinted.draw([&rec_on_accent](ui::Renderer& r) { rec_on_accent(r, true); });
    CHECK_MESSAGE(count(tinted.check(), LegibilityKind::Contrast) >= 1u, ui::theme(ui::theme_id_at(t)).name);  // half transparent: the text shows through
  }
}

TEST_CASE("legibility: a colour pair outside the contrast table fails; a pair of the table passes") {
  for (unsigned t = 0; t < ui::kThemeCount; ++t) {
    Frame bad(t);
    bad.draw([](ui::Renderer& r) {
      r.panel({20, 20, 200, 24}, Color::Accent);
      r.text(24, 22, "REC ON ACCENT", Color::Rec, Face::Data);  // two colours that no theme test has looked at together, in any theme
    });
    const auto findings = bad.check();
    CHECK_MESSAGE(count(findings, LegibilityKind::Contrast) >= 1u, ui::theme(ui::theme_id_at(t)).name);
    CHECK(all_text(findings).find("Rec on Accent is not a pair of the contrast table") != std::string::npos);

    Frame same(t);
    same.draw([](ui::Renderer& r) { r.text(24, 22, "INVISIBLE", Color::Pane, Face::Data); });  // ink the colour of the pane
    CHECK(count(same.check(), LegibilityKind::Contrast) >= 1u);

    Frame good(t);
    good.draw([](ui::Renderer& r) {
      r.panel({20, 20, 200, 24}, Color::Chrome);
      r.text(24, 22, "CHROME TEXT", Color::ChromeText, Face::Data);
    });
    CHECK(good.check().empty());
  }
}

TEST_CASE("legibility: the allow list lets a named finding through and says why; others stay findings") {
  Frame f;
  f.draw([](ui::Renderer& r) {
    r.text(100, 100, "HELLO", Color::Text, Face::Data);
    r.text(104, 100, "WORLD", Color::Text, Face::Data);
  });
  const std::vector<sim::LegibilityAllow> allow = {{"gallery/test_", LegibilityKind::Overlap, "10x20 'W'", "a test: W is allowed to touch"}};
  const auto findings = f.check(allow);
  unsigned allowed = 0;
  for (const sim::LegibilityFinding& x : findings) {
    allowed += x.allowed != nullptr ? 1u : 0u;
    if (x.allowed != nullptr) {
      CHECK(std::string(x.allowed) == "a test: W is allowed to touch");
      CHECK(x.what.find("10x20 'W'") != std::string::npos);
    }
  }
  CHECK(allowed >= 1u);
  CHECK(count(findings, LegibilityKind::Overlap) >= 1u);  // the pairs that do not name W are still findings
  // The wrong frame, or the wrong kind, allows nothing.
  const std::vector<sim::LegibilityAllow> other = {{"gallery/other_", LegibilityKind::Overlap, "", "x"}, {"gallery/test_", LegibilityKind::Clipped, "", "y"}};
  for (const sim::LegibilityFinding& x : f.check(other)) {
    CHECK(x.allowed == nullptr);
  }
  CHECK(sim::legibility_allow_list().empty());  // no excuse is listed: a finding is fixed by colour or layout (OS-BUG-LEGIB), and an entry would be the maintainer's call for a fix that looks worse
}

// OS-BUG-LEGIB: the screens whose findings were fixed by layout or by colour stay clean in every theme: the PARAMS strip of a module instrument with a long name (its page, P1, lay on the VOX
// readout), the progress box of an export of stems (its hint ran past the box), and the strip's mode word on its Accent chip (EDIT of Perform, REC of Record, STOPPED of a module app: the bare
// orange is 1.2:1 on the Amiga chrome), and the info line of Perform with no track on (OS-BUG-PERFINFO: it ran into the hint on its right).
// The rest of the gallery is the e2e sweep's (`tools/ci.sh e2e`).
TEST_CASE("legibility: the module instrument's strip, the stems' progress box and the strip's mode chips have no finding in any theme") {
  struct Sweep {
    const char* prefix;                    // the gallery screens drawn
    std::vector<const char*> frames;       // frames named in this sweep that must be among them, one per theme each
  };
  const Sweep sweeps[] = {{"modules", {"modules_named_", "modules_app_stopped_"}}, {"export", {"export_stems_progress_"}}, {"perform", {"perform_edit_", "perform_off_"}}, {"record", {"record_recording_"}}};
  for (const Sweep& sweep : sweeps) {
    sim::GalleryFilter filter;
    filter.prefix = sweep.prefix;
    sim::GalleryObserver observer;
    observer.record_glyphs_below = ui::kRamGEnd;
    std::vector<unsigned> seen(sweep.frames.size(), 0u);
    std::string open;
    observer.frame = [&](const sim::GalleryShot& shot, const sim::EveRaster& raster, const ui::Theme& theme) {
      for (std::size_t i = 0; i < sweep.frames.size(); ++i) {
        seen[i] += shot.key.find(sweep.frames[i]) != std::string::npos ? 1u : 0u;
      }
      for (const sim::LegibilityFinding& f : sim::check_legibility(raster, theme, shot.key)) {
        open += f.allowed == nullptr ? shot.key + " " + sim::legibility_kind_name(f.kind) + ": " + f.what + "\n" : "";
      }
    };
    std::vector<sim::GalleryShot> shots;
    std::string error;
    const bool drawn = sim::render_gallery(std::string(), shots, error, filter, &observer);
    CHECK_MESSAGE(drawn, error);
    if (!drawn) {
      return;  // (no exceptions: a failed REQUIRE is not available)
    }
    CHECK_MESSAGE(open.empty(), open);
    for (std::size_t i = 0; i < sweep.frames.size(); ++i) {
      CHECK_MESSAGE(seen[i] == ui::kThemeCount, sweep.frames[i]);  // (the frames that this checks were drawn: one per theme)
    }
  }
}

TEST_CASE("legibility: recording the glyphs changes no pixel and costs nothing when off") {
  const auto content = [](ui::Renderer& r) {
    r.text(20, 20, "PATTERN 01", Color::Text, Face::Data);
    r.text(776, 60, "CUT OFF", Color::Text, Face::Data);
    r.hex_tile(20, 100, 0xA5, Color::Instr);
  };
  Frame on(0, true), off(0, false);
  on.draw(content);
  off.draw(content);
  CHECK(std::memcmp(on.raster->rgba(), off.raster->rgba(), sim::EveRaster::kFramebufferBytes) == 0);
  CHECK_FALSE(on.raster->glyph_log().empty());
  CHECK(off.raster->glyph_log().empty());
  CHECK(off.raster->glyph_ink().empty());
  on.raster->record_glyphs(0);  // off again: the log is cleared and stays empty
  CHECK(on.raster->glyph_log().empty());
}
