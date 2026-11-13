#!/usr/bin/env python3
"""
Frequency response of limpkin's 2-stage CDM324 amplifier: original values vs. ours.

Each stage is an inverting amplifier: Zin = R4 + 1/(sC3), Zf = R7 || 1/(sC6).
The op-amp is modeled with a single-pole open-loop gain A(s) = GBW/s.
Only needs numpy.
"""
import numpy as np


def stage(f, rin, cin, rf, cf, gbw):
    s = 2j * np.pi * f
    zin = rin + 1 / (s * cin)
    zf = rf / (1 + s * rf * cf)
    ideal = -zf / zin
    a = gbw / s                                  # open-loop gain, single-pole op-amp
    return ideal / (1 + (1 - ideal) / a)         # noise gain = 1 + Zf/Zin


FPS_HZ = 49.05                                   # Hz per fps at 24.125 GHz
POINTS = [("hand/arm motion (~3 m/s)", 480), ("100 fps", 100 * FPS_HZ), ("200 fps", 200 * FPS_HZ),
          ("350 fps", 350 * FPS_HZ), ("aliasing zone (150 kHz)", 150e3)]
DESIGNS = {
    "original (590k, 270p, 10u)": dict(rin=4.7e3, cin=10e-6, rf=590e3, cf=270e-12),
    "modified (220k, 27p, 22n)":  dict(rin=4.7e3, cin=22e-9, rf=220e3, cf=27e-12),
}


def main():
    for gbw_name, gbw in (("OPA2365, 50 MHz", 50e6), ("MCP6022, 10 MHz", 10e6)):
        print(f"--- op-amp {gbw_name} ---")
        for name, d in DESIGNS.items():
            rows = [f"{label}: {20 * np.log10(abs(stage(f, gbw=gbw, **d) ** 2)):5.1f} dB" for label, f in POINTS]
            print(f"{name}\n   " + "\n   ".join(rows))
    print("corners: HPF", round(1 / (2 * np.pi * 4.7e3 * 22e-9)), "Hz, LPF",
          round(1 / (2 * np.pi * 220e3 * 27e-12)), "Hz, gain",
          round(20 * np.log10((220e3 / 4.7e3) ** 2), 1), "dB")


if __name__ == "__main__":
    main()
