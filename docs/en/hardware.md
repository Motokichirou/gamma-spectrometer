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
  2. HV board          Ø45 mm   MAX1847 flyback + 4-stage CW multiplier, 0…−1500 V
  3. MCU board         Ø45 mm   STM32G474CBT6, CR-RC² shaper, ADC
  4. Divider board     Ø42–45 mm  R1307 divider + ADA4817 TIA
[end cap — R1307 PMT + NaI(Tl)]
```

### Inter-board connections

There are no connectors in the stack — everything is soldered.

| Link | From → To | How |
|---|---|---|
| Stack bus (power, control, divider ±5V_A) | board ↔ board | Ø1.5 mm collet pins on an R20 mm arc, coaxial; 44 sites: USB 11, HV 15, MCU 13, divider 5 |
| `AMP_OUT` signal | Divider → MCU | thin coax soldered to pad pairs (signal + shield) |
| −HV | HV → Divider | shielded ≥5 kV wire through a Ø4.5 mm hole in the MCU board |
| Pt1000 | crystal housing → MCU | twisted pair to pads |
| Mechanics | — | three M2.5 standoffs (ground) in the centre of each board |

### Printed circuit boards

- 2 layers, 1.5 mm; all four boards are panelised into one 187.5 × 45 mm blank with
  break-off tabs. Fab: PS-Electro. No silkscreen.
- HV clearance norms of 3 mm/kV are encoded as rules in
  `pcb/gamma_spectrometer/gamma_spectrometer.kicad_dru`.

## Signal Chain

```
NaI(Tl)+R1307 → ADA4817 TIA (Zt=499Ω) → coax → CR(OPAMP1) → RC(OPAMP2) → PGA×4(OPAMP3) → ADC
[divider board]  ±5V_A                 τ=1 µs        τ=510 ns      DC=0.41 V       3.3 V
anode ≈ 0 V (virtual gnd)  amp_out = +469 mV @Cs-137  [MCU board — G474]
```

**Polarity:** the ADC pulse goes *up* from the 0.41 V baseline (the PMT anode sinks
current, so the TIA output rises). In DSP: `amplitude = sample − baseline`.

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

- **MAX1847** flyback controller + **IRLML0060** switch (60 V: margin over the 43–45 V
  worst-case drain spike) + **ATB322524** transformer (1:10.2).
- Primary snubber: BAS516 + 10 nF (100 V) + 2×BZT52C18.
- **4-stage** Cockcroft–Walton (CW) multiplier (8 BAV23S diodes) producing 0 … −1500 V.
- Powered from the raw +5V_HV rail (not from the clean analog rails).
- Output RC filter chain (3×100 kΩ + HV capacitors): output ripple is a few nV.
- Feedback: CW output → 1 GΩ divider → MAX1847 error amplifier; `V_HV = −V_CTRL × 454.5`.
  The loop is stable with 90–100° phase margin over 500–1500 V. The filter sits outside the
  loop, so under load the output is ≈ 0.958 × setpoint — compensated in firmware via `HV_MONITOR`.
- Monitoring: 68 MΩ divider + **TL431** reference → `HV_MONITOR` on an MCU ADC pin.
- `HV_ENABLE` is active-high (MCU high = HV on); a pull-down keeps HV off until the MCU runs.
- The assembled board is potted in silicone after test.

Detailed schematic: [`docs/hv_board_schematic.md`](../hv_board_schematic.md) (RU).

## 3. MCU board (v1.6)

The signal-processing and interface core.

- **STM32G474CBT6** (LQFP-48, 128 KB flash) @ 170 MHz with internal **OPAMP1/2/3** and a
  12-bit ADC at 4 Msps. Until October 2026 the board was laid out for LQFP-80; it was re-routed
  when the 80-pin order fell through — every pin in use exists on the 48-pin package too.
- Input: coax carrying ~470 mV @Cs-137 from the ADA4817 TIA on the divider board.
- Analog shaper: **CR** (10 kΩ + 100 pF + 10 kΩ) → **RC** (1 kΩ + 510 pF + 1 kΩ),
  forming a CR-RC²-style pulse, followed by a ×4 PGA into the ADC.
- **REF35250** voltage reference (2.500 V, 12 ppm/°C, SOT-23-6) plus DC-bias dividers
  for the ADC baseline. Without it the board runs from the MCU's on-chip VREFBUF (2.500 V ±0.16 %).
- **W25Q64** SPI flash, 16 MHz crystal (CL 9 pF).
- **Pt1000** temperature sensor (thin-film, class A, wire leads) on the crystal housing,
  read ratiometrically via ADC4 for thermal-drift compensation.
- `HV_CONTROL` PWM → RC filter → HV board.
- Programming over SWD with wires soldered straight to the MCU pins (37 SWDIO, 38 SWCLK,
  7 NRST); later updates go through the system bootloader over the same USB-UART.

Detailed doc: [`docs/adc_mcu_board.md`](../adc_mcu_board.md) (RU).

## 4. Divider board

Active PMT divider and front-end amplifier, mounted closest to the tube.

- **Hamamatsu R1307-01** (flying leads), 11 pins.
- Resistor chain R401–R412, 7.72 MΩ (tapered for spectrometry) with bypass/storage caps on the
  last dynodes only (C408/C409/C410 = 22/47/100 nF on Dy6–Dy8).
- Three **MMBTA42** transistor buffers (Q401/Q402/Q403) on dynodes Dy6/Dy7/Dy8, biased by
  the divider current (the chain runs through the dynodes via emitter resistors, per
  Hamamatsu Fig 5-15). B–E clamp diodes D401–D403 (1N4148W) protect against reverse
  breakdown on the HV ramp.
- **ADA4817-1ARDZ** transimpedance amplifier (TIA): anode directly on −IN (virtual ground),
  `Rf = 499 Ω`, `Cf = 4.7 pF` (NP0), Zt = 499 Ω, BW ≈ 92 MHz. ~18% lower noise than the
  former AD8000 inverter. A 22 Ω output resistor is mandatory: without it the TIA oscillates
  on the real 3.4–4.6 pF input capacitance (found by parasitic extraction).
- Powered from ±5V_A delivered over the stack.

Detailed doc: [`docs/divider_board.md`](../divider_board.md) (RU).

---

See [simulations.md](simulations.md) for the analog-chain simulations and the
full noise budget, and [firmware.md](firmware.md) for the digital pulse processing.
