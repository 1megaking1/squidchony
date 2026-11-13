# Parts

Prices and stock are from DigiKey's US site in November 2026 and will drift. About $82 for
the DigiKey order, or $94 with the optional socket strip, before shipping and tax.

## DigiKey order

`digikey-bulk-add.txt` pastes straight into DigiKey's bulk-add box. It leaves out resistors
(they come from a 1% metal-film kit, see below) and the parts bought elsewhere. Delete the
last line (`ED6264-64-ND`) if you're skipping the socket strip.

**Controller and screen**

| Qty | DigiKey # | Part | Used for |
|---|---|---|---|
| 1 | 2648-SC1634-ND | Raspberry Pi Pico 2 WH | controller (W = Wi-Fi, H = headers fitted) |
| 1 | 1528-4311-ND | Adafruit 4311, 2.0" 320×240 IPS (ST7789) | screen |
| 2 | 296-20801-ND | OPA2365AID | amplifier op-amp (1 + spare) |
| 1 | 1528-1071-ND | Adafruit 1212 SOIC-8 adapters (6-pack) | op-amp onto perfboard |
| 4 | 2N3904FS-ND | 2N3904 | button LED switches (2 + spares) |

**Power**

| Qty | DigiKey # | Part | Used for |
|---|---|---|---|
| 2 | BH-18650-W-ND | 18650 holder, wire leads | two holders in series (2S) |
| 1 | 576-1136-ND | MIC2940A-5.0 | 5 V for the Pico side |
| 2 | MCP1702-5002E/TO-ND | MCP1702 5.0 V | 5 V for the radar side (1 + spare) |
| 5 | 1N5817-E3/73GICT-ND | 1N5817 Schottky | reverse protection, Pico feed |
| 2 | P5141-ND | 470 µF 16 V | filter and regulator inputs |
| 2 | P5127-ND | 1000 µF 10 V | radar supply filter |
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
| 1 | ED6264-64-ND | Mill-Max 64-pin socket strip (optional) | cut into sockets for swappable parts |

**Resistors** (from a 1% metal-film kit; measure each one before soldering, kit bags get mixed up)

| Value | How many | Where |
|---|---|---|
| 2.2 Ω | 1 | radar supply filter, after the MCP1702 |
| 10 Ω | 1 | radar supply filter, before the MCP1702 |
| 4.7 kΩ | 3 + 2 | amplifier R4, R5, R6; LED transistor bases |
| 10 kΩ | 3 | bias divider R2, R3; ADC divider top |
| 15 kΩ | 1 | ADC divider bottom |
| 220 kΩ | 2 | gain R7, R8 (starting value) |
| 100 kΩ, 150 kΩ, 330 kΩ | 2 each | other gain steps; 100 kΩ also battery divider bottom |
| 470 kΩ | 2 + 1 | gain step; battery divider top |

If you'd rather order resistors too: `S10CACT-ND` (10 Ω), `2.2QBK-ND` (2.2 Ω), and
`S4.7KCACT-ND`, `S10KCACT-ND`, `S15KCACT-ND`, `S100KCACT-ND`, `S150KCACT-ND`,
`S220KCACT-ND`, `S330KCACT-ND`, `S473KCACT-ND` (10 of each is cheaper than fewer).

Backups if something sells out: 1 nF `C315C102JAG5TA-ND`; 220 kΩ `CMF50220K00FHEA-ND`.

## Bought elsewhere

| Part | Where | Notes |
|---|---|---|
| CDM324 24 GHz radar module, ×2 | AliExpress (ordered) | an InnoSenT IPM-165 copy: 4.75 to 5.25 V |
| LED momentary buttons, ×2 | AliExpress (ordered) | check what voltage their LEDs are rated for (see docs/hardware.md) |
| 2 × 18650 cells, matched pair | a US battery shop (18650batterystore, imrbatteries, 18650battery.com) | Samsung 35E or LG MJ1, unprotected, same batch, within 0.05 V of each other. Not AliExpress or Amazon (fakes) |
| 2S BMS (protection) board | AliExpress: "2S 7.4V 18650 BMS balance", e.g. HX-2S-D20 | any current rating; the chrono draws ~150 mA |
| USB-C 2S charger board (IP2326) | AliExpress: "Type-C 2S 8.4V boost charging board IP2326" | must say 2S / 8.4 V and have a BM (midpoint) pad |
| Power switch | anywhere | the charger has no power path: charge with the switch off |
| Perfboard, pin headers, wire, heat-shrink | anywhere | |
| Copper foil tape with conductive adhesive | anywhere | shielding |
| Enclosure | 3D print or project box | see docs/hardware.md for the radar window |
