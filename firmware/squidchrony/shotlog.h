// Strings of shots, their statistics, and the journal that saves them.
//
// Every change appends one text line to a journal (see the format below). The firmware
// writes those lines to flash when the chrono is idle and replays them at boot, so a reset
// loses at most the shots since the last flush. Portable C++ (host-tested).
//
// Journal lines (space separated, names run to the end of the line):
//   B boot                                  boot counter, written once per power-up
//   S id proj mount created_s name          new string
//   H id muzzle chrono sigma snr k offset flags time_s boot t_ms
//                                           shot (speeds m/s, offset m, t_ms since boot)
//   D id index / U id index                 delete / restore shot index of string id
//   N id name                               rename
//   T id proj mount                         change an empty string's projectile/mount
//   R id                                    remove a whole string
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "settings.h"

struct Shot {
  float muzzle_mps;
  float chrono_mps;
  float sigma_mps;
  float snr_db;
  float k;          // drag constant used, 1/m
  float offset_m;   // fitted flight-line offset
  uint32_t time_s;  // wall clock (Unix seconds) if the phone has set it, else 0
  uint32_t t_ms;    // milliseconds since power-up (rate of fire)
  uint16_t boot;    // power-up count; times only compare within one boot
  uint8_t flags;    // dsp::ShotFlags
  uint8_t deleted;
};

struct ShotString {
  uint16_t id;
  Projectile proj;
  Mount mount;
  uint32_t created_s;
  uint16_t first;  // index of its first shot in the log
  uint16_t count;  // shots including deleted ones
  char name[32];
};

struct ShotStats {
  int n = 0;  // live (not deleted) shots
  float mean = 0, sd = 0, es = 0, lo = 0, hi = 0;  // m/s
  float rof_bps = 0;  // most recent run of 3+ shots under 0.6 s apart, 0 if none
};

class ShotLog {
 public:
  static constexpr int MAX_SHOTS = 1500;
  static constexpr int MAX_STRINGS = 64;
  static constexpr size_t JOURNAL_MAX = 4096;

  ShotLog() { clear(); }
  // Forget everything and start one empty string (journal emptied too).
  void clear();

  int stringCount() const { return nstr_; }
  const ShotString& string(int i) const { return str_[i]; }  // 0 = oldest
  const ShotString& current() const { return str_[nstr_ - 1]; }
  const ShotString* findString(uint16_t id) const;
  const Shot& shot(int index) const { return shots_[index]; }
  int liveCount(const ShotString& s) const;
  // The i-th live shot of a string (deleted shots skipped), or null.
  const Shot* liveShot(const ShotString& s, int i) const;
  const Shot* lastShot() const;  // newest live shot of the current string

  uint32_t rev() const { return rev_; }               // changes on every edit
  uint32_t structureRev() const { return srev_; }     // strings added, removed or renamed
  uint16_t boot() const { return boot_; }

  // Edits. Each one records a journal line.
  void startBoot();                                   // boot_ = last seen + 1
  void newString(Projectile p, Mount m, uint32_t now_s, const char* name = nullptr);
  bool addShot(const Shot& s);                        // to the current string
  bool deleteLast();                                  // in the current string
  bool undeleteLast();
  bool rename(uint16_t id, const char* name);
  bool retypeCurrent(Projectile p, Mount m);          // only while it has no shots
  bool removeString(uint16_t id);                     // the current string can't be removed

  ShotStats stats(const ShotString& s, Reading r) const;

  // Journal lines recorded since the last take, '\n' separated. Returns the length
  // copied and empties the journal. If it overflowed, overflowed() says so and the
  // caller should rewrite the whole file with dump().
  size_t takeJournal(char* out, size_t n);
  size_t journalLength() const { return jlen_; }
  bool overflowed() const { return joverflow_; }

  // Replay one journal line (at boot, without recording it). False if it is malformed.
  bool apply(const char* line);
  // The whole state as journal lines, one call per line (without the newline).
  void dump(void (*emit)(const char* line, void* ctx), void* ctx) const;

 private:
  void record(const char* line);
  bool doNewString(uint16_t id, Projectile p, Mount m, uint32_t created_s, const char* name);
  bool doAddShot(uint16_t id, const Shot& s);
  int indexOf(uint16_t id) const;
  void removeAt(int k);
  void dropOldestString();
  static void setName(char* dst, const char* src);

  Shot shots_[MAX_SHOTS];
  ShotString str_[MAX_STRINGS];
  int nshots_ = 0;
  int nstr_ = 0;
  uint16_t next_id_ = 1;
  uint16_t boot_ = 0;
  uint32_t rev_ = 0;
  uint32_t srev_ = 0;
  char journal_[JOURNAL_MAX];
  size_t jlen_ = 0;
  bool joverflow_ = false;
  bool replaying_ = false;
};
