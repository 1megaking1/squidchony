#include "dsp_task.h"

#include <Arduino.h>
#include <pico/critical_section.h>
#include <pico/mutex.h>
#include <pico/util/queue.h>

#include "acquisition.h"

namespace dsptask {
namespace {

constexpr int SHOT_QUEUE = 8;
constexpr int FRAMES_PER_LOOP = 16;  // catch up in bites so the loop stays responsive
constexpr uint32_t LAST_MAX = 20000;  // 0.4 s at 50 kS/s
constexpr uint32_t LAST_PAD = 2500;   // keep 50 ms either side of the track

// Shared with core 0
critical_section_t cs;
queue_t shot_q;
mutex_t last_mtx;
dsp::Config cfg_shared;
volatile uint32_t cfg_version = 0;
volatile bool armed_flag = false;
volatile bool busy_flag = false;
Status st_shared;
int16_t last_buf[LAST_MAX];
uint32_t last_n = 0;
uint32_t last_seq = 0;

// Core 1 only
dsp::Config cfg;
uint32_t cfg_seen = 0;
dsp::FrameAnalyzer analyzer;
dsp::ShotTracker tracker;
acq::History history;
int16_t frame[dsp::WIN];
uint32_t next_end = 0;
uint32_t gaps_seen = 0;
uint32_t last_publish_ms = 0;
bool noise_init = false;
Status st;

void restart() {
  tracker.reset();
  next_end = acq::head() + dsp::WIN;
}

void keepSamples(const dsp::ShotResult& r, uint32_t seq) {
  if (!mutex_try_enter(&last_mtx, nullptr)) return;  // core 0 is sending the last one
  uint32_t a = r.first_sample - dsp::WIN / 2 - LAST_PAD;
  uint32_t b = r.last_sample + dsp::WIN / 2 + LAST_PAD;
  uint32_t h = acq::head();
  if ((int32_t)(b - h) > 0) b = h;
  uint32_t n = b - a;
  if (n > LAST_MAX) n = LAST_MAX;
  if (acq::read(a, (int)n, last_buf)) {
    last_n = n;
    last_seq = seq;
  }
  mutex_exit(&last_mtx);
}

void onShot(dsp::ShotResult& r) {
  if (acq::clippedBetween(r.first_sample - dsp::WIN / 2, r.last_sample + dsp::WIN / 2))
    r.flags |= dsp::SHOT_CLIPPED;
  ShotMsg m;
  m.r = r;
  m.seq = ++st.shots;
  if (!queue_try_add(&shot_q, &m)) st.dropped++;
  keepSamples(r, m.seq);
}

}  // namespace

void init(const dsp::Config& c) {
  critical_section_init(&cs);
  queue_init(&shot_q, sizeof(ShotMsg), SHOT_QUEUE);
  mutex_init(&last_mtx);
  cfg_shared = c;
  cfg_version = 1;
}

void begin() {
  critical_section_enter_blocking(&cs);
  cfg = cfg_shared;
  cfg_seen = cfg_version;
  critical_section_exit(&cs);
  analyzer.init(cfg);
  tracker.setSampleSource(&history);
  acq::begin();
  restart();
}

void loop() {
  acq::service();  // backstop in case the DMA interrupt is late

  if (cfg_version != cfg_seen) {
    critical_section_enter_blocking(&cs);
    cfg = cfg_shared;
    cfg_seen = cfg_version;
    critical_section_exit(&cs);
    analyzer.setConfig(cfg);
  }

  uint32_t g = acq::gaps();
  if (g != gaps_seen) {
    gaps_seen = g;
    st.gaps = g;
    restart();
  }

  const bool armed = armed_flag;
  uint32_t head = acq::head();
  for (int done = 0; done < FRAMES_PER_LOOP && (int32_t)(head - next_end) >= 0; done++) {
    uint32_t start = next_end - dsp::WIN;
    if (!acq::read(start, dsp::WIN, frame)) {  // fell out of the history: start over
      restart();
      break;
    }
    dsp::FrameResult fr = analyzer.analyze(frame, tracker.trackFreq());
    st.frames++;
    if (!noise_init) {
      st.noise_db = fr.noise_db;
      noise_init = true;
    } else {
      st.noise_db += 0.01f * (fr.noise_db - st.noise_db);
    }
    st.peak_snr_db = fr.snr_db;
    st.peak_hz = fr.f_hz;

    dsp::ShotResult res;
    dsp::ShotTracker::Result r = tracker.push(fr, start + dsp::WIN / 2, armed, cfg, &res);
    if (r == dsp::ShotTracker::SHOT) {
      onShot(res);
    } else if (r == dsp::ShotTracker::REJECTED) {
      analyzer.absorbLastFrame();
      st.rejected++;
    }
    next_end += dsp::HOP;
    head = acq::head();  // a fit may have taken a while
  }

  int32_t behind = (int32_t)(head - next_end);
  st.lag_frames = behind > 0 ? (uint32_t)behind / dsp::HOP : 0;
  st.lost = tracker.lost();
  busy_flag = tracker.active() || st.lag_frames > 4;

  uint32_t now = millis();
  if (now - last_publish_ms >= 100) {
    last_publish_ms = now;
    critical_section_enter_blocking(&cs);
    st_shared = st;
    critical_section_exit(&cs);
  }
}

void setConfig(const dsp::Config& c) {
  critical_section_enter_blocking(&cs);
  cfg_shared = c;
  cfg_version = cfg_version + 1;
  critical_section_exit(&cs);
}

void setArmed(bool armed) { armed_flag = armed; }

bool busy() { return busy_flag; }

bool popShot(ShotMsg* out) { return queue_try_remove(&shot_q, out); }

Status status() {
  critical_section_enter_blocking(&cs);
  Status s = st_shared;
  critical_section_exit(&cs);
  return s;
}

bool lockLastShot(const int16_t** samples, uint32_t* count, uint32_t* seq) {
  mutex_enter_blocking(&last_mtx);
  if (!last_n) {
    mutex_exit(&last_mtx);
    return false;
  }
  *samples = last_buf;
  *count = last_n;
  *seq = last_seq;
  return true;
}

void unlockLastShot() { mutex_exit(&last_mtx); }

}  // namespace dsptask
