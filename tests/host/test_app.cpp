// Host tests for the firmware's settings and shot log (firmware/squidchrony/settings.*,
// shotlog.*). Exits non-zero if a check fails.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "power.h"
#include "settings.h"
#include "shotlog.h"

namespace {

int g_failures = 0;
void check(bool ok, const char* what) {
  if (!ok) {
    std::printf("  [FAIL] %s\n", what);
    g_failures++;
  } else {
    std::printf("  [PASS] %s\n", what);
  }
}

bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

Shot makeShot(float muzzle, float chrono, uint32_t t_ms, uint16_t boot = 1) {
  Shot s{};
  s.muzzle_mps = muzzle;
  s.chrono_mps = chrono;
  s.sigma_mps = 0.05f;
  s.snr_db = 30.0f;
  s.k = 0.022f;
  s.offset_m = 0.07f;
  s.t_ms = t_ms;
  s.boot = boot;
  return s;
}

void collect(const char* line, void* ctx) {
  static_cast<std::vector<std::string>*>(ctx)->push_back(line);
}

// Two logs hold the same strings and live shots.
bool sameContent(const ShotLog& a, const ShotLog& b) {
  if (a.stringCount() != b.stringCount()) return false;
  for (int k = 0; k < a.stringCount(); k++) {
    const ShotString &sa = a.string(k), &sb = b.string(k);
    if (sa.id != sb.id || strcmp(sa.name, sb.name) || sa.proj != sb.proj || sa.count != sb.count)
      return false;
    if (a.liveCount(sa) != b.liveCount(sb)) return false;
    for (int i = 0; i < a.liveCount(sa); i++) {
      const Shot *x = a.liveShot(sa, i), *y = b.liveShot(sb, i);
      if (!near(x->muzzle_mps, y->muzzle_mps, 0.002f) || x->t_ms != y->t_ms) return false;
    }
  }
  return true;
}

void testSettings() {
  std::printf("Settings\n");
  Settings s;
  check(settingsSet(s, "proj", "dart") && s.proj == Projectile::Dart, "set projectile");
  check(!settingsSet(s, "proj", "rock") && s.proj == Projectile::Dart, "bad value is refused");
  check(!settingsSet(s, "dist_ft", "0.2") && s.dist_ft == 3.0f, "out-of-range distance is refused");
  check(settingsSet(s, "dist_ft", "4.5") && s.dist_ft == 4.5f, "set distance");
  check(!settingsSet(s, "wifi_pass", "short"), "Wi-Fi password under 8 characters is refused");
  check(settingsSet(s, "wifi_pass", "paintball1") && !strcmp(s.wifi_pass, "paintball1"), "set password");
  check(settingsSet(s, "wifi_pass", "") && s.wifi_pass[0] == 0, "empty password = open network");
  check(!settingsSet(s, "nonsense", "1"), "unknown key is refused");
  settingsSet(s, "sens", "high");
  settingsSet(s, "mount", "barrel");
  settingsSet(s, "cal", "1.012");
  settingsSet(s, "auto", "1");

  char text[512];
  settingsToText(s, text, sizeof text);
  Settings t;
  int n = settingsFromText(t, text);
  check(n >= 15 && t.proj == s.proj && t.sens == s.sens && t.mount == s.mount && t.auto_read &&
            near(t.cal, 1.012f, 1e-5f) && near(t.dist_ft, 4.5f, 1e-4f),
        "settings survive a save and load");
  check(settingsFromText(t, "garbage\nproj=paintball\r\n=x\n") == 1 && t.proj == Projectile::Paintball,
        "bad lines in the file are skipped");

  char json[512];
  size_t len = settingsToJson(s, json, sizeof json);
  check(len > 50 && strstr(json, "\"proj\":\"dart\"") && strstr(json, "\"auto\":true") &&
            !strstr(json, "paintball1"),
        "settings JSON (and no password in it)");

  dsp::Config c = toDspConfig(s);
  check(c.mount == 1 && near(c.k_default, s.k_dart, 1e-6f) && c.detect_db < 15.0f,
        "DSP config follows mount, drag and sensitivity");
  check(near(c.hz_per_mps, dsp::hzPerMps(24.125e9f) / 1.012f, 0.01f), "calibration scales speeds");
}

void testShotLog() {
  std::printf("Shot log\n");
  static ShotLog log;  // large; keep it off the stack
  log.clear();
  log.startBoot();
  log.newString(Projectile::Paintball, Mount::Downrange, 1700000000, "Marker A");
  const float v[] = {86.0f, 86.5f, 85.5f, 86.2f, 85.8f};
  uint32_t t = 10000;
  for (float x : v) {
    log.addShot(makeShot(x, x - 1.5f, t));
    t += 80;  // a 12.5 balls/s burst
  }
  ShotStats st = log.stats(log.current(), Reading::Muzzle);
  check(st.n == 5 && near(st.mean, 86.0f, 1e-4f), "count and average");
  check(near(st.sd, 0.3808f, 1e-3f) && near(st.es, 1.0f, 1e-4f) && st.lo == 85.5f && st.hi == 86.5f,
        "SD, extreme spread, low and high");
  check(near(st.rof_bps, 12.5f, 0.01f), "rate of fire from the burst");
  ShotStats sc = log.stats(log.current(), Reading::Chrono);
  check(near(sc.mean, 84.5f, 1e-4f), "stats at the chrono use the chrono speeds");

  check(log.deleteLast() && log.liveCount(log.current()) == 4, "delete last shot");
  check(log.deleteLast() && log.liveCount(log.current()) == 3, "delete another");
  check(log.undeleteLast() && log.liveShot(log.current(), 3)->muzzle_mps == 86.2f,
        "undo restores the most recent deletion first");
  check(log.rename(log.current().id, "Marker A, new reg") && !strcmp(log.current().name, "Marker A, new reg"),
        "rename");

  log.newString(Projectile::Paintball, Mount::Downrange, 1700000100, nullptr);
  check(log.stringCount() == 2 && !strcmp(log.current().name, "String 2"), "new string gets a default name");
  check(log.retypeCurrent(Projectile::Dart, Mount::Barrel) && log.current().proj == Projectile::Dart,
        "an empty string can change projectile");
  log.addShot(makeShot(45.0f, 45.0f, 50000));
  log.addShot(makeShot(46.0f, 46.0f, 56000));  // 6 s later: no burst
  check(log.stats(log.current(), Reading::Muzzle).rof_bps == 0.0f, "no rate of fire without a burst");

  // Journal replay reproduces the log.
  char journal[ShotLog::JOURNAL_MAX];
  size_t jl = log.takeJournal(journal, sizeof journal);
  static ShotLog replay;
  replay.clear();
  int bad = 0;
  for (char* line = strtok(journal, "\n"); line; line = strtok(nullptr, "\n"))
    if (!replay.apply(line)) bad++;
  check(jl > 0 && bad == 0 && sameContent(log, replay), "journal replay rebuilds the same log");
  check(replay.boot() == 1, "boot counter replays");

  // A full dump replays to the same log too (used to compact the file).
  std::vector<std::string> lines;
  log.dump(collect, &lines);
  static ShotLog fromDump;
  fromDump.clear();
  bad = 0;
  for (auto& l : lines)
    if (!fromDump.apply(l.c_str())) bad++;
  check(bad == 0 && sameContent(log, fromDump), "dump replays to the same log");

  check(!replay.apply("H 99 1 2 3 4 5 6 7 8 9 10"), "shot for a missing string is rejected");
  check(!replay.apply("S x y"), "malformed line is rejected");

  // Removing an earlier string keeps the rest consistent.
  uint16_t first_id = log.string(0).id;
  check(log.removeString(first_id) && log.stringCount() == 1 && log.liveCount(log.current()) == 2,
        "remove an earlier string");
  check(!log.removeString(log.current().id), "the current string cannot be removed");
  check(!log.retypeCurrent(Projectile::Paintball, Mount::Downrange), "a string with shots keeps its projectile");

  // Capacity: the oldest strings make room.
  log.clear();
  log.startBoot();
  for (int k = 0; k < ShotLog::MAX_STRINGS + 5; k++) {
    log.newString(Projectile::Paintball, Mount::Downrange, 0);
    log.addShot(makeShot(80.0f + k * 0.01f, 79.0f, 1000 * k));
  }
  check(log.stringCount() == ShotLog::MAX_STRINGS, "string count stays at the limit");
  int added = 0;
  for (int i = 0; i < ShotLog::MAX_SHOTS + 10; i++) added += log.addShot(makeShot(85.0f, 84.0f, i));
  check(log.stringCount() == 1 && added == ShotLog::MAX_SHOTS - 1,
        "a full log drops old strings, then refuses shots");
  check(log.overflowed(), "journal overflow is reported (the file gets rewritten)");
}

// Runs the policy in 10 ms steps from `from` to `to` (inclusive), returning the last output and
// whether any step asked to switch off or reported the radar settling.
struct Sim {
  PowerPolicy p;
  PowerInputs in;
  bool any_settled = false;
  PowerOutputs run(uint32_t from, uint32_t to) {
    PowerOutputs out;
    for (uint32_t t = from; t <= to; t += 10) {
      in.now_ms = t;
      out = p.update(in);
      in.activity = false;  // activity is an event, not a state
      if (out.radar_settled) any_settled = true;
      if (out.off != OffReason::None) break;
    }
    return out;
  }
};

void testPower() {
  std::printf("Power\n");
  PowerConfig cfg;  // defaults: boot hold 400 ms, auto-off 15 min, radar hold 60 s, settle 500 ms
  {
    Sim s;
    s.p.begin(0, true, cfg);
    s.in.mode_held = true;
    PowerOutputs o = s.run(0, 300);
    check(o.booting && !o.radar_on, "while MODE is still being checked: booting, radar off");
    s.in.mode_held = false;
    o = s.run(310, 400);
    check(o.off == OffReason::BootTap && !o.radar_on, "a quick tap on MODE switches straight back off");
  }
  {
    Sim s;
    s.p.begin(0, true, cfg);
    s.in.mode_held = true;
    PowerOutputs o = s.run(0, 700);
    check(o.off == OffReason::None && !o.booting, "holding MODE through the check keeps it on");
    s.in.mode_held = false;
    o = s.run(710, 1400);
    check(o.off == OffReason::None && o.radar_on, "letting go after the check is fine; the radar comes on");
    s.in.mode_held = true;  // a later long press (the firmware's own 3 s hold switches off)
    o = s.run(1410, 30000);
    check(o.off == OffReason::None, "a later long press is not a stuck button");
  }
  {
    Sim s;
    s.p.begin(0, true, cfg);
    s.in.mode_held = true;
    PowerOutputs o = s.run(0, 9900);
    check(o.off == OffReason::None, "MODE held from switch-on: still on after 9.9 s...");
    o = s.run(9910, 10100);
    check(o.off == OffReason::Stuck, "...and off at 10 s (stuck down in a bag?)");
  }
  {
    Sim s;
    s.p.begin(0, false, cfg);  // powered some other way (USB)
    PowerOutputs o = s.run(0, 100);
    check(o.off == OffReason::None && o.radar_on, "no power-button check without a button boot");
    check(!o.measuring, "not measuring while the radar settles");
    s.in.want_measure = true;
    o = s.run(110, 600);
    check(o.measuring && s.any_settled, "measuring once the radar has settled (half a second)");
    s.in.want_measure = false;
    o = s.run(610, 60000);
    check(o.radar_on && !o.measuring, "radar stays on for a while after READ is let go");
    o = s.run(60010, 61000);
    check(!o.radar_on, "radar off a minute after it was last needed");
    s.any_settled = false;
    s.in.want_measure = true;
    o = s.run(61010, 61400);
    check(o.radar_on && !o.measuring, "pressing READ powers the radar back up...");
    o = s.run(61410, 61600);
    check(o.measuring && s.any_settled, "...and measures half a second later");
  }
  {
    PowerConfig always = cfg;
    always.radar_hold_ms = 0;
    Sim s;
    s.p.begin(0, false, always);
    PowerOutputs o = s.run(0, 10 * 60000);
    check(o.radar_on, "radar hold 0 keeps the radar on");
  }
  {
    Sim s;
    s.p.begin(0, false, cfg);
    PowerOutputs o = s.run(0, 14 * 60000 + 50000);
    check(o.off == OffReason::None && o.off_in_ms > 0 && o.off_in_ms <= 15000,
          "warns in the last 15 s before an idle shutdown");
    s.in.activity = true;
    o = s.run(14 * 60000 + 50010, 14 * 60000 + 50020);
    check(o.off_in_ms == 0, "any use cancels the countdown");
    o = s.run(14 * 60000 + 50030, 30 * 60000);
    check(o.off == OffReason::Idle, "switches off after 15 idle minutes");
  }
  {
    Sim s;
    s.p.begin(0, false, cfg);
    s.in.want_measure = true;  // auto read: always armed, but nobody touches it
    PowerOutputs o = s.run(0, 16 * 60000);
    check(o.off == OffReason::Idle, "auto read alone doesn't keep it on forever");
  }
  {
    PowerConfig never = cfg;
    never.auto_off_ms = 0;
    Sim s;
    s.p.begin(0, false, never);
    PowerOutputs o = s.run(0, 120 * 60000);
    check(o.off == OffReason::None && o.off_in_ms == 0, "auto-off 0 means never");
  }
  {
    Sim s;
    s.p.begin(0, false, cfg);
    s.in.batt_valid = true;
    s.in.batt_v = 7.4f;
    s.run(0, 1000);
    s.in.batt_v = 6.3f;
    PowerOutputs o = s.run(1010, 5000);
    s.in.batt_v = 6.6f;  // a dip that recovers
    o = s.run(5010, 6000);
    s.in.batt_v = 6.3f;
    o = s.run(6010, 15000);
    check(o.off == OffReason::None, "a short dip below 6.4 V doesn't switch it off");
    o = s.run(15010, 17000);
    check(o.off == OffReason::Battery, "10 s below 6.4 V switches it off");
    Sim u;
    u.p.begin(0, false, cfg);
    u.in.batt_valid = false;  // USB power: no battery reading
    o = u.run(0, 60000);
    check(o.off == OffReason::None, "no battery reading, no low-battery shutdown");
  }
  {
    Sim s;
    s.p.begin(0, false, cfg);
    s.run(0, 1000);
    s.p.requestOff();
    PowerOutputs o = s.run(1010, 1010);
    check(o.off == OffReason::User && !o.measuring, "turning off on request");
  }
  {
    Settings st;
    settingsSet(st, "auto_off_min", "30");
    settingsSet(st, "radar_hold_s", "0");
    PowerConfig pc = toPowerConfig(st);
    check(pc.auto_off_ms == 30u * 60000u && pc.radar_hold_ms == 0, "power settings convert");
    check(!settingsSet(st, "auto_off_min", "500"), "auto-off over 4 hours is refused");
  }
}

}  // namespace

int main() {
  testSettings();
  testShotLog();
  testPower();
  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
