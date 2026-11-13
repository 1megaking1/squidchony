// Host tests for the firmware's settings and shot log (firmware/squidchrony/settings.*,
// shotlog.*). Exits non-zero if a check fails.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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

}  // namespace

int main() {
  testSettings();
  testShotLog();
  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
