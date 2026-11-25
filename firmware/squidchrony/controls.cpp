#include "controls.h"

#include <Arduino.h>

#include "config.h"

namespace controls {
namespace {

constexpr uint32_t DEBOUNCE_MS = 25;

struct Button {
  int pin;
  Event down, up, click, lng, off;  // off = NONE: this button has no hold-to-turn-off
  bool stable = false;  // debounced pressed state
  bool raw = false;
  uint32_t raw_since = 0;
  uint32_t down_at = 0;
  bool off_sent = false;
  bool ignore = false;  // this press is the one that switched the chrono on: no events
};

Button read_btn{PIN_BTN_READ, READ_DOWN, READ_UP, READ_CLICK, READ_LONG, NONE};
Button mode_btn{PIN_BTN_MODE, MODE_DOWN, MODE_UP, MODE_CLICK, MODE_LONG, MODE_OFF};

Event queue[8];
uint8_t q_head = 0, q_tail = 0;
int led_read = -1, led_mode = -1;

void push(Event e) {
  uint8_t next = (q_head + 1) & 7;
  if (next == q_tail) return;  // full: drop
  queue[q_head] = e;
  q_head = next;
}

void update(Button& b, uint32_t now) {
  bool r = digitalRead(b.pin) == LOW;
  if (r != b.raw) {
    b.raw = r;
    b.raw_since = now;
  }
  if (b.raw != b.stable && now - b.raw_since >= DEBOUNCE_MS) {
    b.stable = b.raw;
    if (b.stable) {
      b.down_at = now;
      b.off_sent = false;
      b.ignore = false;
      push(b.down);
    } else if (b.ignore) {
      b.ignore = false;  // released the power-on press: nothing to report
    } else {
      push(b.up);
      uint32_t held = now - b.down_at;
      if (!b.off_sent) push(held < LONG_MS ? b.click : b.lng);
    }
  }
  if (b.stable && !b.ignore && b.off != NONE && !b.off_sent && now - b.down_at >= OFF_MS) {
    b.off_sent = true;
    push(b.off);
  }
}

void init(Button& b, uint32_t now) {
  pinMode(b.pin, INPUT_PULLUP);
  delayMicroseconds(50);  // let the pull-up charge the line
  b.raw = b.stable = digitalRead(b.pin) == LOW;
  b.raw_since = b.down_at = now;
  b.ignore = b.stable;
}

}  // namespace

bool begin() {
  uint32_t now = millis();
  init(read_btn, now);
  init(mode_btn, now);
  pinMode(PIN_LED_READ, OUTPUT);
  pinMode(PIN_LED_MODE, OUTPUT);
  setLeds(false, false);
  return mode_btn.stable;
}

Event poll(uint32_t now) {
  update(read_btn, now);
  update(mode_btn, now);
  if (q_tail == q_head) return NONE;
  Event e = queue[q_tail];
  q_tail = (q_tail + 1) & 7;
  return e;
}

bool readHeld() { return read_btn.stable && !read_btn.ignore; }
bool modeHeld() { return mode_btn.stable; }

uint32_t modeHeldMs(uint32_t now) {
  return mode_btn.stable && !mode_btn.ignore ? now - mode_btn.down_at : 0;
}

void setLeds(bool read_on, bool mode_on) {
  if ((int)read_on != led_read) {
    led_read = read_on;
    digitalWrite(PIN_LED_READ, read_on ? HIGH : LOW);
  }
  if ((int)mode_on != led_mode) {
    led_mode = mode_on;
    digitalWrite(PIN_LED_MODE, mode_on ? HIGH : LOW);
  }
}

}  // namespace controls
