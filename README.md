# Squidchrony

![build](https://github.com/1megaking1/squidchony/actions/workflows/build.yml/badge.svg)

A DIY radar chronograph for paintball and half-length foam darts (about 100 to 350 fps).
A 24 GHz CDM324 Doppler radar module feeds a two-stage op-amp amplifier into a
Raspberry Pi Pico 2 W. The Pico works out each shot's speed, shows it on a 2.0" screen with
the string's average, SD, extreme spread, low, high and rate of fire, and serves a phone page
over its own Wi-Fi network, so it works in the woods with no internet or cell signal.

**Status: early development.** The signal processing runs and is tested in simulation. The
firmware compiles for the Pico 2 W (CI builds it on every push) but has not run on real
hardware yet. Expect to tune things once there are real echoes to look at.

## Docs

- [Using the chrono](docs/using.md): buttons, screen, phone, getting good readings
- [Hardware](docs/hardware.md): amplifier, power, pins, shielding, where to put it
- [Firmware](docs/firmware.md): building, how it works, settings, HTTP API, what to tune
- [Parts](bom/README.md): DigiKey list (`bom/digikey-bulk-add.txt`) and the parts from elsewhere

## What's here

| Path | What it is |
| --- | --- |
| `firmware/squidchrony/` | Arduino sketch for the Pico 2 W (earlephilhower arduino-pico core) |
| `firmware/squidchrony/dsp.*` | Signal processing: decimation filter, per-frame spectrum, shot tracking, drag and geometry fit. Plain C++, so it also runs on a PC |
| `tests/host/` | Simulated shots run through the same code, plus settings and shot-log tests (`make -C tests/host`) |
| `web/chrono-remote.html` | The phone page. Served by the chrono it shows live data; opened anywhere else it's a demo with simulated shots |
| `tools/` | `design_fir.py` (decimation filter), `make_web_header.py` (packs the page into the firmware), `mock_chrono.py` (fake API for working on the page) |
| `sim/` | The original feasibility simulation and amplifier frequency-response check |

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

In simulation, with the echo 20 dB or more above the noise at 1 m, 95% of shots land within
1 fps of the true speed. That holds for paintballs and half darts, with the radar 3 or 6 ft
downrange or beside the barrel, at up to 16 balls a second, and next to a steady flywheel
tone. How strong real echoes are, especially from foam darts, is the big unknown until the
hardware is built.

## Building and testing

- Firmware: see [docs/firmware.md](docs/firmware.md#building-and-flashing). In the Arduino IDE
  choose Raspberry Pi Pico 2W and a Flash Size with a filesystem (for example "4MB (Sketch:
  3MB, FS: 1MB)"). Or download the `.uf2` from the latest CI run.
- Tests on a PC: `make -C tests/host` (needs g++). `./tests/host/test_dsp 200` runs more
  trials; `./tests/host/test_dsp dump 0 25` prints the frames and fit stages of one shot.
- Phone page without hardware: `python3 tools/mock_chrono.py`, then open
  `http://localhost:8000/?live=1`.

## Credits

The amplifier is based on limpkin's [cdm324_backpack](https://github.com/limpkin/cdm324_backpack)
(CERN OHL v1.2), with the gain and filter values changed for projectile speeds.
