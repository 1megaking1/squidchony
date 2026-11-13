#include "storage.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <string.h>

namespace storage {
namespace {

const char* const SETTINGS_PATH = "/settings.txt";
const char* const SETTINGS_TMP = "/settings.tmp";
const char* const LOG_PATH = "/log.txt";
const char* const LOG_TMP = "/log.new";
constexpr size_t LOG_COMPACT_BYTES = 192 * 1024;

bool fs_ok = false;
bool need_rewrite = false;
char jbuf[ShotLog::JOURNAL_MAX];

void writeLine(const char* line, void* ctx) {
  File* f = static_cast<File*>(ctx);
  f->write((const uint8_t*)line, strlen(line));
  f->write((const uint8_t*)"\n", 1);
}

}  // namespace

bool begin() {
  fs_ok = LittleFS.begin();
  return fs_ok;
}

bool ok() { return fs_ok; }

bool loadSettings(Settings& s) {
  if (!fs_ok) return false;
  File f = LittleFS.open(SETTINGS_PATH, "r");
  if (!f) return false;
  char buf[768];
  size_t n = f.read((uint8_t*)buf, sizeof(buf) - 1);
  f.close();
  buf[n] = '\0';
  return settingsFromText(s, buf) > 0;
}

bool saveSettings(const Settings& s) {
  if (!fs_ok) return false;
  char buf[768];
  size_t n = settingsToText(s, buf, sizeof buf);
  File f = LittleFS.open(SETTINGS_TMP, "w");
  if (!f) return false;
  bool good = f.write((const uint8_t*)buf, n) == n;
  f.close();
  return good && LittleFS.rename(SETTINGS_TMP, SETTINGS_PATH);
}

int loadLog(ShotLog& log) {
  if (!fs_ok) return 0;
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return 0;
  int applied = 0;
  char line[192];
  size_t len = 0;
  bool too_long = false;
  uint8_t chunk[256];
  for (;;) {
    int got = f.read(chunk, sizeof chunk);
    if (got <= 0) break;
    for (int i = 0; i < got; i++) {
      char c = (char)chunk[i];
      if (c == '\n') {
        line[len] = '\0';
        if (!too_long && log.apply(line)) applied++;
        len = 0;
        too_long = false;
      } else if (len < sizeof(line) - 1) {
        line[len++] = c;
      } else {
        too_long = true;
      }
    }
  }
  f.close();
  // A last line without its newline was cut short by a power cut: ignore it, but make sure
  // the next save starts from a clean file.
  if (len) need_rewrite = true;
  return applied;
}

bool rewriteLog(ShotLog& log) {
  if (!fs_ok) return false;
  log.takeJournal(jbuf, sizeof jbuf);  // the full dump covers these lines
  File f = LittleFS.open(LOG_TMP, "w");
  if (!f) {
    need_rewrite = true;
    return false;
  }
  log.dump(writeLine, &f);
  f.close();
  bool good = LittleFS.rename(LOG_TMP, LOG_PATH);
  need_rewrite = !good;
  return good;
}

bool flushLog(ShotLog& log) {
  if (!fs_ok) {
    log.takeJournal(jbuf, sizeof jbuf);  // nowhere to put it
    return false;
  }
  if (!log.journalLength() && !log.overflowed() && !need_rewrite) return true;
  size_t size = 0;
  {
    File f = LittleFS.open(LOG_PATH, "r");
    if (f) {
      size = f.size();
      f.close();
    }
  }
  if (need_rewrite || log.overflowed() || size > LOG_COMPACT_BYTES) return rewriteLog(log);
  size_t n = log.takeJournal(jbuf, sizeof jbuf);
  File f = LittleFS.open(LOG_PATH, "a");
  if (!f) {
    need_rewrite = true;
    return false;
  }
  bool good = f.write((const uint8_t*)jbuf, n) == n;
  f.close();
  if (!good) need_rewrite = true;
  return good;
}

}  // namespace storage
