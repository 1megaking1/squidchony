// Squidchrony: a radar chronograph for paintball and foam darts.
//
// Board: Raspberry Pi Pico 2 W with the earlephilhower arduino-pico core.
//   Tools > Board > Raspberry Pi Pico 2W
//   Tools > Flash Size > 4MB (Sketch: 3MB, FS: 1MB)   (so shots and settings are kept)
// Libraries: "Adafruit ST7735 and ST7789 Library" and "Adafruit GFX Library".
//
// Core 1 measures (acquisition.cpp, dsp_task.cpp). Core 0, below, runs the buttons, the
// screen, the LEDs, the battery check, the Wi-Fi page and saving to flash. Anything that
// switches current or talks on SPI waits until no shot is being measured.
//
// Buttons:
//   MODE  hold about a second: switch on. Click: new string. Hold and let go: menu (in the
//         menu MODE = next item, READ = change, hold and let go = done). Hold 3 s: switch off.
//   READ  hold while shooting to measure (or turn on "Auto read" and just shoot)
// It also switches itself off after a while unused (Auto off) and before the battery runs flat,
// and powers the radar only while measuring and for a minute after.
#include <Arduino.h>
#include <math.h>
#include <pico/unique_id.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "acquisition.h"
#include "app.h"
#include "config.h"
#include "controls.h"
#include "display.h"
#include "dsp_task.h"
#include "power.h"
#include "settings.h"
#include "shotlog.h"
#include "storage.h"
#include "web.h"

// Give core 1 its own stack instead of sharing core 0's.
bool core1_separate_stack = true;

namespace app {
Settings settings;
ShotLog shots;
}  // namespace app

using app::settings;
using app::shots;

namespace {

volatile bool core0_ready = false;
char ssid_buf[20];

PowerPolicy power;
bool power_button_boot = false;
bool radar_pin = false;
bool want_measure_now = false;  // READ held, auto read, or the phone's Measure switch
bool measuring_now = false;     // ...and the radar is powered and settled
bool web_activity = false;
uint32_t off_in_ms = 0;
const char* status_msg = nullptr;  // live status shown instead of the shot line
char status_buf[40];
uint32_t last_activity_ms = 0;
uint32_t last_shot_ms = 0;
uint32_t last_flush_ms = 0;
bool web_arm = false;
uint32_t web_arm_until = 0;
bool wall_known = false;
int64_t wall_offset_ms = 0;
float batt_v = 0.0f;
int batt_pct = -1;
bool batt_low = false;
uint32_t last_batt_ms = 0;
bool settings_dirty = false;
uint32_t settings_dirty_ms = 0;
bool log_rewrite = false;
uint32_t lost_seen = 0;
uint32_t blink_until = 0;

char message_buf[40];
uint32_t message_until = 0;

bool menu_open = false;
int menu_index = 0;
uint32_t menu_touched = 0;
char menu_value_buf[24];

void say(uint32_t ms, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(message_buf, sizeof message_buf, fmt, ap);
  va_end(ap);
  message_until = millis() + ms;
}

// ---------------------------------------------------------------- battery

int batteryPercent(float pack_v) {
  // 2S Li-ion, resting voltage per cell
  static const float cell[] = {3.00f, 3.30f, 3.50f, 3.60f, 3.70f, 3.80f, 3.90f, 4.00f, 4.10f, 4.20f};
  static const int pct[] = {0, 3, 10, 20, 35, 50, 65, 80, 90, 100};
  float v = pack_v / 2.0f;
  if (v <= cell[0]) return 0;
  for (int i = 1; i < 10; i++)
    if (v <= cell[i]) return pct[i - 1] + (int)((pct[i] - pct[i - 1]) * (v - cell[i - 1]) / (cell[i] - cell[i - 1]));
  return 100;
}

void updateBattery(uint32_t now) {
  if (now - last_batt_ms < 500) return;
  last_batt_ms = now;
  acq::Levels l = acq::levels();
  if (l.battery_counts < 50.0f) return;  // nothing measured yet
  float v = l.battery_counts / 4095.0f * ADC_VREF * BATT_DIVIDER * settings.batt_cal + BATT_DIODE_V;
  batt_v = batt_v > 0.0f ? batt_v + 0.2f * (v - batt_v) : v;
  batt_pct = batteryPercent(batt_v);
  bool low = batt_v < 2.0f * BATT_LOW_CELL_V;
  if (low && !batt_low) say(4000, "Battery low: charge soon");
  batt_low = low;
}

// ---------------------------------------------------------------- shots

void handleShot(const dsptask::ShotMsg& m, uint32_t now) {
  Shot s{};
  s.muzzle_mps = m.r.v_muzzle_mps;
  s.chrono_mps = m.r.v_chrono_mps;
  s.sigma_mps = m.r.sigma_mps;
  s.snr_db = m.r.peak_snr_db;
  s.k = m.r.k_used;
  s.offset_m = m.r.offset_m;
  s.flags = m.r.flags;
  s.time_s = app::wallClockS();
  s.boot = shots.boot();
  s.t_ms = m.r.first_sample / (acq::DEC_RATE / 1000);
  if (!shots.addShot(s)) say(4000, "Log full: start a new string");
  last_shot_ms = now;
  last_activity_ms = now;
  blink_until = now + 150;
  if (m.r.flags & dsp::SHOT_CLIPPED) say(2500, "Echo clipped: move back");
}

// ---------------------------------------------------------------- menu

constexpr int MENU_N = 10;
const char* const MENU_NAMES[MENU_N] = {"Projectile", "Report", "Units", "Sensitivity", "Mounting",
                                        "Distance", "Auto read", "Auto off", "Wi-Fi", "Delete last"};
const float DISTANCES[] = {2.0f, 2.5f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f};
const uint16_t AUTO_OFF_MIN[] = {5, 15, 30, 60, 0};

bool menuVisible(int i) { return i != 5 || settings.mount == Mount::Downrange; }

const char* menuValue(int i) {
  char* b = menu_value_buf;
  size_t n = sizeof menu_value_buf;
  switch (i) {
    case 0: snprintf(b, n, "%s", projectileLabel(settings.proj)); break;
    case 1: snprintf(b, n, "%s", settings.reading == Reading::Chrono ? "At chrono" : "Muzzle"); break;
    case 2: snprintf(b, n, "%s", settings.units == Units::Mps ? "m/s" : "fps"); break;
    case 3: snprintf(b, n, "%s", settings.sens == Sensitivity::Low ? "Low" : settings.sens == Sensitivity::High ? "High" : "Normal"); break;
    case 4: snprintf(b, n, "%s", settings.mount == Mount::Barrel ? "Barrel" : "Downrange"); break;
    case 5: snprintf(b, n, "%.1f ft", settings.dist_ft); break;
    case 6: snprintf(b, n, "%s", settings.auto_read ? "On" : "Off"); break;
    case 7:
      if (settings.auto_off_min) snprintf(b, n, "%u min", (unsigned)settings.auto_off_min);
      else snprintf(b, n, "Never");
      break;
    case 8: snprintf(b, n, "%s", settings.wifi ? "On" : "Off"); break;
    default: snprintf(b, n, "READ deletes"); break;
  }
  return b;
}

void menuChange(int i) {
  char buf[12];
  switch (i) {
    case 0: app::setSetting("proj", settings.proj == Projectile::Dart ? "paintball" : "dart"); break;
    case 1: app::setSetting("reading", settings.reading == Reading::Chrono ? "muzzle" : "chrono"); break;
    case 2: app::setSetting("units", settings.units == Units::Mps ? "fps" : "ms"); break;
    case 3: app::setSetting("sens", settings.sens == Sensitivity::Low ? "normal" : settings.sens == Sensitivity::Normal ? "high" : "low"); break;
    case 4: app::setSetting("mount", settings.mount == Mount::Barrel ? "downrange" : "barrel"); break;
    case 5: {
      int k = 0;
      const int nd = sizeof(DISTANCES) / sizeof(DISTANCES[0]);
      while (k < nd && DISTANCES[k] <= settings.dist_ft + 0.01f) k++;
      snprintf(buf, sizeof buf, "%.1f", DISTANCES[k % nd]);
      app::setSetting("dist_ft", buf);
      break;
    }
    case 6: app::setSetting("auto", settings.auto_read ? "0" : "1"); break;
    case 7: {
      const int na = sizeof(AUTO_OFF_MIN) / sizeof(AUTO_OFF_MIN[0]);
      int k = 0;
      while (k < na && AUTO_OFF_MIN[k] != settings.auto_off_min) k++;
      snprintf(buf, sizeof buf, "%u", (unsigned)AUTO_OFF_MIN[(k + 1) % na]);
      app::setSetting("auto_off_min", buf);
      break;
    }
    case 8: app::setSetting("wifi", settings.wifi ? "0" : "1"); break;
    default:
      if (app::deleteLast()) say(2500, "Deleted the last shot");
      else say(2500, "No shots to delete");
      return;
  }
  app::settingsChanged();
}

void menuNext() {
  for (int k = 0; k < MENU_N; k++) {
    menu_index = (menu_index + 1) % MENU_N;
    if (menuVisible(menu_index)) return;
  }
}

// ---------------------------------------------------------------- buttons

void handleButton(controls::Event e, uint32_t now) {
  static uint32_t ignore_until = 0;
  if (e == controls::NONE) return;
  last_activity_ms = now;
  if (!display::backlight()) {
    // The press that wakes the screen does nothing else (holding READ still measures).
    display::setBacklight(true);
    ignore_until = now + 600;
    return;
  }
  if (e == controls::MODE_OFF) {
    power.requestOff();
    return;
  }
  if ((int32_t)(ignore_until - now) > 0 && e != controls::MODE_LONG) return;
  if (menu_open) {
    menu_touched = now;
    if (e == controls::MODE_CLICK) menuNext();
    else if (e == controls::READ_CLICK) menuChange(menu_index);
    else if (e == controls::MODE_LONG) menu_open = false;
    return;
  }
  switch (e) {
    case controls::MODE_CLICK:
      app::newString();
      say(2500, "New string: %s", shots.current().name);
      break;
    case controls::MODE_LONG:
      menu_open = true;
      menu_index = 0;
      menu_touched = now;
      break;
    default:
      break;
  }
}

// ---------------------------------------------------------------- screen

void buildModel(display::Model& m, uint32_t now) {
  const bool fps = settings.units == Units::Fps;
  const ShotString& cur = shots.current();
  m.reading = settings.reading == Reading::Chrono ? "AT CHRONO" : "MUZZLE";
  m.proj = cur.proj == Projectile::Dart ? "DART" : "PAINT";
  m.units = fps ? "fps" : "m/s";
  m.decimals = fps ? 1 : 2;
  m.armed = measuring_now;
  m.auto_read = settings.auto_read;
  m.wifi = web::running() ? (web::clients() > 0 ? 2 : 1) : 0;
  m.batt_v = batt_v;
  m.batt_pct = batt_pct;
  m.batt_low = batt_low;

  ShotStats st = shots.stats(cur, settings.reading);
  m.n = st.n;
  m.avg = toUserUnits(settings, st.mean);
  m.sd = toUserUnits(settings, st.sd);
  m.es = toUserUnits(settings, st.es);
  m.lo = toUserUnits(settings, st.lo);
  m.hi = toUserUnits(settings, st.hi);
  m.rof = st.rof_bps;

  const Shot* last = shots.lastShot();
  m.have_shot = last != nullptr;
  if (last) {
    float v = settings.reading == Reading::Chrono ? last->chrono_mps : last->muzzle_mps;
    m.value = toUserUnits(settings, v);
    m.shot_no = st.n;
    m.snr_db = last->snr_db;
    m.weak = last->flags & (dsp::SHOT_WEAK | dsp::SHOT_POOR_FIT | dsp::SHOT_OFF_LINE);
    m.has_vs_avg = st.n > 1;
    m.vs_avg = m.value - m.avg;
  }
  m.message = status_msg ? status_msg : ((int32_t)(message_until - now) > 0 ? message_buf : nullptr);
  m.menu = menu_open;
  if (menu_open) {
    int count = 0, pos = 0;
    for (int i = 0; i < MENU_N; i++) {
      if (!menuVisible(i)) continue;
      if (i == menu_index) pos = count;
      count++;
    }
    m.menu_index = pos;
    m.menu_count = count;
    m.menu_item = MENU_NAMES[menu_index];
    m.menu_value = menuValue(menu_index);
  }
}

// ---------------------------------------------------------------- quiet-time work

void updateLeds(uint32_t now) {
  bool read_led = measuring_now && (int32_t)(now - blink_until) >= 0;
  bool mode_led = false;
  if (web::running()) mode_led = web::clients() > 0 || (now / 500) % 2;
  controls::setLeds(read_led, mode_led);
}

void saveWhenIdle(uint32_t now) {
  // Flash writes pause core 1, so wait for a lull in the shooting.
  if (controls::readHeld() || now - last_shot_ms < 3000) return;
  if (settings_dirty && now - settings_dirty_ms > 1500) {
    storage::saveSettings(settings);
    settings_dirty = false;
    return;  // one write per pass
  }
  if (now - last_flush_ms < 2000) return;
  if (log_rewrite) {
    storage::rewriteLog(shots);
    log_rewrite = false;
    last_flush_ms = now;
  } else if (shots.journalLength() || shots.overflowed()) {
    storage::flushLog(shots);
    last_flush_ms = now;
  }
}

void matchStringToSettings() {
  const ShotString& cur = shots.current();
  if (cur.proj == settings.proj && cur.mount == settings.mount) return;
  if (!shots.retypeCurrent(settings.proj, settings.mount))
    shots.newString(settings.proj, settings.mount, app::wallClockS());
}

// ---------------------------------------------------------------- switching off

// After letting go of the power switch the Pico normally dies within a fraction of a second.
// It keeps running while MODE is still held (its diode holds the switch on) or when it is
// powered over USB for programming; say so instead of looking frozen.
[[noreturn]] void waitForPowerLoss() {
  // Debounced, so a bounce while MODE is held can't look like a release and then a new press.
  bool raw = digitalRead(PIN_BTN_MODE) == LOW;
  bool down = raw;
  uint32_t raw_since = millis(), changed = raw_since;
  int shown = 0;  // 1: asked to let go, 2: screen off again (MODE stuck?), 3: on USB power
  for (;;) {
    uint32_t now = millis();
    bool r = digitalRead(PIN_BTN_MODE) == LOW;
    if (r != raw) {
      raw = r;
      raw_since = now;
    }
    if (raw != down && now - raw_since >= 30) {
      down = raw;
      changed = now;
      if (down && shown == 3) rp2040.reboot();  // on USB power: start again
    }
    uint32_t t = now - changed;
    if (down && shown == 0 && t > 1000) {
      display::setBacklight(true);
      display::splash("LET GO", "Release MODE to turn off");
      shown = 1;
    } else if (down && shown == 1 && t > 6000) {
      display::setBacklight(false);  // still held: something is pressing it; draw less
      shown = 2;
    } else if (!down && shown != 3 && t > 1500) {
      display::setBacklight(true);
      display::splash("USB POWER", "Hold MODE to restart");
      shown = 3;
    }
    delay(5);
  }
}

[[noreturn]] void shutdown(OffReason reason) {
  dsptask::setArmed(false);
  dsptask::setRadarPowered(false);
  digitalWrite(PIN_RADAR_EN, LOW);
  controls::setLeds(false, false);
  if (reason != OffReason::BootTap) {
    // Save first: flash writes pause core 1, which no longer matters.
    display::setBacklight(true);
    display::splash("TURNING OFF", offReasonText(reason));
    if (settings_dirty) storage::saveSettings(settings);
    if (log_rewrite)
      storage::rewriteLog(shots);
    else
      storage::flushLog(shots);
    web::stop();
    display::splash("OFF", "Hold MODE to switch on");
    delay(700);
  }
  display::setBacklight(false);
  digitalWrite(PIN_POWER_HOLD, LOW);
  waitForPowerLoss();
}

void holdPower() { digitalWrite(PIN_POWER_HOLD, HIGH); }

// One pass of the power policy: radar on/off, measuring, switching off.
PowerOutputs runPower(uint32_t now, bool activity) {
  PowerInputs in;
  in.now_ms = now;
  in.want_measure = want_measure_now;
  in.activity = activity;
  in.mode_held = controls::modeHeld();
  in.batt_valid = batt_pct >= 0;
  in.batt_v = batt_v;
  PowerOutputs po = power.update(in);
  if (po.off != OffReason::None) shutdown(po.off);
  if (po.radar_on != radar_pin) {
    radar_pin = po.radar_on;
    digitalWrite(PIN_RADAR_EN, radar_pin ? HIGH : LOW);
    dsptask::setRadarPowered(radar_pin);
  }
  if (po.radar_settled) dsptask::relearnBackground();
  measuring_now = po.measuring;
  dsptask::setArmed(measuring_now);
  off_in_ms = po.off_in_ms;
  return po;
}

}  // namespace

// ---------------------------------------------------------------- app interface

namespace app {

Live live() {
  Live l;
  l.batt_v = batt_v;
  l.batt_pct = batt_pct;
  l.batt_low = batt_low;
  l.armed = measuring_now;
  l.want_measure = want_measure_now;
  l.radar_on = radar_pin;
  l.off_in_s = (off_in_ms + 999) / 1000;
  l.held = controls::readHeld();
  l.web_armed = web_arm && (int32_t)(web_arm_until - millis()) > 0;
  l.busy = dsptask::busy();
  l.clients = web::clients();
  l.dsp = dsptask::status();
  return l;
}

uint32_t wallClockS() {
  if (!wall_known) return 0;
  return (uint32_t)(((int64_t)millis() + wall_offset_ms) / 1000);
}

void setWallClockMs(uint64_t epoch_ms) {
  wall_offset_ms = (int64_t)epoch_ms - (int64_t)millis();
  wall_known = true;
}

bool setSetting(const char* key, const char* value) { return settingsSet(settings, key, value); }

void settingsChanged() {
  dsptask::setConfig(toDspConfig(settings));
  power.setConfig(toPowerConfig(settings));
  matchStringToSettings();
  if (settings.wifi && !web::running()) web::begin();
  if (!settings.wifi && web::running()) web::stop();
  digitalWrite(PIN_SMPS_MODE, HIGH);
  settings_dirty = true;
  settings_dirty_ms = millis();
}

void newString() { shots.newString(settings.proj, settings.mount, wallClockS()); }

bool deleteLast() { return shots.deleteLast(); }

bool undeleteLast() { return shots.undeleteLast(); }

bool renameString(uint16_t id, const char* name) { return shots.rename(id, name); }

bool removeString(uint16_t id) { return shots.removeString(id); }

void clearAll() {
  shots.clear();
  shots.startBoot();
  shots.newString(settings.proj, settings.mount, wallClockS());
  log_rewrite = true;
}

bool calibrate(float ref_mps) {
  ShotStats st = shots.stats(shots.current(), settings.reading);
  if (st.n < 3 || !(ref_mps > 0.0f) || !(st.mean > 0.0f)) return false;
  float ratio = ref_mps / st.mean;
  if (fabsf(ratio - 1.0f) > 0.10f) return false;
  char buf[16];
  snprintf(buf, sizeof buf, "%.5f", settings.cal * ratio);
  if (!setSetting("cal", buf)) return false;
  settingsChanged();
  say(4000, "Calibration x%.4f", settings.cal);
  return true;
}

void webArm(bool on) {
  web_arm = on;
  web_arm_until = millis() + 60000;  // the page renews it while its switch is on
}

void touch() {
  last_activity_ms = millis();
  web_activity = true;
}

void powerOff() { power.requestOff(); }

const char* ssid() { return ssid_buf; }

}  // namespace app

// ---------------------------------------------------------------- core 0

void setup() {
  // MODE held now means the power button switched us on: hold the soft switch on at once.
  power_button_boot = controls::begin();
  pinMode(PIN_POWER_HOLD, OUTPUT);
  digitalWrite(PIN_POWER_HOLD, LOW);
  if (power_button_boot) holdPower();
  pinMode(PIN_RADAR_EN, OUTPUT);
  digitalWrite(PIN_RADAR_EN, LOW);  // the power policy switches the radar on when wanted
  pinMode(PIN_SMPS_MODE, OUTPUT);
  digitalWrite(PIN_SMPS_MODE, HIGH);  // power supply in PWM mode: no bursts in the radar band
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);

  display::begin();
  display::splash("SQUIDCHRONY", power_button_boot ? "Keep holding MODE" : "Starting " FIRMWARE_VERSION);

  // Powered some other way (USB): switch the battery in only after a second, and before
  // touching flash. A Pico that restarts on the capacitors' last charge while switching off
  // dies during this wait instead of switching itself back on.
  if (!power_button_boot) {
    while (millis() < 1000) delay(5);
    holdPower();
  }

  bool fs = storage::begin();
  if (fs) storage::loadSettings(settings);
  shots.clear();
  if (fs) storage::loadLog(shots);
  shots.startBoot();
  if (!shots.stringCount())
    shots.newString(settings.proj, settings.mount, 0);
  else
    matchStringToSettings();

  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  snprintf(ssid_buf, sizeof ssid_buf, "%s%02X%02X", AP_PREFIX, id.id[6], id.id[7]);

  dsptask::init(toDspConfig(settings));
  dsptask::setRadarPowered(false);
  core0_ready = true;  // core 1 may start measuring

  // A power-button boot only sticks if MODE stays held a moment longer (a knock in a bag
  // switches straight back off). Check that before starting Wi-Fi.
  power.begin(millis(), power_button_boot, toPowerConfig(settings));
  for (;;) {
    uint32_t now = millis();
    while (controls::poll(now) != controls::NONE) {
    }
    if (!runPower(now, false).booting) break;
    delay(5);
  }

  if (settings.wifi) web::begin();
  digitalWrite(PIN_SMPS_MODE, HIGH);

  display::invalidate();
  if (!fs) say(5000, "No flash storage set up");
  else if (settings.wifi) say(4000, "Wi-Fi %s", ssid_buf);
  last_activity_ms = millis();
}

void loop() {
  const uint32_t now = millis();
  bool activity = false;

  dsptask::ShotMsg msg;
  while (dsptask::popShot(&msg)) {
    handleShot(msg, now);
    activity = true;
  }

  controls::Event e;
  while ((e = controls::poll(now)) != controls::NONE) {
    handleButton(e, now);
    activity = true;
  }
  if (menu_open && now - menu_touched > 30000) menu_open = false;

  bool held = controls::readHeld() && !menu_open;
  bool web_armed = web_arm && (int32_t)(web_arm_until - now) > 0;
  want_measure_now = settings.auto_read || held || web_armed;
  // Pressing READ counts as use, but just holding it doesn't: a button stuck down in a bag
  // must not keep the chrono on.
  if (web_activity) activity = true;
  web_activity = false;

  runPower(now, activity);

  status_msg = nullptr;
  uint32_t mode_ms = controls::modeHeldMs(now);
  if (mode_ms > 1200) {
    status_msg = menu_open ? "Let go: done. Hold: off" : "Let go: menu. Hold: off";
  } else if (off_in_ms) {
    snprintf(status_buf, sizeof status_buf, "Off in %lu s: tap a button",
             (unsigned long)((off_in_ms + 999) / 1000));
    status_msg = status_buf;
  } else if (want_measure_now && !measuring_now) {
    status_msg = "Radar warming up";
  }

  // A track that never became a shot while measuring: tell the shooter.
  dsptask::Status st = dsptask::status();
  if (st.lost != lost_seen) {
    if (measuring_now && now - last_shot_ms > 300) say(2500, "No reading: weak echo");
    lost_seen = st.lost;
  }

  web::loop();
  updateBattery(now);

  if (dsptask::busy()) return;  // a shot is in the air: nothing noisy now

  updateLeds(now);
  bool screen_on = settings.backlight_s == 0 || now - last_activity_ms < settings.backlight_s * 1000u ||
                   web::clients() > 0;
  if (screen_on != display::backlight()) display::setBacklight(screen_on);
  if (display::backlight()) {
    static display::Model model;
    buildModel(model, now);
    display::update(model);
  }
  saveWhenIdle(now);
}

// ---------------------------------------------------------------- core 1

void setup1() {
  while (!core0_ready) delay(1);
  dsptask::begin();
}

void loop1() { dsptask::loop(); }
