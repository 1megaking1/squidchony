#include "shotlog.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// Minimal tokenizer: strtof/strtoul only (no sscanf, whose float support is optional on
// some embedded C libraries).
struct Tok {
  const char* p;
  bool ok = true;
  explicit Tok(const char* s) : p(s) {}
  void skip() {
    while (*p == ' ') p++;
  }
  long i() {
    skip();
    char* end;
    long v = strtol(p, &end, 10);
    if (end == p) ok = false;
    p = end;
    return v;
  }
  unsigned long u() {
    skip();
    char* end;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) ok = false;
    p = end;
    return v;
  }
  float f() {
    skip();
    char* end;
    float v = strtof(p, &end);
    if (end == p) ok = false;
    p = end;
    return v;
  }
  const char* rest() {
    skip();
    return p;
  }
};

constexpr uint32_t kRunGapMs = 600;  // shots closer than this count as one burst

}  // namespace

void ShotLog::setName(char* dst, const char* src) {
  size_t j = 0;
  if (src) {
    for (size_t i = 0; src[i] && j < sizeof(ShotString::name) - 1; i++) {
      char c = src[i];
      if (c == '\r' || c == '\n' || c == '\t') c = ' ';
      if ((unsigned char)c < 0x20) continue;
      dst[j++] = c;
    }
  }
  while (j > 0 && dst[j - 1] == ' ') j--;
  dst[j] = '\0';
}

void ShotLog::clear() {
  nshots_ = 0;
  nstr_ = 0;
  next_id_ = 1;
  boot_ = 0;
  rev_++;
  srev_++;
  jlen_ = 0;
  joverflow_ = false;
  replaying_ = false;
}

void ShotLog::record(const char* line) {
  if (replaying_) return;
  size_t len = strlen(line);
  if (jlen_ + len + 1 > JOURNAL_MAX) {
    joverflow_ = true;
    return;
  }
  memcpy(journal_ + jlen_, line, len);
  jlen_ += len;
  journal_[jlen_++] = '\n';
}

size_t ShotLog::takeJournal(char* out, size_t n) {
  size_t len = jlen_ < n ? jlen_ : (n ? n - 1 : 0);
  memcpy(out, journal_, len);
  if (len < n) out[len] = '\0';
  jlen_ = 0;
  joverflow_ = false;
  return len;
}

int ShotLog::indexOf(uint16_t id) const {
  for (int i = 0; i < nstr_; i++)
    if (str_[i].id == id) return i;
  return -1;
}

const ShotString* ShotLog::findString(uint16_t id) const {
  int i = indexOf(id);
  return i < 0 ? nullptr : &str_[i];
}

int ShotLog::liveCount(const ShotString& s) const {
  int n = 0;
  for (int i = 0; i < s.count; i++)
    if (!shots_[s.first + i].deleted) n++;
  return n;
}

const Shot* ShotLog::liveShot(const ShotString& s, int k) const {
  for (int i = 0; i < s.count; i++) {
    const Shot& sh = shots_[s.first + i];
    if (sh.deleted) continue;
    if (k-- == 0) return &sh;
  }
  return nullptr;
}

const Shot* ShotLog::lastShot() const {
  if (!nstr_) return nullptr;
  const ShotString& s = current();
  for (int i = s.count - 1; i >= 0; i--)
    if (!shots_[s.first + i].deleted) return &shots_[s.first + i];
  return nullptr;
}

void ShotLog::startBoot() {
  boot_++;
  char line[16];
  snprintf(line, sizeof line, "B %u", (unsigned)boot_);
  record(line);
}

bool ShotLog::doNewString(uint16_t id, Projectile p, Mount m, uint32_t created_s,
                          const char* name) {
  if (indexOf(id) >= 0) return false;
  if (nstr_ == MAX_STRINGS) dropOldestString();
  if (nstr_ == MAX_STRINGS) return false;
  ShotString& s = str_[nstr_++];
  s.id = id;
  s.proj = p;
  s.mount = m;
  s.created_s = created_s;
  s.first = (uint16_t)nshots_;
  s.count = 0;
  setName(s.name, name);
  if (!s.name[0]) snprintf(s.name, sizeof s.name, "String %u", (unsigned)id);
  if (id >= next_id_) next_id_ = id + 1;
  rev_++;
  srev_++;
  return true;
}

void ShotLog::newString(Projectile p, Mount m, uint32_t now_s, const char* name) {
  uint16_t id = next_id_;
  if (!doNewString(id, p, m, now_s, name)) return;
  const ShotString& s = current();
  char line[80];
  snprintf(line, sizeof line, "S %u %d %d %lu %s", (unsigned)s.id, (int)s.proj, (int)s.mount,
           (unsigned long)s.created_s, s.name);
  record(line);
}

bool ShotLog::doAddShot(uint16_t id, const Shot& sh) {
  if (!nstr_ || current().id != id) return false;  // shots only ever go to the newest string
  while (nshots_ >= MAX_SHOTS && nstr_ > 1) dropOldestString();
  if (nshots_ >= MAX_SHOTS) return false;
  shots_[nshots_] = sh;
  shots_[nshots_].deleted = 0;
  nshots_++;
  str_[nstr_ - 1].count++;
  rev_++;
  return true;
}

bool ShotLog::addShot(const Shot& sh) {
  if (!nstr_) return false;
  uint16_t id = current().id;
  if (!doAddShot(id, sh)) return false;
  char line[160];
  snprintf(line, sizeof line, "H %u %.3f %.3f %.3f %.1f %.4f %.3f %u %lu %u %lu", (unsigned)id,
           sh.muzzle_mps, sh.chrono_mps, sh.sigma_mps, sh.snr_db, sh.k, sh.offset_m,
           (unsigned)sh.flags, (unsigned long)sh.time_s, (unsigned)sh.boot,
           (unsigned long)sh.t_ms);
  record(line);
  return true;
}

bool ShotLog::deleteLast() {
  if (!nstr_) return false;
  const ShotString& s = current();
  for (int i = s.count - 1; i >= 0; i--) {
    Shot& sh = shots_[s.first + i];
    if (sh.deleted) continue;
    sh.deleted = 1;
    rev_++;
    char line[24];
    snprintf(line, sizeof line, "D %u %d", (unsigned)s.id, i);
    record(line);
    return true;
  }
  return false;
}

bool ShotLog::undeleteLast() {
  if (!nstr_) return false;
  const ShotString& s = current();
  // Undo the newest deletion: the first shot of the run of deleted shots at the end.
  int i = s.count - 1;
  while (i >= 0 && shots_[s.first + i].deleted) i--;
  int target = i + 1;
  if (target >= s.count) return false;
  shots_[s.first + target].deleted = 0;
  rev_++;
  char line[24];
  snprintf(line, sizeof line, "U %u %d", (unsigned)s.id, target);
  record(line);
  return true;
}

bool ShotLog::rename(uint16_t id, const char* name) {
  int i = indexOf(id);
  if (i < 0) return false;
  char clean[sizeof(ShotString::name)];
  setName(clean, name);
  if (!clean[0]) return false;
  if (!strcmp(clean, str_[i].name)) return true;
  memcpy(str_[i].name, clean, sizeof clean);
  rev_++;
  srev_++;
  char line[48];
  snprintf(line, sizeof line, "N %u %s", (unsigned)id, clean);
  record(line);
  return true;
}

bool ShotLog::retypeCurrent(Projectile p, Mount m) {
  if (!nstr_ || str_[nstr_ - 1].count) return false;
  ShotString& s = str_[nstr_ - 1];
  if (s.proj == p && s.mount == m) return true;
  s.proj = p;
  s.mount = m;
  rev_++;
  srev_++;
  char line[32];
  snprintf(line, sizeof line, "T %u %d %d", (unsigned)s.id, (int)p, (int)m);
  record(line);
  return true;
}

void ShotLog::removeAt(int k) {
  int first = str_[k].first, cnt = str_[k].count;
  memmove(&shots_[first], &shots_[first + cnt], (size_t)(nshots_ - first - cnt) * sizeof(Shot));
  nshots_ -= cnt;
  memmove(&str_[k], &str_[k + 1], (size_t)(nstr_ - k - 1) * sizeof(ShotString));
  nstr_--;
  for (int i = k; i < nstr_; i++) str_[i].first -= (uint16_t)cnt;
  rev_++;
  srev_++;
}

void ShotLog::dropOldestString() {
  if (nstr_ > 1) removeAt(0);
}

bool ShotLog::removeString(uint16_t id) {
  int k = indexOf(id);
  if (k < 0 || k == nstr_ - 1) return false;
  removeAt(k);
  char line[16];
  snprintf(line, sizeof line, "R %u", (unsigned)id);
  record(line);
  return true;
}

ShotStats ShotLog::stats(const ShotString& s, Reading r) const {
  ShotStats st;
  double sum = 0;
  for (int i = 0; i < s.count; i++) {
    const Shot& sh = shots_[s.first + i];
    if (sh.deleted) continue;
    float x = r == Reading::Chrono ? sh.chrono_mps : sh.muzzle_mps;
    if (st.n == 0 || x < st.lo) st.lo = x;
    if (st.n == 0 || x > st.hi) st.hi = x;
    sum += x;
    st.n++;
  }
  if (!st.n) return st;
  st.mean = (float)(sum / st.n);
  st.es = st.hi - st.lo;
  if (st.n > 1) {
    double ss = 0;
    for (int i = 0; i < s.count; i++) {
      const Shot& sh = shots_[s.first + i];
      if (sh.deleted) continue;
      double d = (r == Reading::Chrono ? sh.chrono_mps : sh.muzzle_mps) - st.mean;
      ss += d * d;
    }
    st.sd = (float)sqrt(ss / (st.n - 1));
  }
  // Rate of fire: the most recent run of 3+ live shots fired less than 0.6 s apart.
  int end = -1;
  for (int i = s.count - 1; i >= 0; i--)
    if (!shots_[s.first + i].deleted) {
      end = i;
      break;
    }
  while (end >= 0) {
    int start = end, count = 1, prev = end;
    for (int i = end - 1; i >= 0; i--) {
      const Shot& a = shots_[s.first + i];
      if (a.deleted) continue;
      const Shot& b = shots_[s.first + prev];
      if (a.boot != b.boot || b.t_ms - a.t_ms >= kRunGapMs) break;
      start = i;
      prev = i;
      count++;
    }
    if (count >= 3) {
      uint32_t span = shots_[s.first + end].t_ms - shots_[s.first + start].t_ms;
      if (span > 0) st.rof_bps = (count - 1) * 1000.0f / span;
      break;
    }
    // Continue with the shots before this run.
    int next = -1;
    for (int i = start - 1; i >= 0; i--)
      if (!shots_[s.first + i].deleted) {
        next = i;
        break;
      }
    end = next;
  }
  return st;
}

bool ShotLog::apply(const char* line) {
  if (!line || !line[0]) return true;
  replaying_ = true;
  bool ok = false;
  Tok t(line + 1);
  switch (line[0]) {
    case 'B': {
      unsigned long b = t.u();
      if (t.ok) {
        if (b > boot_) boot_ = (uint16_t)b;
        ok = true;
      }
      break;
    }
    case 'S': {
      long id = t.i(), proj = t.i(), mount = t.i();
      unsigned long created = t.u();
      const char* name = t.rest();
      if (t.ok && id > 0 && id < 65536 && (proj == 0 || proj == 1) && (mount == 0 || mount == 1))
        ok = doNewString((uint16_t)id, (Projectile)proj, (Mount)mount, (uint32_t)created, name);
      break;
    }
    case 'H': {
      long id = t.i();
      Shot sh;
      sh.muzzle_mps = t.f();
      sh.chrono_mps = t.f();
      sh.sigma_mps = t.f();
      sh.snr_db = t.f();
      sh.k = t.f();
      sh.offset_m = t.f();
      sh.flags = (uint8_t)t.u();
      sh.time_s = (uint32_t)t.u();
      sh.boot = (uint16_t)t.u();
      sh.t_ms = (uint32_t)t.u();
      sh.deleted = 0;
      if (t.ok && id > 0) ok = doAddShot((uint16_t)id, sh);
      break;
    }
    case 'D':
    case 'U': {
      long id = t.i(), index = t.i();
      int k = t.ok ? indexOf((uint16_t)id) : -1;
      if (k >= 0 && index >= 0 && index < str_[k].count) {
        shots_[str_[k].first + index].deleted = line[0] == 'D' ? 1 : 0;
        rev_++;
        ok = true;
      }
      break;
    }
    case 'N': {
      long id = t.i();
      const char* name = t.rest();
      int k = t.ok ? indexOf((uint16_t)id) : -1;
      if (k >= 0 && name[0]) {
        setName(str_[k].name, name);
        rev_++;
        srev_++;
        ok = true;
      }
      break;
    }
    case 'T': {
      long id = t.i(), proj = t.i(), mount = t.i();
      int k = t.ok ? indexOf((uint16_t)id) : -1;
      if (k >= 0 && (proj == 0 || proj == 1) && (mount == 0 || mount == 1)) {
        str_[k].proj = (Projectile)proj;
        str_[k].mount = (Mount)mount;
        rev_++;
        srev_++;
        ok = true;
      }
      break;
    }
    case 'R': {
      long id = t.i();
      int k = t.ok ? indexOf((uint16_t)id) : -1;
      if (k >= 0 && k < nstr_ - 1) {
        removeAt(k);
        ok = true;
      }
      break;
    }
    default:
      break;
  }
  replaying_ = false;
  return ok;
}

void ShotLog::dump(void (*emit)(const char* line, void* ctx), void* ctx) const {
  char line[160];
  snprintf(line, sizeof line, "B %u", (unsigned)boot_);
  emit(line, ctx);
  for (int k = 0; k < nstr_; k++) {
    const ShotString& s = str_[k];
    snprintf(line, sizeof line, "S %u %d %d %lu %s", (unsigned)s.id, (int)s.proj, (int)s.mount,
             (unsigned long)s.created_s, s.name);
    emit(line, ctx);
    for (int i = 0; i < s.count; i++) {
      const Shot& sh = shots_[s.first + i];
      snprintf(line, sizeof line, "H %u %.3f %.3f %.3f %.1f %.4f %.3f %u %lu %u %lu",
               (unsigned)s.id, sh.muzzle_mps, sh.chrono_mps, sh.sigma_mps, sh.snr_db, sh.k,
               sh.offset_m, (unsigned)sh.flags, (unsigned long)sh.time_s, (unsigned)sh.boot,
               (unsigned long)sh.t_ms);
      emit(line, ctx);
    }
    for (int i = 0; i < s.count; i++) {
      if (!shots_[s.first + i].deleted) continue;
      snprintf(line, sizeof line, "D %u %d", (unsigned)s.id, i);
      emit(line, ctx);
    }
  }
}
