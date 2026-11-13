// Signal processing for the radar chronograph.
//
// Portable C++ (no Arduino or Pico SDK includes) so the same code runs on the Pico and in
// the host tests under tests/host.
//
// Data flow:
//   ADC ch0 @ 200 kS/s -> Decimator (low-pass, keep 1 of 4) -> 50 kS/s stream (kept in a ring)
//   every HOP samples: FrameAnalyzer looks at the last WIN samples -> peak frequency + SNR
//   ShotTracker strings consecutive detections into one projectile track
//   TrackFitter re-measures the track's frames from the ring and fits drag plus pass-by
//   geometry, giving the speed where the projectile passes the radar and at the muzzle.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "fir_coeffs.h"

namespace dsp {

constexpr float FT_PER_M = 3.28084f;
constexpr float SPEED_OF_LIGHT = 299792458.0f;

// Analysis frames on the decimated stream.
constexpr int WIN = 256;        // 5.12 ms Hann window at 50 kS/s
constexpr int HOP = 64;         // 1.28 ms between frames
constexpr int NFFT = 512;       // window zero-padded 2x for finer peak interpolation
constexpr int MAX_TRACK = 200;  // most frames in one shot (256 ms)

// Doppler shift per m/s of closing speed for a CW radar at f0.
inline float hzPerMps(float f0_hz) { return 2.0f * f0_hz / SPEED_OF_LIGHT; }

struct Config {
  float fs = 50000.0f;           // decimated sample rate, Hz
  float band_lo_hz = 2000.0f;    // search band (2 kHz ~ 41 fps)
  float band_hi_hz = 20000.0f;   // (20 kHz ~ 408 fps)
  float detect_db = 15.0f;       // a frame counts as a detection this far above noise/background
  float weak_db = 24.0f;         // shots whose strongest frame is below this are flagged weak
  float hz_per_mps = 160.94f;    // 2*f0/c times the calibration factor
  float k_default = 0.022f;      // drag constant (1/m) in dv/dx = -k*v; paintball ~0.022
  uint8_t mount = 0;             // 0 = downrange facing the shooter, 1 = beside the barrel
  float dist_m = 0.9144f;        // downrange: muzzle-to-radar distance
  uint16_t min_frames = 6;       // shortest track reported as a shot (~7.7 ms)
  uint16_t end_gap_frames = 12;  // this many frames without the target ends a track (~15 ms)
  float jump_frac = 0.06f;       // max relative frequency jump between frames of one track
  float bg_tau_s = 0.4f;         // averaging time of the steady-interference background
  float max_track_s = 0.25f;     // longer "tracks" are steady interference, not projectiles
  uint16_t holdoff_frames = 12;  // ignore detections this long after a shot (echo after it passes)
  // Track fit
  float k_prior_frac = 0.3f;     // how far the fitted drag may wander from k_default (1 sigma)
  float model_err_mps = 0.03f;   // per-frame error floor added to the noise (model mismatch)
  float max_angle_deg = 25.0f;   // frames seen further than this off the flight line are left out
  float max_offset_m = 0.30f;    // largest flight-line-to-radar offset the fit considers
  // Beside the barrel: distance from the bore line to the middle of the radar's antenna,
  // set by the mount (0 = unknown), and how sure we are of it.
  float offset_m = 0.04f;
  float offset_sigma_m = 0.015f;
  float muzzle_ahead_m = 0.02f;  // beside the barrel: muzzle this far in front of the antenna
};

struct FrameResult {
  float f_hz;      // interpolated peak frequency
  float snr_db;    // peak power vs. the noise floor
  float score_db;  // peak power vs. max(noise floor, steady background at that frequency)
  float noise_db;  // noise floor per bin, dB re 1 count^2
};

enum ShotFlags : uint8_t {
  SHOT_WEAK = 1 << 0,       // low SNR or short track
  SHOT_K_DEFAULT = 1 << 1,  // track too short to measure drag, so about the preset was used
  SHOT_CLIPPED = 1 << 2,    // set by the caller when the ADC clipped during the shot
  SHOT_TRUNCATED = 1 << 3,  // track hit MAX_TRACK
  SHOT_OFF_LINE = 1 << 4,   // most frames were at a large angle: radar far off the line of fire
  SHOT_POOR_FIT = 1 << 5,   // the speed track did not follow the model (interference, deflection)
};

struct ShotResult {
  uint32_t first_sample;   // decimated-stream sample index of the first frame center
  uint32_t last_sample;
  uint32_t strong_sample;  // frame with the strongest echo
  float v_chrono_mps;      // speed where it passes the radar (beside the barrel: = muzzle)
  float v_muzzle_mps;      // estimated muzzle speed
  float sigma_mps;         // 1-sigma uncertainty of v_chrono from the fit
  float k_used;            // drag constant used for the estimate, 1/m
  float offset_m;          // fitted sideways distance between the flight line and the radar
  float t_pass_s;          // time the projectile passes the radar, from the first frame
  float resid_mps;         // weighted RMS residual of the speed fit
  float peak_snr_db;
  float f_strong_hz;
  uint16_t n_frames;
  uint16_t n_used;  // frames used by the fit
  uint8_t flags;
};

// Read access to the decimated stream for re-measuring a finished track. The firmware backs
// it with its ring buffer; the host tests with a vector.
class SampleSource {
 public:
  // Copy count samples starting at absolute index first. Returns false if they are gone.
  virtual bool read(uint32_t first, int count, int16_t* out) const = 0;

 protected:
  ~SampleSource() = default;
};

// Radix-2 complex FFT of NFFT points plus the Hann window used for all frames.
class Fft {
 public:
  void init();
  void run(float* re, float* im) const;
  const float* window() const { return window_; }

 private:
  float window_[WIN];
  float cos_[NFFT / 2];
  float sin_[NFFT / 2];
  uint16_t rev_[NFFT];
};

// Polyphase-style decimating FIR: push 200 kS/s samples, get 50 kS/s samples.
class Decimator {
 public:
  void reset();
  // Push one input sample (ADC counts with the mid-scale removed).
  // Returns true and writes *out when an output sample is ready (every FIR_DECIM inputs).
  inline bool push(int16_t s, int16_t* out) {
    delay_[w_] = s;
    delay_[w_ + FIR_TAPS] = s;
    if (++w_ == FIR_TAPS) w_ = 0;
    if (++phase_ < FIR_DECIM) return false;
    phase_ = 0;
    const int16_t* d = &delay_[w_];  // oldest .. newest
    int32_t acc = 0;
    for (int i = 0; i < FIR_TAPS; i++) acc += (int32_t)d[i] * FIR_COEFFS[i];
    acc = (acc + (1 << 14)) >> 15;
    if (acc > 32767) acc = 32767;
    if (acc < -32768) acc = -32768;
    *out = (int16_t)acc;
    return true;
  }

 private:
  int16_t delay_[2 * FIR_TAPS] = {};
  int w_ = 0;
  int phase_ = 0;
};

// Finds the strongest Doppler tone in one analysis window.
class FrameAnalyzer {
 public:
  void init(const Config& cfg);
  void setConfig(const Config& cfg);  // recompute band limits after a settings change
  // x: WIN samples of the decimated stream. The frame also updates the steady background,
  // except within a few bins of protect_hz (pass the frequency of the track in progress,
  // or 0 when idle) so a projectile is not learned as background while it is measured.
  FrameResult analyze(const int16_t* x, float protect_hz);
  // Treat everything in the last frame as background (after rejecting a steady tone).
  void absorbLastFrame();
  void resetBackground() { bg_ready_ = false; }

 private:
  Fft fft_;
  float re_[NFFT];
  float im_[NFFT];
  float pow_[NFFT / 2 + 1];
  float bg_[NFFT / 2 + 1];
  float scratch_[NFFT / 2 + 1];
  int klo_ = 21;
  int khi_ = 204;
  float bg_alpha_ = 0.0032f;
  float bin_hz_ = 97.66f;
  bool bg_ready_ = false;
};

// Fits the speed track of one shot.
//
// The radar measures the closing speed v*cos(theta), where theta is the angle between the
// flight line and the line of sight. The projectile cannot fly through the radar, so it passes
// at some sideways offset h, and cos(theta) = s / sqrt(s^2 + h^2) with s the distance to the
// point of closest approach. Within a few feet of the radar that reads several percent slow,
// and the offset changes from shot to shot. The fit therefore models
//
//   radial(t) = v(t) * |t - t_c| / sqrt((t - t_c)^2 + tau^2),   1/v(t) = a + k*t
//
// with t_c the time of closest approach, tau = h/v, and k the drag constant (quadratic drag
// makes 1/v exactly linear in time). For a candidate (t_c, tau), a and k follow from weighted
// linear least squares, with a prior on k around the preset; a grid over (t_c, tau) is searched
// and refined around the best point.
//
// Near the radar the closing speed changes a lot within one 5 ms frame, and the FFT peak of
// such a frame does not belong to its center. So the frames are measured again from the
// stored samples after removing the frequency sweep predicted for that window ("dechirping"):
// first the sweep of the raw track, then twice the sweep of the fitted model. What is left is
// a steady tone at the instantaneous frequency of the frame center.
#ifdef DSP_DEBUG
extern bool g_fit_debug;  // host tests: print each stage of the fit
extern float g_debug_tc, g_debug_tau;  // host tests: also show this geometry
#endif

class TrackFitter {
 public:
  // centers: frame-center sample indices; f_hz, snr_db, noise_db: the frames as detected.
  // src may be null, in which case the frames are fitted as they are (less accurate close in).
  bool fit(const uint32_t* centers, const float* f_hz, const float* snr_db, const float* noise_db,
           int n, const Config& cfg, const SampleSource* src, ShotResult* out);
#ifdef DSP_DEBUG
  // Host tests only: print the cost breakdown and per-frame residuals for one geometry, using
  // the frames and outlier marks of the last fit.
  void debugEval(float tc, float tau);
#endif

 private:
  struct Cand {
    float tc = 0, tau = 0, A = 0, B = 0, chi2 = 0, data_chi2 = 0;
    int used = 0;
  };
  void setWeights(float floor_mps);
  void evaluate(Cand* c);
  void searchGrid(float tc_lo, float tc_hi, int ntc, float tau_lo, float tau_hi, int ntau,
                  Cand* best);
  void solveFree(Cand* best);
  void fixFrames(const Cand& g);
  void solveFixed(Cand* best);
  int markOutliers(const Cand& c);
  int classify(float s) const;
  float modelHz(const Cand& c, float t) const;
  float rawHz(float t) const;
  int remeasure(const SampleSource* src, const Cand* model);

  bool fft_ready_ = false;
  Fft fft_;
  float re_[NFFT];
  float im_[NFFT];
  int16_t samples_[WIN];
  float dhz_[WIN];

  // Problem constants for the current fit
  int n_ = 0;
  bool downrange_ = true;
  float fs_ = 50000.0f;
  float hz_per_mps_ = 160.94f;
  float floor_rel_ = 0.0f;  // model error floor relative to s0
  float dtc_ = 0.0f, dtau_ = 0.0f;  // coarse grid steps
  float V_ = 1.0f;          // reference speed (m/s)
  float s0_ = 1.0f;         // speed noise (m/s) of a frame with SNR = 1 (0 dB)
  float B0_ = 0.0f, varB0_ = 1.0f;
  float tc0_ = 0.0f, var_tc_ = 1.0f, tau0_ = 0.0f, var_tau_ = 1.0f;
  float cos_min_ = 0.94f;
  float muzzle_dt_ = 0.0f;  // downrange: flight time from the muzzle to the radar
  float tc_lo_ = 0.0f, tc_hi_ = 0.0f, tau_max_ = 0.0f;
  uint32_t c0_ = 0;  // center sample of the first frame (t = 0)
  // Per-frame data and scratch
  float t_[MAX_TRACK];     // frame-center time
  float f_raw_[MAX_TRACK];
  float snr_raw_[MAX_TRACK];
  float f_[MAX_TRACK];     // current frequency estimate
  float snr_[MAX_TRACK];   // linear
  float noise_[MAX_TRACK]; // linear power per bin
  float m_[MAX_TRACK];     // measured speed, m/s
  float w_[MAX_TRACK];     // weight: 1 / (noise variance + floor variance), in units of 1/s0^2
  float z_[MAX_TRACK];
  float wz_[MAX_TRACK];
  float e2_[MAX_TRACK];
  float tmp_[MAX_TRACK];
  uint8_t ok_[MAX_TRACK];   // 0 = rejected as an outlier
  uint8_t use_[MAX_TRACK];  // frame set of the fixed-set search
  bool fixed_ = false;      // search over a fixed frame set instead of per-candidate cuts
};

// Turns per-frame detections into one track per projectile.
class ShotTracker {
 public:
  enum Result { NONE = 0, SHOT = 1, REJECTED = -1 };
  void reset();
  void setSampleSource(const SampleSource* src) { src_ = src; }
  bool active() const { return n_ > 0; }
  float trackFreq() const { return n_ > 0 ? f_last_ : 0.0f; }
  // Feed one frame (in order). center_sample is the frame-center index in the decimated stream.
  // While armed is false nothing new starts, and a track in progress is finished.
  // Returns SHOT and fills *out when a shot is complete, or REJECTED when a track lasted
  // longer than max_track_s (steady interference such as revving flywheels).
  Result push(const FrameResult& fr, uint32_t center_sample, bool armed, const Config& cfg,
              ShotResult* out);
#ifdef DSP_DEBUG
  TrackFitter& fitter() { return fitter_; }
#endif

 private:
  Result finish(const Config& cfg, ShotResult* out);
  void store(const FrameResult& fr, uint32_t center_sample);
  void clearTrack();

  const SampleSource* src_ = nullptr;
  int n_ = 0;
  int miss_ = 0;
  int holdoff_ = 0;
  float f_last_ = 0.0f;
  bool truncated_ = false;
  uint32_t t_[MAX_TRACK];
  float f_[MAX_TRACK];
  float snr_[MAX_TRACK];
  float noise_[MAX_TRACK];
  TrackFitter fitter_;
};

}  // namespace dsp
