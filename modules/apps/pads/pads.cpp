// SPDX-License-Identifier: MIT
//
// "Pads", the example app of the SDK (OS-012b, ABI 1.1): it shows that an app receives the Tracker's controls and draws a view, nothing more. The 48 pads
// are drawn as the grid they are (12 x 4, pad 0 at the top left); a pad lights while it is held (or until Delete, with HOLD on), the encoder moves a bar,
// and the last button and the number of events it has seen are text. Its two parameters are standard OS controls on the app's page (Shift + Up / Down and the
// encoder): COLOUR picks the colour a lit pad takes from the theme's roles, HOLD keeps pads lit after release.
//
// Everything an app may do is here: all its state is in the block the OS gives it (no globals); input() only records; view_draw() only draws, from that state
// and from the parameter values the canvas carries, through canvas->draw; nothing blocks, nothing allocates, no libc.
#include <o3module.h>

namespace {

enum Param : uint32_t { kColour = 0, kHold, kParamCount };

const o3m_param kParams[kParamCount] = {
    {"Colour", 0.0f, 3.0f, 0.0f, O3M_PARAM_INT},  // 0 accent, 1 play, 2 instrument, 3 effect
    {"Hold", 0.0f, 1.0f, 0.0f, O3M_PARAM_BOOL},   // lit pads stay lit after release until Delete
};

const uint8_t kRoles[4] = {O3M_COLOR_ACCENT, O3M_COLOR_PLAY, O3M_COLOR_INSTR, O3M_COLOR_FX};

struct State {
  uint8_t lit[48];     // velocity of a lit pad, 0 = dark
  int32_t bar;         // the encoder's sum, -100..100
  uint32_t last_button;
  uint32_t events;
  uint32_t hold;       // the HOLD parameter as set_param() last heard it
};

int init(void* raw, const o3m_host* host) {
  State* s = static_cast<State*>(raw);
  s->last_button = 0xFFFFFFFFu;
  return host->draw != nullptr ? 0 : 1;  // an OS of ABI 1.0 lends no draw API: this app has nothing to run on
}

void set_param(void* raw, uint32_t id, float value) {
  if (id == kHold) {
    static_cast<State*>(raw)->hold = value >= 0.5f ? 1u : 0u;
  }
}

void enter(void* raw) {
  State* s = static_cast<State*>(raw);
  for (uint32_t i = 0; i < 48; ++i) {
    s->lit[i] = 0;
  }
  s->bar = 0;
}

void input(void* raw, const o3m_ui_event* ev) {
  State* s = static_cast<State*>(raw);
  ++s->events;
  if (ev->kind == O3M_EV_PAD && ev->id < 48u) {
    if (ev->value > 0) {
      s->lit[ev->id] = static_cast<uint8_t>(ev->value);
    } else if (s->hold == 0u) {
      s->lit[ev->id] = 0;
    }
  } else if (ev->kind == O3M_EV_BUTTON && ev->value != 0) {
    s->last_button = ev->id;
    if (ev->id == O3M_BTN_DELETE) {
      for (uint32_t i = 0; i < 48; ++i) {
        s->lit[i] = 0;
      }
    }
  } else if (ev->kind == O3M_EV_ENCODER) {
    int32_t b = s->bar + ev->value;
    s->bar = b < -100 ? -100 : (b > 100 ? 100 : b);
  }
}

// "<label><number>" in a small buffer: no libc.
uint32_t put_number(char* out, uint32_t at, uint32_t value) {
  char digits[10];
  uint32_t n = 0;
  do {
    digits[n++] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  } while (value != 0u && n < 10u);
  while (n != 0u) {
    out[at++] = digits[--n];
  }
  return at;
}

void view_draw(void* raw, uint32_t, o3m_canvas* c) {
  const State* s = static_cast<const State*>(raw);
  const o3m_draw_api* api = c->draw;
  uint32_t colour = static_cast<uint32_t>(c->params[kColour] + 0.5f);
  colour = colour > 3u ? 3u : colour;
  const int32_t cell_w = c->width / 12;
  const int32_t cell_h = (c->height - 64) / 4;
  api->rect(c, 0, 0, c->width, c->height, O3M_COLOR_PANE);
  for (int32_t pad = 0; pad < 48; ++pad) {
    const int32_t x = (pad % 12) * cell_w + 2;
    const int32_t y = (pad / 12) * cell_h + 2;
    if (s->lit[pad] != 0u) {
      api->rect(c, x, y, cell_w - 4, cell_h - 4, kRoles[colour]);
    } else {
      api->outline(c, x, y, cell_w - 4, cell_h - 4, O3M_COLOR_EMPTY);
    }
  }
  // the encoder's bar: from the middle to the left or to the right
  const int32_t mid = c->width / 2;
  const int32_t len = s->bar * (mid - 8) / 100;
  const int32_t bar_y = c->height - 52;
  api->outline(c, 4, bar_y, c->width - 8, 16, O3M_COLOR_EMPTY);
  api->rect(c, len < 0 ? mid + len : mid, bar_y + 2, len < 0 ? -len : len, 12, O3M_COLOR_SEL);
  api->rect(c, mid, bar_y - 2, 1, 20, O3M_COLOR_TEXT_DIM);
  char text[32];
  uint32_t n = 0;
  const char* label = "BUTTON ";
  while (*label != '\0') {
    text[n++] = *label++;
  }
  if (s->last_button == 0xFFFFFFFFu) {
    text[n++] = '-';
  } else {
    n = put_number(text, n, s->last_button);
  }
  text[n] = '\0';
  api->text(c, 6, c->height - 28, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
  n = 0;
  label = "EVENTS ";
  while (*label != '\0') {
    text[n++] = *label++;
  }
  n = put_number(text, n, s->events);
  text[n] = '\0';
  api->text(c, c->width / 2, c->height - 28, text, O3M_FACE_DATA, O3M_COLOR_TEXT);
}

void leave(void*) {}
void shutdown(void*) {}

const o3m_ui kUi = {O3M_UI_SIZE(2), enter, input, view_draw, leave, nullptr};

}  // namespace

// The example stays ABI 1.2 on purpose (OS-H1): it asks for no capability, so it is the real-file proof that an app packed before ABI 1.3 loads and runs on an OS of 1.3 (tests/pads_app_test.cpp).
extern "C" O3M_EXPORT const o3m_module o3m_entry __attribute__((section(".o3m_entry"))) = {
    O3M_ABI_MAJOR, 2, O3M_KIND_APP, O3M_MODULE_SIZE(2), "Pads", sizeof(State), kParamCount, kParams,
    init, nullptr, set_param, nullptr, nullptr, shutdown, &kUi, 1,
};
