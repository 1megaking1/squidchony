# Using the chrono

## Quick start

1. Switch it on. The screen shows the Wi-Fi name (`CHRONO-xxxx`) for a few seconds.
2. Set it up downrange (3–6 ft in front of the muzzle, aimed back at it, the shot passing a
   few inches over it) or strap it beside the barrel. Pick the same mounting in the menu or on
   the phone, and enter the distance.
3. **Hold READ and shoot.** Let go when you're done. Each shot appears as a big number with
   the string's statistics underneath. Or turn on Auto read and just shoot.

## Buttons

| Button | Press | Does |
|---|---|---|
| READ | hold | measure while held (the READ LED is on; it blinks off once per shot) |
| MODE | click | start a new string |
| MODE | hold | open the menu |
| MODE | held while switching on | start with Wi-Fi off |

The first press after the screen has timed out only wakes it (holding READ still measures).

**Menu:** MODE moves to the next item, READ changes it, holding MODE closes the menu (it also
closes after 30 s). Items: projectile, report (muzzle or at the chrono), units, sensitivity,
mounting, distance (downrange only), auto read, Wi-Fi, delete last shot.

**MODE LED:** on when a phone is connected, blinking while Wi-Fi waits for one, off with Wi-Fi off.

## The screen

- Top line: what the big number is (MUZZLE or AT CHRONO), the projectile (`+A` = auto read),
  `Wi` when a phone is connected (lowercase while waiting), and the battery. A green line
  under it means it's measuring.
- The big number is the last shot.
- Below it: the shot number in this string, how far the echo stood above the noise (dB), and
  how far the shot was from the average. WEAK in yellow means the echo was weak or the track
  didn't fit well; treat that reading with suspicion.
- The tiles: average (with the shot count), SD, extreme spread, low, high, and rate of fire
  for the latest burst (3+ shots under 0.6 s apart).
- Messages replace the shot line for a few seconds: "New string", "No reading: weak echo",
  "Echo clipped: move back", "Battery low".

## The phone

Join `CHRONO-xxxx` in Settings > Wi-Fi (it says "No Internet Connection"; that's expected),
then open Safari to `192.168.4.1` or `chrono.local`. Add it to the Home Screen once and it opens
like an app. The page shows the same numbers plus a chart, the shot log, earlier strings, and
the settings. On the phone you can:

- **Measure** without touching the chrono (it measures until you tap again, or for a minute
  after the phone goes away).
- **Delete last** (with undo), rename strings, start a new string.
- **Export CSV** (download to Files or copy).
- **Calibrate:** shoot a string through this and another chrono, type in the other one's
  average, tap Calibrate. Later shots are scaled to match (each radar module's frequency is a
  little off from 24.125 GHz, up to about ±0.4%).
- **Last shot (WAV):** download the raw samples of the last shot, for working on the signal
  processing.

## Getting good readings

- **Downrange:** keep the line of fire within a few inches of the radar's face. The fit
  corrects for the angle, but a small angle means a stronger, cleaner echo. Farther away is
  fine for paintballs; darts give much weaker echoes, so start at 3 ft.
- **Beside the barrel:** aim it parallel to the bore, with the antenna about level with the
  muzzle, and enter the two mount distances on the phone. The radar sits next to the marker's
  moving parts (or a blaster's flywheels) this way, so if readings drop out, try downrange.
- "No reading: weak echo" often: move closer, set sensitivity High, or raise the amplifier
  gain one step (docs/hardware.md).
- "Echo clipped" often: move back, or lower the gain one step.
- Speeds from shots marked WEAK, or with a low dB, are the least trustworthy.

## Battery and charging

Charge over USB-C **with the power switch off** (about 3–4 hours from a phone charger). The
battery shows LOW! below 6.6 V; charge soon after that.
