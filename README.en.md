<h1 align="center">NaI(Tl) Gamma Spectrometer</h1>

<p align="center">
  <img src="https://img.shields.io/badge/MCU-STM32G474-blue" alt="MCU">
  <img src="https://img.shields.io/badge/detector-NaI(Tl)%20%C3%9863%C3%9763-green" alt="Detector">
  <img src="https://img.shields.io/badge/PMT-Hamamatsu%20R1307-green" alt="PMT">
  <img src="https://img.shields.io/badge/protocol-shproto-orange" alt="Protocol">
  <img src="https://img.shields.io/badge/status-prototype%20working-brightgreen" alt="Status">
  <img src="https://img.shields.io/badge/HW-CERN--OHL--S--2.0-lightgrey" alt="HW License">
  <img src="https://img.shields.io/badge/SW-MIT-lightgrey" alt="SW License">
</p>

<!-- README-I18N:START -->
<p align="center"><a href="./README.md">Русский</a> | <b>English</b></p>
<!-- README-I18N:END -->

Portable cylindrical gamma spectrometer based on a NaI(Tl) Ø63×63 mm crystal and Hamamatsu R1307 PMT, with a USB-C interface compatible with [BecqMoni](https://github.com/Am6er/BecqMoni) / AtomSpectra (shproto protocol).

> 📚 **Documentation:** [hardware overview](docs/en/hardware.md) · [firmware](docs/en/firmware.md) · [shproto protocol](docs/en/protocol.md) · [PC software](host/README.md)

> ✅ **November 2026: the instrument is assembled and works with BecqMoni.** On a temporary NaI(Tl) 40×40
> detector — **Cs-137 6.9 % FWHM**, PMT at ~750 V. Next stage — the target NaI(Tl) Ø63×63 + R1307.

---

## Key Specifications

| Parameter | Value |
|---|---|
| Detector | NaI(Tl) Ø63×63 mm |
| PMT | Hamamatsu R1307 (8-stage, Ø76 mm) |
| Energy range | 50 keV – 3.5 MeV |
| FWHM @ Cs-137 | **6.9 % measured** (NaI 40×40, PMT from a Sypher build); target for 63×63 — 6.0–6.5 % |
| Spectrum channels | 8192 |
| Count rate | up to ~30 000 cps |
| MCU | STM32G474CBT6 (LQFP-48) @ 156 MHz, 12-bit ADC (signal at 3.47 Msps), on-chip op-amps |
| HV supply | 0 … −1200 V, ~750 V in operation (MAX1847 + 4-stage CW multiplier) |
| Interface | USB-C → UART 600 kbps (FT231X), shproto protocol |
| Power | USB 5 V, ~370 mA |
| Form factor | Cylindrical stack, Ø45 mm boards |

---

## Hardware Architecture

### Board Stack (bottom → top)

```
 ┌─────────────────────────────────────────┐
 │  USB/Power board  Ø45 mm               │  ← USB-C cable
 │  LP5907 · AMS1117 · TPS60400 · FT231X  │
 ├─────────────────────────────────────────┤
 │  HV board         Ø45 mm               │
 │  MAX1847 · IRLML0060 · CW ×4 · ≤−1200 V│
 ├─────────────────────────────────────────┤
 │  MCU board        Ø45 mm               │
 │  STM32G474CBT6 · shaper · ADC · SPI    │
 ├─────────────────────────────────────────┤
 │  Divider board    Ø42–45 mm            │  ← Hamamatsu R1307 + NaI(Tl)
 │  R1307 divider · MMBTA42 buffers       │
 │  ADA4817-1 TIA (Zt = 2.7 kΩ)           │
 └─────────────────────────────────────────┘
```

Inter-board connections — no connectors:
- **Stack bus**: Ø1.5 mm collet pins on an R20 mm arc, coaxial across all boards (44 sites:
  USB 11, HV 15, MCU 13, divider 5); the divider's ±5 V_A supply runs on it too
- **Signal** (Divider → MCU): thin coax soldered to pads
- **−HV**: shielded ≥5 kV wire (HV → Divider) through a Ø4.5 mm hole in the MCU board
- **Pt1000** on the crystal housing: twisted pair to MCU-board pads
- Mechanics: three M2.5 standoffs in the centre of each board

### Signal Chain

```
NaI(Tl) + R1307             Divider board                 MCU board
─────────────    ─────────────────────────────   ──────────────────────────────────
 Scintillation →  Divider (7.72 MΩ chain)     →  OPAMP1: integrator, τ=1 µs
 τ_decay=230 ns   Active buffers (Dy6/7/8)        OPAMP2: integrator, τ=1.8 µs
 PMT at ~750 V    ADA4817 TIA, Zt=2.7 kΩ       →  PGA ×4 (OPAMP3)
                                                   V_ADC = 0.394 V baseline
                                                   ADC3_IN1, 3.47 Msps
                                                   pulse: 5–6-sample rise, 3.6 µs FWHM
                                                   amplitude by shape template (64 phases)
                                                   8192-ch histogram → USB
```

**ADC polarity:** the anode sinks current ⇒ the TIA output swings up ⇒ pulses are **positive** w.r.t. the 0.394 V baseline → `amplitude = sample − baseline`

### Energy Calibration (measured: NaI 40×40, ~755 V, Ra-226)

5-point calibration in BecqMoni: `E[keV] = −6.01 + 0.5518·x + 6.63·10⁻⁶·x²` (x — channel).
The curvature is mostly NaI light-yield non-proportionality, not the electronics.

| Line | Energy | ADC channel / 8192 | Found by calibration |
|---|---|---|---|
| Pb-214 | 295 keV | 549 | 299 keV |
| Pb-214 | 352 keV | 652 | 356 keV |
| Bi-214 | 609 keV | 1102 | 610 keV |
| Cs-137 | 662 keV | ≈1194 | — |
| Bi-214 | 1120 keV | 1990 | 1118 keV |
| Bi-214 | 1764 keV | 3107 | 1773 keV |
| Ceiling | 3.5 MeV | ≈5930 | — |

Peak positions are set by the HV: for another detector, Cs is placed at channel ~1160–1200 by tuning the HV.

---

## Project Status

| Task | Status |
|---|---|
| Schematics of all 4 boards | ✅ Done (ERC 0) |
| LTspice: TIA + active divider, shaper, noise, HV loop | ✅ Before layout; result confirmed by measurements on the board |
| BOM with MPN + footprints | ✅ 176 components. Device cost **6 749 RUB** in cash (8 540 RUB including parts from own stock, detector excluded) — [`docs/cost.md`](docs/cost.md) |
| PCB layout | ✅ 2 layers, 4 boards in one 187.5 × 45 mm panel, HV clearance norms 3 mm/kV in `.kicad_dru`. MCU board re-routed for LQFP-48 (October 2026) |
| PCB fabrication | ✅ Received (PS-Electro, 1.5 mm, no silkscreen) |
| Components | ✅ All received and checked |
| Assembly and first power-up | ✅ Stack assembled, flashed over UART via the system bootloader, HV under load, signal chain healthy |
| STM32 firmware | ✅ Clean firmware [`firmware/gamma_fw/`](firmware/gamma_fw/) on the board: detector with a pulse-shape template (taken from the real chain), HV with trimming, Pt1000, settings and calibration in flash, updates over the COM port; accepted by BecqMoni. Spec and findings log — [`firmware/board_g474cb/SPEC.md`](firmware/board_g474cb/SPEC.md) (Russian) |
| Signal-chain tuning | ✅ Shaper reworked for long shaping, R417 2.7 kΩ: Cs 7.8 → 6.9 %, HV 1030 → ~750 V |
| Target detector NaI 63×63 + R1307 | ⏭ Next stage: HV tuning, shape template, calibration |
| Host software `gammapult` | ✅ Service console for the production firmware (GUI + CLI, single .exe): telemetry, HV and peak-based HV tuning, HV-monitor and Pt1000 calibration, threshold/PGA, DAC linearity self-test, save to flash. Spectra and energy calibration — in BecqMoni |

---

## Repository Structure

```
gamma-spectrometer/
├── docs/
│   ├── adc_mcu_board.md              # MCU board v1.6 — full technical doc
│   ├── divider_board.md              # Divider + ADA4817 TIA board
│   ├── usb_power_board.md            # USB/Power board overview
│   ├── usb_power_board_schematic.md  # USB/Power board — detailed schematic
│   ├── hv_board_schematic.md         # HV board — detailed schematic
│   ├── mcu_board_bringup.md          # MCU board bring-up (first power-up, rework)
│   ├── hv_board_bringup.md           # HV board bring-up, 257 kHz interference and shield
│   ├── assembly_notes.md             # Assembly notes
│   ├── fab_pselectro.md              # Fab requirements and check
│   ├── cost.md                       # Cost breakdown
│   └── ru/, en/                      # Curated documentation (RU/EN)
├── firmware/
│   ├── board_g474cb/SPEC.md          # Firmware spec and findings log (Russian)
│   └── gamma_fw/                     # Production firmware (build.sh, PC tests, tools/gfw.py)
├── host/                             # PC software `gammapult` (Win32/ImGui, GUI + CLI)
├── pcb/
│   └── gamma_spectrometer/           # KiCad project (all 4 boards, panel, Gerbers)
```

---

## Resolution (measured, Cs-137)

| Configuration | FWHM |
|---|---|
| First chain (pulse shorter than an ADC sample), ~1040 V | 7.6–7.8 % |
| Shaper V2 (5–6-sample rise), 1030 V | 6.99 % |
| Shaper V2 + R417 2.7 kΩ, ~755 V | 7.1 % |
| Same, 40×40 crystal with higher light yield, 735 V | **6.9 %** |
| The same Sypher detector on its own electronics (in energy, keV) | 7.08 % |

An HV scan on the new chain decomposes as FWHM² = a² + (b/channel)²: a constant part a ≈ 7.0 %
(crystal + PMT + processing) and electronics noise of ≈0.8 % at the Cs channel. Resolution is limited by
the detector, not by the chain. Resolution is compared in energy only: on chains with a nonlinear scale
the per-channel percentage looks better than it really is. Details — [`firmware/board_g474cb/SPEC.md`](firmware/board_g474cb/SPEC.md) §10 (Russian).

---

## Protocol: shproto / AtomSpectra

Compatible with [BecqMoni](https://github.com/Am6er/BecqMoni) and [nanopro](https://github.com/Am6er/nanopro).

- UART 600 000 bps, 8N1
- Frame: `0xFE | CMD | PAYLOAD (byte-stuffed) | CRC16-MODBUS | 0xA5`
- Cmd `0x01`: spectrum (up to 8192 × 32-bit channels)
- Cmd `0x04`: status (elapsed time, CPS, invalid pulses)
- Keep-alive: `0xFF` from PC every ~1 s

Full reference: [docs/en/protocol.md](docs/en/protocol.md).

---

## License

Hardware design files (KiCad, LTspice) — [CERN OHL-S v2](https://ohwr.org/cern_ohl_s_v2.txt)  
Software (firmware, host) — [MIT](https://opensource.org/licenses/MIT)
