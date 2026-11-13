// The chrono's own Wi-Fi network and the phone page.
//
// It is an access point (no internet needed): join CHRONO-xxxx, then open 192.168.4.1 or
// chrono.local. Everything runs on core 0.
//
// HTTP API (all JSON unless noted; commands and settings accept GET or POST):
//   GET /                      the phone page (gzipped)
//   GET /api/state             live status, settings and current-string summary
//   GET /api/data[?cur=1]      strings with their shots (cur=1: only the current one)
//   GET /api/cmd?c=...         new | del | undel | name&v=..[&id=..] | rm&id=.. | clear |
//                              arm&v=0/1 | time&v=<Unix ms> | cal&v=<reference m/s>
//   GET /api/set?key=value..   change settings (see settings.cpp for keys)
//   GET /api/export.csv        every string as CSV
//   GET /api/lastshot.wav      the samples around the last shot, 50 kS/s 16-bit mono
#pragma once

namespace web {

void begin();  // start the access point and the server
void stop();
void loop();
bool running();
int clients();

}  // namespace web
