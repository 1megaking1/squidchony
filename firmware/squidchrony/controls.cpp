#include "controls.h"

#include <Arduino.h>

#include "config.h"

namespace controls {
namespace {

constexpr uint32_t DEBOUNCE_MS = 25;
constexpr uint32_t LONG_MS = 700;

struct Button {
  int pin;
  Event down, up, click, lng;
  bool stable = false;  // debounced pressed state
  bool raw = false;
  uint32_t raw_since = 0;
  uint32_t down_at = 0;
  bool long_sent = false;
};

Button read_btn{PIN_BTN_READ, READ_DOWN, READ_UP, READ_CLICK, READ_LONG};
Button mode_btn{PIN_BTN_MODE, MODE_DOWN, MODE_UP, MODE_CLICK, MODE_LONG};

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
      b.long_sent = false;
      push(b.down);
    } else {
      push(b.up);
      if (!b.long_sent) push(b.click);
    }
  }
  if (b.stable && !b.long_sent && now - b.down_at >= LONG_MS) {
    b.long_sent = true;
    push(b.lng);
  }
}

}  // namespace

void begin() {
  pinMode(PIN_BTN_READ, INPUT_PULLUP);
  pinMode(PIN_BTN_MODE, INPUT_PULLUP);
  pinMode(PIN_LED_READ, OUTPUT);
  pinMode(PIN_LED_MODE, OUTPUT);
  setLeds(false, false);
}

Event poll(uint32_t now) {
  update(read_btn, now);
  update(mode_btn, now);
  if (q_tail == q_head) return NONE;
  Event e = queue[q_tail];
  q_tail = (q_tail + 1) & 7;
  return e;
}

bool readHeld() { return read_btn.stable; }
bool modeHeld() { return mode_btn.stable; }
bool modePressedRaw() { return digitalRead(PIN_BTN_MODE) == LOW; }

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
