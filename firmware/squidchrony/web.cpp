#include "web.h"

#include <Arduino.h>
#include <LEAmDNS.h>
#include <WebServer.h>
#include <WiFi.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "app.h"
#include "config.h"
#include "web_page.h"

namespace web {
namespace {

WebServer server(80);
bool up = false;
bool routes_added = false;
bool mdns_up = false;
int client_count = 0;  // asking the radio chip is slow, so this is refreshed once a second
uint32_t clients_checked_ms = 0;

constexpr float FPS_PER_MPS = 3.28084f;

// Buffers small writes into chunks of a chunked HTTP response.
class Out {
 public:
  void begin(int code, const char* type) {
    n_ = 0;
    server.sendHeader("Cache-Control", "no-store");
    server.chunkedResponseModeStart(code, type);
  }
  void add(const char* fmt, ...) {
    for (int attempt = 0; attempt < 2; attempt++) {
      va_list ap;
      va_start(ap, fmt);
      int len = vsnprintf(buf_ + n_, sizeof(buf_) - n_, fmt, ap);
      va_end(ap);
      if (len >= 0 && (size_t)len < sizeof(buf_) - n_) {
        n_ += (size_t)len;
        return;
      }
      buf_[n_] = '\0';
      flush();  // no room: send what we have and try again
    }
  }
  // A JSON string value with quotes and escapes.
  void str(const char* s) {
    char tmp[80];
    size_t j = 0;
    tmp[j++] = '"';
    for (; *s && j < sizeof(tmp) - 8; s++) {
      unsigned char c = (unsigned char)*s;
      if (c == '"' || c == '\\') {
        tmp[j++] = '\\';
        tmp[j++] = (char)c;
      } else if (c < 0x20) {
        j += snprintf(tmp + j, sizeof(tmp) - j, "\\u%04x", c);
      } else {
        tmp[j++] = (char)c;
      }
    }
    tmp[j++] = '"';
    tmp[j] = '\0';
    add("%s", tmp);
  }
  void flush() {
    if (n_) server.sendContent(buf_, n_);
    n_ = 0;
  }
  void end() {
    flush();
    server.chunkedResponseFinalize();
  }

 private:
  char buf_[1024];
  size_t n_ = 0;
};

Out out;

void sendJson(const char* json) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", String(json));
}

void handleRoot() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html", (PGM_P)WEB_PAGE_GZ, WEB_PAGE_GZ_LEN);
}

void handleState() {
  app::Live l = app::live();
  const ShotString& cur = app::shots.current();
  char settings[640];
  settingsToJson(app::settings, settings, sizeof settings);
  out.begin(200, "application/json");
  out.add("{\"rev\":%lu,\"srev\":%lu,\"fw\":\"%s\",\"ssid\":", (unsigned long)app::shots.rev(),
          (unsigned long)app::shots.structureRev(), FIRMWARE_VERSION);
  out.str(app::ssid());
  out.add(",\"armed\":%s,\"held\":%s,\"web_armed\":%s,\"busy\":%s,\"clients\":%d,\"time_set\":%s,",
          l.armed ? "true" : "false", l.held ? "true" : "false", l.web_armed ? "true" : "false",
          l.busy ? "true" : "false", l.clients, app::wallClockS() ? "true" : "false");
  out.add("\"batt_v\":%.2f,\"batt_pct\":%d,\"batt_low\":%s,", l.batt_v, l.batt_pct,
          l.batt_low ? "true" : "false");
  out.add("\"dsp\":{\"noise_db\":%.1f,\"peak_db\":%.1f,\"peak_hz\":%.0f,\"frames\":%lu,\"shots\":%lu,"
          "\"lost\":%lu,\"rejected\":%lu,\"gaps\":%lu,\"dropped\":%lu,\"lag\":%lu},",
          l.dsp.noise_db, l.dsp.peak_snr_db, l.dsp.peak_hz, (unsigned long)l.dsp.frames,
          (unsigned long)l.dsp.shots, (unsigned long)l.dsp.lost, (unsigned long)l.dsp.rejected,
          (unsigned long)l.dsp.gaps, (unsigned long)l.dsp.dropped, (unsigned long)l.dsp.lag_frames);
  out.add("\"cur\":{\"id\":%u,\"name\":", (unsigned)cur.id);
  out.str(cur.name);
  out.add(",\"proj\":\"%s\",\"mount\":\"%s\",\"n\":%d},\"settings\":%s}", projectileName(cur.proj),
          mountName(cur.mount), app::shots.liveCount(cur), settings);
  out.end();
}

void writeString(const ShotString& s, bool first) {
  out.add("%s{\"id\":%u,\"name\":", first ? "" : ",", (unsigned)s.id);
  out.str(s.name);
  out.add(",\"proj\":\"%s\",\"mount\":\"%s\",\"created\":%lu,\"shots\":[", projectileName(s.proj),
          mountName(s.mount), (unsigned long)s.created_s);
  // t: milliseconds since the string's first shot, for rate of fire. Shots from different
  // power-ups are not comparable, so they are put an hour apart.
  uint32_t t = 0;
  const Shot* prev = nullptr;
  int n = app::shots.liveCount(s);
  for (int i = 0; i < n; i++) {
    const Shot* sh = app::shots.liveShot(s, i);
    if (prev) t += sh->boot == prev->boot ? sh->t_ms - prev->t_ms : 3600000u;
    out.add("%s[%.2f,%.2f,%.1f,%u,%lu,%.2f,%lu]", i ? "," : "", sh->muzzle_mps * FPS_PER_MPS,
            sh->chrono_mps * FPS_PER_MPS, sh->snr_db, (unsigned)sh->flags, (unsigned long)t,
            sh->sigma_mps * FPS_PER_MPS, (unsigned long)sh->time_s);
    prev = sh;
  }
  out.add("]}");
}

void handleData() {
  bool cur_only = server.hasArg("cur") && server.arg("cur") == "1";
  out.begin(200, "application/json");
  out.add("{\"rev\":%lu,\"srev\":%lu,\"cur\":%u,\"fields\":[\"muzzle_fps\",\"chrono_fps\",\"snr_db\","
          "\"flags\",\"t_ms\",\"sigma_fps\",\"time_s\"],\"strings\":[",
          (unsigned long)app::shots.rev(), (unsigned long)app::shots.structureRev(),
          (unsigned)app::shots.current().id);
  if (cur_only) {
    writeString(app::shots.current(), true);
  } else {
    for (int k = app::shots.stringCount() - 1, first = 1; k >= 0; k--, first = 0)
      writeString(app::shots.string(k), first);  // newest first
  }
  out.add("]}");
  out.end();
}

void handleCmd() {
  String c = server.arg("c");
  String v = server.arg("v");
  bool ok = false;
  const char* msg = "";
  if (c == "new") {
    app::newString();
    ok = true;
  } else if (c == "del") {
    ok = app::deleteLast();
    if (!ok) msg = "No shots to delete.";
  } else if (c == "undel") {
    ok = app::undeleteLast();
  } else if (c == "name") {
    uint16_t id = server.hasArg("id") ? (uint16_t)server.arg("id").toInt() : app::shots.current().id;
    ok = app::renameString(id, v.c_str());
  } else if (c == "rm") {
    ok = app::removeString((uint16_t)server.arg("id").toInt());
    if (!ok) msg = "The current string can't be removed.";
  } else if (c == "clear") {
    app::clearAll();
    ok = true;
  } else if (c == "arm") {
    app::webArm(v == "1");
    ok = true;
  } else if (c == "time") {
    uint64_t ms = strtoull(v.c_str(), nullptr, 10);
    ok = ms > 1600000000000ull;
    if (ok) app::setWallClockMs(ms);
  } else if (c == "cal") {
    float ref = strtof(v.c_str(), nullptr);
    ok = app::calibrate(ref);
    if (!ok) msg = "Need 3+ shots in this string, and a reference within 10%.";
  } else {
    msg = "Unknown command.";
  }
  if (ok) app::touch();
  char buf[160];
  snprintf(buf, sizeof buf, "{\"ok\":%s,\"rev\":%lu,\"cal\":%.5f,\"msg\":\"%s\"}", ok ? "true" : "false",
           (unsigned long)app::shots.rev(), app::settings.cal, msg);
  sendJson(buf);
}

void handleSet() {
  char bad[96] = "";
  size_t bl = 0;
  bool any = false;
  for (int i = 0; i < server.args(); i++) {
    String k = server.argName(i);
    if (k == "plain") continue;  // raw POST body
    if (app::setSetting(k.c_str(), server.arg(i).c_str())) {
      any = true;
    } else if (bl + k.length() + 4 < sizeof bad) {
      bl += snprintf(bad + bl, sizeof bad - bl, "%s\"%s\"", bl ? "," : "", k.c_str());
    }
  }
  if (any) {
    app::settingsChanged();
    app::touch();
  }
  char settings[640];
  settingsToJson(app::settings, settings, sizeof settings);
  char buf[820];
  snprintf(buf, sizeof buf, "{\"ok\":%s,\"bad\":[%s],\"settings\":%s}", bl ? "false" : "true", bad,
           settings);
  sendJson(buf);
}

void csvName(const char* s, char* outb, size_t n) {
  // Quote the field if it holds a comma, quote or newline.
  bool q = strpbrk(s, ",\"\n") != nullptr;
  size_t j = 0;
  if (q && j < n - 1) outb[j++] = '"';
  for (; *s && j < n - 3; s++) {
    if (*s == '"') outb[j++] = '"';
    outb[j++] = *s;
  }
  if (q) outb[j++] = '"';
  outb[j] = '\0';
}

void handleCsv() {
  server.sendHeader("Content-Disposition", "attachment; filename=\"chrono.csv\"");
  out.begin(200, "text/csv");
  out.add("string,projectile,mount,shot,muzzle_fps,at_chrono_fps,sigma_fps,signal_db,flags,time\n");
  for (int k = 0; k < app::shots.stringCount(); k++) {
    const ShotString& s = app::shots.string(k);
    char name[80];
    csvName(s.name, name, sizeof name);
    int n = app::shots.liveCount(s);
    for (int i = 0; i < n; i++) {
      const Shot* sh = app::shots.liveShot(s, i);
      char when[24] = "";
      if (sh->time_s) {
        time_t tt = (time_t)sh->time_s;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        strftime(when, sizeof when, "%Y-%m-%dT%H:%M:%SZ", &tmv);
      }
      out.add("%s,%s,%s,%d,%.2f,%.2f,%.2f,%.1f,%u,%s\n", name, projectileLabel(s.proj),
              mountName(s.mount), i + 1, sh->muzzle_mps * FPS_PER_MPS, sh->chrono_mps * FPS_PER_MPS,
              sh->sigma_mps * FPS_PER_MPS, sh->snr_db, (unsigned)sh->flags, when);
    }
  }
  out.end();
}

void put16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}

void handleWav() {
  const int16_t* s;
  uint32_t n, seq;
  if (!dsptask::lockLastShot(&s, &n, &seq)) {
    server.send(404, "text/plain", "No shot recorded yet.");
    return;
  }
  const uint32_t rate = 50000, bytes = n * 2;
  uint8_t h[44];
  memcpy(h, "RIFF", 4);
  put32(h + 4, 36 + bytes);
  memcpy(h + 8, "WAVEfmt ", 8);
  put32(h + 16, 16);
  put16(h + 20, 1);  // PCM
  put16(h + 22, 1);  // mono
  put32(h + 24, rate);
  put32(h + 28, rate * 2);
  put16(h + 32, 2);
  put16(h + 34, 16);
  memcpy(h + 36, "data", 4);
  put32(h + 40, bytes);
  char disp[64];
  snprintf(disp, sizeof disp, "attachment; filename=\"shot-%lu.wav\"", (unsigned long)seq);
  server.sendHeader("Content-Disposition", disp);
  server.setContentLength(sizeof h + bytes);
  server.send(200, "audio/wav", "");
  server.sendContent((const char*)h, sizeof h);
  server.sendContent((const char*)s, bytes);  // the RP2350 is little-endian, like WAV
  dsptask::unlockLastShot();
}

void handleNotFound() {
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

}  // namespace

void begin() {
  if (up) return;
  WiFi.mode(WIFI_AP);
  IPAddress ip(AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3]);
  WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0));
  const char* pass = app::settings.wifi_pass[0] ? app::settings.wifi_pass : nullptr;
  WiFi.softAP(app::ssid(), pass);
  if (!routes_added) {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/api/state", HTTP_GET, handleState);
    server.on("/api/data", HTTP_GET, handleData);
    server.on("/api/cmd", HTTP_ANY, handleCmd);
    server.on("/api/set", HTTP_ANY, handleSet);
    server.on("/api/export.csv", HTTP_GET, handleCsv);
    server.on("/api/lastshot.wav", HTTP_GET, handleWav);
    server.onNotFound(handleNotFound);
    routes_added = true;
  }
  server.begin();
  if (!mdns_up && MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    mdns_up = true;
  }
  up = true;
}

void stop() {
  if (!up) return;
  server.stop();
  WiFi.softAPdisconnect(true);
  up = false;
  client_count = 0;
}

void loop() {
  if (!up) return;
  server.handleClient();
  if (mdns_up) MDNS.update();
  uint32_t now = millis();
  if (now - clients_checked_ms >= 1000) {
    clients_checked_ms = now;
    client_count = WiFi.softAPgetStationNum();
  }
}

bool running() { return up; }

int clients() { return up ? client_count : 0; }

}  // namespace web
