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
| ADC Baseline | ≈0.394 V (≈640 codes; sags to ≈620 under load); pulses are positive: `amplitude = sample − baseline` |
| Pulse | 10% rise time to peak 5–6 samples, FWHM ≈14 samples (3.6 µs) |
| Spectrum | 8192 channels, channel = 2 × amplitude in ADC codes |
| Service ADCs | HV monitor (ADC2), VREFINT (reference), Pt1000 via OPAMP6 ×8 (ADC4), +3.3V_D |

## Modules

| Module | Function |
|---|---|
| `core/detector.c` | Pulse detection, pile-up and glitch rejection, amplitude via template matching, tail pile-up peeling, ADC DNL correction, baseline tracking, auto-threshold 5σ |
| `core/acq.c` | Acquisition: start/stop/reset, live and real time, CPU load |
| `core/spectrum.c` | 8192-channel histogram |
| `core/proto.c`, `core/shproto.c` | shproto protocol: spectrum and status frames, text commands |
| `core/svc.c` | Service measurements and service commands (`-hv`, `-gst`, `-diag` …) |
| `core/hv_ctl.c` | HV control: smooth ramp-up, adjustment via HV monitor, faults |
| `core/temp.c` | Pt1000 temperature |
| `core/settings.c`, `core/flashstore.c`, `core/becq_cal.c` | Settings (format v4), template, tail table, DNL list and BecqMoni calibration in flash |
| `core/logger.c`, `core/logrec.c`, `core/w25.c` | Spectrum and telemetry logger to the W25Q64 flash |
| `core/baud.c` | Temporary UART speed for experiments (`-baud`) |
| `core/dactest.c` | Self-test of the signal chain using internal DAC pulses (step 1…1350 codes; `gfw.py lin` checks linearity) |
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
- **Tail pile-up peeling** (`-pile`, on by default when a table is accepted): tails of preceding pulses are subtracted from the
  fit window using a tail table tied to the template (`-tlw`/`-tlm`/`-tlok`; `-gst` shows `pile=` and `rec=`, the number of
  recovered pulses). On the board a fit with peeling costs ≈5000 cycles instead of ≈2700. On a ~20.6 kcps mix with threshold 18,
  17 380 cps are accepted versus 16 560 without peeling.
- **ADC DNL correction** (`-dnl`, `-gst` shows `dnl=`): a list of up to 16 pairs "code — code-centre shift" is applied only
  inside the fit window. It cures the spectrum dip at the MSB transition (codes around 2048). The table is computed and
  written by `gammapult adc dnl`.
- Baseline is tracked using quiet samples. For 40 samples after a pulse, data is excluded from the baseline: tail
  of a long pulse.

## High Voltage

- Setpoint 0…1200 V via `-hv` command. Ramp-up is smooth, output is adjusted via the HV monitor.
- Protections: multiplier ceiling 1300 V, reference failure, stuck monitor, output deviation. HV
  is disabled upon entering the bootloader.
- Operating point depends on the PMT. On the standard NaI 63×63 + R1307 ~565 V, Cs-137 peak at channel ~1160–1200 (the earlier 40×40 needed ~735–755 V).

## Commands

BecqMoni commands: `-sta`, `-sto`, `-rst`, `-stt`, `-sho`, `-mode`, `-inf`, `-cal` (energy calibration
in BecqMoni format, 40 registers).

Service commands (response is a single line `-ok …` / `-err …`):

| Command | Function |
|---|---|
| `-hv <0…1200>` / `-hv off` / `-hv` | HV setpoint, V / disable / query |
| `-log` / `-log on <period_s> [delay_s]` / `-log off` / `-log start` / `-log stop` / `-log time <unix>` / `-log erase` / `-log list` / `-log read <sector>` | Logger: spectra and telemetry recorded to the W25Q64 flash without a PC (auto-start at power-up, ring journal, download with gammapult). Spec — `firmware/board_g474cb/SPEC_logger.md` |
| `-hvboot <V>` / `-hvboot off` / `-hvboot` | HV auto-start at power-up (closed unit): setpoint stored with `-save`; HV turns on ~5 s after start if reference and monitor are in range |
| `-hvzero`, `-hvk [mV/V]` | HV monitor calibration: zero and gain |
| `-ptcal <°C>` | Pt1000 calibration against reference temperature |
| `-setup [chk <unix> <0\|1> \| zero <unix> \| hv <unix> <V> <ch> \| tpl <unix> \| res <unix> <FWHM%> <ch> \| done <unix> \| clr]` | Instrument setup record (PC wizard). Bare — `-ok setup tpl=<0\|1> cal=<0\|1> chk=<unix>/<0\|1> zero=<unix> hv=<unix>/<V>/<ch> tplu=<unix> res=<unix>/<FWHM %>/<ch> done=<unix>`; with arguments — written to RAM (`-ok setup` / `-err setup`), to flash by `-save` |
| `-gst` | Instrument status in one line `key=value`: HV, reference, temperature, baseline, noise, input/accepted rate, rejections (`rej_swpb`), `pile=`/`rec=` (peeling), `dnl=` (pairs in the DNL list), load, live time; `saved=1\|0\|-` — RAM settings equal to flash / not / unknown (before `flash=`) |
| `-diag` | Signal chain node voltages and verdict against specs |
| `-thr [codes]` | Detector threshold in ADC codes (0 — auto 5σ; on the device under load the auto threshold inflates, so `-thr 18` is set) |
| `-opa [hs 0\|1 \| pga 2\|4\|8\|16]` | Shaper OPAMP mode and PGA gain (only outside acquisition, not written to flash) |
| `-smp`, `-noise` | ADC sampling time; baseline noise measurement |
| `-osc [raw]` | Pulse windows or raw ADC chunks (frames 0x02) |
| `-dac …` | Test pulses from internal DAC |
| `-tplw`, `-tplok`, `-tplclr` | Load shape template (64 lines) / apply / restore embedded (also drops the tail table) |
| `-tlw`, `-tlm`, `-tlok`, `-pile [on\|off]` | Tail table for pile-up peeling: 64 `-tlw` lines + 4 `-tlm` lines → `-tlok`; `-pile` switches peeling on/off |
| `-dnl [clr \| code shift …]` | ADC DNL correction: query / clear / add pairs (code 0…4095, shift in 1/16 LSB, up to 16 pairs) |
| `-baud [N]` | Temporary UART speed (600000, 1000000, 1500000, 2000000, 3000000); RAM-only, falls back to 600000 after 5 s without host frames |
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

- Under load (≥10 kcps) pulse tails leak into the baseline noise estimate: the auto threshold inflates and cuts the low end of the spectrum, and the baseline sags (640 → ≈620). Workaround — fixed `-thr 18`; a firmware fix is planned.
- Live time does not account for rejected pulses.
- The detector slow path costs ≈2700 cycles per pulse, ≈5000 with peeling: at ~20 kcps the CPU is ≈64 % busy and ≈17 400 cps are accepted (the per-chunk fit limit trims ≈11 %). The count-rate target is up to 30 kcps.

Details, measurements, and findings log — in [SPEC.md](../../firmware/board_g474cb/SPEC.md) (Russian).