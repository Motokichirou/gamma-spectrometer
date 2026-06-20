<h1 align="center">NaI(Tl) Gamma Spectrometer</h1>

<p align="center">
  <img src="https://img.shields.io/badge/MCU-STM32G474-blue" alt="MCU">
  <img src="https://img.shields.io/badge/detector-NaI(Tl)%20%C3%9863%C3%9763-green" alt="Detector">
  <img src="https://img.shields.io/badge/PMT-Hamamatsu%20R1307-green" alt="PMT">
  <img src="https://img.shields.io/badge/protocol-shproto-orange" alt="Protocol">
  <img src="https://img.shields.io/badge/status-in%20development-yellow" alt="Status">
  <img src="https://img.shields.io/badge/HW-CERN--OHL--S--2.0-lightgrey" alt="HW License">
  <img src="https://img.shields.io/badge/SW-MIT-lightgrey" alt="SW License">
</p>

<!-- README-I18N:START -->
<p align="center"><a href="./README.md">Русский</a> | <b>English</b></p>
<!-- README-I18N:END -->

Portable cylindrical gamma spectrometer based on a NaI(Tl) Ø63×63 mm crystal and Hamamatsu R1307 PMT, with a USB-C interface compatible with [BecqMoni](https://github.com/Am6er/BecqMoni) / AtomSpectra (shproto protocol).

> 📚 **Documentation:** [hardware overview](docs/en/hardware.md) · [firmware](docs/en/firmware.md) · [shproto protocol](docs/en/protocol.md) · [PC software](host/README.md) · [simulations](docs/en/simulations.md)

---

## Key Specifications

| Parameter | Value |
|---|---|
| Detector | NaI(Tl) Ø63×63 mm |
| PMT | Hamamatsu R1307 (8-stage, Ø76 mm) |
| Energy range | 50 keV – 3.5 MeV |
| Target FWHM @ Cs-137 | 6.0–6.5 % |
| Spectrum channels | 8192 |
| Count rate | up to ~30 000 cps |
| MCU | STM32G474 @ 170 MHz (12-bit ADC, 4 Msps) |
| HV supply | 0 … −1500 V (MAX1847 + 8-stage CW multiplier) |
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
 │  MAX1847 · IRLML0040 · CW ×8 · −1500 V │
 ├─────────────────────────────────────────┤
 │  MCU board        Ø45 mm               │
 │  STM32G474 · CR-RC² shaper · ADC · SPI │
 ├─────────────────────────────────────────┤
 │  Divider board    Ø42–45 mm            │  ← Hamamatsu R1307 + NaI(Tl)
 │  R1307 divider · MMBTA42 buffers       │
 │  AD8000 CFA (×−15, BW = 99 MHz)        │
 └─────────────────────────────────────────┘
```

Inter-board connections:
- **Signal**: RG-178 coax + U.FL (Divider → MCU, ±431 mV for Cs-137)
- **±5 V_A power**: JST PH 3-pin (MCU → Divider)
- **−HV**: shielded ≥5 kV wire along stack edge (HV → Divider)
- **All other signals**: 2×N 2.54 mm pin headers

### Signal Chain

```
NaI(Tl) + R1307             Divider board                 MCU board
─────────────    ─────────────────────────────   ──────────────────────────────────
 Scintillation →  Divider (7.72 MΩ chain)     →  CR  (OPAMP1, τ=1 µs)
 τ_decay=230 ns   Active buffers (Dy6/7/8)        RC  (OPAMP2, τ=510 ns)
 I_anode(Cs-137)  AD8000 ×(−15), BW=99 MHz    →  PGA ×4 (OPAMP3)
  = 0.94 mA       V_out = −431 mV (Cs-137)        V_ADC = 2.116 V baseline
                                                   ADC1_IN12, 4 Msps
                                                   8192-ch histogram → USB
```

**ADC polarity:** pulses are negative w.r.t. baseline 2.116 V → `amplitude = baseline − sample`

### Energy Calibration (simulated, G474 OPAMP model)

| Source | Energy | ADC channel / 8192 |
|---|---|---|
| Am-241 | 59.5 keV | 101 |
| Cs-137 | 662 keV | 1138 |
| Co-60 | 1.33 MeV | 2277 |
| Tl-208 | 2.61 MeV | 4470 |
| Ceiling | 3.5 MeV | 6021 |

---

## Project Status

| Task | Status |
|---|---|
| Schematic — USB/Power board | ✅ Done (ERC 0) |
| Schematic — HV board | ✅ Done (ERC 0) |
| Schematic — MCU board | ✅ Done (ERC 0) |
| Schematic — Divider board | ✅ Done (ERC 0) |
| LTspice sim1: AD8000 stability | ✅ Done (BW=99 MHz, peaking<0.5 dB) |
| LTspice sim2: CR-RC² shaper | ✅ Done (PF=0.202, pulse width ~1.5 µs) |
| LTspice sim3: noise budget | ✅ Done (σ=94.4 µV rms, 0.31 ch rms) |
| BOM with MPN + footprints | ✅ Done (81 caps / 17 MPN, 67 R, all active) |
| PCB layout | 🚧 USB & HV routed (DRC electrically clean, schematic parity 0), inter-board bus placed (USB+HV); MCU and divider pending |
| STM32 firmware | 🚧 Stages 1–2 + self-test + energy calibration in flash running on Nucleo (shproto, real histogram); stage 3 on the target board |
| Host software `gammapult` | 🚧 Connection, diagnostics, live spectrum, energy-calibration wizard (GUI + CLI, single .exe); calibration interoperates with BecqMoni's buttons |

---

## Repository Structure

```
gamma-spectrometer/
├── docs/
│   ├── en/                           # English documentation set
│   ├── adc_mcu_board.md              # MCU board v1.6 — full technical doc
│   ├── divider_board.md              # Divider + AD8000 board
│   ├── usb_power_board.md            # USB/Power board overview
│   ├── usb_power_board_schematic.md  # USB/Power board — detailed schematic
│   ├── hv_board_schematic.md         # HV board — detailed schematic
│   ├── cheatsheet_mistakes.md        # Design rules & past mistakes log
│   └── refs/                         # Datasheets
├── firmware/                         # STM32G474 firmware (stages 1–2 run on Nucleo)
├── host/                             # PC software `gammapult` (Win32/ImGui, GUI + CLI)
├── pcb/
│   └── gamma_spectrometer/           # KiCad project (all 4 boards)
└── simulation/
    ├── models/                       # SPICE models (opamp_g474, AD8000)
    ├── scripts/                      # Python analysis scripts
    ├── sim1_ad8000.cir               # AD8000 stability & bandwidth
    ├── sim2_shaper.cir               # CR-RC² pulse shaper
    └── sim3_noise.cir                # Full noise budget
```

---

## Noise Budget (sim3)

| Contribution | FWHM |
|---|---|
| PMT R1307 + NaI(Tl) statistics | 6.3 % |
| HV ripple (typical) | 0.3–0.5 % |
| ADC nonlinearity (12-bit, ENOB ~10.5) | 0.1 % |
| Electronics noise (σ = 94.4 µV rms) | **0.064 %** |
| **Total** | **~6.32 %** |

Electronics noise is **two orders of magnitude below** PMT statistics — resolution is PMT-limited, not electronics-limited.

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
