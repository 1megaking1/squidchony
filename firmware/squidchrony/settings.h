// User settings: what they are, how they are stored (key=value text), and how they turn
// into the signal-processing configuration. Portable C++ (host-tested).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "dsp.h"

enum class Projectile : uint8_t { Paintball = 0, Dart = 1 };
enum class Sensitivity : uint8_t { Low = 0, Normal = 1, High = 2 };
enum class Units : uint8_t { Fps = 0, Mps = 1 };
enum class Reading : uint8_t { Muzzle = 0, Chrono = 1 };
enum class Mount : uint8_t { Downrange = 0, Barrel = 1 };

struct Settings {
  Projectile proj = Projectile::Paintball;
  Sensitivity sens = Sensitivity::Normal;
  Units units = Units::Fps;
  Reading reading = Reading::Muzzle;
  Mount mount = Mount::Downrange;
  float dist_ft = 3.0f;        // downrange: muzzle to radar
  float offset_mm = 40.0f;     // beside the barrel: bore line to the middle of the antenna
  float ahead_mm = 20.0f;      // beside the barrel: muzzle in front of the antenna
  bool auto_read = false;      // always measuring, no need to hold READ
  bool wifi = true;
  float cal = 1.0f;            // multiplies every speed (radar frequency tolerance)
  float batt_cal = 1.0f;       // multiplies the battery voltage
  float k_paintball = 0.022f;  // drag constants, 1/m (dv/dx = -k v)
  float k_dart = 0.098f;
  uint16_t backlight_s = 120;  // screen timeout, 0 = always on
  char wifi_pass[24] = "";     // empty = open network; otherwise 8+ characters (WPA2)
};

// Names used in the settings file, the web API and on screen.
const char* projectileName(Projectile p);   // "paintball" / "dart"
const char* projectileLabel(Projectile p);  // "Paintball" / "Half dart"
const char* sensitivityName(Sensitivity s);
const char* unitsName(Units u);
const char* readingName(Reading r);
const char* mountName(Mount m);

// Set one setting from text. Returns false (and changes nothing) for an unknown key or a
// value out of range.
bool settingsSet(Settings& s, const char* key, const char* value);

// Whole settings as key=value lines. Returns the length written (truncated to n - 1).
size_t settingsToText(const Settings& s, char* out, size_t n);
// Apply key=value lines; unknown or bad lines are skipped. Returns the number applied.
int settingsFromText(Settings& s, const char* text);

// Settings as a JSON object (no surrounding whitespace). Returns the length written.
size_t settingsToJson(const Settings& s, char* out, size_t n);

float dragConstant(const Settings& s, Projectile p);
dsp::Config toDspConfig(const Settings& s);

// Speed in the user's units.
inline float toUserUnits(const Settings& s, float mps) {
  return s.units == Units::Fps ? mps * dsp::FT_PER_M : mps;
}
