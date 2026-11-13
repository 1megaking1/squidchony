#include "display.h"

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Arduino.h>
#include <SPI.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace display {
namespace {

// RGB565
constexpr uint16_t BG = 0x0000;
constexpr uint16_t FG = 0xFFFF;
constexpr uint16_t DIM = 0x7BEF;
constexpr uint16_t GRID = 0x2945;
constexpr uint16_t SEG_ON = 0xFD80;   // amber
constexpr uint16_t SEG_OFF = 0x2100;  // faint, like an unlit LCD segment
constexpr uint16_t GREEN = 0x07E0;
constexpr uint16_t YELLOW = 0xFFE0;
constexpr uint16_t RED = 0xF800;

// Layout
constexpr int STATUS_H = 22;
constexpr int READ_Y = 28, DIG_W = 56, DIG_H = 94, DIG_T = 10, DIG_G = 12, DIG_X0 = 6;
constexpr int META_Y = 128;
constexpr int GRID_Y = 148, TILE_W = 106, TILE_H = 46;

Adafruit_ST7789 tft(&SPI, PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
bool bl_on = false;

enum Section { STATUS = 0, READOUT, META, LOWER, NSECT };
char drawn[NSECT][96];  // what each section showed last
bool lower_is_menu = false;
bool grid_drawn = false;

// Segment bits: a b c d e f g (a = top, then clockwise, g = middle)
constexpr uint8_t SEGS[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};
constexpr uint8_t SEG_MINUS = 0x40;
constexpr uint8_t SEG_BLANK = 0x00;

void drawDigit(int x, int y, uint8_t segs) {
  const int W = DIG_W, H = DIG_H, T = DIG_T;
  const int hv = H / 2 - T - T / 2;  // vertical segment length
  struct R {
    int x, y, w, h;
  } r[7] = {
      {x + T, y, W - 2 * T, T},                     // a
      {x + W - T, y + T, T, hv},                    // b
      {x + W - T, y + H / 2 + T / 2, T, hv},        // c
      {x + T, y + H - T, W - 2 * T, T},             // d
      {x, y + H / 2 + T / 2, T, hv},                // e
      {x, y + T, T, hv},                            // f
      {x + T, y + H / 2 - T / 2, W - 2 * T, T},     // g
  };
  for (int i = 0; i < 7; i++) tft.fillRect(r[i].x, r[i].y, r[i].w, r[i].h, (segs >> i) & 1 ? SEG_ON : SEG_OFF);
}

void text(int x, int y, int size, uint16_t color, const char* s, int width_chars) {
  char buf[64];
  int n = snprintf(buf, sizeof buf, "%-*.*s", width_chars, width_chars, s);
  (void)n;
  tft.setTextSize(size);
  tft.setTextColor(color, BG);
  tft.setCursor(x, y);
  tft.print(buf);
}

void drawStatus(const Model& m) {
  tft.drawFastHLine(0, STATUS_H, 320, m.armed ? GREEN : GRID);
  tft.drawFastHLine(0, STATUS_H + 1, 320, m.armed ? GREEN : BG);
  text(4, 4, 2, m.armed ? GREEN : FG, m.reading, 9);
  char buf[16];
  snprintf(buf, sizeof buf, "%s%s", m.proj, m.auto_read ? "+A" : "");
  text(124, 4, 2, DIM, buf, 7);
  const char* w = m.wifi == 2 ? "Wi" : (m.wifi == 1 ? "wi" : "  ");
  text(214, 4, 2, m.wifi == 2 ? FG : DIM, w, 2);
  if (m.batt_pct < 0)
    snprintf(buf, sizeof buf, "  -- ");
  else if (m.batt_low)
    snprintf(buf, sizeof buf, " LOW!");
  else
    snprintf(buf, sizeof buf, "%4d%%", m.batt_pct);
  text(254, 4, 2, m.batt_low ? RED : FG, buf, 5);
}

void drawReadout(const Model& m) {
  uint8_t segs[4];
  int dp = m.decimals == 2 ? 1 : 2;  // decimal point after this digit cell
  if (!m.have_shot) {
    for (int i = 0; i < 4; i++) segs[i] = SEG_MINUS;
  } else {
    float scale = m.decimals == 2 ? 100.0f : 10.0f;
    long v = lroundf(m.value * scale);
    if (v < 0) v = 0;
    if (v > 9999) v = 9999;
    int d[4] = {(int)(v / 1000), (int)(v / 100 % 10), (int)(v / 10 % 10), (int)(v % 10)};
    bool lead = true;
    for (int i = 0; i < 4; i++) {
      if (lead && i < dp && d[i] == 0) {
        segs[i] = SEG_BLANK;
      } else {
        lead = false;
        segs[i] = SEGS[d[i]];
      }
    }
  }
  for (int i = 0; i < 4; i++) {
    int x = DIG_X0 + i * (DIG_W + DIG_G);
    drawDigit(x, READ_Y, segs[i]);
    // decimal point in the gap after the digit
    tft.fillRect(x + DIG_W + 1, READ_Y + DIG_H - DIG_T, DIG_G - 2, DIG_T, i == dp ? SEG_ON : BG);
  }
  text(276, READ_Y + DIG_H - 16, 2, DIM, m.units, 3);
}

void drawMeta(const Model& m) {
  char buf[48];
  if (m.message) {
    text(4, META_Y, 2, YELLOW, m.message, 26);
    return;
  }
  if (!m.have_shot) {
    text(4, META_Y, 2, DIM, m.armed ? "Measuring..." : "Hold READ and shoot", 26);
    return;
  }
  int len = snprintf(buf, sizeof buf, "#%d  %d dB", m.shot_no, (int)lroundf(m.snr_db));
  if (m.has_vs_avg)
    len += snprintf(buf + len, sizeof buf - len, "  %+.*f avg", m.decimals, m.vs_avg);
  text(4, META_Y, 2, FG, buf, 26);
  if (m.weak) text(4 + 12 * 21, META_Y, 2, YELLOW, "WEAK", 4);
}

void drawGrid() {
  tft.fillRect(0, GRID_Y - 2, 320, 240 - GRID_Y + 2, BG);
  tft.drawFastHLine(0, GRID_Y - 2, 320, GRID);
  tft.drawFastHLine(0, GRID_Y + TILE_H, 320, GRID);
  tft.drawFastVLine(TILE_W, GRID_Y - 2, 240 - GRID_Y + 2, GRID);
  tft.drawFastVLine(2 * TILE_W + 1, GRID_Y - 2, 240 - GRID_Y + 2, GRID);
  grid_drawn = true;
}

void tile(int col, int row, const char* label, const char* value) {
  int x = col * (TILE_W + 1) + 4, y = GRID_Y + row * (TILE_H + 1) + 3;
  text(x, y, 1, DIM, label, 8);
  text(x, y + 14, 3, FG, value, 5);
}

void drawStats(const Model& m) {
  if (!grid_drawn) drawGrid();
  char v[6][12];
  const char* dash = "  -";
  int d = m.decimals;
  if (m.n) {
    snprintf(v[0], 12, "%.*f", d, m.avg);
    snprintf(v[3], 12, "%.*f", d, m.lo);
    snprintf(v[4], 12, "%.*f", d, m.hi);
  } else {
    strcpy(v[0], dash);
    strcpy(v[3], dash);
    strcpy(v[4], dash);
  }
  if (m.n > 1) {
    snprintf(v[1], 12, "%.*f", d + 1, m.sd);
    snprintf(v[2], 12, "%.*f", d, m.es);
  } else {
    strcpy(v[1], dash);
    strcpy(v[2], dash);
  }
  if (m.rof > 0)
    snprintf(v[5], 12, "%.1f", m.rof);
  else
    strcpy(v[5], dash);
  char avg_label[12];
  snprintf(avg_label, sizeof avg_label, "AVG  n=%d", m.n);
  tile(0, 0, avg_label, v[0]);
  tile(1, 0, "SD", v[1]);
  tile(2, 0, "ES", v[2]);
  tile(0, 1, "LOW", v[3]);
  tile(1, 1, "HIGH", v[4]);
  tile(2, 1, "RATE /s", v[5]);
}

void drawMenu(const Model& m) {
  char buf[40];
  snprintf(buf, sizeof buf, "MENU %d/%d  %s", m.menu_index + 1, m.menu_count, m.menu_item);
  text(4, GRID_Y + 2, 2, DIM, buf, 26);
  snprintf(buf, sizeof buf, "< %s >", m.menu_value);
  text(4, GRID_Y + 30, 3, FG, buf, 17);
  text(4, GRID_Y + 66, 1, DIM, "MODE: next item    READ: change", 52);
  text(4, GRID_Y + 78, 1, DIM, "Hold MODE: done", 52);
}

void signature(Section s, const Model& m, char* out, size_t n) {
  switch (s) {
    case STATUS:
      snprintf(out, n, "%s|%s|%s|%d|%d|%d|%d|%d", m.reading, m.proj, m.units, m.armed, m.auto_read,
               m.wifi, m.batt_pct, m.batt_low);
      break;
    case READOUT:
      snprintf(out, n, "%d|%.*f|%d|%s", m.have_shot, m.decimals, m.value, m.decimals, m.units);
      break;
    case META:
      if (m.message)
        snprintf(out, n, "msg|%s", m.message);
      else
        snprintf(out, n, "%d|%d|%d|%.1f|%d|%d|%.*f|%d", m.have_shot, m.armed, m.shot_no, m.snr_db,
                 m.weak, m.has_vs_avg, m.decimals, m.vs_avg, m.decimals);
      break;
    default:
      if (m.menu)
        snprintf(out, n, "menu|%d|%d|%s|%s", m.menu_index, m.menu_count, m.menu_item, m.menu_value);
      else
        snprintf(out, n, "%d|%.3f|%.3f|%.3f|%.3f|%.3f|%.1f|%d", m.n, m.avg, m.sd, m.es, m.lo, m.hi,
                 m.rof, m.decimals);
      break;
  }
}

}  // namespace

void begin() {
  pinMode(PIN_TFT_BL, OUTPUT);
  digitalWrite(PIN_TFT_BL, LOW);
  SPI.setRX(PIN_SPI_MISO);
  SPI.setTX(PIN_SPI_MOSI);
  SPI.setSCK(PIN_SPI_SCK);
  tft.init(240, 320);
  tft.setRotation(1);  // landscape, 320 wide
  tft.setSPISpeed(40000000);
  tft.setTextWrap(false);
  tft.fillScreen(BG);
  invalidate();
  setBacklight(true);
}

void setBacklight(bool on) {
  bl_on = on;
  digitalWrite(PIN_TFT_BL, on ? HIGH : LOW);
}

bool backlight() { return bl_on; }

void invalidate() {
  for (int i = 0; i < NSECT; i++) drawn[i][0] = '\0';
  grid_drawn = false;
}

void splash(const char* line1, const char* line2) {
  tft.fillScreen(BG);
  text(10, 80, 3, SEG_ON, line1, 17);
  text(10, 120, 2, DIM, line2, 25);
  invalidate();
}

bool update(const Model& m) {
  char sig[96];
  bool drew = false;
  for (int s = 0; s < NSECT; s++) {
    signature((Section)s, m, sig, sizeof sig);
    if (!strcmp(sig, drawn[s])) continue;
    if (drew) return true;  // one section per call
    switch (s) {
      case STATUS:
        drawStatus(m);
        break;
      case READOUT:
        drawReadout(m);
        break;
      case META:
        drawMeta(m);
        break;
      default:
        if (m.menu != lower_is_menu || !grid_drawn) {
          tft.fillRect(0, GRID_Y - 2, 320, 240 - GRID_Y + 2, BG);
          grid_drawn = false;
          lower_is_menu = m.menu;
          if (m.menu) grid_drawn = true;  // the menu has no grid
        }
        if (m.menu)
          drawMenu(m);
        else
          drawStats(m);
        break;
    }
    strcpy(drawn[s], sig);
    drew = true;
  }
  return false;
}

}  // namespace display
