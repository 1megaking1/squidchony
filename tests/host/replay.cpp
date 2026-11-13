// Run the firmware's signal processing on a recorded shot.
//
//   make -C tests/host replay
//   ./tests/host/replay shot.wav [key=value ...]
//
// shot.wav is what the phone page's "Last shot (WAV)" downloads: 16-bit mono at 50 kS/s (the
// decimated stream). A 200 kS/s recording is decimated first. key=value pairs are chrono
// settings, as in /api/set: for example mount=barrel offset_mm=35, or dist_ft=4 proj=dart.
// Prints every frame with a detection and each shot found.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "dsp.h"
#include "settings.h"

using namespace dsp;

namespace {

bool readWav(const char* path, std::vector<int16_t>* out, uint32_t* rate) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return false;
  std::vector<uint8_t> b;
  uint8_t buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) b.insert(b.end(), buf, buf + n);
  std::fclose(f);
  auto u16 = [&](size_t i) { return (uint32_t)b[i] | ((uint32_t)b[i + 1] << 8); };
  auto u32 = [&](size_t i) { return u16(i) | (u16(i + 2) << 16); };
  if (b.size() < 12 || std::memcmp(&b[0], "RIFF", 4) || std::memcmp(&b[8], "WAVE", 4)) return false;
  uint16_t channels = 0, bits = 0, format = 0;
  for (size_t i = 12; i + 8 <= b.size();) {
    uint32_t len = u32(i + 4);
    if (!std::memcmp(&b[i], "fmt ", 4) && len >= 16) {
      format = (uint16_t)u16(i + 8);
      channels = (uint16_t)u16(i + 10);
      *rate = u32(i + 12);
      bits = (uint16_t)u16(i + 22);
    } else if (!std::memcmp(&b[i], "data", 4)) {
      if (format != 1 || channels != 1 || bits != 16) {
        std::fprintf(stderr, "need 16-bit mono PCM (got format %u, %u channels, %u bits)\n", format,
                     channels, bits);
        return false;
      }
      size_t count = std::min<size_t>(len, b.size() - i - 8) / 2;
      out->resize(count);
      for (size_t k = 0; k < count; k++) (*out)[k] = (int16_t)u16(i + 8 + 2 * k);
      return true;
    }
    i += 8 + len + (len & 1);
  }
  return false;
}

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

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s shot.wav [setting=value ...]\n", argv[0]);
    return 2;
  }
  Settings s;
  for (int i = 2; i < argc; i++) {
    char key[64];
    const char* eq = std::strchr(argv[i], '=');
    if (!eq || (size_t)(eq - argv[i]) >= sizeof key) {
      std::fprintf(stderr, "expected key=value, got %s\n", argv[i]);
      return 2;
    }
    std::memcpy(key, argv[i], eq - argv[i]);
    key[eq - argv[i]] = '\0';
    if (!settingsSet(s, key, eq + 1)) {
      std::fprintf(stderr, "bad setting %s\n", argv[i]);
      return 2;
    }
  }
  std::vector<int16_t> in;
  uint32_t rate = 0;
  if (!readWav(argv[1], &in, &rate)) {
    std::fprintf(stderr, "could not read %s as a WAV file\n", argv[1]);
    return 1;
  }
  std::vector<int16_t> hist;
  if (rate == 200000) {
    Decimator dec;
    dec.reset();
    for (int16_t x : in) {
      int16_t y;
      if (dec.push(x, &y)) hist.push_back(y);
    }
  } else {
    if (rate != 50000) std::fprintf(stderr, "warning: %u samples/s, treating it as 50000\n", rate);
    hist = in;
  }

  Config cfg = toDspConfig(s);
  std::printf("%s: %zu samples (%.0f ms); %s, %s, %s", argv[1], hist.size(), hist.size() / 50.0,
              projectileLabel(s.proj), mountName(s.mount), sensitivityName(s.sens));
  if (s.mount == Mount::Downrange) std::printf(", %.1f ft", s.dist_ft);
  std::printf(", cal %.4f\n", s.cal);
  std::printf("    t(ms)   f(Hz)   fps   snr  score\n");

  static FrameAnalyzer fa;
  static ShotTracker tr;
  VectorSource src(&hist);
  fa.init(cfg);
  tr.reset();
  tr.setSampleSource(&src);
  int shots = 0;
  // Frames as the firmware runs them, then enough empty frames to close the last track.
  for (size_t end = WIN; end <= hist.size() + (size_t)(cfg.end_gap_frames + 1) * HOP; end += HOP) {
    bool past = end > hist.size();
    FrameResult fr;
    if (!past) {
      fr = fa.analyze(&hist[end - WIN], tr.trackFreq());
    } else {
      fr = FrameResult{0, 0, -99, 0};
    }
    double t_ms = (end - WIN / 2) / 50.0;
    if (!past && fr.score_db >= cfg.detect_db - 6)
      std::printf("  %7.2f %7.0f %6.1f %5.1f %5.1f%s\n", t_ms, fr.f_hz, fr.f_hz / cfg.hz_per_mps * FT_PER_M,
                  fr.snr_db, fr.score_db, fr.score_db >= cfg.detect_db ? "" : "   (below threshold)");
    ShotResult r;
    ShotTracker::Result res = tr.push(fr, (uint32_t)(end - WIN / 2), true, cfg, &r);
    if (res == ShotTracker::REJECTED) {
      fa.absorbLastFrame();
      std::printf("  -- steady tone rejected\n");
    } else if (res == ShotTracker::SHOT) {
      shots++;
      std::printf("shot %d: at the chrono %.2f fps, muzzle %.2f fps (+-%.2f), %d frames (%d fitted), "
                  "peak %.1f dB, k %.4f/m, offset %.1f cm, passes the radar %.2f ms after the first "
                  "frame, residual %.2f fps, flags 0x%02x\n",
                  shots, r.v_chrono_mps * FT_PER_M, r.v_muzzle_mps * FT_PER_M, r.sigma_mps * FT_PER_M,
                  r.n_frames, r.n_used, r.peak_snr_db, r.k_used, r.offset_m * 100, r.t_pass_s * 1e3,
                  r.resid_mps * FT_PER_M, r.flags);
    }
  }
  if (!shots) std::printf("no shot found (%u short tracks)\n", (unsigned)tr.lost());
  return 0;
}
