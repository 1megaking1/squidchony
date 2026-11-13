#include "dsp.h"

#include <math.h>
#include <string.h>

#include <algorithm>
#ifdef DSP_DEBUG
#include <stdio.h>
#endif

namespace dsp {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kLn2 = 0.69314718f;
constexpr float kTiny = 1e-12f;

inline float toDb(float p) { return 10.0f * log10f(p > kTiny ? p : kTiny); }

constexpr float kBgRiseCap = 8.0f;  // background learns from at most 8x its current level

// Gaussian (log-parabolic) interpolation of a spectral peak at bin k: offset in bins.
inline float peakOffset(float pm, float p0, float pp) {
  float a = logf(pm + kTiny), b = logf(p0 + kTiny), c = logf(pp + kTiny);
  float den = a - 2.0f * b + c;
  float d = den < -1e-9f ? 0.5f * (a - c) / den : 0.0f;
  return d > 0.5f ? 0.5f : (d < -0.5f ? -0.5f : d);
}
}  // namespace

// ---------------------------------------------------------------- Fft

void Fft::init() {
  for (int n = 0; n < WIN; n++) window_[n] = 0.5f - 0.5f * cosf(2.0f * kPi * n / (WIN - 1));
  for (int k = 0; k < NFFT / 2; k++) {
    cos_[k] = cosf(2.0f * kPi * k / NFFT);
    sin_[k] = sinf(2.0f * kPi * k / NFFT);
  }
  int bits = 0;
  while ((1 << bits) < NFFT) bits++;
  for (int i = 0; i < NFFT; i++) {
    int r = 0;
    for (int b = 0; b < bits; b++)
      if (i & (1 << b)) r |= 1 << (bits - 1 - b);
    rev_[i] = (uint16_t)r;
  }
}

void Fft::run(float* re, float* im) const {
  for (int i = 0; i < NFFT; i++) {
    int j = rev_[i];
    if (j > i) {
      float t = re[i];
      re[i] = re[j];
      re[j] = t;
      t = im[i];
      im[i] = im[j];
      im[j] = t;
    }
  }
  for (int len = 2; len <= NFFT; len <<= 1) {
    int half = len >> 1;
    int step = NFFT / len;
    for (int i = 0; i < NFFT; i += len) {
      for (int j = 0; j < half; j++) {
        float wr = cos_[j * step];
        float wi = -sin_[j * step];
        int a = i + j, b = a + half;
        float xr = re[b] * wr - im[b] * wi;
        float xi = re[b] * wi + im[b] * wr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
      }
    }
  }
}

// ---------------------------------------------------------------- Decimator

void Decimator::reset() {
  memset(delay_, 0, sizeof(delay_));
  w_ = 0;
  phase_ = 0;
}

// ---------------------------------------------------------------- FrameAnalyzer

void FrameAnalyzer::init(const Config& cfg) {
  fft_.init();
  setConfig(cfg);
  bg_ready_ = false;
}

void FrameAnalyzer::setConfig(const Config& cfg) {
  bin_hz_ = cfg.fs / NFFT;
  klo_ = (int)ceilf(cfg.band_lo_hz / bin_hz_);
  khi_ = (int)floorf(cfg.band_hi_hz / bin_hz_);
  if (klo_ < 2) klo_ = 2;
  if (khi_ > NFFT / 2 - 2) khi_ = NFFT / 2 - 2;
  if (khi_ <= klo_ + 4) khi_ = klo_ + 5;
  bg_alpha_ = (float)HOP / (cfg.fs * (cfg.bg_tau_s > 0.01f ? cfg.bg_tau_s : 0.01f));
  if (bg_alpha_ > 1.0f) bg_alpha_ = 1.0f;
}

void FrameAnalyzer::absorbLastFrame() {
  for (int k = klo_; k <= khi_; k++)
    if (pow_[k] > bg_[k]) bg_[k] = pow_[k];
}

FrameResult FrameAnalyzer::analyze(const int16_t* x, float protect_hz) {
  const float* window = fft_.window();
  float mean = 0.0f;
  for (int n = 0; n < WIN; n++) mean += x[n];
  mean /= WIN;
  for (int n = 0; n < WIN; n++) {
    re_[n] = (x[n] - mean) * window[n];
    im_[n] = 0.0f;
  }
  for (int n = WIN; n < NFFT; n++) re_[n] = im_[n] = 0.0f;
  fft_.run(re_, im_);

  // Powers over the band plus one bin either side for interpolation.
  for (int k = klo_ - 1; k <= khi_ + 1; k++) pow_[k] = re_[k] * re_[k] + im_[k] * im_[k];

  // Noise floor: median of the band. For noise-only bins the power is exponentially
  // distributed, so median / ln2 estimates the mean. A few tonal bins don't move it.
  int nb = khi_ - klo_ + 1;
  for (int i = 0; i < nb; i++) scratch_[i] = pow_[klo_ + i];
  std::nth_element(scratch_, scratch_ + nb / 2, scratch_ + nb);
  float noise = scratch_[nb / 2] / kLn2;
  if (noise < kTiny) noise = kTiny;

  if (!bg_ready_) {
    for (int k = klo_; k <= khi_; k++) bg_[k] = noise;
    bg_ready_ = true;
  }

  // Score each bin against the noise floor, or against the steady background where a
  // constant tone (spinning flywheels, motors, fans) has been sitting.
  int best = klo_;
  float best_score = -1.0f;
  for (int k = klo_; k <= khi_; k++) {
    float b = bg_[k] > 2.0f * noise ? bg_[k] : 0.0f;
    float score = pow_[k] / (noise + b);
    if (score > best_score) {
      best_score = score;
      best = k;
    }
  }
  // Learn the steady background, except around a track in progress.
  int plo = 1, phi = 0;
  if (protect_hz > 0.0f) {
    int pk = (int)(protect_hz / bin_hz_ + 0.5f);
    plo = pk - 4;
    phi = pk + 4;
  }
  // A single loud frame (a shot right next to the radar, or the amplifier clipping) must not
  // lift the background for seconds, so each frame may raise a bin by a few percent at most.
  // A tone that really is steady is learned within about a second, and the tracker's
  // max_track_s rejection plus absorbLastFrame() take care of it sooner.
  for (int k = klo_; k <= khi_; k++) {
    if (k >= plo && k <= phi) continue;
    float p = pow_[k];
    float cap = kBgRiseCap * bg_[k];
    if (p > cap) p = cap;
    bg_[k] += bg_alpha_ * (p - bg_[k]);
  }

  // The score picks the bin; the frequency comes from the power peak next to it.
  for (int k = best - 2; k <= best + 2; k++)
    if (k >= klo_ && k <= khi_ && pow_[k] > pow_[best]) best = k;

  FrameResult r;
  r.f_hz = (best + peakOffset(pow_[best - 1], pow_[best], pow_[best + 1])) * bin_hz_;
  r.snr_db = toDb(pow_[best] / noise);
  r.score_db = toDb(best_score);
  r.noise_db = toDb(noise);
  return r;
}

// ---------------------------------------------------------------- ShotTracker

void ShotTracker::clearTrack() {
  n_ = 0;
  miss_ = 0;
  f_last_ = 0.0f;
  truncated_ = false;
}

void ShotTracker::reset() {
  clearTrack();
  holdoff_ = 0;
}

void ShotTracker::store(const FrameResult& fr, uint32_t center_sample) {
  t_[n_] = center_sample;
  f_[n_] = fr.f_hz;
  snr_[n_] = fr.snr_db;
  noise_[n_] = fr.noise_db;
  n_++;
  f_last_ = fr.f_hz;
  miss_ = 0;
}

ShotTracker::Result ShotTracker::push(const FrameResult& fr, uint32_t center_sample, bool armed,
                                      const Config& cfg, ShotResult* out) {
  if (!armed) {
    holdoff_ = 0;
    if (n_ == 0) return NONE;
    return finish(cfg, out);
  }
  if (n_ == 0 && holdoff_ > 0) {
    holdoff_--;
    return NONE;
  }
  bool in_band = fr.f_hz >= cfg.band_lo_hz && fr.f_hz <= cfg.band_hi_hz;
  bool hit = in_band && fr.score_db >= cfg.detect_db;
  bool continuous = n_ == 0 || fabsf(fr.f_hz - f_last_) <= cfg.jump_frac * f_last_;

  if (n_ > 0) {
    float span_s = (float)(center_sample - t_[0]) / cfg.fs;
    if (span_s > cfg.max_track_s) {
      clearTrack();
      return REJECTED;  // far too long for a projectile: steady interference
    }
  }

  if (hit && continuous) {
    store(fr, center_sample);
    if (n_ >= MAX_TRACK) {
      truncated_ = true;
      return finish(cfg, out);
    }
    return NONE;
  }
  if (hit && n_ < 3) {
    // The first frame(s) were probably a noise blip; restart the track here.
    clearTrack();
    store(fr, center_sample);
    return NONE;
  }
  if (n_ > 0) {
    miss_++;
    if (n_ < 3 && miss_ > 2) {
      clearTrack();
      return NONE;
    }
    if (miss_ >= cfg.end_gap_frames) return finish(cfg, out);
  }
  return NONE;
}

ShotTracker::Result ShotTracker::finish(const Config& cfg, ShotResult* out) {
  int n = n_;
  bool truncated = truncated_;
  if (n < cfg.min_frames || n < 3) {
    clearTrack();
    return NONE;
  }
  int strong = 0;
  for (int i = 0; i < n; i++)
    if (snr_[i] > snr_[strong]) strong = i;
  bool ok = fitter_.fit(t_, f_, snr_, noise_, n, cfg, src_, out);
  if (ok) {
    out->first_sample = t_[0];
    out->last_sample = t_[n - 1];
    out->strong_sample = t_[strong];
    if (truncated) out->flags |= SHOT_TRUNCATED;
    holdoff_ = cfg.holdoff_frames;
  }
  clearTrack();
  return ok ? SHOT : NONE;
}

// ---------------------------------------------------------------- TrackFitter

namespace {
// Leaving a frame out costs as much as a 2-sigma miss, so the search cannot "improve" a fit
// by declaring good frames off-angle.
constexpr float kExcludedChi2 = 4.0f;
constexpr float kImpossibleChi2 = 25.0f;
// While the geometry is still unknown, frames are fitted with a loose error floor so small
// model mismatches in strong frames cannot outvote everything else.
constexpr float kLooseFloorMps = 0.3f;
constexpr float kOutlierSigma = 4.0f;
// Noise of the interpolated FFT peak: sigma_f = kFreqNoise * fs / (pi * WIN * sqrt(SNR)).
// fs / (pi * WIN * sqrt(SNR)) is the Cramer-Rao bound for a rectangular window; the Hann
// window and the interpolation cost a little more.
constexpr float kFreqNoise = 1.3f;
constexpr float kHugeChi2 = 1e30f;
// Frame classes relative to the pass (see classify()); margins in window lengths.
constexpr int kFrameUse = 0, kFrameSkip = 1, kFrameImpossible = 2;
constexpr float kStraddle = 0.5f;
constexpr float kOnsetEarly = 0.45f;
constexpr float kOnsetSkip = 0.35f;
constexpr float kMuzzleSlack = 0.0004f;  // s; the entered muzzle distance is not exact
// Re-measuring may move a frame by at most this fraction (plus 2 bins) from its raw value.
constexpr float kMaxShift = 0.04f;
}  // namespace

#ifdef DSP_DEBUG
bool g_fit_debug = false;
float g_debug_tc = -1.0f, g_debug_tau = -1.0f;
#endif

// Where a frame sits relative to the pass, for a candidate pass time (s = t - t_c).
int TrackFitter::classify(float s) const {
  const float T = WIN / fs_;
  if (downrange_) {
    // A frame whose window holds the pass mixes approach and retreat; one holding the moment
    // the projectile left the muzzle (about dist/v before the pass) has only a sliver of
    // echo, whose broad spectral peak noise can push around.
    if (fabsf(s) < kStraddle * T) return kFrameSkip;
    float sm = s + muzzle_dt_;
    if (sm > -kOnsetEarly * T && sm < kOnsetSkip * T + kMuzzleSlack) return kFrameSkip;
    return kFrameUse;
  }
  // Beside the barrel the echo starts as the projectile leaves the muzzle at the radar. The
  // first frame is often centered a little before that, but a frame centered long before it
  // cannot exist; frames with the muzzle exit near their center are mostly the (often
  // clipped) start of the echo and are skipped.
  if (s < -kOnsetEarly * T) return kFrameImpossible;
  if (s < kOnsetSkip * T) return kFrameSkip;
  return kFrameUse;
}

float TrackFitter::modelHz(const Cand& c, float t) const {
  float s = t - c.tc;
  float rho = sqrtf(s * s + c.tau * c.tau);
  float cth = rho > 0.0f ? fabsf(s) / rho : 1.0f;
  float z = c.A + c.B * t;
  float v = V_ * V_ / (z + V_);
  return hz_per_mps_ * v * cth;
}

float TrackFitter::rawHz(float t) const {
  if (t <= t_[0]) return f_raw_[0];
  for (int i = 1; i < n_; i++) {
    if (t <= t_[i]) {
      float u = (t - t_[i - 1]) / (t_[i] - t_[i - 1]);
      return f_raw_[i - 1] + u * (f_raw_[i] - f_raw_[i - 1]);
    }
  }
  return f_raw_[n_ - 1];
}

// Measure each frame again with the sweep predicted for its window removed. model == null
// uses the raw track (straight lines between frames) as the prediction.
int TrackFitter::remeasure(const SampleSource* src, const Cand* model) {
  const float* window = fft_.window();
  const float bin_hz = fs_ / NFFT;
  const float half = (WIN - 1) * 0.5f;
  int changed = 0;
  for (int i = 0; i < n_; i++) {
    uint32_t center = c0_ + (uint32_t)lroundf(t_[i] * fs_);
    if (!src->read(center - WIN / 2, WIN, samples_)) continue;
    float fc = model ? modelHz(*model, t_[i]) : rawHz(t_[i]);
    if (!(fc > 2.0f * bin_hz) || fc > fs_ * 0.5f - 4.0f * bin_hz) continue;
    // A prediction far from what the frame showed means the model is wrong here.
    if (fabsf(fc - f_raw_[i]) > kMaxShift * f_raw_[i] + 2.0f * bin_hz) continue;
    for (int k = 0; k < WIN; k++) {
      float tk = t_[i] + (k - half) / fs_;
      dhz_[k] = (model ? modelHz(*model, tk) : rawHz(tk)) - fc;
    }
    float mean = 0.0f;
    for (int k = 0; k < WIN; k++) mean += samples_[k];
    mean /= WIN;
    // Multiply by exp(-i * phase of the predicted sweep): the echo becomes a steady tone at fc.
    float ph = 0.0f;
    for (int k = 0; k < WIN; k++) {
      if (k > 0) ph += kPi * (dhz_[k - 1] + dhz_[k]) / fs_;
      float x = (samples_[k] - mean) * window[k];
      re_[k] = x * cosf(ph);
      im_[k] = -x * sinf(ph);
    }
    for (int k = WIN; k < NFFT; k++) re_[k] = im_[k] = 0.0f;
    fft_.run(re_, im_);
    int k0 = (int)lroundf(fc / bin_hz);
    int best = -1;
    float pbest = -1.0f;
    for (int k = k0 - 4; k <= k0 + 4; k++) {
      if (k < 1 || k >= NFFT / 2 - 1) continue;
      float p = re_[k] * re_[k] + im_[k] * im_[k];
      if (p > pbest) {
        pbest = p;
        best = k;
      }
    }
    if (best < 0 || best == k0 - 4 || best == k0 + 4) continue;  // peak not near the prediction
    auto pw = [&](int k) { return re_[k] * re_[k] + im_[k] * im_[k]; };
    float f_new = (best + peakOffset(pw(best - 1), pbest, pw(best + 1))) * bin_hz;
    float snr_new = pbest / noise_[i];
    // Removing the right sweep concentrates the echo; if the peak got much weaker, the
    // prediction was wrong for this frame and the raw measurement stays.
    if (snr_new < 0.5f * snr_raw_[i]) continue;
    f_[i] = f_new;
    snr_[i] = snr_new;
    changed++;
  }
  return changed;
}

void TrackFitter::setWeights(float floor_mps) {
  floor_rel_ = floor_mps / s0_;
  for (int i = 0; i < n_; i++) {
    m_[i] = f_[i] / hz_per_mps_;
    float inv = 1.0f / (snr_[i] > 1e-3f ? snr_[i] : 1e-3f);
    w_[i] = 1.0f / (inv + floor_rel_ * floor_rel_);
    ok_[i] = m_[i] > 0.5f ? 1 : 0;
  }
}

void TrackFitter::evaluate(Cand* c) {
  const float tc = c->tc;
  const float tau2 = c->tau * c->tau;
  const float V2 = V_ * V_;
  const float cos2_min = cos_min_ * cos_min_;
  float S = 0.0f, St = 0.0f, Sz = 0.0f;
  int used = 0, excluded = 0, impossible = 0;
  for (int i = 0; i < n_; i++) {
    wz_[i] = 0.0f;
    if (!ok_[i] || (fixed_ && !use_[i])) continue;
    float s = t_[i] - tc;
    float s2 = s * s;
    float c2 = s2 > 0.0f ? s2 / (s2 + tau2) : 0.0f;
    int cls = classify(s);
    if (cls == kFrameImpossible) {
      impossible++;
      continue;
    }
    if (cls == kFrameSkip || (!fixed_ && c2 < cos2_min)) {
      excluded++;
      continue;
    }
    float cth = sqrtf(c2);
    float z = V2 * cth / m_[i] - V_;       // ~ V - v: linear in t under quadratic drag
    float g = m_[i] * m_[i] / (cth * V2);  // |d(measured)/dz|
    float w = w_[i] * g * g;
    z_[i] = z;
    wz_[i] = w;
    S += w;
    St += w * t_[i];
    Sz += w * z;
    used++;
  }
  c->used = used;
  if (used < 3 || !(S > 0.0f)) {
    c->chi2 = kHugeChi2;
    return;
  }
  float tb = St / S, zb = Sz / S;
  float Stt = 0.0f, Stz = 0.0f, Szz = 0.0f;
  for (int i = 0; i < n_; i++) {
    float w = wz_[i];
    if (w <= 0.0f) continue;
    float dt = t_[i] - tb, dz = z_[i] - zb;
    Stt += w * dt * dt;
    Stz += w * dt * dz;
    Szz += w * dz * dz;
  }
  const float is2 = 1.0f / (s0_ * s0_);
  float B = (Stz * is2 + B0_ / varB0_) / (Stt * is2 + 1.0f / varB0_);
  float data = (Szz - 2.0f * B * Stz + B * B * Stt) * is2;
  if (data < 0.0f) data = 0.0f;
  float dtc = tc - tc0_;
  float dtau = c->tau - tau0_;
  c->chi2 = data + (B - B0_) * (B - B0_) / varB0_ + excluded * kExcludedChi2 +
            impossible * kImpossibleChi2 + dtc * dtc / var_tc_ + dtau * dtau / var_tau_;
  c->A = zb - B * tb;
  c->B = B;
  c->data_chi2 = data;
}

void TrackFitter::searchGrid(float tc_lo, float tc_hi, int ntc, float tau_lo, float tau_hi,
                             int ntau, Cand* best) {
  if (tau_lo < 0.0f) tau_lo = 0.0f;
  Cand c;
  best->chi2 = kHugeChi2;
  for (int i = 0; i < ntc; i++) {
    c.tc = ntc > 1 ? tc_lo + (tc_hi - tc_lo) * i / (ntc - 1) : tc_lo;
    for (int j = 0; j < ntau; j++) {
      c.tau = ntau > 1 ? tau_lo + (tau_hi - tau_lo) * j / (ntau - 1) : tau_lo;
      evaluate(&c);
      if (c.chi2 < best->chi2) *best = c;
    }
  }
}

int TrackFitter::markOutliers(const Cand& best) {
  Cand c = best;
  evaluate(&c);  // refresh z_ / wz_ for this geometry
  if (c.chi2 >= kHugeChi2) return 0;
  const float is2 = 1.0f / (s0_ * s0_);
  // Normalized squared residuals of the frames in use; the median gives a robust noise scale.
  int m = 0;
  for (int i = 0; i < n_; i++) {
    e2_[i] = -1.0f;
    if (wz_[i] <= 0.0f) continue;
    float r = z_[i] - (c.A + c.B * t_[i]);
    e2_[i] = wz_[i] * r * r * is2;
    tmp_[m++] = e2_[i];
  }
  if (m < 6) return 0;
  std::nth_element(tmp_, tmp_ + m / 2, tmp_ + m);
  float scale = tmp_[m / 2] / 0.455f;  // median of chi-square(1) is 0.455
  if (scale < 1.0f) scale = 1.0f;
  float thr = kOutlierSigma * kOutlierSigma * scale;
  int marked = 0, max_marked = m / 4;
  while (marked < max_marked) {
    int worst = -1;
    for (int i = 0; i < n_; i++)
      if (e2_[i] > thr && (worst < 0 || e2_[i] > e2_[worst])) worst = i;
    if (worst < 0) break;
    ok_[worst] = 0;
    e2_[worst] = -1.0f;
    marked++;
  }
  return marked;
}

// Search the whole (t_c, tau) range, cutting frames per candidate by angle.
void TrackFitter::solveFree(Cand* best) {
  fixed_ = false;
  const int NTC = 41, NTAU = 16;
  searchGrid(tc_lo_, tc_hi_, NTC, 0.0f, tau_max_, NTAU, best);
  if (best->chi2 >= kHugeChi2) return;
  dtc_ = (tc_hi_ - tc_lo_) / (NTC - 1);
  dtau_ = tau_max_ / (NTAU - 1);
  float dtc = dtc_, dtau = dtau_;
  for (int pass = 0; pass < 3; pass++) {
    if (pass < 2) markOutliers(*best);
    float c_tc = best->tc, c_tau = best->tau;
    searchGrid(c_tc - 1.5f * dtc, c_tc + 1.5f * dtc, 7, c_tau - 1.5f * dtau, c_tau + 1.5f * dtau,
               7, best);
    if (best->chi2 >= kHugeChi2) return;
    dtc *= 0.5f;
    dtau *= 0.5f;
  }
  evaluate(best);  // leave z_/wz_ for the chosen geometry
}

// Choose the frames to fit from geometry g: those within the angle limit, not straddling
// the pass, on the right side of it.
void TrackFitter::fixFrames(const Cand& g) {
  const float tau2 = g.tau * g.tau;
  const float cos2_min = cos_min_ * cos_min_;
  for (int i = 0; i < n_; i++) {
    float s = t_[i] - g.tc;
    float s2 = s * s;
    float c2 = s2 > 0.0f ? s2 / (s2 + tau2) : 0.0f;
    use_[i] = classify(s) == kFrameUse && c2 >= cos2_min ? 1 : 0;
  }
}

// Refine the geometry near *best with the frame set fixed.
void TrackFitter::solveFixed(Cand* best) {
  fixed_ = true;
  float dtc = dtc_, dtau = dtau_;
  for (int pass = 0; pass < 4; pass++) {
    if (pass == 1) markOutliers(*best);
    float c_tc = best->tc, c_tau = best->tau;
    searchGrid(c_tc - 2.0f * dtc, c_tc + 2.0f * dtc, 9, c_tau - 2.0f * dtau, c_tau + 2.0f * dtau,
               9, best);
    if (best->chi2 >= kHugeChi2) break;
    dtc *= 0.5f;
    dtau *= 0.5f;
  }
  evaluate(best);
  fixed_ = false;
}

bool TrackFitter::fit(const uint32_t* centers, const float* f_hz, const float* snr_db,
                      const float* noise_db, int n, const Config& cfg, const SampleSource* src,
                      ShotResult* out) {
  if (n < 3) return false;
  if (n > MAX_TRACK) n = MAX_TRACK;
  if (!fft_ready_) {
    fft_.init();
    fft_ready_ = true;
  }
  n_ = n;
  fs_ = cfg.fs;
  hz_per_mps_ = cfg.hz_per_mps;
  downrange_ = cfg.mount == 0;
  c0_ = centers[0];
  const float T = WIN / cfg.fs;

  int strong = 0;
  for (int i = 0; i < n; i++) {
    t_[i] = (float)(centers[i] - c0_) / cfg.fs;
    f_raw_[i] = f_[i] = f_hz[i];
    snr_raw_[i] = snr_[i] = powf(10.0f, snr_db[i] / 10.0f);
    noise_[i] = powf(10.0f, noise_db[i] / 10.0f);
    z_[i] = f_hz[i] / cfg.hz_per_mps;
    if (snr_db[i] > snr_db[strong]) strong = i;
  }
  std::nth_element(z_, z_ + n / 2, z_ + n);
  V_ = z_[n / 2];
  if (V_ < 1.0f) return false;

  // A frame's error is frequency noise (shrinking with SNR) plus a floor for what the model
  // does not capture; weights are 1/variance in units of s0^2.
  s0_ = kFreqNoise * cfg.fs / (kPi * WIN) / cfg.hz_per_mps;
  const float V2 = V_ * V_;
  B0_ = V2 * cfg.k_default;
  float sB = V2 * cfg.k_default * (cfg.k_prior_frac > 0.001f ? cfg.k_prior_frac : 0.001f);
  varB0_ = sB * sB;
  if (varB0_ < 1e-12f) varB0_ = 1e-12f;
  muzzle_dt_ = downrange_ ? cfg.dist_m / V_ : 0.0f;
  tau_max_ = cfg.max_offset_m / V_;
  if (!downrange_ && cfg.offset_m > 0.0f) {
    // Beside the barrel the offset is set by the mount and barely changes between shots.
    tau0_ = cfg.offset_m / V_;
    float st = (cfg.offset_sigma_m > 0.001f ? cfg.offset_sigma_m : 0.001f) / V_;
    var_tau_ = st * st;
    if (tau_max_ < tau0_ + 4.0f * st) tau_max_ = tau0_ + 4.0f * st;
  } else {
    tau0_ = 0.0f;
    var_tau_ = tau_max_ * tau_max_;
  }
  const float t_first = t_[0], t_last = t_[n - 1];
  if (downrange_) {
    // The projectile reaches the radar about dist/v after it leaves the muzzle, which is
    // around the first frame when the radar sees it from the muzzle on; the track usually
    // ends around or a little after the pass.
    float guess = t_first + cfg.dist_m / V_;
    if (guess > t_last + 0.002f) guess = t_last + 0.002f;
    tc0_ = guess;
    var_tc_ = 0.004f * 0.004f;
    tc_lo_ = t_first + 0.2f * (t_last - t_first);
    tc_hi_ = t_last + 0.015f;
  } else {
    // On the barrel the track starts as the projectile leaves the muzzle, beside the radar;
    // a loud start is detected while it is still in the last part of the first window.
    tc0_ = t_first + 0.1f * T;
    var_tc_ = 0.0025f * 0.0025f;
    tc_lo_ = t_first - T - 0.003f;
    tc_hi_ = t_first + kOnsetEarly * T;
  }

  cos_min_ = cosf(cfg.max_angle_deg * kPi / 180.0f);
  Cand g;
  // 1. Geometry from the frames with the raw track's sweep removed, loose error floor.
  if (src) remeasure(src, nullptr);
  setWeights(kLooseFloorMps);
  solveFree(&g);
#ifdef DSP_DEBUG
  if (g_fit_debug) {
    printf("stage 0 (raw sweep removed, loose floor):\n");
    debugEval(g.tc, g.tau);
    if (g_debug_tau >= 0.0f) {
      printf(" truth at stage 0:\n");
      debugEval(g_debug_tc, g_debug_tau);
    }
  }
#endif
  if (g.chi2 >= kHugeChi2) return false;
  // 2. Re-measure with the model's sweep, fix the frame set, refine with the real floor.
  const int iters = src ? 3 : 1;
  for (int iter = 0; iter < iters; iter++) {
    if (src) {
      for (int i = 0; i < n; i++) {
        f_[i] = f_raw_[i];
        snr_[i] = snr_raw_[i];
      }
      remeasure(src, &g);
    }
    setWeights(cfg.model_err_mps);
    fixFrames(g);
    solveFixed(&g);
#ifdef DSP_DEBUG
    if (g_fit_debug) {
      printf("stage %d (model sweep removed, fixed frames):\n", iter + 1);
      fixed_ = true;
      debugEval(g.tc, g.tau);
      fixed_ = false;
    }
#endif
    if (g.chi2 >= kHugeChi2) return false;
  }

  // Speed where the projectile passes the radar, and its uncertainty.
  const float is2 = 1.0f / (s0_ * s0_);
  float S = 0.0f, St = 0.0f, Stt = 0.0f, rs = 0.0f;
  for (int i = 0; i < n; i++) {
    float w = wz_[i];
    if (w <= 0.0f) continue;
    S += w;
    St += w * t_[i];
    float r = z_[i] - (g.A + g.B * t_[i]);
    rs += w * r * r;
  }
  if (!(S > 0.0f)) return false;
  float tm = St / S;
  for (int i = 0; i < n; i++)
    if (wz_[i] > 0.0f) Stt += wz_[i] * (t_[i] - tm) * (t_[i] - tm);
  float varB = 1.0f / (Stt * is2 + 1.0f / varB0_);
  float zc = g.A + g.B * g.tc;
  float v_pass = V2 / (zc + V_);
  if (!(v_pass > 0.0f)) return false;
  float k = g.B / V2;
  int dof = g.used - 4;
  float red = dof > 0 ? g.data_chi2 / dof : 1.0f;
  float var_z = s0_ * s0_ / S + (g.tc - tm) * (g.tc - tm) * varB;
  float scale = v_pass / V_;
  float sigma = sqrtf(var_z * (red > 1.0f ? red : 1.0f)) * scale * scale;

  uint8_t flags = 0;
  if (varB > 0.5f * varB0_) flags |= SHOT_K_DEFAULT;
  if (g.used * 2 < n) flags |= SHOT_OFF_LINE;
  if (red > 25.0f) flags |= SHOT_POOR_FIT;
  if (snr_db[strong] < cfg.weak_db || g.used < 8) flags |= SHOT_WEAK;

  // Quadratic drag gives v(x) = v0 * exp(-k x). Downrange the radar sits dist_m in front of
  // the muzzle. Beside the barrel the radar's closest point is a little behind the muzzle.
  if (downrange_) {
    out->v_chrono_mps = v_pass;
    out->v_muzzle_mps = v_pass * expf(k * cfg.dist_m);
  } else {
    out->v_muzzle_mps = v_pass * expf(-k * cfg.muzzle_ahead_m);
    out->v_chrono_mps = out->v_muzzle_mps;
  }
  out->sigma_mps = sigma;
  out->k_used = k;
  out->offset_m = g.tau * v_pass;
  out->t_pass_s = g.tc;
  out->resid_mps = sqrtf(rs / S);
  out->peak_snr_db = snr_db[strong];
  out->f_strong_hz = f_hz[strong];
  out->n_frames = (uint16_t)n;
  out->n_used = (uint16_t)g.used;
  out->flags = flags;
  return true;
}

#ifdef DSP_DEBUG
void TrackFitter::debugEval(float tc, float tau) {
  Cand c;
  c.tc = tc;
  c.tau = tau;
  evaluate(&c);
  printf("  geometry tc %.3f ms tau %.3f ms: chi2 %.2f (data %.2f), used %d, k %.4f, v_pass %.3f fps\n",
         tc * 1e3, tau * 1e3, c.chi2, c.data_chi2, c.used, c.B / (V_ * V_),
         V_ * V_ / (c.A + c.B * tc + V_) / 0.3048f);
  const float is2 = 1.0f / (s0_ * s0_);
  for (int i = 0; i < n_; i++) {
    float raw = f_raw_[i] / hz_per_mps_ / 0.3048f;
    if (wz_[i] <= 0.0f) {
      printf("    %7.3f ms  raw %8.3f  now %8.3f fps  %s\n", t_[i] * 1e3, raw, m_[i] / 0.3048f,
             ok_[i] ? "excluded" : "outlier");
      continue;
    }
    float r = z_[i] - (c.A + c.B * t_[i]);
    printf("    %7.3f ms  raw %8.3f  now %8.3f fps  snr %5.1f dB  resid %+7.3f fps  chi2 %7.2f\n",
           t_[i] * 1e3, raw, m_[i] / 0.3048f, 10.0f * log10f(snr_[i]), -r / 0.3048f,
           wz_[i] * r * r * is2);
  }
}
#endif

}  // namespace dsp
