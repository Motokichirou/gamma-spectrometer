# Firmware

<!-- README-I18N:START -->
[Русский](../ru/firmware.md) | **English**
<!-- README-I18N:END -->

Production firmware for the MCU board — [`firmware/gamma_fw/`](../../firmware/gamma_fw/). Written from scratch per the spec
[`firmware/board_g474cb/SPEC.md`](../../firmware/board_g474cb/SPEC.md) (Russian): bare CMSIS registers, no HAL.
Portable logic resides in `src/core/` and is covered by PC-based tests; hardware drivers are in `src/hw/`.
Compatible with [BecqMoni](https://github.com/Am6er/BecqMoni) / AtomSpectra via the shproto protocol
(see [protocol.md](protocol.md)).

## Signal Chain and Clocking

| Parameter | Value |
|---|---|
| MCU | STM32G474CBT6, 156 MHz from 16 MHz crystal (PLL M=2, N=39, R=2) |
| Signal ADC | ADC3_IN1 (PB1), 12-bit, **3.467 MS/s**, 2.5-cycle sampling, continuous via DMA in chunks of 8192 samples |
| Shaper | Built-in OPAMP1 and OPAMP2 — integrators (τ ≈ 1 and ≈ 1.8 µs), OPAMP3 — PGA ×4 |
| ADC Baseline | ≈0.394 V (≈645 codes); pulses are positive: `amplitude = sample − baseline` |
| Pulse | 10% rise time to peak 5–6 samples, FWHM ≈14 samples (3.6 µs) |
| Spectrum | 8192 channels, channel = 2 × amplitude in ADC codes |
| Service ADCs | HV monitor (ADC2), VREFINT (reference), Pt1000 via OPAMP6 ×8 (ADC4), +3.3V_D |

## Modules

| Module | Function |
|---|---|
| `core/detector.c` | Pulse detection, pile-up and glitch rejection, amplitude via template matching, baseline tracking, auto-threshold 5σ |
| `core/acq.c` | Acquisition: start/stop/reset, live and real time, CPU load |
| `core/spectrum.c` | 8192-channel histogram |
| `core/proto.c`, `core/shproto.c` | shproto protocol: spectrum and status frames, text commands |
| `core/svc.c` | Service measurements and service commands (`-hv`, `-gst`, `-diag` …) |
| `core/hv_ctl.c` | HV control: smooth ramp-up, adjustment via HV monitor, faults |
| `core/temp.c` | Pt1000 temperature |
| `core/settings.c`, `core/flashstore.c`, `core/becq_cal.c` | Settings, reference and BecqMoni calibration in flash |
| `core/dactest.c` | Self-test of the signal chain using internal DAC pulses |
| `core/tpl_pmt.h` | Pulse shape template, captured from the real chain (64 phases × 7 samples, Q14) |
| `core/tpl_dac.h` | DAC pulse shape template for self-test |
| `hw/*` | Clocking, ADC, OPAMPs, HV PWM, UART, flash, bootloader entry |

## Detector

- Pulses are detected by a threshold above the baseline. The threshold is set in ADC codes or selected automatically
  (5σ of baseline noise).
- Amplitude is calculated by fitting the shape template over 7 samples around the maximum, iterating through 64
  sub-sample phases. The template is captured from the real chain (`tools/tpl_pmt.py`) and embedded by default.
  A custom template can be loaded via `-tplw`/`-tplok` commands and saved to flash with `-save`.
- Rejection: width outside window (4…56 samples on the board), pile-ups, bad shape (χ²), noise glitches.
- Baseline is tracked using quiet samples. For 40 samples after a pulse, data is excluded from the baseline: tail
  of a long pulse.

## High Voltage

- Setpoint 0…1200 V via `-hv` command. Ramp-up is smooth, output is adjusted via the HV monitor.
- Protections: multiplier ceiling 1300 V, reference failure, stuck monitor, output deviation. HV
  is disabled upon entering the bootloader.
- Operating point depends on the PMT. On the current detector ~735–755 V, Cs-137 peak at channel ~1160–1200.

## Commands

BecqMoni commands: `-sta`, `-sto`, `-rst`, `-stt`, `-sho`, `-mode`, `-inf`, `-cal` (energy calibration
in BecqMoni format, 40 registers).

Service commands (response is a single line `-ok …` / `-err …`):

| Command | Function |
|---|---|
| `-hv <0…1200>` / `-hv off` / `-hv` | HV setpoint, V / disable / query |
| `-hvzero`, `-hvk [mV/V]` | HV monitor calibration: zero and gain |
| `-ptcal <°C>` | Pt1000 calibration against reference temperature |
| `-gst` | Instrument status in one line `key=value`: HV, reference, temperature, baseline, noise, load, live time |
| `-diag` | Signal chain node voltages and verdict against specs |
| `-thr [codes]` | Detector threshold in ADC codes (0 — auto 5σ) |
| `-opa [hs 0\|1 \| pga 2\|4\|8\|16]` | Shaper OPAMP mode and PGA gain (only outside acquisition, not written to flash) |
| `-smp`, `-noise` | ADC sampling time; baseline noise measurement |
| `-osc [raw]` | Pulse windows or raw ADC chunks (frames 0x02) |
| `-dac …` | Test pulses from internal DAC |
| `-tplw`, `-tplok`, `-tplclr` | Load shape template (64 lines) / apply / restore embedded |
| `-tc [t0 k1 k2]` | Gain thermal compensation coefficients (query without arguments) |
| `-save`, `-defaults` | Write settings to flash / restore defaults (in RAM) |
| `!boot` | Disable HV and enter system bootloader (firmware update) |

## Build, Tests, Update

- Build: `sh firmware/gamma_fw/build.sh` (arm-none-eabi-gcc), RAM budget is checked.
- PC tests: `firmware\gamma_fw\tests\run_tests.cmd`.
- Update via USB, no programmer:
  `python -I firmware/gamma_fw/tools/gfw.py flash firmware/gamma_fw/gamma_fw.bin`.
  The firmware receives `!boot`, disables HV, and enters the STM32 system bootloader (USART1 via FT231X).
  SWD does not work on the first board prototype. If the main firmware fails to start, the bootloader can
  be entered via the BOOT0 pin.
- Tools (`firmware/gamma_fw/tools/`): `gfw.py` (commands, spectrum, flashing), `tpl_pmt.py`
  (shape template from real chain), `tpl_dac.py` (DAC template).

## Known Limitations

- Under high load (≥5 kcps), the baseline droops, and resolution degrades slightly.
- Pt1000 open and short circuits are not yet diagnosed.
- Live time does not account for rejected pulses.
- DAC self-test has not yet been recalculated for the long pulse of the current shaper.

Details, measurements, and findings log — in [SPEC.md](../../firmware/board_g474cb/SPEC.md) (Russian).