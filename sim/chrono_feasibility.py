#!/usr/bin/env python3
"""
Feasibility check for a DIY CDM324 (24 GHz CW Doppler) chronograph.

Simulates the radar's IF ("beat") signal for a decelerating projectile flying
toward the radar, adds noise, digitizes it like a 12-bit microcontroller ADC,
then estimates speed with a short-time FFT + drag-law fit.
Prints error statistics versus signal-to-noise ratio.

All assumptions are in the constants and PROJECTILES below; edit and rerun.
Only needs numpy.
"""
import numpy as np

C = 299_792_458.0           # speed of light, m/s
F0 = 24.125e9               # CDM324 nominal transmit frequency, Hz
HZ_PER_MPS = 2 * F0 / C     # Doppler shift per m/s of closing speed (~160.9 Hz)
FPS = 0.3048                # m/s per ft/s
FS = 50_000                 # ADC sample rate, Hz
ADC_BITS = 12
NOISE_LSB = 3.0             # amplified analog noise reaching the ADC, LSB rms

WIN = 256                   # STFT window: 5.1 ms at 50 kS/s
HOP = 32                    # 0.64 ms between frames
NFFT = 4096                 # zero-padded FFT for finer peak interpolation
BAND = (2_000, 20_000)      # search band, Hz (~41 to ~408 fps)
DETECT_DB = 15.0            # frame counts as "detected" this far above the noise floor
MIN_FRAMES = 8              # need ~5 ms of continuous track to report a shot

# Quadratic drag: dv/dx = -k*v  ->  v(t) = v0 / (1 + k*v0*t), so 1/v is linear in t.
# k = rho * Cd * A / (2 m)
#   paintball: 1.2 * 0.47 * 2.35e-4 / (2 * 0.0030) ~ 0.022 /m  (~2 fps lost per foot at 300 fps)
#   half dart: 1.2 * 0.9  * 1.27e-4 / (2 * 0.0007) ~ 0.098 /m  (rough guess)
# The radar sits on the flight line, start_range from the muzzle; the track is
# analyzed until the projectile is end_range from the radar.
PROJECTILES = {
    "paintball, 300 fps": dict(v0_fps=300.0, k=0.022, start_range=3.0, end_range=1.0),
    "half dart, 150 fps": dict(v0_fps=150.0, k=0.098, start_range=2.0, end_range=0.5),
}
PEAK_SNR_DB = [10, 15, 20, 25, 30, 40]   # spectrogram SNR at the closest point of the track
TRIALS = 300


def simulate_shot(p, peak_snr_db, rng):
    v0, k = p["v0_fps"] * FPS, p["k"]
    t_flight = (np.exp(k * (p["start_range"] - p["end_range"])) - 1) / (k * v0)
    pad = int(0.010 * FS)                       # 10 ms of noise before and after
    t = np.arange(-pad, int(t_flight * FS) + pad) / FS   # t = 0 is muzzle exit
    tc = np.clip(t, 0.0, t_flight)
    r = p["start_range"] - np.log1p(k * v0 * tc) / k     # range to radar
    phase = 4 * np.pi * (p["start_range"] - r) * F0 / C + rng.uniform(0, 2 * np.pi)

    # Echo amplitude ~ 1/R^2. Scale so the Hann-window FFT-bin SNR at end_range is peak_snr_db.
    a_peak = np.sqrt(6 * NOISE_LSB**2 * 10 ** (peak_snr_db / 10) / WIN)
    in_flight = (t >= 0) & (t <= t_flight)
    amp = np.where(in_flight, a_peak * (p["end_range"] / r) ** 2, 0.0)

    analog = amp * np.cos(phase) + rng.normal(0, NOISE_LSB, t.size)
    lo, hi = -(2 ** (ADC_BITS - 1)), 2 ** (ADC_BITS - 1) - 1
    adc = np.clip(np.round(analog + rng.normal(0, 1.0, t.size)), lo, hi)  # +1 LSB ADC noise
    return t, adc, v0, k, t_flight


def track_speed(t, adc):
    """Short-time FFT -> interpolated peak per frame -> longest detected run."""
    starts = np.arange(0, adc.size - WIN, HOP)
    frames = np.stack([adc[s:s + WIN] for s in starts]).astype(float)
    frames = (frames - frames.mean(axis=1, keepdims=True)) * np.hanning(WIN)
    spec = np.abs(np.fft.rfft(frames, NFFT, axis=1)) ** 2
    freqs = np.fft.rfftfreq(NFFT, 1 / FS)
    band = (freqs >= BAND[0]) & (freqs <= BAND[1])
    sb, fb = spec[:, band], freqs[band]

    noise_mean = np.median(sb) / np.log(2)      # median of exponential noise -> mean
    rows = np.arange(sb.shape[0])
    i = np.clip(np.argmax(sb, axis=1), 1, sb.shape[1] - 2)
    a, b, c = (np.log(sb[rows, j]) for j in (i - 1, i, i + 1))
    delta = 0.5 * (a - c) / (a - 2 * b + c)     # Gaussian (log-parabolic) interpolation
    f_peak = fb[i] + delta * (fb[1] - fb[0])
    snr_db = 10 * np.log10(sb[rows, i] / noise_mean)
    t_center = t[starts + WIN // 2]

    hit = snr_db > DETECT_DB
    best, run_start = (0, 0), None              # longest contiguous run of detections
    for n, h in enumerate(np.append(hit, False)):
        if h and run_start is None:
            run_start = n
        elif not h and run_start is not None:
            if n - run_start > best[1] - best[0]:
                best = (run_start, n)
            run_start = None
    sel = slice(*best)
    if best[1] - best[0] < MIN_FRAMES:
        return None
    return t_center[sel], f_peak[sel] / HZ_PER_MPS, snr_db[sel]


def fit_drag(tt, v, snr_db):
    """Weighted fit of 1/v = a + b*t (exact for quadratic drag); returns v(t)."""
    w = np.sqrt(np.minimum(10 ** (snr_db / 10), 1e5))
    A = np.vstack([np.ones_like(tt), tt]).T
    a, b = np.linalg.lstsq(A * w[:, None], (1 / v) * w, rcond=None)[0]
    return lambda tq: 1 / (a + b * tq)


def main():
    rng = np.random.default_rng(1)
    print(f"24.125 GHz -> {HZ_PER_MPS * FPS:.1f} Hz per fps; ADC {FS / 1000:.0f} kS/s, {ADC_BITS}-bit\n")
    for name, p in PROJECTILES.items():
        print(name)
        print("  peak SNR | shots found | fixed-moment err (fps) | muzzle err (fps)   | naive peak-pick err")
        print("           |             |  mean    SD    95%     |  mean    SD   95%  |  mean (vs muzzle)")
        for snr in PEAK_SNR_DB:
            e_mid, e_muz, e_naive = [], [], []
            for _ in range(TRIALS):
                t, adc, v0, k, t_flight = simulate_shot(p, snr, rng)
                tr = track_speed(t, adc)
                if tr is None:
                    continue
                tt, v, s = tr
                v_fit = fit_drag(tt, v, s)
                t_mid = t_flight / 2
                v_true_mid = v0 / (1 + k * v0 * t_mid)
                e_mid.append((v_fit(t_mid) - v_true_mid) / FPS)
                e_muz.append((v_fit(0.0) - v0) / FPS)
                e_naive.append((v[np.argmax(s)] - v0) / FPS)
            if not e_mid:
                print(f"  {snr:5.0f} dB |   0/{TRIALS}     |   (nothing detected)")
                continue
            m, z, nv = map(np.array, (e_mid, e_muz, e_naive))
            print(f"  {snr:5.0f} dB | {len(m):3d}/{TRIALS}     |"
                  f" {m.mean():+5.2f} {m.std():5.2f} {np.percentile(abs(m), 95):5.2f}     |"
                  f" {z.mean():+5.2f} {z.std():5.2f} {np.percentile(abs(z), 95):5.2f} |"
                  f"  {nv.mean():+6.1f}")
        print()

    print("Systematic errors the electronics can't see (at 300 fps):")
    for mhz in (25, 50, 100):
        print(f"  transmit frequency off by {mhz:3d} MHz -> {300 * mhz * 1e6 / F0:4.2f} fps scale error")
    for deg in (2, 5, 8):
        print(f"  radar {deg} deg off the flight line     -> {300 * (1 - np.cos(np.radians(deg))):4.2f} fps low")


if __name__ == "__main__":
    main()
