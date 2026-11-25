# Parts

Prices and stock are from DigiKey's US site on 25 November 2026 and will drift. About $97 for
the DigiKey order (about $90 without the 18650 holders), before shipping and tax.

## DigiKey order

`digikey-bulk-add.txt` pastes straight into DigiKey's bulk-add box (36 lines: everything
except the parts bought elsewhere, listed at the bottom).

**Controller and screen**

| Qty | DigiKey # | Part | Used for |
|---|---|---|---|
| 1 | 2648-SC1634-ND | Raspberry Pi Pico 2 WH | controller (W = Wi-Fi, H = headers fitted) |
| 1 | 1528-4311-ND | Adafruit 4311, 2.0" 320×240 IPS (ST7789) | screen |
| 2 | 296-20801-ND | OPA2365AID | amplifier op-amp (1 + spare) |
| 1 | 1528-1071-ND | Adafruit 1212 SOIC-8 adapters (6-pack) | op-amp onto perfboard |
| 6 | 2N3904FS-ND | 2N3904 | button LEDs (2), power hold (1), radar switch (1), spares (2) |

**Power**

| Qty | DigiKey # | Part | Used for |
|---|---|---|---|
| 2 | BH-18650-W-ND | 18650 holder, wire leads | two holders in series (2S); skip with a pouch pack |
| 1 | 576-1136-ND | MIC2940A-5.0 | 5 V for the Pico side |
| 2 | MCP1702-5002E/TO-ND | MCP1702 5.0 V | 5 V for the radar side (1 + spare) |
| 5 | 1N5817-E3/73GICT-ND | 1N5817 Schottky | reverse protection, Pico feed |
| 2 | FQP27P06-ND | FQP27P06 P-channel MOSFET, TO-220 | soft power switch (1), radar/amplifier switch (1) |
| 10 | 1N4148FS-ND | 1N4148 | the MODE button's two diodes in the power switch |
| 2 | P5141-ND | 470 µF 16 V | filter and regulator inputs |
| 2 | P5142-ND | 1000 µF 16 V (Panasonic ECA-1CM102) | radar supply filter |
| 3 | 10-ECA-1EM101BCT-ND | 100 µF | MIC2940A output |
| 4 | 445-173374-1-ND | 1 µF | MCP1702 in/out |
| 5 | 1934-1476-1-ND | ferrite bead | wires into the radar chamber |

**Amplifier**

| Qty | DigiKey # | Part | Used for |
|---|---|---|---|
| 10 | 1928-1635-ND | 22 nF film (WIMA MKS2) | C3, C8 (and the series/parallel trick) |
| 4 | 399-14074-ND | 27 pF C0G | C6, C7 |
| 3 | BC1025CT-ND | 1 nF C0G (Vishay K102J15C0GF5TL2) | at the Pico's ADC pin |
| 10 | BC1084CT-ND | 100 nF | decoupling, battery sense |
| 10 | P5189-ND | 10 µF 63 V | bias and supply decoupling |
| 3 | S7001-ND | 3-pin socket | the radar module plugs in |

**Gain-tuning parts** (swap R7/R8 and C6/C7 together)

| Qty | DigiKey # | Part | Pairs with |
|---|---|---|---|
| 4 | BC1010CT-ND | 56 pF C0G | 100 kΩ (53 dB) |
| 4 | BC1008CT-ND | 39 pF C0G | 150 kΩ (60 dB) |
| 4 | BC1004CT-ND | 18 pF C0G | 330 kΩ (74 dB) |
| 4 | BC1002CT-ND | 12 pF C0G | 470 kΩ (80 dB) |

For the parts you might swap (R7, R8, C6, C7, C3, C8), IC socket strips cut to size work as
sockets. Test-fit a resistor lead first and don't force it; a tight lead stretches the contact.

**Resistors** (Stackpole RNMF14FTC 1% metal film, 1/4 W). Ten of a value costs about the same
as three.

| Qty | DigiKey # | Value | Where (how many used) |
|---|---|---|---|
| 10 | S10CACT-ND | 10 Ω | radar supply filter before the MCP1702 (1); four in parallel after it (4) |
| 10 | S4.7KCACT-ND | 4.7 kΩ | amplifier R4, R5, R6 (3); LED transistor bases (2) |
| 10 | S10KCACT-ND | 10 kΩ | bias divider R2, R3 (2); ADC divider top (1); power and radar switch transistor bases (2) |
| 10 | S15KCACT-ND | 15 kΩ | ADC divider bottom (1) |
| 10 | S220KCACT-ND | 220 kΩ | gain R7, R8 (2), the starting value |
| 10 | S100KCACT-ND | 100 kΩ | battery divider bottom (1); MOSFET gate pull-ups (2); transistor base pull-downs (2); gain step (2) |
| 10 | S150KCACT-ND | 150 kΩ | gain step (2) |
| 10 | S330KCACT-ND | 330 kΩ | gain step (2) |
| 10 | S473KCACT-ND | 470 kΩ | battery divider top (1); gain step (2) |
| 10 | S150CACT-ND | 150 Ω | button LEDs, only if they have no built-in resistor (2) |

The radar supply filter after the MCP1702 was a single 2.2 Ω, which DigiKey has on backorder
until late December 2026. Four of the 10 Ω in parallel (2.5 Ω) do the same job: anything from
1 to 3.3 Ω works there.

Backup if something sells out: 1 nF `C315C102JAG5TA-ND`; 1000 µF 16 V `493-1044-ND` (Nichicon
UVR1C102MPD).

## Bought elsewhere

| Part | Where | Notes |
|---|---|---|
| CDM324 24 GHz radar module, ×2 | AliExpress (ordered) | an InnoSenT IPM-165 copy: 4.75 to 5.25 V |
| LED momentary buttons, ×2 | AliExpress (ordered) | check what voltage their LEDs are rated for (see docs/hardware.md) |
| 2S battery | see below | either a 2S Li-ion/LiPo pouch pack or 2 × 18650 cells |
| 2S BMS (protection) board | AliExpress: "2S 7.4V BMS balance", e.g. HX-2S-D20 | the Li-ion/LiPo (4.2 V per cell) version, not LiFePO4. Skip it only if the pack has protection built in |
| USB-C 2S charger board (IP2326) | AliExpress: "Type-C 2S 8.4V boost charging board IP2326" (for example the LX-LISC board) | 2S, 8.4 V, with a balance (midpoint) pad |
| Perfboard, pin headers, wire, heat-shrink | anywhere | |
| Copper foil tape with conductive adhesive | anywhere | shielding |
| Enclosure | 3D print or project box | see docs/hardware.md for the radar window |

**Battery:** a 2S pouch pack of 1500–2200 mAh covers a full day (run times are in
docs/hardware.md). RC-style packs (balance plug plus a separate power plug) have no protection
of their own, so they need the BMS board. For 18650s, buy a matched pair (Samsung 35E or LG MJ1,
same batch, within 0.05 V of each other) from a US battery shop (18650batterystore,
imrbatteries, 18650battery.com), not AliExpress or Amazon, where fakes are common.

No separate power switch is needed: the chrono switches itself fully off (docs/hardware.md).
