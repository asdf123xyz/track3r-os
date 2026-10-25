// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Module views: the restricted draw API and its containment (ADR 0002 amendment 1, OS-012b, ABI 1.1). The OS owns every page; a module paints a VIEW,
// a rectangle the page gives it, through the o3m_draw_api table (sdk/o3module.h) and nothing else: rectangles, text in the theme's faces, polylines and
// curves, in colours that are the theme's roles. There is no display list and no register in reach.
//
// RECORD, THEN REPLAY. A view's calls do not draw: they are checked, clipped and recorded in this object (fixed memory, about 5 KB), and the page replays
// the record to the renderer only when view_draw() came back inside its budgets. A view that goes over one is therefore BLANKED, not half drawn: nothing of
// its frame reaches the display list, and the budgets are what keeps one view from filling the frame buffer the page shares with it (a view costs a bounded
// number of display-list words: kViewMaxWords, estimated per call).
//
// BUDGETS (per frame and view; O3M_VIEW_* in the SDK header are the numbers a module may rely on):
//   calls   O3M_VIEW_MAX_CALLS draw calls (text_width() is not one),
//   points  O3M_VIEW_MAX_POINTS points in polylines and curves together,
//   text    O3M_VIEW_MAX_TEXT characters in text calls,
//   words   kViewMaxWords estimated display-list words: for the views of one page TOGETHER (the page gives each view what the earlier ones left: ViewHost::draw's
//           `max_words`), since the frame buffer they land in is one and shared with the page's own words,
//   time    O3M_VIEW_MAX_US microseconds from the call into the module to its return (checked at every draw call and when it returns, with a clock the platform
//           gives; a module that never returns and never draws cannot be stopped from here: the platform's watchdog and crash record catch it, and the next
//           boot quarantines the module that was running).
// The first budget that is exceeded is the verdict (ViewStatus); calls after it are ignored at once. The caller (app/module_ui.h) quarantines the module.
//
// CLIPPING. Coordinates are canvas pixels, signed. A rectangle is cut to the canvas here; text is cut by whole characters (a character not wholly
// inside is not drawn, a row not wholly inside vertically is dropped); a polyline wholly outside is dropped and one that crosses the edge is left to the
// sink, which draws it inside a scissor of the canvas rectangle (the FT812's); a curve is drawn by ui::plot inside its own area. Nothing of a view ever
// lands outside its rectangle.
//
// Contexts: main loop only (the views are drawn from Screen::render and apps' input from the App's pass). One ViewHost serves every view, one at a time.
// No heap, no double.

#include <o3module.h>

#include <cstdint>

namespace o3os::module {

inline constexpr std::uint32_t kViewMaxCalls = O3M_VIEW_MAX_CALLS;
inline constexpr std::uint32_t kViewMaxPoints = O3M_VIEW_MAX_POINTS;
inline constexpr std::uint32_t kViewMaxPolyPoints = O3M_VIEW_MAX_POLY_POINTS;
inline constexpr std::uint32_t kViewMaxText = O3M_VIEW_MAX_TEXT;
inline constexpr std::uint32_t kViewMaxTextCall = 64;     // characters one text call keeps (the rest is cut)
inline constexpr std::uint32_t kViewMaxUs = O3M_VIEW_MAX_US;
inline constexpr std::uint32_t kViewMaxWords = 700;       // display-list words of a view's frame, as estimated by word_cost()
inline constexpr int kViewMinSize = 16;                   // the smallest canvas a page gives
inline constexpr int kViewMaxW = 800;
inline constexpr int kViewMaxH = 480;

enum class ViewStatus : std::uint8_t {
  Ok = 0,
  Calls,    // more than kViewMaxCalls draw calls
  Points,   // more than kViewMaxPoints polyline / curve points
  Text,     // more than kViewMaxText characters
  Words,    // the estimated display-list cost went over kViewMaxWords
  Time,     // took longer than kViewMaxUs
  NoView,   // the module has no such view (or none at all), or the canvas is not a legal size: nothing was called
};

constexpr const char* view_status_name(ViewStatus s) noexcept {
  switch (s) {
    case ViewStatus::Ok: return "ok";
    case ViewStatus::Calls: return "too many draw calls";
    case ViewStatus::Points: return "too many points";
    case ViewStatus::Text: return "too much text";
    case ViewStatus::Words: return "too much for the display list";
    case ViewStatus::Time: return "too slow";
    case ViewStatus::NoView: return "no such view";
  }
  return "?";
}

// What the page replays a record to. Coordinates are canvas pixels (the sink adds the canvas's place on the page). `fill` and `text` are already clipped;
// `polyline` may reach outside the canvas and the sink clips it with a scissor; `curve` is a series for ui::plot inside the canvas.
struct CurveCall {
  const float* x;  // null: evenly spaced over x_lo..x_hi
  const float* y;
  std::uint32_t n;
  std::uint8_t x_axis;  // O3M_AXIS_*
  std::uint8_t y_axis;
  std::uint8_t flags;   // O3M_CURVE_*
  std::uint8_t color;   // O3M_COLOR_*
  float x_lo, x_hi, y_lo, y_hi;
};

class ViewSink {
 public:
  virtual void fill(int x, int y, int w, int h, std::uint8_t color) noexcept = 0;
  virtual void text(int x, int y, const char* s, std::uint8_t face, std::uint8_t color) noexcept = 0;
  virtual void polyline(const std::int16_t* xy, std::uint32_t points, std::uint8_t color) noexcept = 0;  // xy: x0, y0, x1, y1, ...
  virtual void curve(const CurveCall& c) noexcept = 0;

 protected:
  ViewSink() = default;
  ~ViewSink() = default;
};

// Cuts a rectangle to the canvas [0, cw) x [0, ch): false when nothing is left (or w or h is not positive).
bool clip_rect(int& x, int& y, int& w, int& h, int cw, int ch) noexcept;

// The display-list words the budget charges for a call (estimates, deliberately generous: the renderer's real cost is lower).
std::uint32_t word_cost_rect() noexcept;
std::uint32_t word_cost_outline() noexcept;
std::uint32_t word_cost_text(std::uint32_t chars) noexcept;
std::uint32_t word_cost_poly(std::uint32_t points) noexcept;
std::uint32_t word_cost_curve(std::uint32_t points, std::uint32_t flags) noexcept;

// The drawing table lent to modules (audio::ModuleVoices puts it in o3m_host.draw).
const o3m_draw_api* draw_api() noexcept;

class ViewHost {
 public:
  // What the platform lends: the microsecond clock (null: no time budget, tests), and the crash-context stamp of audio/module_voices.h's Hooks::enter
  // (null: none) so that a fault inside view code names its module.
  struct Hooks {
    std::uint32_t (*now_us)(void* ctx) = nullptr;
    void* ctx = nullptr;
    std::uint16_t (*enter)(std::uint16_t fault_id) = nullptr;
  };
  // The theme's face cell sizes, for text_width() and for dropping text that lies outside the canvas (set from the theme before drawing).
  struct Metrics {
    std::int16_t cell_w[O3M_FACE_COUNT] = {12, 6, 6, 4};
    std::int16_t cell_h[O3M_FACE_COUNT] = {16, 12, 8, 6};
  };

  ViewHost() noexcept { reset(); }
  ViewHost(const ViewHost&) = delete;
  ViewHost& operator=(const ViewHost&) = delete;

  void set_hooks(const Hooks& hooks) noexcept { hooks_ = hooks; }
  void set_metrics(const Metrics& m) noexcept { metrics_ = m; }

  // Runs view `view` of `mod` on a canvas of w x h (kViewMinSize..kViewMaxW / kViewMaxH) with the parameter values `params` (count of them, at most O3M_PARAMS_MAX).
  // `state` is the instance's state block (see the SDK header for who may touch it). Ok: the record is there to replay(). Anything else: the record is empty
  // (blanked) and the status is the cause. `fault_id` is module::fault_id() of the module, for the crash context.
  // `max_words` (at most kViewMaxWords) is what is left of the page's words for this view.
  ViewStatus draw(const o3m_module& mod, void* state, std::uint16_t fault_id, std::uint32_t view, int w, int h, const float* params, std::uint32_t count,
                  std::uint32_t max_words = kViewMaxWords) noexcept;
  // Replays the record of the last draw() that said Ok (nothing after a failure).
  void replay(ViewSink& sink) const noexcept;

  // An app's entry points, run under the same time budget and crash stamp. enter / exit have no argument; input() takes the event. Ok or Time (NoView: the
  // module has no ui table or no such entry, nothing was called).
  ViewStatus call_enter(const o3m_module& mod, void* state, std::uint16_t fault_id) noexcept;
  ViewStatus call_input(const o3m_module& mod, void* state, std::uint16_t fault_id, const o3m_ui_event& ev) noexcept;
  ViewStatus call_exit(const o3m_module& mod, void* state, std::uint16_t fault_id) noexcept;
  // ABI 1.9: an app's keys() (the key row's labels) under the same budget. The table and the strings it points at are the app's, so they are READ INSIDE the call, still under the app's crash stamp
  // and before its time is taken: a wild pointer faults in the app's context and the quarantine finds it, never later in the OS's. Ok: `labels` holds the 16 labels, F1..F8 then Shift + F1..F8, as the OS keeps
  // them (printable ASCII, at most O3M_KEY_LABEL_MAX characters, a label with any other character blank, F8's two slots blank, a NULL table or label blank). NoView when the module has no keys() or does not declare 1.9;
  // Time when it overran. In every case but Ok the labels are blank.
  static constexpr unsigned kKeySlots = 16;
  using KeyLabels = char[kKeySlots][O3M_KEY_LABEL_MAX + 1];
  ViewStatus call_keys(const o3m_module& mod, void* state, std::uint16_t fault_id, KeyLabels& labels) noexcept;
  // An app's set_param (it may be NULL: NoView) under the time budget, and init() with the crash context stamped (no budget: init may take a while, like an
  // instrument's). `init` returns the module's own result (0 = ok).
  ViewStatus call_set_param(const o3m_module& mod, void* state, std::uint16_t fault_id, std::uint32_t id, float value) noexcept;
  int call_init(const o3m_module& mod, void* state, std::uint16_t fault_id, const o3m_host& host) noexcept;
  void call_shutdown(const o3m_module& mod, void* state, std::uint16_t fault_id) noexcept;

  // What the last draw() used (the tests and `diag`).
  std::uint32_t calls() const noexcept { return calls_; }
  std::uint32_t points() const noexcept { return points_; }
  std::uint32_t text_chars() const noexcept { return chars_; }
  std::uint32_t words() const noexcept { return words_; }
  std::uint32_t elapsed_us() const noexcept { return elapsed_; }
  // OS-H3A: microseconds of the running call that were the HOST's, not the module's (an app's edit_end applies its group inside the app's input() call): they do not count against the 4 ms budget.
  // Cleared by the next call. Main loop only.
  void credit_host_us(std::uint32_t us) noexcept { credit_ += us; }
  std::uint32_t recorded() const noexcept { return cmd_count_; }

 private:
  friend struct ViewApi;
  enum class Op : std::uint8_t { Rect, Outline, Text, Poly, Curve };
  struct Cmd {
    Op op;
    std::uint8_t color;
    std::uint8_t a;       // text: face; curve: x_axis | y_axis << 4
    std::uint8_t b;       // curve: flags
    std::int16_t x, y, w, h;
    std::uint16_t off;    // text: offset in text_; poly: first point in poly_; curve: offset in pool_
    std::uint16_t n;      // text: length; poly / curve: points
  };
  static_assert(sizeof(Cmd) == 16, "a recorded call is four words");
  static constexpr std::uint32_t kCmdMax = kViewMaxCalls;
  static constexpr std::uint32_t kPoolWords = 2 * kViewMaxPoints + 4 * 8;  // curves: 4 range floats, then y (and x) floats; the points budget bounds the rest
  static constexpr std::uint32_t kTextBytes = kViewMaxText + kViewMaxCalls;

  struct Canvas {
    o3m_canvas pub;
    ViewHost* host;
    std::uint32_t magic;
  };
  static constexpr std::uint32_t kMagic = 0x4F33564Du;  // "O3VM"

  void reset() noexcept;
  bool charge(std::uint32_t words) noexcept;  // one call; false once a budget is gone
  bool clock_check() noexcept;
  std::uint32_t now() const noexcept { return hooks_.now_us != nullptr ? hooks_.now_us(hooks_.ctx) : 0u; }
  void fail(ViewStatus s) noexcept;
  std::uint16_t stamp(std::uint16_t fault_id) noexcept { return hooks_.enter != nullptr ? hooks_.enter(fault_id) : std::uint16_t{0}; }
  void unstamp(std::uint16_t previous) noexcept {
    if (hooks_.enter != nullptr) {
      hooks_.enter(previous);
    }
  }
  ViewStatus finish_call() noexcept;  // after a call into the module: the elapsed time against the budget

  Hooks hooks_;
  Metrics metrics_;
  Canvas canvas_{};
  float params_[O3M_PARAMS_MAX] = {};
  ViewStatus status_ = ViewStatus::Ok;
  bool open_ = false;  // a draw() is running: the API accepts calls
  std::uint32_t max_words_ = kViewMaxWords;
  std::uint32_t t0_ = 0;
  std::uint32_t elapsed_ = 0;
  std::uint32_t credit_ = 0;  // host time inside the running call (credit_host_us)
  std::uint32_t calls_ = 0, points_ = 0, chars_ = 0, words_ = 0;
  std::uint32_t cmd_count_ = 0, pool_used_ = 0, text_used_ = 0, poly_used_ = 0;
  bool ready_ = false;  // the record is complete and may be replayed
  Cmd cmd_[kCmdMax];
  float pool_[kPoolWords];
  std::int16_t poly_[2 * kViewMaxPoints];  // polyline points, x then y
  char text_[kTextBytes];
};

}  // namespace o3os::module
