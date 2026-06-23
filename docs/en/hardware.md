# Hardware Overview

<!-- README-I18N:START -->
[Русский](../ru/hardware.md) | **English**
<!-- README-I18N:END -->

The instrument is a cylindrical stack of four Ø45 mm boards mounted directly
behind a NaI(Tl) Ø63×63 mm scintillator coupled to a Hamamatsu R1307
photomultiplier tube (PMT). A single USB-C cable exits the end cap and carries
both power and data.

## Board Stack (bottom → top)

```
[end cap — USB-C cable]
  1. USB/Power board   Ø45 mm   USB-C (JAE DX07S024WJ3R400)
  2. HV board          Ø45 mm   MAX1847 flyback + 8-stage CW multiplier, 0…−1500 V
  3. MCU board         Ø45 mm   STM32G474, CR-RC² shaper, ADC
  4. Divider board     Ø42–45 mm  R1307 divider + ADA4817 TIA
[end cap — R1307 PMT + NaI(Tl)]
```

### Inter-board connections

| Link | From → To | Connector |
|---|---|---|
| Stack power/control | board ↔ board | 2×N 2.54 mm pin headers |
| Signal | Divider → MCU | RG-178 coax + U.FL |
| Divider analog power | MCU → Divider | JST PH 3-pin (+5V_A, AGND, −5V_A) |
| −HV | HV → Divider | shielded ≥5 kV wire along the stack edge |

## Signal Chain

```
NaI(Tl)+R1307 → ADA4817 TIA (Zt=499Ω) → coax → CR(OPAMP1) → RC(OPAMP2) → PGA×4(OPAMP3) → ADC
[divider board]  ±5V_A          U.FL   τ=1 µs        τ=510 ns      DC=2.13 V       3.3 V
V(anode)=31 mV   amp_out=−431 mV       [MCU board — G474]
```

**Polarity:** the ADC pulse goes *down* from the 2.13 V baseline. In DSP:
`amplitude = baseline − sample`.

---

## 1. USB/Power board

USB-C interface and all supply rails for the stack.

- **FT231XQ** (QFN-20) — USB ↔ UART bridge at 600 kbps, exposed as a virtual COM port.
- **LP5907** → +3.3V_A (low-noise analog), **AMS1117CD-3.3** → +3.3V_D (digital).
- **TPS60400** charge pump → −5V_A.
- 47 µH LC filters on +5V_A and −5V_A (≈84 dB at 300 kHz).
- **+5V_HV** — raw USB 5 V routed through an LC filter to the HV board.
- **PRTR5V0U2X** ESD protection on the USB data lines.
- USB 2.0 budget ≈ 370 mA < 500 mA.

Detailed schematic: [`docs/usb_power_board_schematic.md`](../usb_power_board_schematic.md) (RU).

## 2. HV board

Generates the regulated negative high voltage for the PMT.

- **MAX1847** flyback controller + **IRLML0040** switch + **ATB322524** transformer (1:10.2).
- 8-stage Cockcroft–Walton (CW) multiplier producing 0 … −1500 V.
- Powered from the raw +5V_HV rail (not from the clean analog rails).
- Output RC filter chain (3×100 kΩ + HV capacitors) for ripple suppression.
- Feedback: CW output → 1 GΩ divider → MAX1847 error amplifier; `V_HV = −V_CTRL × 454.5`.
- Monitoring: 68 MΩ divider + **TL431** reference → `HV_MONITOR` on an MCU ADC pin.
- `HV_ENABLE` is active-high (MCU high = HV on).
- The assembled board is potted in silicone after test.

Detailed schematic: [`docs/hv_board_schematic.md`](../hv_board_schematic.md) (RU).

## 3. MCU board (v1.6)

The signal-processing and interface core.

- **STM32G474MET6** @ 170 MHz with internal **OPAMP1/2/3** and a 12-bit ADC at 4 Msps.
- Input: U.FL connector carrying ~470 mV @Cs-137 from the ADA4817 TIA on the divider board.
- Analog shaper: **CR** (10 kΩ + 100 pF + 10 kΩ) → **RC** (1 kΩ + 510 pF + 1 kΩ),
  forming a CR-RC²-style pulse, followed by a ×4 PGA into the ADC.
- **TL431** voltage reference (2.495 V) plus DC-bias dividers for the ADC baseline.
- **W25Q64** SPI flash, 16 MHz crystal.
- **Pt1000** temperature sensor (Honeywell 700-102BAA-B00) on the crystal housing,
  read ratiometrically for thermal-drift compensation.
- `HV_CONTROL` PWM → RC filter → HV board.

Detailed doc: [`docs/adc_mcu_board.md`](../adc_mcu_board.md) (RU).

## 4. Divider board

Active PMT divider and front-end amplifier, mounted closest to the tube.

- **Hamamatsu R1307-01** (flying leads), 11 pins.
- Resistor chain R401–R412 (tapered for spectrometry) with bypass/storage caps on the
  last dynodes only (C408/C409/C410 = 22/47/100 nF on Dy6–Dy8).
- Three **MMBTA42** transistor buffers (Q401/Q402/Q403) on dynodes Dy6/Dy7/Dy8, biased by
  the divider current (the chain runs through the dynodes via emitter resistors, per
  Hamamatsu Fig 5-15). B–E clamp diodes D401–D403 (1N4148W) protect against reverse
  breakdown on the HV ramp.
- **ADA4817-1ARDZ** transimpedance amplifier (TIA): anode directly on −IN (virtual ground),
  `Rf = 499 Ω`, `Cf = 4.7 pF` (NP0), Zt = 499 Ω, BW ≈ 92 MHz. ~18% lower noise than the
  former AD8000 inverter.
- Powered from ±5V_A delivered over the stack.

Detailed doc: [`docs/divider_board.md`](../divider_board.md) (RU).

---

See [simulations.md](simulations.md) for the analog-chain simulations and the
full noise budget, and [firmware.md](firmware.md) for the digital pulse processing.
