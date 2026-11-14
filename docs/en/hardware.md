# Hardware Overview

<!-- README-I18N:START -->
[Русский](../ru/hardware.md) | **English**
<!-- README-I18N:END -->

The device is a cylindrical stack of four Ø45 mm PCBs mounted directly behind a
NaI(Tl) Ø63×63 mm scintillator coupled to a Hamamatsu R1307 PMT. A single
USB-C cable exits from the end cap, carrying both power and data.

> The first prototype is assembled and operational. Since 2026-11-09 it carries the standard NaI(Tl) 63×63 + R1307
> detector (HV ~565 V, Cs-137 6.7–6.9 % FWHM). It was initially debugged on a temporary NaI(Tl) 40×40
> detector with an R1306-class PMT (6.9 % at ~750 V).

## PCB Stack (bottom → top)

```
[end cap — USB-C cable]
  1. USB/Power board   Ø45 mm   USB-C (JAE DX07S024WJ3R400)
  2. HV board          Ø45 mm   MAX1847 flyback + 4-stage CW multiplier, setpoint 0…−1200 V
  3. MCU board         Ø45 mm   STM32G474CBT6, shaper using internal OPAMPs, ADC
  4. Divider board     Ø42–45 mm  R1307 divider + ADA4817 TIA
[end cap — PMT R1307 + NaI(Tl)]
```

### Inter-board Connections

There are no connectors in the stack — everything is soldered.

| Connection | From → To | Method |
|---|---|---|
| Stack bus (power, control, ±5V_A divider) | board ↔ board | Ø1.5 mm collet pins on an R20 mm arc, aligned across boards; 44 mounting positions: USB 11, HV 15, MCU 13, divider 5 |
| `AMP_OUT` signal | divider → MCU | thin coaxial cable, soldered to pad pairs (signal + braid) |
| −HV | HV → divider | shielded wire ≥5 kV through a Ø4.5 mm hole in the MCU board |
| Pt1000 | crystal housing → MCU | twisted pair to pads |
| Mechanics | — | three M2.5 standoffs (ground) in the center of each board |
| Shield | between HV and MCU boards | plate connected to GND: without it, HV converter noise (257 kHz) couples into the shaper |

### Printed Circuit Boards

- 2 layers, 1.5 mm; all four boards are assembled into a single 187.5 × 45 mm panel with
  breakaway tabs. Manufacturer — PS-Electro. No silkscreen.
- HV clearance norms of 3 mm/kV are defined in `pcb/gamma_spectrometer/gamma_spectrometer.kicad_dru`.

## Signal Chain

```
NaI(Tl)+PMT → ADA4817 TIA (Zt=2.7 kΩ) → coax → integrator (OPAMP1) → integrator (OPAMP2) → PGA×4 (OPAMP3) → ADC3
[divider board] ±5V_A                           τ≈1 µs              τ≈1.8 µs             base 0.394 V      3.467 MS/s
anode ≈ 0 V (virtual ground)                         [MCU board — G474]
ADC pulse: rise time 5–6 samples, FWHM 3.6 µs
```

**Polarity:** The pulse at the ADC goes *up* from the 0.394 V baseline (the PMT anode sinks current,
causing the TIA output to rise). In DSP: `amplitude = sample − baseline`.

Why the pulse is this long: with shorter shaping, the pulse fits into fewer than
two ADC samples, and phase estimation error degrades resolution (7.8 % instead of 7.0 % on the first signal chain).
Requirement — at least 4 samples on the rise time.

---

## 1. USB/Power Board

USB-C interface and all stack power buses.

- **FT231XQ** (QFN-20) — USB ↔ UART bridge at 600 kbit/s, appears as a virtual COM port.
- **LP5907** → +3.3V_A (low-noise analog), **AMS1117CD-3.3** → +3.3V_D (digital).
- Charge pump **TPS60400** → −5V_A.
- 47 µH LC filters on +5V_A and −5V_A (≈84 dB @ 300 kHz).
- **+5V_HV** — raw USB 5 V through an LC filter to the HV board.
- ESD protection **PRTR5V0U2X** on USB data lines.
- USB 2.0 budget ≈ 370 mA < 500 mA.
- ⚠ On v1 boards pin 4 of U104 (TPS60400) sits on a GND island with no via: the pump had no ground, −5V_A = +2.3 V. Fixed with a jumper to the C113 GND pad; corrected in `.kicad_pcb`, the fabricated boards carry the jumper.

Detailed schematic: [`docs/usb_power_board_schematic.md`](../usb_power_board_schematic.md) (Russian).

## 2. HV Board

Generates a regulated negative high voltage for the PMT.

- Flyback controller **MAX1847** + switch **IRLML0060** (60 V: margin over worst-case
  drain spike of 43–45 V) + transformer **ATB322524** (1:10.2).
- Primary snubber: BAS516 + 10 nF (100 V) + 2×BZT52C18.
- **4-stage** Cockcroft–Walton multiplier (8 BAV23S diodes), calculated limit −1500 V;
  firmware limits the setpoint to 1200 V (multiplier — 1300 V). Operating HV on the standard PMT ~565 V.
- Powered from the raw +5V_HV bus (not from clean analog buses).
- Output RC filter (3×100 kΩ + HV capacitors): output ripple — single-digit nV.
- Feedback: CW output → 1 GΩ divider → MAX1847 error amplifier; `V_HV = −V_CTRL × 454.5`.
  Loop is stable with 90–100° phase margin (calculated for 500–1500 V). The filter is outside the loop,
  so under load the output ≈ 0.958 × setpoint — compensated programmatically via `HV_MONITOR`.
- Monitoring: 68 MΩ divider + **TL431** reference → `HV_MONITOR` to MCU ADC pin.
- `HV_ENABLE` is active HIGH (HIGH from MCU = HV on), pull-down to ground keeps HV
  off until the MCU starts.
- The assembled board is potted with silicone after testing.

Detailed schematic: [`docs/hv_board_schematic.md`](../hv_board_schematic.md) (Russian).

## 3. MCU Board (v1.6)

Core of signal processing and interface.

- **STM32G474CBT6** (LQFP-48, 128 KB flash) @ 156 MHz with internal **OPAMP1/2/3** and 12-bit
  ADC; signal is digitized by ADC3_IN1 (PB1) at 3.467 MS/s. Until October 2026 the board was designed for LQFP-80 — re-routed when the
  80-pin chip order failed; all used pins are present in the 48-pin package.
- Input: coaxial cable from the ADA4817 TIA on the divider board.
- Analog shaper: OPAMP1 — integrator (R309 + R314 = 20 kΩ at input, R305 10 kΩ ∥ C323 100 pF
  in feedback, τ ≈ 1 µs); OPAMP2 — integrator (R308 1 kΩ, R306 8.2 kΩ ∥ C316 220 pF,
  τ ≈ 1.8 µs); then PGA ×4 to ADC. PGA input offset — R310 220 kΩ / R311 9.1 kΩ.
  ⚠ On v1.6 board this is a mod: C315 removed, C323 piggy-backed on R305, other values replaced
  (KiCad schematic still shows old values).
- Voltage reference **REF35250** (2.500 V, 12 ppm/°C, SOT-23-6) plus DC offset dividers
  for ADC baseline. In the worst case, the board works without it, using the MCU internal VREFBUF (2.500 V ±0.16 %).
- SPI flash **W25Q64**, 16 MHz crystal (CL 9 pF).
- Temperature sensor **Pt1000** (thin-film, class A, with leads) on the crystal housing,
  read via OPAMP6 ×8 on ADC4 for drift thermal compensation. ⚠ On v1.6 board, Pt1000 pads
  have no ground: a jumper between pads is needed, second sensor lead — to GND.
- `HV_CONTROL` PWM → RC filter → HV board.
- Firmware is updated via the STM32 system bootloader over the same USB-UART (`gfw.py flash`).
  SWD did not work on the first prototype. v1.6 mods: 100 nF on NRST (without it the MCU
  kept resetting), wire from BOOT0 pin for emergency bootloader entry.

Detailed doc: [`docs/adc_mcu_board.md`](../adc_mcu_board.md) (Russian).

## 4. Divider Board

Active PMT divider and input amplifier, closest to the tube.

- **Hamamatsu R1307-01** (flexible leads), 11 pins. Mounting is compatible with R1306.
- Resistor chain R401–R412, 7.72 MΩ (taper for spectroscopy); storage caps
  only on the last dynodes (C408/C409/C410 = 22/47/100 nF on Dy6–Dy8).
- Three transistor buffers **MMBTA42** (Q401/Q402/Q403) on dynodes Dy6/Dy7/Dy8, biased
  by divider current (chain goes through dynode emitter resistors, per Hamamatsu Fig 5-15).
  Protective B–E diodes D401–D403 (1N4148W) — against reverse breakdown during HV ramp.
- **ADA4817-1ARDZ** transimpedance amplifier (TIA): anode directly to −IN (virtual
  ground), `Rf = R417 = 2.7 kΩ`, `Cf = 4.7 pF` (NP0), feedback pole ≈12.5 MHz. Initially
  Rf was 499 Ω at HV ~1040 V; with 2.7 kΩ the PMT ran at ~750 V with the same resolution (on the 40×40), and anode
  current is five times lower. Output has 22 Ω (without it TIA oscillates on real
  input capacitance 3.4–4.6 pF, found via parasitic simulation).
- Powered from ±5V_A, supplied via the stack.

Detailed doc: [`docs/divider_board.md`](../divider_board.md) (Russian).

---

See [firmware.md](firmware.md) — digital pulse processing. Resolution measurements and calibration —
in the root [README](../../README.en.md). What to change in the next board revision (NRST, BOOT0, SWD, Pt1000 ground, SYNC,
direct MCU↔USB ground, a designed shield, Gerber re-export) — [`docs/revision_todo.md`](../revision_todo.md) (Russian).