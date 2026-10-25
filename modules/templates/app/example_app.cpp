// SPDX-License-Identifier: MIT
//
// TEMPLATE of an app module (SDK guide: module/sdk/README.md). `o3m_gen.py new --kind app --id <id>` copies this directory under your id; the descriptor header is
// generated from module.json. An app makes no sound (no render, note_on, note_off: pass NULL) and has no hot code. The OS opens a page for it (MODULES screen, F1) whose body is
// view 0: the app gets enter() when the page opens, input() for every control but the OS's own (F8, which is BACK, and Play, which is the transport's; Shift + the arrows and the encoder, which edit the app's
// parameters on the page), view_draw() whenever the page is drawn (30 times a second) and exit() when it closes. Its parameters are standard OS controls on the page; it reads
// their values from canvas->params when it draws and hears them in set_param().
#include <o3module.h>

#include "example_app_gen.h"

namespace {

struct State {
  int32_t x;           // the square's place, from the encoder
  uint32_t last_button;
  uint32_t events;
};
static_assert(sizeof(State) <= EXAMPLE_APP_STATE_BYTES, "module.json: state_bytes is too small for State");

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->last_button = 0xFFFFFFFFu;
  return host->draw != nullptr ? 0 : 1;  // an OS of ABI 1.0 lends no draw API
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  s->x = 0;
  s->events = 0;
}

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  ++s->events;
  if (ev->kind == O3M_EV_ENCODER) {
    s->x += ev->value * 4;
  } else if (ev->kind == O3M_EV_BUTTON && ev->value != 0) {
    s->last_button = ev->id;
  }
}

void leave(void*) {}
void shutdown(void*) {}
void set_param(void*, uint32_t, float) {}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  const State* s = static_cast<const State*>(raw);
  const o3m_draw_api* api = c->draw;
  const int32_t size = static_cast<int32_t>(c->params[EXAMPLE_APP_PARAM_SIZE]);
  int32_t x = c->width / 2 - size / 2 + s->x;
  x = x < 0 ? 0 : (x > c->width - size ? c->width - size : x);  // the draw API cuts what is outside, but an app keeps its square in view
  api->rect(c, 0, 0, c->width, c->height, O3M_COLOR_PANE);
  api->rect(c, x, c->height / 2 - size / 2, size, size, O3M_COLOR_ACCENT);
  api->text(c, 6, 6, s->last_button == 0xFFFFFFFFu ? "NO BUTTON YET" : "A BUTTON WAS PRESSED", O3M_FACE_DATA, O3M_COLOR_TEXT);
}

const o3m_ui kUi = {O3M_UI_SIZE(EXAMPLE_APP_ABI_MINOR), enter, input, view_draw, leave, nullptr};

}  // namespace

extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    EXAMPLE_APP_ENTRY_HEAD, sizeof(State), EXAMPLE_APP_ENTRY_PARAMS, init, nullptr, set_param, nullptr, nullptr, shutdown, EXAMPLE_APP_ENTRY_VIEWS(&kUi),
};
