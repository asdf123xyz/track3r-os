// SPDX-License-Identifier: GPL-3.0-or-later
#include <o3os/module/view.h>

#include <cstring>

namespace o3os::module {

namespace {

constexpr std::int32_t kCoordLimit = 32000;  // coordinates are held as int16: a module's wilder numbers are pinned here (they are off the canvas anyway)

std::int32_t pin(std::int32_t v) noexcept { return v < -kCoordLimit ? -kCoordLimit : (v > kCoordLimit ? kCoordLimit : v); }

bool finite_less(float lo, float hi) noexcept { return lo < hi && lo > -1.0e30f && hi < 1.0e30f; }  // false for NaN, infinity and an empty range

}  // namespace

bool clip_rect(int& x, int& y, int& w, int& h, int cw, int ch) noexcept {
  if (w <= 0 || h <= 0 || cw <= 0 || ch <= 0) {
    return false;
  }
  std::int64_t x0 = x, y0 = y;
  std::int64_t x1 = static_cast<std::int64_t>(x) + w, y1 = static_cast<std::int64_t>(y) + h;
  x0 = x0 < 0 ? 0 : x0;
  y0 = y0 < 0 ? 0 : y0;
  x1 = x1 > cw ? cw : x1;
  y1 = y1 > ch ? ch : y1;
  if (x0 >= x1 || y0 >= y1) {
    return false;
  }
  x = static_cast<int>(x0);
  y = static_cast<int>(y0);
  w = static_cast<int>(x1 - x0);
  h = static_cast<int>(y1 - y0);
  return true;
}

std::uint32_t word_cost_rect() noexcept { return 4; }
std::uint32_t word_cost_outline() noexcept { return 12; }
std::uint32_t word_cost_text(std::uint32_t chars) noexcept { return chars + 2; }
std::uint32_t word_cost_poly(std::uint32_t points) noexcept { return points + 10; }  // a raw section (save, scissor, colour, width, begin, end, restore) and a vertex a point
std::uint32_t word_cost_curve(std::uint32_t points, std::uint32_t flags) noexcept {
  const std::uint32_t vertices = points < kViewMaxPolyPoints ? points : kViewMaxPolyPoints;
  return 2u * vertices + 16u + ((flags & O3M_CURVE_AXES) != 0u ? 200u : 0u);
}

// The draw API's functions live here as a friend of ViewHost so that they can record into it.
struct ViewApi {
  static ViewHost* host(const o3m_canvas* c) noexcept {
    if (c == nullptr) {
      return nullptr;
    }
    const ViewHost::Canvas* cv = reinterpret_cast<const ViewHost::Canvas*>(c);  // o3m_canvas is the first member of the one Canvas a ViewHost makes
    return cv->magic == ViewHost::kMagic && cv->host != nullptr && &cv->host->canvas_ == cv ? cv->host : nullptr;
  }

  static ViewHost::Cmd* push(ViewHost& h, ViewHost::Op op, std::uint8_t color) noexcept {
    ViewHost::Cmd* c = &h.cmd_[h.cmd_count_++];  // charge() bounded the calls to kCmdMax
    std::memset(c, 0, sizeof *c);
    c->op = op;
    c->color = color;
    return c;
  }

  static void rect(o3m_canvas* cv, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t hh, std::uint32_t color) noexcept {
    ViewHost* h = host(cv);
    if (h == nullptr || !h->charge(word_cost_rect()) || color >= O3M_COLOR_COUNT) {
      return;
    }
    int ix = static_cast<int>(pin(x)), iy = static_cast<int>(pin(y)), iw = static_cast<int>(pin(w)), ih = static_cast<int>(pin(hh));
    if (!clip_rect(ix, iy, iw, ih, cv->width, cv->height)) {
      return;
    }
    ViewHost::Cmd* c = push(*h, ViewHost::Op::Rect, static_cast<std::uint8_t>(color));
    c->x = static_cast<std::int16_t>(ix);
    c->y = static_cast<std::int16_t>(iy);
    c->w = static_cast<std::int16_t>(iw);
    c->h = static_cast<std::int16_t>(ih);
  }

  static void outline(o3m_canvas* cv, std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t hh, std::uint32_t color) noexcept {
    ViewHost* h = host(cv);
    if (h == nullptr || !h->charge(word_cost_outline()) || color >= O3M_COLOR_COUNT) {
      return;
    }
    int ix = static_cast<int>(pin(x)), iy = static_cast<int>(pin(y)), iw = static_cast<int>(pin(w)), ih = static_cast<int>(pin(hh));
    int tx = ix, ty = iy, tw = iw, th = ih;
    if (!clip_rect(tx, ty, tw, th, cv->width, cv->height)) {
      return;  // wholly outside: no edge of it can be inside
    }
    ViewHost::Cmd* c = push(*h, ViewHost::Op::Outline, static_cast<std::uint8_t>(color));
    c->x = static_cast<std::int16_t>(ix);  // the edges are cut one by one at replay, from the rectangle as given
    c->y = static_cast<std::int16_t>(iy);
    c->w = static_cast<std::int16_t>(iw);
    c->h = static_cast<std::int16_t>(ih);
  }

  static void text(o3m_canvas* cv, std::int32_t x, std::int32_t y, const char* s, std::uint32_t face, std::uint32_t color) noexcept {
    ViewHost* h = host(cv);
    if (h == nullptr) {
      return;
    }
    std::uint32_t len = 0;
    while (s != nullptr && len < kViewMaxTextCall && s[len] != '\0') {
      ++len;
    }
    if (!h->charge(word_cost_text(len))) {
      return;
    }
    if (s == nullptr || len == 0u || face >= O3M_FACE_COUNT || color >= O3M_COLOR_COUNT) {
      return;
    }
    h->chars_ += len;
    if (h->chars_ > kViewMaxText) {
      h->fail(ViewStatus::Text);
      return;
    }
    // Text is cut by whole characters (the faces are fixed pitch): a character that is not wholly inside the canvas is not drawn, so the sink needs no scissor
    // and nothing of a view lands outside its rectangle. A row of text that is not wholly inside vertically is not drawn at all.
    const std::int32_t cw = h->metrics_.cell_w[face], chh = h->metrics_.cell_h[face];
    const std::int32_t px = pin(x), py = pin(y);
    if (cw <= 0 || py < 0 || py + chh > cv->height) {
      return;
    }
    std::uint32_t first = 0;
    while (first < len && px + static_cast<std::int32_t>(first) * cw < 0) {
      ++first;
    }
    std::uint32_t last = len;  // one past the last whole character
    while (last > first && px + static_cast<std::int32_t>(last) * cw > cv->width) {
      --last;
    }
    if (first >= last) {
      return;  // not one whole character inside
    }
    const std::uint32_t shown = last - first;
    ViewHost::Cmd* c = push(*h, ViewHost::Op::Text, static_cast<std::uint8_t>(color));
    c->a = static_cast<std::uint8_t>(face);
    c->x = static_cast<std::int16_t>(px + static_cast<std::int32_t>(first) * cw);
    c->y = static_cast<std::int16_t>(py);
    c->off = static_cast<std::uint16_t>(h->text_used_);
    c->n = static_cast<std::uint16_t>(shown);
    std::memcpy(h->text_ + h->text_used_, s + first, shown);
    h->text_[h->text_used_ + shown] = '\0';
    h->text_used_ += shown + 1u;
  }

  static std::int32_t text_width(const o3m_canvas* cv, const char* s, std::uint32_t face) noexcept {
    const ViewHost* h = host(cv);
    if (h == nullptr || s == nullptr || face >= O3M_FACE_COUNT) {
      return 0;
    }
    std::int32_t len = 0;
    while (len < 256 && s[len] != '\0') {
      ++len;
    }
    return len * h->metrics_.cell_w[face];
  }

  static void polyline(o3m_canvas* cv, const std::int16_t* xy, std::uint32_t points, std::uint32_t color) noexcept {
    ViewHost* h = host(cv);
    if (h == nullptr) {
      return;
    }
    const std::uint32_t n = points < 0xFFFFu ? points : 0xFFFFu;
    if (!h->charge(word_cost_poly(n < kViewMaxPolyPoints ? n : kViewMaxPolyPoints))) {
      return;
    }
    if (xy == nullptr || n < 2u || n > kViewMaxPolyPoints || color >= O3M_COLOR_COUNT) {
      return;
    }
    h->points_ += n;
    if (h->points_ > kViewMaxPoints) {
      h->fail(ViewStatus::Points);
      return;
    }
    std::int32_t lo_x = xy[0], hi_x = xy[0], lo_y = xy[1], hi_y = xy[1];
    for (std::uint32_t i = 1; i < n; ++i) {
      lo_x = xy[2 * i] < lo_x ? xy[2 * i] : lo_x;
      hi_x = xy[2 * i] > hi_x ? xy[2 * i] : hi_x;
      lo_y = xy[2 * i + 1] < lo_y ? xy[2 * i + 1] : lo_y;
      hi_y = xy[2 * i + 1] > hi_y ? xy[2 * i + 1] : hi_y;
    }
    if (hi_x < 0 || lo_x >= cv->width || hi_y < 0 || lo_y >= cv->height) {
      return;  // its bounding box is wholly outside
    }
    ViewHost::Cmd* c = push(*h, ViewHost::Op::Poly, static_cast<std::uint8_t>(color));
    c->off = static_cast<std::uint16_t>(h->poly_used_);
    c->n = static_cast<std::uint16_t>(n);
    std::memcpy(h->poly_ + 2u * h->poly_used_, xy, n * 2u * sizeof(std::int16_t));
    h->poly_used_ += n;
  }

  static void curve(o3m_canvas* cv, const o3m_curve* spec) noexcept {
    ViewHost* h = host(cv);
    if (h == nullptr) {
      return;
    }
    const std::uint32_t n = spec != nullptr ? (spec->n < 0xFFFFu ? spec->n : 0xFFFFu) : 0u;
    if (!h->charge(word_cost_curve(n, spec != nullptr ? spec->flags : 0u))) {
      return;
    }
    if (spec == nullptr || spec->y == nullptr || n < 2u || n > kViewMaxPolyPoints || spec->x_axis > O3M_AXIS_DB || spec->y_axis > O3M_AXIS_DB ||
        spec->color >= O3M_COLOR_COUNT || (spec->flags & ~(O3M_CURVE_AXES | O3M_CURVE_FILL)) != 0u || !finite_less(spec->x_lo, spec->x_hi) ||
        !finite_less(spec->y_lo, spec->y_hi) || (spec->x_axis == O3M_AXIS_LOG_FREQ && !(spec->x_lo > 0.0f)) ||
        (spec->y_axis == O3M_AXIS_LOG_FREQ && !(spec->y_lo > 0.0f))) {
      return;
    }
    h->points_ += n;
    if (h->points_ > kViewMaxPoints) {
      h->fail(ViewStatus::Points);
      return;
    }
    const std::uint32_t need = 4u + n + (spec->x != nullptr ? n : 0u);
    if (h->pool_used_ + need > ViewHost::kPoolWords) {
      h->fail(ViewStatus::Points);  // too many small curves for the record
      return;
    }
    float* at = h->pool_ + h->pool_used_;
    at[0] = spec->x_lo;
    at[1] = spec->x_hi;
    at[2] = spec->y_lo;
    at[3] = spec->y_hi;
    std::memcpy(at + 4, spec->y, n * sizeof(float));
    if (spec->x != nullptr) {
      std::memcpy(at + 4 + n, spec->x, n * sizeof(float));
    }
    ViewHost::Cmd* c = push(*h, ViewHost::Op::Curve, static_cast<std::uint8_t>(spec->color));
    c->a = static_cast<std::uint8_t>(spec->x_axis | (spec->y_axis << 4));
    c->b = static_cast<std::uint8_t>(spec->flags);
    c->w = spec->x != nullptr ? 1 : 0;
    c->off = static_cast<std::uint16_t>(h->pool_used_);
    c->n = static_cast<std::uint16_t>(n);
    h->pool_used_ += need;
  }
};

namespace {

const o3m_draw_api kDrawApi = {
    O3M_DRAW_API_VERSION, sizeof(o3m_draw_api), &ViewApi::rect, &ViewApi::outline, &ViewApi::text, &ViewApi::text_width, &ViewApi::polyline, &ViewApi::curve,
};

}  // namespace

const o3m_draw_api* draw_api() noexcept { return &kDrawApi; }

// ---- the host ---------------------------------------------------------------------------------------------------------------

void ViewHost::reset() noexcept {
  status_ = ViewStatus::Ok;
  open_ = false;
  ready_ = false;
  elapsed_ = 0;
  credit_ = 0;
  calls_ = points_ = chars_ = words_ = 0;
  cmd_count_ = pool_used_ = text_used_ = poly_used_ = 0;
}

void ViewHost::fail(ViewStatus s) noexcept {
  if (status_ == ViewStatus::Ok) {
    status_ = s;
  }
}

bool ViewHost::clock_check() noexcept {
  if (hooks_.now_us != nullptr && now() - t0_ > kViewMaxUs) {
    fail(ViewStatus::Time);
    return false;
  }
  return true;
}

bool ViewHost::charge(std::uint32_t words) noexcept {
  if (!open_ || status_ != ViewStatus::Ok) {
    return false;
  }
  if (++calls_ > kViewMaxCalls) {
    fail(ViewStatus::Calls);
    return false;
  }
  words_ += words;
  if (words_ > max_words_) {
    fail(ViewStatus::Words);
    return false;
  }
  return clock_check();
}

ViewStatus ViewHost::draw(const o3m_module& mod, void* state, std::uint16_t fault_id, std::uint32_t view, int w, int h, const float* params,
                          std::uint32_t count, std::uint32_t max_words) noexcept {
  reset();
  max_words_ = max_words < kViewMaxWords ? max_words : kViewMaxWords;
  if (mod.ui == nullptr || mod.ui->view_draw == nullptr || view >= mod.view_count || view >= O3M_VIEWS_MAX || w < kViewMinSize || w > kViewMaxW ||
      h < kViewMinSize || h > kViewMaxH || count > O3M_PARAMS_MAX || (count != 0u && params == nullptr)) {
    return ViewStatus::NoView;
  }
  for (std::uint32_t i = 0; i < count; ++i) {
    params_[i] = params[i];
  }
  canvas_.pub.struct_size = sizeof(o3m_canvas);
  canvas_.pub.view_index = view;
  canvas_.pub.width = w;
  canvas_.pub.height = h;
  canvas_.pub.param_count = count;
  canvas_.pub.params = params_;
  canvas_.pub.draw = draw_api();
  canvas_.host = this;
  canvas_.magic = kMagic;
  open_ = true;
  const std::uint16_t before = stamp(fault_id);
  t0_ = now();
  mod.ui->view_draw(state, view, &canvas_.pub);
  elapsed_ = now() - t0_;
  unstamp(before);
  open_ = false;
  if (status_ == ViewStatus::Ok && hooks_.now_us != nullptr && elapsed_ > kViewMaxUs) {
    status_ = ViewStatus::Time;
  }
  if (status_ != ViewStatus::Ok) {
    const ViewStatus verdict = status_;
    cmd_count_ = pool_used_ = text_used_ = poly_used_ = 0;  // blanked
    ready_ = false;
    return verdict;
  }
  ready_ = true;
  return ViewStatus::Ok;
}

ViewStatus ViewHost::finish_call() noexcept {
  elapsed_ = elapsed_ > credit_ ? elapsed_ - credit_ : 0u;  // (the host's own time inside the call is not the module's: credit_host_us)
  if (hooks_.now_us != nullptr && elapsed_ > kViewMaxUs) {
    return ViewStatus::Time;
  }
  return ViewStatus::Ok;
}

ViewStatus ViewHost::call_enter(const o3m_module& mod, void* state, std::uint16_t fault_id) noexcept {
  if (mod.ui == nullptr || mod.ui->enter == nullptr) {
    return ViewStatus::NoView;
  }
  reset();
  const std::uint16_t before = stamp(fault_id);
  t0_ = now();
  mod.ui->enter(state);
  elapsed_ = now() - t0_;
  unstamp(before);
  return finish_call();
}

ViewStatus ViewHost::call_input(const o3m_module& mod, void* state, std::uint16_t fault_id, const o3m_ui_event& ev) noexcept {
  if (mod.ui == nullptr || mod.ui->input == nullptr) {
    return ViewStatus::NoView;
  }
  reset();
  const std::uint16_t before = stamp(fault_id);
  t0_ = now();
  mod.ui->input(state, &ev);
  elapsed_ = now() - t0_;
  unstamp(before);
  return finish_call();
}

ViewStatus ViewHost::call_exit(const o3m_module& mod, void* state, std::uint16_t fault_id) noexcept {
  if (mod.ui == nullptr || mod.ui->exit == nullptr) {
    return ViewStatus::NoView;
  }
  reset();
  const std::uint16_t before = stamp(fault_id);
  t0_ = now();
  mod.ui->exit(state);
  elapsed_ = now() - t0_;
  unstamp(before);
  return finish_call();
}

ViewStatus ViewHost::call_keys(const o3m_module& mod, void* state, std::uint16_t fault_id, KeyLabels& labels) noexcept {
  std::memset(labels, 0, sizeof labels);
  if (mod.abi_minor < 9u || mod.ui == nullptr || mod.ui->struct_size < __builtin_offsetof(o3m_ui, keys) + sizeof(mod.ui->keys) || mod.ui->keys == nullptr) {
    return ViewStatus::NoView;
  }
  reset();
  const std::uint16_t before = stamp(fault_id);
  t0_ = now();
  const o3m_keys* table = mod.ui->keys(state);
  // The copy is part of the call: `table` and every string are app pointers, read here under the app's stamp with no more than O3M_KEY_LABEL_MAX + 1 bytes of each. F8's slots (7 and 15) are the OS's.
  for (unsigned i = 0; table != nullptr && i < kKeySlots; ++i) {
    const char* src = i == 7u || i == 15u ? nullptr : (i < 8u ? table->f[i] : table->shift_f[i - 8u]);
    char* dst = labels[i];
    unsigned n = 0;
    while (src != nullptr && n < O3M_KEY_LABEL_MAX && src[n] >= ' ' && src[n] <= '~') {
      dst[n] = src[n];
      ++n;
    }
    if (src != nullptr && n < O3M_KEY_LABEL_MAX && src[n] != '\0') {
      n = 0;  // a character that is not printable ASCII before the end: the whole label is blank
    }
    dst[n] = '\0';
  }
  elapsed_ = now() - t0_;
  unstamp(before);
  const ViewStatus v = finish_call();
  if (v != ViewStatus::Ok) {
    std::memset(labels, 0, sizeof labels);
  }
  return v;
}

ViewStatus ViewHost::call_set_param(const o3m_module& mod, void* state, std::uint16_t fault_id, std::uint32_t id, float value) noexcept {
  if (mod.set_param == nullptr) {
    return ViewStatus::NoView;
  }
  reset();
  const std::uint16_t before = stamp(fault_id);
  t0_ = now();
  mod.set_param(state, id, value);
  elapsed_ = now() - t0_;
  unstamp(before);
  return finish_call();
}

int ViewHost::call_init(const o3m_module& mod, void* state, std::uint16_t fault_id, const o3m_host& host) noexcept {
  if (mod.init == nullptr) {
    return 1;
  }
  const std::uint16_t before = stamp(fault_id);
  const int result = mod.init(state, &host);
  unstamp(before);
  return result;
}

void ViewHost::call_shutdown(const o3m_module& mod, void* state, std::uint16_t fault_id) noexcept {
  if (mod.shutdown != nullptr) {
    const std::uint16_t before = stamp(fault_id);
    mod.shutdown(state);
    unstamp(before);
  }
}

void ViewHost::replay(ViewSink& sink) const noexcept {
  if (!ready_) {
    return;
  }
  const int cw = canvas_.pub.width;
  const int ch = canvas_.pub.height;
  for (std::uint32_t i = 0; i < cmd_count_; ++i) {
    const Cmd& c = cmd_[i];
    switch (c.op) {
      case Op::Rect:
        sink.fill(c.x, c.y, c.w, c.h, c.color);
        break;
      case Op::Outline: {
        // The four edges of the rectangle as given, each cut to the canvas; a rectangle of two pixels or fewer in a direction is all border: one fill.
        const int x = c.x, y = c.y, w = c.w, h = c.h;
        if (w <= 2 || h <= 2) {
          int ex = x, ey = y, ew = w, eh = h;
          if (clip_rect(ex, ey, ew, eh, cw, ch)) {
            sink.fill(ex, ey, ew, eh, c.color);
          }
          break;
        }
        const int edges[4][4] = {{x, y, w, 1}, {x, y + h - 1, w, 1}, {x, y + 1, 1, h - 2}, {x + w - 1, y + 1, 1, h - 2}};
        for (unsigned e = 0; e < 4; ++e) {
          int ex = edges[e][0], ey = edges[e][1], ew = edges[e][2], eh = edges[e][3];
          if (clip_rect(ex, ey, ew, eh, cw, ch)) {
            sink.fill(ex, ey, ew, eh, c.color);
          }
        }
        break;
      }
      case Op::Text:
        sink.text(c.x, c.y, text_ + c.off, c.a, c.color);
        break;
      case Op::Poly:
        sink.polyline(poly_ + 2u * c.off, c.n, c.color);
        break;
      case Op::Curve: {
        const float* at = pool_ + c.off;
        CurveCall cc{};
        cc.n = c.n;
        cc.y = at + 4;
        cc.x = c.w != 0 ? at + 4 + c.n : nullptr;
        cc.x_axis = static_cast<std::uint8_t>(c.a & 0x0Fu);
        cc.y_axis = static_cast<std::uint8_t>(c.a >> 4);
        cc.flags = c.b;
        cc.color = c.color;
        cc.x_lo = at[0];
        cc.x_hi = at[1];
        cc.y_lo = at[2];
        cc.y_hi = at[3];
        sink.curve(cc);
        break;
      }
    }
  }
}

}  // namespace o3os::module
