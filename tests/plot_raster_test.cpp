// SPDX-License-Identifier: GPL-3.0-or-later
//
// What ui::plot asks of the simulator's rasteriser (sim/README.md, "Fidelity"): EDGE_STRIP_A / _B with their vertices on pixel
// centres fill exactly the rows from the vertex's row (the vector wave mode and the curve fills rest on it), a wide L4 bitmap
// (BITMAP_SIZE_H) has the alpha of its nibbles at every column, SAVE / RESTORE_CONTEXT bring back alpha, scissor, line width and
// point size (a raw section is one), and the strip primitives the toolkit does not use fail the frame instead of being skipped.
#include <doctest/doctest.h>

#include <o3os/eve/dl.h>
#include <o3os/sim/eve_raster.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace dl = o3os::eve::dl;
using o3os::eve::BitmapFormat;
using o3os::eve::Filter;
using o3os::eve::Primitive;
using o3os::eve::Wrap;
using o3os::sim::EveRaster;

namespace {

using Words = std::vector<std::uint32_t>;

std::uint32_t px(const EveRaster& r, int x, int y) {
  const std::uint8_t* p = r.rgba() + (static_cast<std::size_t>(y) * EveRaster::kWidth + static_cast<std::size_t>(x)) * 4;
  return (std::uint32_t{p[0]} << 16) | (std::uint32_t{p[1]} << 8) | p[2];
}

bool run(EveRaster& r, Words w) {
  w.push_back(dl::display());
  return r.render(w.data(), w.size());
}

std::uint32_t centre(int x, int y) { return dl::vertex2f(x * 16 + 8, y * 16 + 8); }

constexpr std::uint32_t kWhite = 0xFFFFFF, kBlack = 0;

}  // namespace

TEST_CASE("plot raster: an edge strip with its vertices on pixel centres fills exactly from the vertex's row, down (B) or up (A)") {
  EveRaster r;
  const int rows[] = {40, 12, 12, 77, 3, 100, 100, 55};  // one vertex a column, columns 200 .. 207
  for (const Primitive prim : {Primitive::EdgeStripB, Primitive::EdgeStripA}) {
    Words w = {dl::color_rgb(255, 255, 255), dl::line_width(8), dl::begin(prim)};
    for (int i = 0; i < 8; ++i) {
      w.push_back(centre(200 + i, rows[i]));
    }
    w.push_back(dl::end());
    CHECK(run(r, w));
    for (int i = 0; i < 8; ++i) {
      for (int y = 0; y < EveRaster::kHeight; y += 1) {
        const bool filled = prim == Primitive::EdgeStripB ? y >= rows[i] : y <= rows[i];
        CHECK_MESSAGE(px(r, 200 + i, y) == (filled ? kWhite : kBlack), "column " << i << " row " << y);
      }
    }
    CHECK(px(r, 199, 50) == kBlack);  // nothing outside the strip's columns
    CHECK(px(r, 208, 50) == kBlack);
  }
}

TEST_CASE("plot raster: a scissor clips a strip, a second strip over the first knocks out below its own vertices") {
  EveRaster r;
  // the wave's two fills: the wave colour from the maximum down, the background from just under the minimum down, both
  // clipped to the lane (x 100 .. 109, y 50 .. 99)
  const int top[] = {60, 55, 70, 50, 90, 60, 58, 52, 80, 75}, bot[] = {80, 95, 70, 99, 90, 75, 62, 70, 85, 99};
  Words w = {dl::color_rgb(0, 0, 0), dl::clear(true, false, false), dl::save_context(), dl::scissor_xy(100, 50), dl::scissor_size(10, 50),
             dl::line_width(8), dl::color_rgb(255, 255, 255), dl::begin(Primitive::EdgeStripB)};
  for (int i = 0; i < 10; ++i) {
    w.push_back(centre(100 + i, top[i]));
  }
  w.push_back(dl::end());
  w.push_back(dl::color_rgb(0, 0, 0));
  w.push_back(dl::begin(Primitive::EdgeStripB));
  for (int i = 0; i < 10; ++i) {
    w.push_back(centre(100 + i, bot[i] + 1));
  }
  w.push_back(dl::end());
  w.push_back(dl::restore_context());
  CHECK(run(r, w));
  for (int i = 0; i < 10; ++i) {
    for (int y = 0; y < EveRaster::kHeight; ++y) {
      const bool lit = y >= top[i] && y <= bot[i] && y >= 50 && y < 100;
      CHECK_MESSAGE(px(r, 100 + i, y) == (lit ? kWhite : kBlack), "column " << i << " row " << y);
    }
  }
  CHECK(px(r, 99, 70) == kBlack);   // the scissor's edges
  CHECK(px(r, 110, 70) == kBlack);
}

TEST_CASE("plot raster: a wide L4 bitmap (BITMAP_SIZE_H) has the alpha of every nibble, past column 511 too") {
  EveRaster r;
  // 800 x 2 at L4: stride 400; row 0 nibbles = column mod 16, row 1 = 15 - column mod 16
  std::vector<std::uint8_t> img(800);
  for (int c = 0; c < 800; ++c) {
    const unsigned n0 = static_cast<unsigned>(c) % 16u, n1 = 15u - n0;
    const std::uint8_t b0 = static_cast<std::uint8_t>(c % 2 == 0 ? n0 << 4 : n0), b1 = static_cast<std::uint8_t>(c % 2 == 0 ? n1 << 4 : n1);
    img[static_cast<std::size_t>(c / 2)] = static_cast<std::uint8_t>(img[static_cast<std::size_t>(c / 2)] | b0);
    img[400 + static_cast<std::size_t>(c / 2)] = static_cast<std::uint8_t>(img[400 + static_cast<std::size_t>(c / 2)] | b1);
  }
  CHECK(r.write_ram_g(0x20000, img.data(), 800));
  Words w = {dl::color_rgb(255, 255, 255), dl::bitmap_handle(9), dl::bitmap_source(0x20000),
             dl::bitmap_layout(BitmapFormat::L4, 400, 2), dl::bitmap_size(Filter::Nearest, Wrap::Border, Wrap::Border, 800, 2),
             dl::bitmap_size_h(800 >> 9, 0), dl::begin(Primitive::Bitmaps), dl::cell(0), dl::vertex2f(0, 16 * 10), dl::end()};
  CHECK(run(r, w));
  for (const int c : {0, 1, 2, 15, 16, 17, 255, 256, 511, 512, 513, 600, 777, 799}) {
    for (int row = 0; row < 2; ++row) {
      const unsigned nibble = row == 0 ? static_cast<unsigned>(c) % 16u : 15u - static_cast<unsigned>(c) % 16u;
      const unsigned alpha = nibble * 17u;
      const std::uint32_t want = (alpha * 255u + 127u) / 255u;  // white over black: the channel is the alpha
      const std::uint32_t got = px(r, c, 10 + row) & 0xFF;
      CHECK_MESSAGE(got == want, "column " << c << " row " << row << ": alpha " << alpha << " gave " << got);
    }
  }
  CHECK(px(r, 799, 12) == kBlack);  // past the two rows: transparent (BORDER)
  CHECK(px(r, 799, 9) == kBlack);
}

TEST_CASE("plot raster: SAVE / RESTORE_CONTEXT bring back alpha, scissor, line width, point size and the colour") {
  EveRaster r;
  Words w = {dl::line_width(8), dl::color_rgb(200, 100, 50), dl::point_size(40),
             dl::save_context(), dl::color_a(0x40), dl::scissor_xy(0, 0), dl::scissor_size(20, 20), dl::line_width(100),
             dl::point_size(160), dl::color_rgb(255, 255, 255), dl::restore_context(),
             // after the restore: opaque, unscissored, colour (200, 100, 50), width 8
             dl::begin(Primitive::Rects), centre(100, 100), centre(109, 109), dl::end(),
             dl::begin(Primitive::Points), centre(300, 300), dl::end()};
  CHECK(run(r, w));
  CHECK(px(r, 105, 105) == 0xC86432);                 // the colour and the alpha are back
  CHECK(px(r, 99, 105) == kBlack);                     // the width is 8 again: half a pixel, so no growth of a whole pixel
  CHECK(px(r, 110, 105) == kBlack);
  CHECK(px(r, 300, 300) == 0xC86432);                  // outside the section's 20 x 20 scissor: it is gone
  CHECK(px(r, 300 + 2, 300) != kBlack);                // POINT_SIZE 40: a disc of radius 2.5, not the section's 10
  CHECK(px(r, 300 + 5, 300) == kBlack);
}

TEST_CASE("plot raster: primitives the toolkit does not use fail the frame loudly") {
  EveRaster r;
  CHECK_FALSE(run(r, {dl::begin(static_cast<Primitive>(5))}));   // EDGE_STRIP_R
  CHECK(r.error().find("outside the subset") != std::string::npos);
  CHECK_FALSE(run(r, {dl::begin(static_cast<Primitive>(6))}));   // EDGE_STRIP_L
  CHECK_FALSE(run(r, {dl::begin(static_cast<Primitive>(10))}));  // an unknown primitive
  CHECK_FALSE(run(r, {dl::blend_func(o3os::eve::BlendFactor::DstAlpha, o3os::eve::BlendFactor::One)}));
  CHECK_FALSE(run(r, {centre(10, 10)}));                         // a vertex outside BEGIN / END
  // and a strip that was begun but never ended is flushed by DISPLAY, not lost
  Words w = {dl::color_rgb(255, 255, 255), dl::line_width(8), dl::begin(Primitive::EdgeStripB), centre(10, 20), centre(11, 20)};
  CHECK(run(r, w));
  CHECK(px(r, 10, 30) == kWhite);
}
