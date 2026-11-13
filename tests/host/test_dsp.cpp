// Host test for the firmware signal processing (firmware/squidchrony/dsp.*).
//
// Builds synthetic radar signals at the ADC's 200 kS/s, runs them through the same chain the
// firmware uses (Decimator -> FrameAnalyzer -> ShotTracker -> TrackFitter), and compares the
// reported speeds with the true ones. Exits non-zero if a check fails.
//
// The simulated projectile flies a straight line past the radar at a sideways offset, so the
// radar sees the closing speed v*cos(angle), with a Gaussian antenna beam and 1/r^4 echo power.
//
//   make -C tests/host            run all checks
//   ./test_dsp 200                more trials per row
//   ./test_dsp dump <setup> <snr> print the frames of one simulated shot (setup 0-5)
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "dsp.h"

using namespace dsp;

namespace {

constexpr double RAW_FS = 200000.0;
constexpr double F0 = 24.125e9;
constexpr double C = 299792458.0;
constexpr double LAMBDA = C / F0;
constexpr int MID = 1862;       // amplifier output at rest: 1.5 V on a 3.3 V ADC
constexpr double FPS = 0.3048;  // m/s per ft/s
constexpr double DEG = M_PI / 180.0;

struct Setup {
  const char* name;
  double v0_fps;
  double k;             // true drag constant, 1/m (dv/dx = -k v)
  double k_preset;      // what the firmware is told
  uint8_t mount;        // 0 downrange facing the shooter, 1 beside the barrel facing downrange
  double dist_m;        // downrange: muzzle-to-radar distance the user entered
  double dist_err_m;    // how far off that entry may be (uniform +-)
  double h_min, h_max;  // sideways offset of the flight line from the radar (uniform)
  double back_m;        // barrel: radar face this far behind the muzzle
  double beam_deg;      // one-way half-power half-width of the beam in the offset plane
  double fly_m;         // how far the projectile flies in the simulation
};

struct ShotSpec {
  double t0;  // muzzle exit time, s
  double v0;  // m/s
  double h;   // offset, m
  double D;   // true muzzle-to-radar distance, m
  double ph;  // phase
};

struct Truth {
  double v_pass;    // speed where it passes the radar (m/s)
  double v_muzzle;  // m/s
  double t_pass;    // raw-stream time of closest approach (s)
};

struct Scenario {
  double snr_db = 25.0;      // spectrogram SNR on the beam axis at 1 m
  double noise_raw = 6.0;    // analog noise at the ADC, counts rms at 200 kS/s
  double tone_hz = 0.0;      // steady interferer (e.g. revved flywheels), 0 = none
  double tone_snr_db = 0.0;  // its spectrogram SNR
  double total_s = 0.20;     // length of the recording
};

double firNoiseGain() {
  double s = 0;
  for (int i = 0; i < FIR_TAPS; i++) {
    double h = FIR_COEFFS[i] / 32768.0;
    s += h * h;
  }
  return std::sqrt(s);
}

// Raw-sample time (s) of a decimated-stream sample index, accounting for the FIR delay.
double decimatedToRawTime(double m) {
  return (FIR_DECIM * m + (FIR_DECIM - 1) - (FIR_TAPS - 1) / 2.0) / RAW_FS;
}

// Tone amplitude (counts) whose Hann-window spectrogram SNR is snr_db.
double ampForSnr(double snr_db, double noise_raw) {
  double sigma_dec = noise_raw * firNoiseGain();
  return std::sqrt(6.0 * sigma_dec * sigma_dec * std::pow(10.0, snr_db / 10.0) / WIN);
}

Truth truthFor(const Setup& p, const ShotSpec& s) {
  Truth t;
  double k = p.k;
  if (p.mount == 0) {
    t.v_pass = s.v0 * std::exp(-k * s.D);
    t.t_pass = s.t0 + (std::exp(k * s.D) - 1) / (k * s.v0);
  } else {
    // The radar's closest point is back_m behind the muzzle, inside the barrel.
    t.v_pass = s.v0 * std::exp(k * p.back_m);
    t.t_pass = s.t0 - (1 - std::exp(-k * p.back_m)) / (k * s.v0);
  }
  t.v_muzzle = s.v0;
  return t;
}

// Position of one projectile at raw time t: distance u still to go before (downrange) or
// already past (barrel) the radar's closest point, its speed, and whether it is in flight.
bool kinematics(const Setup& p, const ShotSpec& s, double t, double* u, double* v) {
  double tp = t - s.t0;
  if (tp < 0) return false;
  double x = std::log1p(p.k * s.v0 * tp) / p.k;  // distance flown from the muzzle
  if (x > p.fly_m) return false;
  *v = s.v0 / (1 + p.k * s.v0 * tp);
  *u = p.mount == 0 ? s.D - x : x + p.back_m;
  return true;
}

// Echo of one projectile at raw time t (0 outside its flight).
double echo(const Setup& p, const ShotSpec& s, double a_ref, double t) {
  double u, v;
  if (!kinematics(p, s, t, &u, &v)) return 0.0;
  double r = std::sqrt(u * u + s.h * s.h);
  double gain;
  if (p.mount == 0) {
    if (u > 0) {
      double off = std::fabs(std::atan2(s.h, u) - std::atan2(s.h, s.D));  // aimed at the muzzle
      gain = std::exp(-std::log(2.0) * std::pow(off / (p.beam_deg * DEG), 2));
    } else {
      gain = 0.18;  // behind the radar: back lobe, about -15 dB
    }
  } else {
    double off = std::atan2(s.h, u);  // beam points downrange, parallel to the barrel
    gain = std::exp(-std::log(2.0) * std::pow(off / (p.beam_deg * DEG), 2));
  }
  double g = 1.0 / r;
  return a_ref * gain * g * g * std::cos(4 * M_PI * r / LAMBDA + s.ph);
}

// What the radar sees at time t: closing speed |v cos(angle)| (m/s).
double radialSpeed(const Setup& p, const ShotSpec& s, double t) {
  double u, v;
  if (!kinematics(p, s, t, &u, &v)) return 0.0;
  return v * std::fabs(u) / std::sqrt(u * u + s.h * s.h);
}

ShotTracker& shotTracker() {
  static ShotTracker tr;  // large; keep it off the stack
  return tr;
}

struct Found {
  ShotResult r;
  double t_strong;  // raw time of the strongest frame
  double t_first;   // raw time of the first frame
};

struct FrameLog {
  double t;  // raw time of the frame center
  FrameResult fr;
};

struct Run {
  std::vector<Found> shots;
  std::vector<FrameLog> frames;
  int rejected = 0;
};

class VectorSource : public SampleSource {
 public:
  explicit VectorSource(const std::vector<int16_t>* v) : v_(v) {}
  bool read(uint32_t first, int count, int16_t* out) const override {
    if ((size_t)first + count > v_->size()) return false;
    std::copy(v_->begin() + first, v_->begin() + first + count, out);
    return true;
  }

 private:
  const std::vector<int16_t>* v_;
};

bool g_use_source = true;
double g_fit_seconds = 0.0;  // time spent finishing tracks (the fit)
long g_fit_count = 0, g_fit_frames = 0;

Run runChain(const Setup& p, const std::vector<ShotSpec>& shots, const Scenario& sc,
             const Config& cfg, std::mt19937& rng, bool log_frames = false) {
  std::normal_distribution<double> gauss(0.0, 1.0);
  std::uniform_real_distribution<double> uni(0.0, 2 * M_PI);
  const long n_raw = (long)(sc.total_s * RAW_FS);
  const double a_ref = ampForSnr(sc.snr_db, sc.noise_raw);
  const double tone_amp = sc.tone_hz > 0 ? ampForSnr(sc.tone_snr_db, sc.noise_raw) : 0.0;
  const double ph_tone = uni(rng);

  Decimator dec;
  dec.reset();
  static FrameAnalyzer fa;  // large; keep it off the stack
  ShotTracker& tr = shotTracker();
  fa.init(cfg);
  tr.reset();

  std::vector<int16_t> hist;
  hist.reserve(n_raw / FIR_DECIM + 8);
  VectorSource src(&hist);
  tr.setSampleSource(g_use_source ? &src : nullptr);
  size_t next_frame_end = WIN;
  Run out;
  for (long i = 0; i < n_raw; i++) {
    double t = i / RAW_FS;
    double s = sc.noise_raw * gauss(rng);
    for (const ShotSpec& sh : shots) s += echo(p, sh, a_ref, t);
    if (tone_amp > 0) s += tone_amp * std::cos(2 * M_PI * sc.tone_hz * t + ph_tone);
    long raw = std::lround(MID + s);
    raw = std::min(4095L, std::max(0L, raw));

    int16_t y;
    if (!dec.push((int16_t)(raw - 2048), &y)) continue;
    hist.push_back(y);
    if (hist.size() != next_frame_end) continue;
    next_frame_end += HOP;

    size_t start = hist.size() - WIN;
    FrameResult fr = fa.analyze(&hist[start], tr.trackFreq());
    ShotResult res;
    uint32_t center = (uint32_t)(start + WIN / 2);
    if (log_frames) out.frames.push_back({decimatedToRawTime(center - 0.5), fr});
    auto t_a = std::chrono::steady_clock::now();
    ShotTracker::Result rr = tr.push(fr, center, true, cfg, &res);
    if (rr == ShotTracker::SHOT) {
      g_fit_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t_a).count();
      g_fit_count++;
      g_fit_frames += res.n_frames;
    }
    if (rr == ShotTracker::REJECTED) {
      fa.absorbLastFrame();
      out.rejected++;
    } else if (rr == ShotTracker::SHOT) {
      Found f;
      f.r = res;
      f.t_strong = decimatedToRawTime(res.strong_sample - 0.5);
      f.t_first = decimatedToRawTime(res.first_sample - 0.5);
      out.shots.push_back(f);
    }
  }
  return out;
}

Config configFor(const Setup& p) {
  Config cfg;
  cfg.hz_per_mps = hzPerMps((float)F0);
  cfg.k_default = (float)p.k_preset;
  cfg.mount = p.mount;
  cfg.dist_m = (float)p.dist_m;
  cfg.muzzle_ahead_m = (float)p.back_m;
  return cfg;
}

ShotSpec randomShot(const Setup& p, double t0, std::mt19937& rng) {
  std::uniform_real_distribution<double> u01(0.0, 1.0);
  ShotSpec s;
  s.t0 = t0;
  s.v0 = p.v0_fps * FPS;
  s.h = p.h_min + (p.h_max - p.h_min) * u01(rng);
  s.D = p.dist_m + p.dist_err_m * (2 * u01(rng) - 1);
  s.ph = 2 * M_PI * u01(rng);
  return s;
}

struct Stats {
  double mean = 0, sd = 0, p95 = 0;
  int n = 0;
};

Stats stats(std::vector<double> v) {
  Stats s;
  s.n = (int)v.size();
  if (v.empty()) return s;
  for (double x : v) s.mean += x;
  s.mean /= v.size();
  for (double x : v) s.sd += (x - s.mean) * (x - s.mean);
  s.sd = std::sqrt(s.sd / v.size());
  for (double& x : v) x = std::fabs(x);
  std::sort(v.begin(), v.end());
  s.p95 = v[(size_t)std::lround(0.95 * (v.size() - 1))];
  return s;
}

int g_failures = 0;
void check(bool ok, const char* what) {
  std::printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
  if (!ok) g_failures++;
}

struct SweepRow {
  double snr;
  int found, trials, extra;
  Stats pass, muzzle, sigma, frames, used, offset_err;
};

// One shot per recording; returns per-SNR rows.
//                 name                   fps    k      preset mount dist   err   h_min h_max back  beam  fly
const Setup kSetups[] = {
    {"paintball 300 fps",               300.0, 0.022, 0.022, 0, 0.914, 0.03, 0.04, 0.10, 0.00, 16.0, 3.0},
    {"paintball 300 fps",               300.0, 0.022, 0.022, 0, 1.829, 0.05, 0.05, 0.15, 0.00, 16.0, 4.0},
    {"half dart 150 fps",               150.0, 0.098, 0.098, 0, 0.914, 0.03, 0.04, 0.10, 0.00, 16.0, 3.0},
    {"half dart 150 fps",               150.0, 0.127, 0.098, 0, 0.914, 0.03, 0.04, 0.10, 0.00, 16.0, 3.0},
    {"paintball 285 fps",               285.0, 0.022, 0.022, 1, 0.000, 0.00, 0.03, 0.05, 0.02, 16.0, 4.0},
    {"half dart 150 fps",               150.0, 0.098, 0.098, 1, 0.000, 0.00, 0.03, 0.05, 0.02, 16.0, 4.0},
};

bool g_verbose = false;

uint32_t trialSeed(int setup, double snr, int trial) {
  return (uint32_t)(setup * 1000003u + (uint32_t)(snr * 1000) * 7919u + trial * 104729u + 17u);
}

std::vector<SweepRow> sweep(int which, const std::vector<double>& snrs, int trials) {
  const Setup& p = kSetups[which];
  std::vector<SweepRow> rows;
  Config cfg = configFor(p);
  std::printf("\n%s, %s, offset %.0f-%.0f cm, beam +-%.0f deg%s; %d trials per row\n", p.name,
              p.mount == 0 ? "radar downrange" : "radar beside the barrel", p.h_min * 100,
              p.h_max * 100, p.beam_deg, p.k != p.k_preset ? ", drag preset 30% low" : "", trials);
  if (p.mount == 0)
    std::printf("  radar %.2f m (%.0f ft) from the muzzle, entered within +-%.0f cm\n", p.dist_m,
                p.dist_m / FPS, p.dist_err_m * 100);
  std::printf("  SNR@1m found | frames used | at-radar err (fps): mean   SD  95%% | muzzle err: mean   SD  95%% | sigma | offset err cm\n");
  for (double snr : snrs) {
    SweepRow row{};
    row.snr = snr;
    row.trials = trials;
    std::vector<double> e_pass, e_muz, sig, frames, used, off;
    for (int t = 0; t < trials; t++) {
      std::mt19937 rng(trialSeed(which, snr, t));
      Scenario sc;
      sc.snr_db = snr;
      ShotSpec s = randomShot(p, 0.03, rng);
      Run run = runChain(p, {s}, sc, cfg, rng);
      if (run.shots.empty()) {
        if (g_verbose) std::printf("    missed: dump %d %.0f %d\n", which, snr, t);
        continue;
      }
      row.found++;
      row.extra += (int)run.shots.size() - 1;
      Truth tr = truthFor(p, s);
      const ShotResult& r = run.shots[0].r;
      e_pass.push_back((r.v_chrono_mps - (p.mount == 0 ? tr.v_pass : tr.v_muzzle)) / FPS);
      if (g_verbose && (std::fabs(e_pass.back()) > 1.0 || run.shots.size() > 1))
        std::printf("    err %+.2f fps, %zu shot(s), flags 0x%02x: dump %d %.0f %d\n", e_pass.back(),
                    run.shots.size(), r.flags, which, snr, t);
      e_muz.push_back((r.v_muzzle_mps - tr.v_muzzle) / FPS);
      sig.push_back(r.sigma_mps / FPS);
      frames.push_back(r.n_frames);
      used.push_back(r.n_used);
      off.push_back((r.offset_m - s.h) * 100);
    }
    row.pass = stats(e_pass);
    row.muzzle = stats(e_muz);
    row.sigma = stats(sig);
    row.frames = stats(frames);
    row.used = stats(used);
    row.offset_err = stats(off);
    std::printf("  %5.0f  %3d/%d | %5.1f %5.1f |              %+6.2f %5.2f %5.2f |      %+6.2f %5.2f %5.2f | %5.2f | %+5.1f %4.1f%s\n",
                snr, row.found, trials, row.frames.mean, row.used.mean, row.pass.mean, row.pass.sd,
                row.pass.p95, row.muzzle.mean, row.muzzle.sd, row.muzzle.p95, row.sigma.mean,
                row.offset_err.mean, row.offset_err.sd, row.extra ? "  (split tracks)" : "");
    rows.push_back(row);
  }
  return rows;
}

void checkRows(const Setup& p, const std::vector<SweepRow>& rows, double from_db, double pass_fps,
               double muzzle_fps) {
  for (const SweepRow& r : rows) {
    if (r.snr < from_db) continue;
    char msg[240];
    std::snprintf(msg, sizeof msg, "%s @ %.0f dB: found at least 95%% of shots, one track each", p.name, r.snr);
    check(r.found >= r.trials * 95 / 100 && r.extra == 0, msg);
    std::snprintf(msg, sizeof msg, "%s @ %.0f dB: speed at the radar within %.1f fps for 95%% of shots", p.name, r.snr, pass_fps);
    check(r.pass.p95 <= pass_fps, msg);
    if (muzzle_fps > 0) {
      std::snprintf(msg, sizeof msg, "%s @ %.0f dB: muzzle speed within %.1f fps for 95%% of shots", p.name, r.snr, muzzle_fps);
      check(r.muzzle.p95 <= muzzle_fps, msg);
    }
  }
}


int dump(int which, double snr, int trial) {
  const Setup& p = kSetups[which];
  std::mt19937 rng(trialSeed(which, snr, trial));
  Config cfg = configFor(p);
  ShotSpec s = randomShot(p, 0.03, rng);
  Scenario sc;
  sc.snr_db = snr;
  g_fit_debug = true;
  {
    // Where the first frame will be is not known yet; take it from a dry run.
    std::mt19937 rng2 = rng;
    Run dry = runChain(p, {s}, sc, cfg, rng2, false);
    Truth tr0 = truthFor(p, s);
    if (!dry.shots.empty()) {
      g_debug_tc = (float)(tr0.t_pass - dry.shots[0].t_first);
      g_debug_tau = (float)(s.h / tr0.v_pass);
    }
  }
  Run run = runChain(p, {s}, sc, cfg, rng, true);
  g_fit_debug = false;
  Truth tr = truthFor(p, s);
  std::printf("%s mount %d: h %.1f cm, D %.3f m, v0 %.2f fps; pass at %.2f ms, v_pass %.2f fps\n",
              p.name, p.mount, s.h * 100, s.D, s.v0 / FPS, tr.t_pass * 1000, tr.v_pass / FPS);
  std::printf("   t(ms)   f(Hz)  meas(fps) radial(fps) err   snr  score\n");
  for (const FrameLog& f : run.frames) {
    if (f.fr.score_db < 8) continue;
    double rad = radialSpeed(p, s, f.t) / FPS;
    double meas = f.fr.f_hz / cfg.hz_per_mps / FPS;
    std::printf("  %7.2f %7.0f  %8.2f  %8.2f  %+6.2f  %5.1f %5.1f\n", f.t * 1000, f.fr.f_hz, meas, rad,
                meas - rad, f.fr.snr_db, f.fr.score_db);
  }
  for (const Found& f : run.shots) {
    const ShotResult& r = f.r;
    std::printf("truth relative to the first frame: tc %.3f ms, tau %.3f ms\n", (tr.t_pass - f.t_first) * 1e3,
                s.h / tr.v_pass * 1e3);
    shotTracker().fitter().debugEval((float)(tr.t_pass - f.t_first), (float)(s.h / tr.v_pass));
    shotTracker().fitter().debugEval(r.t_pass_s, r.offset_m / r.v_chrono_mps);
    std::printf("shot: first frame %.2f ms, pass %.2f ms (true %.2f), frames %d used %d, v_chrono %.2f (err %+.2f) "
                "v_muzzle %.2f (err %+.2f) sigma %.2f fps, k %.4f, offset %.1f cm, flags 0x%02x\n",
                f.t_first * 1000, (f.t_first + r.t_pass_s) * 1000, tr.t_pass * 1000, r.n_frames,
                r.n_used, r.v_chrono_mps / FPS, (r.v_chrono_mps - tr.v_pass) / FPS,
                r.v_muzzle_mps / FPS, (r.v_muzzle_mps - tr.v_muzzle) / FPS, r.sigma_mps / FPS,
                r.k_used, r.offset_m * 100, r.flags);
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (getenv("NO_REMEASURE")) g_use_source = false;
  if (argc >= 2 && std::strcmp(argv[1], "dump") == 0) {
    int which = argc > 2 ? std::atoi(argv[2]) : 0;
    double snr = argc > 3 ? std::atof(argv[3]) : 25.0;
    int trial = argc > 4 ? std::atoi(argv[4]) : 0;
    return dump(which, snr, trial);
  }
  int trials = argc > 1 ? std::atoi(argv[1]) : 50;
  g_verbose = getenv("VERBOSE") != nullptr;
  std::mt19937 rng(12345);
  std::printf("FIR noise gain %.3f; Doppler %.2f Hz per fps; frame hop %.2f ms, window %.2f ms\n",
              firNoiseGain(), hzPerMps((float)F0) * FPS, 1000.0 * HOP / 50000.0,
              1000.0 * WIN / 50000.0);

  // Decimation filter: a 33 kHz tone at 200 kS/s would fold to 17 kHz (a fake 350 fps shot).
  {
    Decimator dec;
    dec.reset();
    double sum2 = 0;
    int n = 0;
    const double amp = 30000.0;
    for (long i = 0; i < 200000; i++) {
      int16_t s = (int16_t)std::lround(amp * std::cos(2 * M_PI * 33000.0 * i / RAW_FS));
      int16_t y;
      if (dec.push(s, &y) && i > 1000) {
        sum2 += (double)y * y;
        n++;
      }
    }
    double rms = std::sqrt(sum2 / n);
    double att = rms > 0 ? 20 * std::log10(rms / (amp / std::sqrt(2.0))) : -99.0;
    std::printf("\nDecimator: a 33 kHz tone comes out %.1f dB down\n", att);
    check(att < -55.0, "decimator rejects 33 kHz by more than 55 dB");
  }

  const Setup& pb3 = kSetups[0];
  const std::vector<double> snrs{15.0, 20.0, 25.0, 30.0};
  const char* only = getenv("ONLY");  // e.g. ONLY=4 runs just that setup
  auto want = [&](int i) { return !only || std::atoi(only) == i; };
  if (want(0)) checkRows(kSetups[0], sweep(0, snrs, trials), 20.0, 1.0, 1.5);
  if (want(1)) checkRows(kSetups[1], sweep(1, snrs, trials), 25.0, 1.0, 1.5);
  if (want(2)) checkRows(kSetups[2], sweep(2, snrs, trials), 20.0, 1.0, 1.5);
  if (want(3)) sweep(3, {25.0}, trials);
  if (want(4)) checkRows(kSetups[4], sweep(4, snrs, trials), 20.0, 1.0, 0.0);
  if (want(5)) checkRows(kSetups[5], sweep(5, snrs, trials), 20.0, 1.0, 0.0);
  if (only) return g_failures ? 1 : 0;

  // Rate of fire: five paintballs 60 ms apart (16 balls/s), each must be its own shot.
  {
    Config cfg = configFor(pb3);
    int ok_runs = 0, runs = 20;
    std::vector<double> e_pass;
    for (int t = 0; t < runs; t++) {
      std::vector<ShotSpec> shots;
      for (int i = 0; i < 5; i++) shots.push_back(randomShot(pb3, 0.03 + 0.060 * i, rng));
      Scenario sc;
      sc.snr_db = 25.0;
      sc.total_s = 0.40;
      Run run = runChain(pb3, shots, sc, cfg, rng);
      if (run.shots.size() == 5) {
        ok_runs++;
        for (int i = 0; i < 5; i++)
          e_pass.push_back((run.shots[i].r.v_chrono_mps - truthFor(pb3, shots[i]).v_pass) / FPS);
      }
    }
    Stats a = stats(e_pass);
    std::printf("\nFive paintballs 60 ms apart at 25 dB: %d/%d strings fully resolved, at-radar 95%% err %.2f fps\n",
                ok_runs, runs, a.p95);
    check(ok_runs >= runs - 1, "16 balls/s: every ball is its own shot");
  }

  // Revved flywheels: a steady 9 kHz tone, 15 dB stronger than the projectile at 1 m,
  // running 0.6 s before the shot.
  {
    Config cfg = configFor(pb3);
    std::vector<double> e_pass;
    int found = 0, wrong = 0, rejected = 0, runs = 20;
    for (int t = 0; t < runs; t++) {
      Scenario sc;
      sc.snr_db = 25.0;
      sc.tone_hz = 9000.0;
      sc.tone_snr_db = 40.0;
      sc.total_s = 0.75;
      ShotSpec s = randomShot(pb3, 0.6, rng);
      Run run = runChain(pb3, {s}, sc, cfg, rng);
      rejected += run.rejected;
      bool got = false;
      for (auto& f : run.shots) {
        if (std::fabs(f.r.f_strong_hz - 9000.0) < 400.0) {
          wrong++;
        } else if (!got) {
          got = true;
          found++;
          e_pass.push_back((f.r.v_chrono_mps - truthFor(pb3, s).v_pass) / FPS);
        }
      }
    }
    Stats a = stats(e_pass);
    std::printf("\nSteady 9 kHz tone (40 dB) before and during the shot: found %d/%d, tone reported as a shot %d times,"
                " tone tracks rejected %d, at-radar 95%% err %.2f fps\n", found, runs, wrong, rejected, a.p95);
    check(wrong == 0, "a steady tone is never reported as a shot");
    check(found >= runs - 1, "shots are still found next to a steady tone");
  }

  // Noise only: no false shots.
  {
    Config cfg = configFor(pb3);
    Scenario sc;
    sc.total_s = 10.0;
    Run run = runChain(pb3, {}, sc, cfg, rng);
    std::printf("\nNoise only, 10 s: %zu false shots\n", run.shots.size());
    check(run.shots.empty(), "no false shots in 10 s of noise");
  }

  if (g_fit_count)
    std::printf("\nTrack fit on this machine: %.2f ms per shot on average (%.1f frames per track)\n",
                1e3 * g_fit_seconds / g_fit_count, (double)g_fit_frames / g_fit_count);
  std::printf("\n%s (%d failure%s)\n", g_failures ? "FAILED" : "ALL CHECKS PASSED", g_failures,
              g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
