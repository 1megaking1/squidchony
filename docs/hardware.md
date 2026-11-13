# Hardware

Nothing here has been built yet. These are the plans the firmware is written against. Check
each step with a multimeter as you go.

```
             radar chamber (copper-shielded)                 digital chamber
 ┌──────────────────────────────────────────────┐  ┌──────────────────────────────────┐
 │ CDM324 ──► 2-stage OPA2365 amp ──► 10k/15k ──┼──┼─► GP26 (ADC0)   Pico 2 W         │
 │   ▲            ▲   (67 dB, 1.5–27 kHz)  + 1 nF│  │                  ├─ SPI ─► 2.0" screen
 │   └── 5 V ─────┴── MCP1702 ◄── RC filter ◄────┼──┼── switched 2S     ├─ READ / MODE buttons
 └──────────────────────────────────────────────┘  │  battery          └─ Wi-Fi ─► phone
                                                   └──────────────────────────────────┘
```

## Radar module (CDM324)

- It's a copy of InnoSenT's IPM-165: **4.75 to 5.25 V**, about 30 mA. It drifts about
  −1 MHz per °C as it warms up, so keep it powered whenever the chrono is on.
- The beam comes out of the **flat side with the square copper patches**, roughly 80° wide one
  way and 32° the other. The metal can and the pins face into the housing.
- On the IPM-165, pin 1 is VCC, pin 2 the IF (signal) and pin 3 GND. Clones may differ:
  read the silkscreen and check which pin connects to the metal can (ground) before powering it.
- It's static sensitive. Touch something grounded first and don't touch the signal pin.

## Amplifier

limpkin's [cdm324_backpack](https://github.com/limpkin/cdm324_backpack) (CERN OHL v1.2): two
OPA2365 stages, without his comparator section (the Pico reads the output directly). Six
values change, because his board was tuned for people walking:

| Part | His value | Ours | What it sets |
|---|---|---|---|
| C3, C8 | 10 µF | 22 nF film | low cutoff 3.4 Hz → 1.5 kHz (ignores hand and body motion) |
| R7, R8 | 590 kΩ | 220 kΩ | gain 84 dB → 67 dB |
| C6, C7 | 270 pF | 27 pF C0G | high cutoff 1 kHz → 27 kHz |

R4, R5, R6 are 4.7 kΩ. The bias divider R2/R3 is 10 kΩ/10 kΩ with C4 = 10 µF, so the output
rests at 2.5 V. From the output (pin 7), a 10 kΩ/15 kΩ divider brings the 0–5 V swing down to
0–3 V for the Pico, with 1 nF C0G from GP26 to ground. Use C0G or film capacitors in the
signal path: ordinary ceramics are microphonic, so the marker's vibration would show up as
signal.

**Speeds to frequencies:** 49.05 Hz per fps at 24.125 GHz, so 100–350 fps is 4.9–17.2 kHz.

**Gain steps.** Swap R7/R8 and C6/C7 as pairs so the high cutoff stays put. The phone page's
diagnostics line (noise floor, weak echoes) and the "Echo clipped" message tell you which way:

| R7, R8 | C6, C7 | Total gain |
|---|---|---|
| 100 kΩ | 56 pF | 53 dB |
| 150 kΩ | 39 pF | 60 dB |
| **220 kΩ** | **27 pF** | **67 dB (start here)** |
| 330 kΩ | 18 pF | 74 dB |
| 470 kΩ | 12 pF | 80 dB |

| You see | Change |
|---|---|
| "Echo clipped: move back" on close shots | one gain step down |
| Many "No reading: weak echo", especially darts | one gain step up, or sensitivity High |
| Junk readings from hands moving | raise the low cutoff: two 22 nF in series (11 nF, ~3.1 kHz) |
| Slow darts (~100 fps) much weaker than fast shots | lower it: two 22 nF in parallel (44 nF, ~770 Hz) |

Sockets (cut from the Mill-Max strip) for R7, R8, C6, C7, C3 and C8 make these swaps a pull
and a push.

## Power

No switching regulators anywhere near the radar: just linear regulators and RC filters.

```
USB-C ─ IP2326 charger ─┐  (switches only while plugged in; ~1 µA otherwise)
2×18650 ─ 2S BMS ─ P+ ──┴─ 1N5817 ─ switch ─┬─ 470µF ─ MIC2940A-5.0 ─┬─ 100µF ─ 1N5817 ─ Pico VSYS
                                            │                         └─ button LEDs (2N3904)
                                            ├─ 10Ω ─ 470µF ─ MCP1702 (1µF in/out) ─┬─ amplifier
                                            │                                       └─ 2.2Ω ─ 1000µF ─ radar
                                            └─ 470k ─┬─ GP27 (battery level)
                                                     100k ∥ 100nF to GND
```

- **Charge with the power switch off.** The IP2326 board has no power path, so with the
  switch on the chrono would run from the charger's switching output.
- The MCP1702 rejects supply ripple poorly at 3–20 kHz, which is why the RC filters are there.
- The second 1N5817 keeps USB from back-feeding the battery when you program the Pico.
- The Pico 2 W's own 3.3 V converter (RT6154) would skip pulses at light load and put bursts
  in the audio band. The firmware holds it in steady PWM mode (wireless-chip GPIO1 high).
- Battery reading: the divider sits after the reverse-protection diode, so the firmware adds
  0.25 V (`BATT_DIODE_V` in `config.h`; set it to 0 if you wire the divider to the battery
  directly). Calibrate it once with `batt_cal` (via `/api/set?batt_cal=...`) against a meter.
- Runtime with 2 × 3500 mAh is roughly 20 hours. The firmware warns below 6.6 V (3.3 V per
  cell); the BMS cuts off at 2.5–3.0 V per cell.

## Pico pins

| Pin | Use |
|---|---|
| GP26 (ADC0) | radar signal from the 10k/15k divider, 1 nF to GND |
| GP27 (ADC1) | battery, from the 470k/100k divider, 100 nF to GND |
| GP18 / GP19 / GP16 | SPI0 SCK / MOSI / MISO to the screen (MISO only for its microSD) |
| GP17 | screen chip select (TCS) |
| GP20 / GP21 | screen DC / RST |
| GP22 | screen backlight (Lite): on/off only |
| GP13 | screen's microSD chip select (unused for now, held high) |
| GP15 / GP14 | READ / MODE buttons to GND (internal pull-ups) |
| GP11 / GP10 | READ / MODE LEDs, through 4.7 kΩ to each 2N3904 base |
| 3V3(OUT) | screen VIN (keep the total under 300 mA) |

Avoid GP23, GP24, GP25 and GP29: the Pico 2 W uses them for the wireless chip.

**Button LEDs:** each 2N3904 has its emitter on ground and the LED (plus resistor) between
the 5 V rail and the collector. Check what your buttons' LEDs are rated for. Ones marked for
5 V usually have the resistor built in and connect straight across. A bare LED needs about
150 Ω in series (around 15 mA at 5 V). Ones rated 12 V will be dim on 5 V. The firmware only
ever switches them fully on or off, never dims them, and doesn't change them while a shot is
being measured.

## Shielding and enclosure

1. **Two chambers.** The radar chamber holds the CDM324, the amplifier right behind it and the
   MCP1702. Everything else (battery, charger, BMS, Pico, screen) goes in the digital chamber.
   The USB-C opening is on the digital side.
2. Line the radar chamber with copper tape and **ground it at one point**, at the MCP1702's
   ground. Several ground points make loops that pick up noise.
3. Wires between the chambers go through one small hole. Power goes through a ferrite bead;
   twist the amplifier output wire with a ground wire.
4. Keep the CDM324-to-amplifier wire as short as possible: it carries microvolts.
5. **Keep the Pico's Wi-Fi antenna** (the end opposite the USB socket) behind plain plastic.
6. Glue the foil down well. Loose foil vibrating when the marker fires shows up as signal.
7. Keep metal out of the beam: foil stays behind the plane of the patches. (Foil flared
   outward in front of the module makes a horn, which narrows the beam and strengthens the
   echo. That's a possible fix later if darts turn out weak.)
8. **The window** in front of the antenna: plain plastic printed solid (100% infill),
   natural, white or gray. Not black filament (carbon absorbs radar) or carbon-fiber blends.
   Either about 0.6 mm thin or about 3.8 mm thick (half a wavelength in PLA/PETG); around 2 mm
   reflects the most. Keep it flat and parallel to the module, with a few mm of air gap.
9. Pad the cells so they can't rattle; a bump that breaks contact resets the Pico. Solder only
   to wires or tabs, never to a cell.

## Where to put it

The radar measures closing speed, v × cos(angle). The fit works out the angle from how the
speed track bends near the radar, but it does best when the angle stays small.

**Downrange (default):** stand the chrono 3 to 6 ft in front of the muzzle, aimed back at it,
with the line of fire passing 5 to 10 cm above (or beside) the patches. Enter the distance
in the settings; an error of a few cm only shifts the muzzle estimate by a fraction of a fps.
This keeps flywheels and motors far from the radar while the projectile comes close.

**On the barrel:** strap it beside the barrel, aimed straight downrange, with the front of the
antenna about level with the muzzle. Measure the distance from the bore line to the middle of
the antenna (`offset_mm`, default 40) and how far the muzzle sticks out past the antenna
(`ahead_mm`, default 20), and set them with `/api/set?offset_mm=..&ahead_mm=..` if they
differ. On a flywheel blaster the wheels sit right next to the radar this way; the firmware
learns a steady tone and ignores it, but a very strong one can still drown out darts.
