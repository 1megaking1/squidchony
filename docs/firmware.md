# Firmware

Arduino sketch for the Raspberry Pi Pico 2 W (RP2350) on the
[earlephilhower arduino-pico](https://github.com/earlephilhower/arduino-pico) core. CI
compiles it on every push and keeps the `.uf2` as a build artifact.

## Building and flashing

1. Arduino IDE: add
   `https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`
   under Preferences > Additional boards manager URLs, then install "Raspberry Pi Pico/RP2040/RP2350".
2. Library Manager: install "Adafruit ST7735 and ST7789 Library" (it pulls in Adafruit GFX
   and BusIO).
3. Tools > Board > **Raspberry Pi Pico 2W**. Tools > Flash Size > **4MB (Sketch: 3MB,
   FS: 1MB)**, or anything with a filesystem. Without one, shots and settings are forgotten at
   power-off and the screen says "No flash storage set up".
4. Open `firmware/squidchrony/squidchrony.ino` and upload. Or hold BOOTSEL while plugging in
   the Pico and copy the `.uf2` from a CI run onto the drive that appears.

With arduino-cli: `arduino-cli compile --fqbn rp2040:rp2040:rpipico2w firmware/squidchrony`.

## How it's split up

| File | Runs on | What it does |
|---|---|---|
| `acquisition.*` | core 1 (interrupt) | ADC + DMA, decimation into the 50 kS/s history, battery and level averages |
| `dsp_task.*` | core 1 | frame loop, tracker, fit; hands shots to core 0 |
| `dsp.*`, `fir_coeffs.h` | core 1 | the signal processing (plain C++, also built by the host tests) |
| `squidchrony.ino` | core 0 | setup, buttons, menu, screen model, LEDs, battery, saving |
| `settings.*`, `shotlog.*` | core 0 | settings, strings of shots, statistics, journal (host-tested) |
| `storage.*` | core 0 | LittleFS files |
| `display.*`, `controls.*` | core 0 | ST7789 screen, buttons and LEDs |
| `web.*`, `web_page.h` | core 0 | Wi-Fi access point, HTTP API, the packed phone page |

### Sampling

The ADC alternates between the radar (GP26) and the battery (GP27) at 400 kS/s, 200 kS/s
each. One DMA channel copies samples into a 16 KB ring. It uses the RP2350's self-retrigger
mode, and the ring wrap is done in hardware, so the DMA can never run past its buffer. Every
1024 samples it interrupts core 1. The interrupt filters the radar samples with a 64-tap
low-pass (flat to 18 kHz, at least 60 dB down from 30 kHz), keeps every 4th one, and appends
it to a 655 ms history. It also notes clipping per 5 ms block and averages the battery.

If core 1 is held up for more than 15 ms the ring may have lapped. The interrupt then counts a
"gap", throws away what it can't trust, and starts over. The usual cause is core 0 writing
flash, which pauses the other core, so core 0 only saves when nothing is being measured.

### Measuring a shot

Every 1.28 ms, core 1 takes the latest 5.12 ms (256 samples) of history, applies a Hann
window, zero-pads to 512 and runs an FFT. Between 2 and 20 kHz it picks the bin that stands
out most, both from the median noise floor and from a slowly learned background. The
background, about 0.4 s long, is what makes revving flywheels or a fan stop counting. One loud
frame can raise it only a little, and tracks longer than 0.25 s are rejected and absorbed into
it. The peak position is interpolated (Gaussian fit over three bins).

Detections at a smoothly changing frequency (under 6% per frame) make a track. A track ends
after 15 ms without the target. Tracks under 6 frames don't count (they show up as
"No reading: weak echo").

The fit (`TrackFitter` in `dsp.cpp`) then models the track as

```
measured(t) = v(t) · |t − t_c| / sqrt((t − t_c)² + τ²),     1/v(t) = a + k·t
```

The projectile passes the radar at time `t_c` with a sideways offset `h = τ·v`. `k` is the drag
constant: under quadratic drag, 1/v is exactly linear in time. For each candidate (t_c, τ), `a`
and `k` come from weighted least squares, with a prior on `k` around the projectile's preset
(±30%). A coarse grid over (t_c, τ) is searched. Its best few local minima are refined, and
outlier frames are dropped. Close to the radar the closing speed changes a lot within one
frame, so each frame is then re-measured from the history with the predicted frequency sweep
removed ("dechirped"). The geometry is fitted again on the re-measured frames, three times.

Results: the speed where the projectile passes the radar, the muzzle speed (downrange:
`v_pass · exp(k · distance)`; on the barrel the pass point is just behind the muzzle), an
uncertainty, the fitted offset, and flags (weak, drag not measurable, far off the line of fire,
poor fit, clipped). One fit takes about 1 ms on a PC, so expect roughly 20–40 ms on the Pico.
Meanwhile the history keeps filling, and the frame loop catches up afterwards.

### Keeping it quiet

- The Pico's 3.3 V converter is held in PWM mode (no pulse skipping in the audio band).
- The screen is redrawn, LEDs switched, the backlight changed and flash written only while
  no track is in progress. The screen draws one section per loop pass so it can stop between
  sections.
- LEDs and the backlight are only fully on or off.
- The Wi-Fi client count is read once a second (asking the radio chip is a bus transaction).

## Settings

Stored as `key=value` lines in `/settings.txt`. Set any of them over the API
(`/api/set?key=value`). Most are also in the button menu and the phone page.

| Key | Values | Default | |
|---|---|---|---|
| `proj` | `paintball`, `dart` | paintball | picks the drag constant; changing it starts a new string |
| `mount` | `downrange`, `barrel` | downrange | also starts a new string |
| `dist_ft` | 1–20 | 3.0 | downrange: muzzle to radar |
| `offset_mm` | 5–300 | 40 | barrel: bore line to the middle of the antenna |
| `ahead_mm` | −100–200 | 20 | barrel: muzzle in front of the antenna |
| `reading` | `muzzle`, `chrono` | muzzle | what the big number shows |
| `units` | `fps`, `ms` | fps | |
| `sens` | `low`, `normal`, `high` | normal | frame threshold 18 / 15 / 12 dB, minimum track 8 / 6 / 5 frames |
| `auto` | 0/1 | 0 | measure all the time, no need to hold READ |
| `wifi` | 0/1 | 1 | |
| `wifi_pass` | empty or 8–23 characters | empty | empty = open network |
| `cal` | 0.9–1.1 | 1 | multiplies every speed (radar frequency tolerance); set by calibration |
| `batt_cal` | 0.8–1.2 | 1 | multiplies the battery voltage |
| `k_paintball`, `k_dart` | 0.001–0.5 | 0.022, 0.098 | drag constants, 1/m (the dart one is an estimate) |
| `backlight_s` | 0–3600 | 120 | screen timeout, 0 = always on |

## HTTP API

The chrono is its own access point, `CHRONO-xxxx` (the last four hex digits of the board ID),
at 192.168.4.1, also announced as `chrono.local`. Commands accept GET or POST.

| Request | Returns |
|---|---|
| `GET /` | the phone page (gzipped) |
| `GET /api/state` | `rev`/`srev` (change counters), arming, battery, Wi-Fi clients, DSP counters (`noise_db`, `lost`, `gaps`, ...), current string summary, all settings |
| `GET /api/data[?cur=1]` | strings, newest first, each with `shots: [[muzzle_fps, chrono_fps, snr_db, flags, t_ms, sigma_fps, unix_s], ...]` |
| `GET /api/cmd?c=new` | new string |
| `c=del` / `c=undel` | delete the last shot of the current string / undo |
| `c=name&v=...[&id=..]` | rename a string |
| `c=rm&id=..` / `c=clear` | remove a string / everything |
| `c=arm&v=1` | measure for the next 60 s (the page renews it); `v=0` stops |
| `c=time&v=<Unix ms>` | set the clock (the page does this on connect) |
| `c=cal&v=<m/s>` | scale `cal` so the current string's average reads this |
| `GET /api/set?key=value&...` | change settings; reports keys it refused |
| `GET /api/export.csv` | every string as CSV |
| `GET /api/lastshot.wav` | the 50 kS/s samples around the last shot, 16-bit mono |

Shot flags: 1 weak, 2 drag preset used (track too short to measure it), 4 clipped,
8 truncated, 16 far off the line of fire, 32 poor fit.

## Saved data

`/log.txt` is an append-only journal (format at the top of `shotlog.h`), written once there's
been no shot for 3 seconds. It's replayed at boot, and rewritten from memory once it passes
192 KB. A power cut can lose only the shots since the last write. The log holds up to 1500
shots in 64 strings; the oldest strings make room.

## Working on it without hardware

- `make -C tests/host` runs the simulation tests and the settings/shot-log tests.
  `./tests/host/test_dsp dump <setup> <snr> [trial]` prints every frame of one simulated
  shot and each stage of its fit.
- `python3 tools/mock_chrono.py` serves the phone page with a fake API at
  `http://localhost:8000/?live=1`.
- After editing `web/chrono-remote.html`, run `python3 tools/make_web_header.py` (CI checks).
- After changing the decimation filter in `tools/design_fir.py`, rerun it (needs numpy).

## What to tune with real data

The simulation guesses at the parts that matter most. Once the hardware works, record a few
shots (`/api/lastshot.wav`) at each distance and projectile, and check:

- **Echo strength** (signal dB on each shot) against the simulation's 15–30 dB at 1 m. It sets
  the gain step and how far away the chrono can sit.
- `detect_db`, `jump_frac`, `end_gap_frames` and `min_frames` in `dsp::Config`, against what
  real tracks look like (foam darts tumbling, paintballs breaking, ricochets).
- `model_err_mps` (the per-frame error floor) and `max_angle_deg`: real frames close to the
  radar may fit worse than simulated ones.
- The drag constant for half darts (0.098/m is an estimate). With long tracks the fit
  measures `k` itself; the shot log stores what each shot used.
- Whether Wi-Fi transmissions show up in the noise floor (compare `noise_db` with Wi-Fi on and
  off: hold MODE while switching on).
