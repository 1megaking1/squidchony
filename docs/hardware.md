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

- It's a copy of InnoSenT's IPM-165: **4.75 to 5.25 V**, about 30 mA. Its frequency drifts
  about −1 MHz per °C as it warms up: 0.004% per °C, or about 0.15 fps at 350 fps for a 10 °C
  change. That's small enough for the chrono to switch it off between strings. For the
  steadiest readings, keep it on (Auto read, or `radar_hold_s=0`).
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

Sockets (cut from an IC socket strip) for R7, R8, C6, C7, C3 and C8 make these swaps a pull
and a push.

## Power

No switching regulators anywhere near the radar: just linear regulators and RC filters. There
is no power switch either: MODE switches the chrono on, and two P-channel MOSFETs do the rest.
Q1 switches everything after the battery's protection board; Q2 switches the radar and its
amplifier, which only get power while you're measuring.

```
USB-C ─ IP2326 charger ─┐  (switches only while plugged in)
2S pack ─ 2S BMS ─ P+ ──┴─ 1N5817 ─ Q1 ─┬─ 470µF ─ MIC2940A-5.0 ─┬─ 100µF ─ 1N5817 ─ Pico VSYS
                         soft power     │                         └─ button LEDs (2N3904)
                         switch         ├─ Q2 ─ 10Ω ─ 470µF ─ MCP1702 (1µF in/out) ─┬─ amplifier
                                        │  radar switch                             └─ 2.5Ω ─ 1000µF ─ radar
                                        └─ 470k ─┬─ GP27 (battery level)
                                                 100k ∥ 100nF to GND
```

- The charger connects to the BMS's P+ and P− just like the chrono, so the BMS guards both.
  Use the Li-ion (4.2 V per cell) version of the BMS, and leave the charger set to 2S (8.4 V).
- **2.5 Ω** is four 10 Ω resistors in parallel (anything from 1 to 3.3 Ω works there).
- The MCP1702 rejects supply ripple poorly at 3–20 kHz, which is why the RC filters are there.
- The second 1N5817 keeps USB from back-feeding the battery when you program the Pico.
- The Pico 2 W's own 3.3 V converter (RT6154) would skip pulses at light load and put bursts
  in the audio band. The firmware holds it in steady PWM mode (wireless-chip GPIO1 high).
- Battery reading: the divider sits after the reverse-protection diode and Q1, so it draws
  nothing while the chrono is off. The firmware adds 0.25 V for the diode (`BATT_DIODE_V` in
  `config.h`). Calibrate it once with `batt_cal` (via `/api/set?batt_cal=...`) against a meter.

### Soft power switch (Q1)

```
 from the 1N5817 ──┬───────── S ─┐
                   │             Q1  FQP27P06
                  100k     ┌── G ─┘   D ──► everything else
                   │       │
              gate ●───────┘
                   │
                   ├───────── Q3 collector   (2N3904: emitter to GND,
                   │                          base 10k to GP12 and 100k to GND)
                   ▼ D1 1N4148
                   │
  GP14 ──►|────────●───── MODE button ───── GND
       D2 1N4148
```

- Pressing MODE pulls Q1's gate down through D1, which switches everything on. The Pico
  starts, sets GP12 high so that Q3 holds the gate down, and checks that MODE is still held a
  moment later, so a knock in a bag switches it straight back off. Hold MODE until the
  readings appear (about a second).
- The Pico still reads MODE on GP14, through D2: about 0.5 V while pressed, which reads as
  low. D2 also keeps the gate's battery voltage away from GP14 while the Pico is unpowered.
- To switch off, the Pico saves, sets GP12 low and the 100k turns Q1 off. If MODE is still
  held, D1 keeps the power on until it's let go.
- Off, the only drain is the BMS and charger boards, about 10–20 µA, so a charged pack lasts
  months (its own self-discharge is the limit). Check that the charger board has no LED lit
  when unplugged.
- Something pressing MODE in a bag keeps the power on. The chrono switches itself off after
  10 s, but the Pico keeps drawing about 30 mA until the button is free, so mount MODE
  recessed or put a guard ring around it.

### Radar switch (Q2)

```
 switched supply ──┬───────── S ─┐
                   │             Q2  FQP27P06
                  100k     ┌── G ─┘   D ──► 10Ω ─ 470µF ─ MCP1702 ...
                   │       │
                   ●───────┘
                   │
                   └───────── Q4 collector   (2N3904: emitter to GND,
                                              base 10k to GP9 and 100k to GND)
```

- The radar and amplifier draw about 45 mA, a third of the total. The firmware switches them
  on when you hold READ (or with Auto read, or the phone's Measure switch) and off a minute
  after you last measured (`radar_hold_s`). They need about half a second to settle; the
  firmware waits that long before it measures, then relearns the background.
- Q2 and Q4 sit in the digital chamber with Q1 and Q3. The switched line goes through the
  ferrite bead into the radar chamber.

**Pinouts.** FQP27P06 (TO-220), printed side facing you, legs down: gate, drain, source. Its
metal tab is the drain, so keep it clear of the grounded copper foil. 2N3904 (TO-92), flat side
facing you, legs down: emitter, base, collector. The band on a diode marks its cathode.

### Run time

The screen turns off after 2 minutes without use (`backlight_s`) and the radar only runs while
you measure, so a day at the field averages about 120 mA.

| Doing | Draw from the pack |
|---|---|
| Measuring, screen and Wi-Fi on | about 180 mA |
| Between strings (radar asleep), screen on | about 135 mA |
| Radar asleep, screen timed out | about 100 mA |
| Wi-Fi off in the menu | about 40 mA less |
| Off | 10–20 µA |

| 2S pack | Shooting nonstop | A day at the field | Off |
|---|---|---|---|
| 1000 mAh | 4.5 h | 7 h | months |
| 1500 mAh | 7 h | 10 h | months |
| 2200 mAh | 10 h | 15 h | months |
| 2 × 18650, 3500 mAh | 16 h | 24 h | months |

These are estimates, counting 85% of the rated capacity as usable; measure the real current
once it's built. The firmware warns below 6.6 V (3.3 V per cell), and below 6.4 V (3.2 V per
cell) it saves and switches itself off, before the BMS would cut the power at 2.5–3.0 V per
cell.

### Charging

The IP2326 board has no power path: it charges the pack, and the chrono, if on, runs from the
pack. You can charge with the chrono on or off, but don't measure while it charges, because
the charger's switching ripple reaches the radar. Left on, the chrono switches itself off
after its Auto off time and the charger then finishes normally. (While the chrono is on, the
charger may never see the current fall and its "full" light may not come on.)

### If it won't switch on

- Measure Q1's gate while pressing MODE: it should drop to about 0.6 V. If not, check D1's
  direction (band toward the button).
- If the gate drops but nothing gets power, check the BMS output. Some BMS boards switch off
  after a short circuit (a big capacitor charging at switch-on can look like one) and only
  come back once the charger is plugged in.
- If it starts but goes off as soon as you let go of MODE, Q3 isn't holding the gate: check
  the 10k from GP12 to its base and the transistor's pinout.

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
| GP15 | READ button to GND (internal pull-up) |
| GP14 | MODE button, through D2 (internal pull-up) |
| GP11 / GP10 | READ / MODE LEDs, through 4.7 kΩ to each 2N3904 base |
| GP12 | soft power switch: high holds the chrono on (10 kΩ to Q3's base) |
| GP9 | radar switch: high powers the radar and amplifier (10 kΩ to Q4's base) |
| 3V3(OUT) | screen VIN (keep the total under 300 mA) |

Avoid GP23, GP24, GP25 and GP29: the Pico 2 W uses them for the wireless chip.

**Button LEDs:** each 2N3904 has its emitter on ground and the LED (plus resistor) between
the 5 V rail and the collector. Check what your buttons' LEDs are rated for. Ones marked for
5 V usually have the resistor built in and connect straight across. A bare LED needs about
150 Ω in series (around 15 mA at 5 V). Ones rated 12 V will be dim on 5 V. The firmware only
ever switches them fully on or off, never dims them, and doesn't change them while a shot is
being measured. The LED needs its own two pins, separate from the switch's (most LED buttons
have C, NO and NC for the switch plus + and − for the LED). Wire MODE's switch between GND and
the two diodes, and READ's between GND and GP15.

## Shielding and enclosure

1. **Two chambers.** The radar chamber holds the CDM324, the amplifier right behind it and the
   MCP1702. Everything else (battery, charger, BMS, the power switches, Pico, screen) goes in
   the digital chamber.
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
