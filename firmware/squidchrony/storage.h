// Settings and shot log on the Pico's flash (LittleFS).
//
// Writing flash pauses the other core for the duration, so the measuring core loses those
// samples. The caller only flushes while nothing is being measured.
//
// The filesystem needs space reserved in the board settings: Tools > Flash Size >
// "4MB (Sketch: 3MB, FS: 1MB)" in the Arduino IDE. Without it the chrono still works but
// forgets everything at power-off.
#pragma once
#include <stddef.h>

#include "settings.h"
#include "shotlog.h"

namespace storage {

bool begin();  // mount; false = no filesystem (nothing is saved)
bool ok();

bool loadSettings(Settings& s);
bool saveSettings(const Settings& s);

// Replay the saved journal into log (call log.clear() first). Returns lines applied.
int loadLog(ShotLog& log);
// Save what changed since the last flush (or rewrite the whole file when it has grown
// large, the in-memory journal overflowed, or a previous write failed).
bool flushLog(ShotLog& log);
// Start the saved log afresh from the current state.
bool rewriteLog(ShotLog& log);

}  // namespace storage
