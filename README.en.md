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

> ✅ **November 2026: the instrument is assembled and works with BecqMoni** on the standard NaI(Tl) Ø63×63 + R1307
> detector (PMT at ~565 V): **Cs-137 6.7–6.9 % FWHM**. Tail pile-up peeling in the firmware: on a ~20 kcps mix about
> 17 400 cps are accepted without loss of resolution; ADC code nonlinearity (DNL) correction. Details — findings log
> [`SPEC.md`](firmware/board_g474cb/SPEC.md) §10 (Russian).

---

## Key Specifications

| Parameter | Value |
|---|---|
| Detector | NaI(Tl) Ø63×63 mm |
| PMT | Hamamatsu R1307 (8-stage, Ø76 mm) |
| Energy range | ≈20 keV (threshold 18 codes) – ≈3.8 MeV (ADC saturation) |
| FWHM @ Cs-137 | **6.7–6.9 % measured** (standard NaI 63×63 + R1307, HV ~565 V) |
| Spectrum channels | 8192 |
| Count rate | up to ~30 000 cps |
| MCU | STM32G474CBT6 (LQFP-48) @ 156 MHz, 12-bit ADC (signal at 3.47 Msps), on-chip op-amps |
| HV supply | 0 … −1200 V, ~565 V in operation on the standard PMT (MAX1847 + 4-stage CW multiplier) |
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
 PMT at ~565 V    ADA4817 TIA, Zt=2.7 kΩ       →  PGA ×4 (OPAMP3)
                                                   V_ADC = 0.394 V baseline
                                                   ADC3_IN1, 3.47 Msps
                                                   pulse: 5–6-sample rise, 3.6 µs FWHM
                                                   amplitude by shape template (64 phases)
                                                   8192-ch histogram → USB
```

**ADC polarity:** the anode sinks current ⇒ the TIA output swings up ⇒ pulses are **positive** w.r.t. the 0.394 V baseline → `amplitude = sample − baseline`

### Energy Calibration (measured on the earlier temporary NaI 40×40 detector, ~755 V, Ra-226)

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
| Ceiling (ADC saturation) | ≈3.8 MeV | ≈6900 | — |

Peak positions are set by the HV: Cs is placed at channel ~1160–1200 by tuning the HV (~565 V on the standard detector).

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
| STM32 firmware | ✅ Clean firmware [`firmware/gamma_fw/`](firmware/gamma_fw/) on the board: detector with a pulse-shape template (taken from the real chain), tail pile-up peeling, ADC DNL correction, HV with trimming, Pt1000, spectrum logger to the W25Q64 flash, settings (v4) and calibration in flash, updates over the COM port; accepted by BecqMoni. Spec and findings log — [`firmware/board_g474cb/SPEC.md`](firmware/board_g474cb/SPEC.md) (Russian) |
| Signal-chain tuning | ✅ Shaper reworked for long shaping, R417 2.7 kΩ: Cs 7.8 → 6.9 % (on 40×40), HV 1030 → ~750 V |
| Standard detector NaI 63×63 + R1307 | ✅ Installed since 2026-11-09, HV ~565 V, Cs-137 6.7–6.9 % FWHM. Remaining: thermal compensation and the board revision — [`docs/revision_todo.md`](docs/revision_todo.md) (Russian) |
| Host software `gammapult` | ✅ Release v0.7.0: service console for the production firmware (GUI + CLI, single .exe; new UI — home screen, setup wizard, advanced mode): telemetry, HV and peak-based HV tuning, HV-monitor and Pt1000 calibration, threshold/PGA, DAC linearity self-test, shape-template and thermal-compensation wizards, **blank-device flashing and a commissioning wizard**, save to flash. Spectra and energy calibration — in BecqMoni |

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

Rows up to and including "The same Sypher detector" were measured on the temporary NaI 40×40 crystal.

| Configuration | FWHM |
|---|---|
| First chain (pulse shorter than an ADC sample), ~1040 V | 7.6–7.8 % |
| Shaper V2 (5–6-sample rise), 1030 V | 6.99 % |
| Shaper V2 + R417 2.7 kΩ, ~755 V | 7.1 % |
| Same, 40×40 crystal with higher light yield, 735 V | **6.9 %** |
| The same Sypher detector on its own electronics (in energy, keV) | 7.08 % |
| **Standard NaI 63×63 + R1307**, ~565 V (since 2026-11-09) | **6.7–6.9 %** |

On the standard detector the baseline noise is σ ≈ 2.2 ADC codes against a Cs amplitude of ≈595 codes:
the electronics contribute at most ≈0.9 % FWHM (an upper bound; the template fit averages further), so of
the 6.7–6.9 % about 6.7 % comes from the crystal and PMT. Resolution is limited by the detector, not by the
chain (on the 40×40 crystal an HV scan showed the same: a ≈ 7.0 %, noise ≈0.8 %). Resolution is compared in energy only: on chains with a nonlinear scale
the per-channel percentage looks better than it really is. Details — [`firmware/board_g474cb/SPEC.md`](firmware/board_g474cb/SPEC.md) §10 (Russian).

---

## Protocol: shproto / AtomSpectra

Compatible with [BecqMoni](https://github.com/Am6er/BecqMoni) and [nanopro](https://github.com/Am6er/nanopro).

- UART 600 000 bps, 8N1
- Frame: `0xFE | CMD | PAYLOAD (byte-stuffed) | CRC16-MODBUS | 0xA5`
- Cmd `0x01`: spectrum (up to 8192 × 32-bit channels)
- Cmd `0x04`: status (elapsed time, CPS, invalid pulses)
- Keep-alive: `0xFF` from PC every ~1 s
- Optional `-baud` command (up to 3 Mbaud) is RAM-only: after 5 s without host frames the device falls back to 600 000

Full reference: [docs/en/protocol.md](docs/en/protocol.md).

---

## License

Hardware design files (KiCad, LTspice) — [CERN OHL-S v2](https://ohwr.org/cern_ohl_s_v2.txt)  
Software (firmware, host) — [MIT](https://opensource.org/licenses/MIT)
