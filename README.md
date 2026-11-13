# Squidchrony

A DIY radar chronograph for paintball and half-length foam darts (about 100 to 350 fps).
A 24 GHz CDM324 Doppler radar module feeds a two-stage op-amp amplifier into a
Raspberry Pi Pico 2 W. The Pico works out each shot's speed, shows it on a 2.0" screen,
and serves a phone page over its own Wi-Fi network, so it works in the woods with no
internet or cell signal.

**Status: early development.** The signal processing runs and is tested in simulation.
The firmware around it is being written. Nothing has run on real hardware yet.

## What's here

| Path | What it is |
| --- | --- |
| `firmware/squidchrony/` | Arduino sketch for the Pico 2 W (earlephilhower arduino-pico core) |
| `firmware/squidchrony/dsp.*` | Signal processing: decimation filter, per-frame spectrum, shot tracking, drag and geometry fit. Plain C++ so it also runs on a PC |
| `tests/host/` | Simulated shots run through the same signal-processing code (`make -C tests/host`) |
| `tools/design_fir.py` | Designs the decimation filter and writes `fir_coeffs.h` |
| `sim/` | The original feasibility simulation and amplifier frequency-response check |
| `web/chrono-remote.html` | The phone page (currently a demo with simulated shots) |

## How a shot is measured

1. The ADC samples the amplified radar signal at 200 kS/s. A 64-tap filter cuts everything
   above 30 kHz and keeps every 4th sample (50 kS/s).
2. Every 1.28 ms the last 5.12 ms of samples go through an FFT. The strongest tone in
   2 to 20 kHz (about 40 to 400 fps) is compared with the noise floor and with a slowly learned
   background, so a steady tone such as revving flywheels doesn't count as a shot.
3. Consecutive detections at a smoothly changing frequency make one track, one per projectile.
4. The radar only sees the closing speed, v × cos(angle). The projectile has to pass beside
   the radar, so close in it reads several percent slow, by an amount that changes with each
   shot. The fit models drag (1/v is linear in time for quadratic drag) together with the
   pass-by geometry (sideways offset and time of closest approach). Frames near the radar are
   measured again with their frequency sweep removed. The result is the speed where the
   projectile passes the radar, the estimated muzzle speed, and an uncertainty.

In simulation (`make -C tests/host`), with the echo 20 dB or more above the noise at 1 m,
95% of shots land within 1 fps of the true speed, both for a radar 3 ft downrange and for
one mounted beside the barrel. How strong real echoes are, especially from foam darts, is
the big unknown until the hardware is built.

## Building and testing

- Signal-processing tests on a PC: `make -C tests/host` (needs g++). `./tests/host/test_dsp 200`
  runs more trials; `./tests/host/test_dsp dump 0 25` prints the frames of one simulated shot.
- Decimation filter: `python3 tools/design_fir.py` (needs numpy) rewrites `fir_coeffs.h`.

## Credits

The amplifier is based on limpkin's [cdm324_backpack](https://github.com/limpkin/cdm324_backpack)
(CERN OHL v1.2), with the gain and filter values changed for projectile speeds.
