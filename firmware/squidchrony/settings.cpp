#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

namespace {

bool parseFloat(const char* v, float lo, float hi, float* out) {
  if (!v || !*v) return false;
  char* end = nullptr;
  float x = strtof(v, &end);
  if (end == v || *end != '\0') return false;
  if (!(x >= lo && x <= hi)) return false;
  *out = x;
  return true;
}

bool parseBool(const char* v, bool* out) {
  if (!v) return false;
  if (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "yes")) {
    *out = true;
    return true;
  }
  if (!strcmp(v, "0") || !strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "no")) {
    *out = false;
    return true;
  }
  return false;
}

template <typename E>
bool parseEnum(const char* v, const char* const* names, int count, E* out) {
  for (int i = 0; i < count; i++) {
    if (!strcmp(v, names[i])) {
      *out = (E)i;
      return true;
    }
  }
  return false;
}

const char* const kProj[] = {"paintball", "dart"};
const char* const kProjLabel[] = {"Paintball", "Half dart"};
const char* const kSens[] = {"low", "normal", "high"};
const char* const kUnits[] = {"fps", "ms"};
const char* const kReading[] = {"muzzle", "chrono"};
const char* const kMount[] = {"downrange", "barrel"};

}  // namespace

const char* projectileName(Projectile p) { return kProj[(int)p & 1]; }
const char* projectileLabel(Projectile p) { return kProjLabel[(int)p & 1]; }
const char* sensitivityName(Sensitivity s) { return kSens[(int)s < 3 ? (int)s : 1]; }
const char* unitsName(Units u) { return kUnits[(int)u & 1]; }
const char* readingName(Reading r) { return kReading[(int)r & 1]; }
const char* mountName(Mount m) { return kMount[(int)m & 1]; }

bool settingsSet(Settings& s, const char* key, const char* value) {
  if (!key || !value) return false;
  float f;
  bool b;
  if (!strcmp(key, "proj")) return parseEnum(value, kProj, 2, &s.proj);
  if (!strcmp(key, "sens")) return parseEnum(value, kSens, 3, &s.sens);
  if (!strcmp(key, "units")) return parseEnum(value, kUnits, 2, &s.units);
  if (!strcmp(key, "reading")) return parseEnum(value, kReading, 2, &s.reading);
  if (!strcmp(key, "mount")) return parseEnum(value, kMount, 2, &s.mount);
  if (!strcmp(key, "dist_ft")) {
    if (!parseFloat(value, 1.0f, 20.0f, &f)) return false;
    s.dist_ft = f;
    return true;
  }
  if (!strcmp(key, "offset_mm")) {
    if (!parseFloat(value, 5.0f, 300.0f, &f)) return false;
    s.offset_mm = f;
    return true;
  }
  if (!strcmp(key, "ahead_mm")) {
    if (!parseFloat(value, -100.0f, 200.0f, &f)) return false;
    s.ahead_mm = f;
    return true;
  }
  if (!strcmp(key, "auto")) {
    if (!parseBool(value, &b)) return false;
    s.auto_read = b;
    return true;
  }
  if (!strcmp(key, "wifi")) {
    if (!parseBool(value, &b)) return false;
    s.wifi = b;
    return true;
  }
  if (!strcmp(key, "cal")) {
    if (!parseFloat(value, 0.9f, 1.1f, &f)) return false;
    s.cal = f;
    return true;
  }
  if (!strcmp(key, "batt_cal")) {
    if (!parseFloat(value, 0.8f, 1.2f, &f)) return false;
    s.batt_cal = f;
    return true;
  }
  if (!strcmp(key, "k_paintball")) {
    if (!parseFloat(value, 0.001f, 0.5f, &f)) return false;
    s.k_paintball = f;
    return true;
  }
  if (!strcmp(key, "k_dart")) {
    if (!parseFloat(value, 0.001f, 0.5f, &f)) return false;
    s.k_dart = f;
    return true;
  }
  if (!strcmp(key, "backlight_s")) {
    if (!parseFloat(value, 0.0f, 3600.0f, &f)) return false;
    s.backlight_s = (uint16_t)f;
    return true;
  }
  if (!strcmp(key, "wifi_pass")) {
    size_t len = strlen(value);
    if (len != 0 && (len < 8 || len >= sizeof(s.wifi_pass))) return false;
    for (size_t i = 0; i < len; i++)
      if (value[i] < 0x20 || value[i] > 0x7e) return false;
    memcpy(s.wifi_pass, value, len + 1);
    return true;
  }
  return false;
}

size_t settingsToText(const Settings& s, char* out, size_t n) {
  int len = snprintf(out, n,
                     "proj=%s\nsens=%s\nunits=%s\nreading=%s\nmount=%s\ndist_ft=%.2f\n"
                     "offset_mm=%.1f\nahead_mm=%.1f\nauto=%d\nwifi=%d\ncal=%.5f\nbatt_cal=%.4f\n"
                     "k_paintball=%.4f\nk_dart=%.4f\nbacklight_s=%u\nwifi_pass=%s\n",
                     projectileName(s.proj), sensitivityName(s.sens), unitsName(s.units),
                     readingName(s.reading), mountName(s.mount), s.dist_ft, s.offset_mm,
                     s.ahead_mm, s.auto_read ? 1 : 0, s.wifi ? 1 : 0, s.cal, s.batt_cal,
                     s.k_paintball, s.k_dart, (unsigned)s.backlight_s, s.wifi_pass);
  if (len < 0) return 0;
  return (size_t)len < n ? (size_t)len : (n ? n - 1 : 0);
}

int settingsFromText(Settings& s, const char* text) {
  int applied = 0;
  char line[96];
  const char* p = text;
  while (p && *p) {
    const char* nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len < sizeof(line)) {
      memcpy(line, p, len);
      line[len] = '\0';
      if (len && line[len - 1] == '\r') line[len - 1] = '\0';
      char* eq = strchr(line, '=');
      if (eq && line[0] != '#') {
        *eq = '\0';
        if (settingsSet(s, line, eq + 1)) applied++;
      }
    }
    p = nl ? nl + 1 : nullptr;
  }
  return applied;
}

size_t settingsToJson(const Settings& s, char* out, size_t n) {
  int len = snprintf(out, n,
                     "{\"proj\":\"%s\",\"sens\":\"%s\",\"units\":\"%s\",\"reading\":\"%s\","
                     "\"mount\":\"%s\",\"dist_ft\":%.2f,\"offset_mm\":%.1f,\"ahead_mm\":%.1f,"
                     "\"auto\":%s,\"wifi\":%s,\"cal\":%.5f,\"batt_cal\":%.4f,"
                     "\"k_paintball\":%.4f,\"k_dart\":%.4f,\"backlight_s\":%u,\"wifi_locked\":%s}",
                     projectileName(s.proj), sensitivityName(s.sens), unitsName(s.units),
                     readingName(s.reading), mountName(s.mount), s.dist_ft, s.offset_mm,
                     s.ahead_mm, s.auto_read ? "true" : "false", s.wifi ? "true" : "false", s.cal,
                     s.batt_cal, s.k_paintball, s.k_dart, (unsigned)s.backlight_s,
                     s.wifi_pass[0] ? "true" : "false");
  if (len < 0) return 0;
  return (size_t)len < n ? (size_t)len : (n ? n - 1 : 0);
}

float dragConstant(const Settings& s, Projectile p) {
  return p == Projectile::Dart ? s.k_dart : s.k_paintball;
}

dsp::Config toDspConfig(const Settings& s) {
  dsp::Config c;
  c.hz_per_mps = dsp::hzPerMps(RADAR_F0_HZ) / (s.cal > 0.5f ? s.cal : 1.0f);
  c.k_default = dragConstant(s, s.proj);
  c.mount = s.mount == Mount::Barrel ? 1 : 0;
  c.dist_m = s.dist_ft * 0.3048f;
  c.offset_m = s.offset_mm * 0.001f;
  c.muzzle_ahead_m = s.ahead_mm * 0.001f;
  switch (s.sens) {
    case Sensitivity::Low:
      c.detect_db = 18.0f;
      c.min_frames = 8;
      break;
    case Sensitivity::High:
      c.detect_db = 12.0f;
      c.min_frames = 5;
      break;
    default:
      c.detect_db = 15.0f;
      c.min_frames = 6;
      break;
  }
  c.weak_db = c.detect_db + 9.0f;
  return c;
}
