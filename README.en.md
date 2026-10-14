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

> 🏭 **October 2026:** the boards are in production and all components are in hand. Next step —
> assembly and first power-up.

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
| MCU | STM32G474CBT6 (LQFP-48) @ 170 MHz, 12-bit ADC 4 Msps, on-chip op-amps |
| HV supply | 0 … −1500 V (MAX1847 + 4-stage CW multiplier) |
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
 │  MAX1847 · IRLML0060 · CW ×4 · −1500 V │
 ├─────────────────────────────────────────┤
 │  MCU board        Ø45 mm               │
 │  STM32G474CBT6 · CR-RC² · ADC · SPI    │
 ├─────────────────────────────────────────┤
 │  Divider board    Ø42–45 mm            │  ← Hamamatsu R1307 + NaI(Tl)
 │  R1307 divider · MMBTA42 buffers       │
 │  ADA4817-1 TIA (Zt = 499 Ω)            │
 └─────────────────────────────────────────┘
```

Inter-board connections — no connectors:
- **Stack bus**: Ø1.5 mm collet pins on an R20 mm arc, coaxial across all boards (44 sites:
  USB 11, HV 15, MCU 13, divider 5); the divider's ±5 V_A supply runs on it too
- **Signal** (Divider → MCU, ~470 mV for Cs-137): thin coax soldered to pads
- **−HV**: shielded ≥5 kV wire (HV → Divider) through a Ø4.5 mm hole in the MCU board
- **Pt1000** on the crystal housing: twisted pair to MCU-board pads
- Mechanics: three M2.5 standoffs in the centre of each board

### Signal Chain

```
NaI(Tl) + R1307             Divider board                 MCU board
─────────────    ─────────────────────────────   ──────────────────────────────────
 Scintillation →  Divider (7.72 MΩ chain)     →  CR  (OPAMP1, τ=1 µs)
 τ_decay=230 ns   Active buffers (Dy6/7/8)        RC  (OPAMP2, τ=510 ns)
 I_anode(Cs-137)  ADA4817 TIA, Zt=499 Ω, BW≈92 MHz →  PGA ×4 (OPAMP3)
  = 0.94 mA       V_out = +469 mV (Cs-137)        V_ADC = 0.408 V baseline
                                                   ADC1_IN12, 4 Msps
                                                   8192-ch histogram → USB
```

**ADC polarity:** the anode sinks current ⇒ the TIA output swings up ⇒ pulses are **positive** w.r.t. the 0.408 V baseline → `amplitude = sample − baseline`

### Energy Calibration (simulated, G474 OPAMP model)

| Source | Energy | ADC channel / 8192 |
|---|---|---|
| Am-241 | 59.5 keV | 105 |
| Cs-137 | 662 keV | 1159 |
| Co-60 | 1.33 MeV | 2321 |
| Tl-208 | 2.61 MeV | 4554 |
| Ceiling | 3.5 MeV | 6133 |

---

## Project Status

| Task | Status |
|---|---|
| Schematics of all 4 boards | ✅ Done (ERC 0) |
| LTspice: TIA + active divider, shaper, noise, HV loop | ✅ Validated (`simulation/`) |
| BOM with MPN + footprints | ✅ 176 components. Device cost **6 749 RUB** in cash (8 540 RUB including parts from own stock, detector excluded) — [`docs/cost.md`](docs/cost.md) |
| PCB layout | ✅ 2 layers, 4 boards in one 187.5 × 45 mm panel, HV clearance norms 3 mm/kV in `.kicad_dru`. MCU board re-routed for LQFP-48 (October 2026) |
| PCB fabrication | 🏭 In production (PS-Electro, 1.5 mm, no silkscreen) |
| Components | ✅ All received and checked, except the REF35250 reference — until it arrives the ADC runs from the on-chip VREFBUF 2.5 V |
| STM32 firmware | 🚧 Stages 1–2 on Nucleo: shproto, real histogram, self-test, energy calibration in flash, amplitude by pulse-shape template. For stage 3 the VREFBUF reference, 128 KB linker script and first power-up procedure are ready ([`firmware/board_g474cb/`](firmware/board_g474cb/README.md)) |
| Host software `gammapult` | 🚧 Connection, diagnostics, live spectrum, energy-calibration wizard (GUI + CLI, single .exe); calibration interoperates with BecqMoni's buttons |
| v2 — 1 000 000 cps | 🔬 Research: pile-up separation, digitising straight from the TIA — [`v2/`](v2/design.md) (Russian). v1 goes to hardware first: it is also the measurement bench for v2 |

---

## Repository Structure

```
gamma-spectrometer/
├── docs/
│   ├── en/                           # English documentation set
│   ├── adc_mcu_board.md              # MCU board v1.6 — full technical doc
│   ├── divider_board.md              # Divider + ADA4817 TIA board
│   ├── usb_power_board.md            # USB/Power board overview
│   ├── usb_power_board_schematic.md  # USB/Power board — detailed schematic
│   ├── hv_board_schematic.md         # HV board — detailed schematic
│   ├── mcu_lqfp48_migration.md       # MCU board move to LQFP-48
│   ├── fab_pselectro.md              # Fab requirements and check
│   ├── cost.md                       # Cost breakdown
│   ├── cheatsheet_mistakes.md        # Design rules & past mistakes log
│   └── refs/                         # Datasheets
├── firmware/
│   ├── core/                         # Portable core (shproto, DSP, histogram)
│   ├── gamma_stage1/                 # Stages 1–2 on Nucleo-G474RE
│   └── board_g474cb/                 # Stage-3 groundwork (target board)
├── host/                             # PC software `gammapult` (Win32/ImGui, GUI + CLI)
├── pcb/
│   └── gamma_spectrometer/           # KiCad project (all 4 boards, panel, Gerbers)
├── simulation/                       # LTspice + Python: signal chain, noise, HV loop, TIA parasitics
├── stock/                            # Purchasing, delivery check, cost script
├── review/                           # Independent design reviews
└── v2/                               # 1 Mcps instrument research
```

---

## Noise Budget (sim3)

| Contribution | FWHM |
|---|---|
| PMT R1307 + NaI(Tl) statistics | 6.3 % |
| HV ripple (after the output RC filter — a few nV) | negligible |
| ADC nonlinearity (12-bit, ENOB ~10.5) | 0.1 % |
| Electronics noise (σ = 76.9 µV rms with the ADA4817 TIA) | **0.05 %** |
| Digital processing (shape template, 30 kcps, Monte Carlo) | 0.44 % |
| **Total** | **≈6.3 %** |

Electronics and processing are an order of magnitude below PMT statistics — resolution is
limited by the crystal and PMT, not by the signal chain.

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
