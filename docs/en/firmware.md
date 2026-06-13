# Firmware

The STM32G474 firmware acquires shaped pulses with the ADC, detects and measures
each pulse, accumulates an 8192-channel histogram, and streams it to a host over
the [shproto protocol](protocol.md) so that [BecqMoni](https://github.com/Am6er/BecqMoni)
can display the spectrum.

## Portable core

The signal-processing code lives in `firmware/core/` and has **no HAL dependency** —
it compiles anywhere and is shared between the Nucleo bring-up board and the target
MCU board:

| Module | Responsibility |
|---|---|
| `shproto.c/.h` | Frame parser/builder: CRC16-MODBUS, byte stuffing, FSM |
| `spectrum.c/.h` | 8192-channel `uint32` histogram |
| `dsp.c/.h` | Pulse detector: baseline tracker, pile-up rejection, quadratic LSQ peak fit |
| `pulsegen.c/.h` | Test-pulse generator (built-in self-test source) |
| `selftest.c/.h` | ENC / linearity self-test FSM |
| `app.c/.h` | Command handling (`-sta/-sto/-rst/-inf/-cal/-tst`) and 1 Hz spectrum + status |

The core talks to hardware through a few port functions (`app_port_uart_send`,
`app_port_millis`, …) implemented in the board-specific glue.

## Development stages

| Stage | Hardware | Goal | Status |
|---|---|---|---|
| 1 | Nucleo-G474RE | shproto over VCP → BecqMoni draws a test spectrum | ✅ working |
| 2 | Nucleo-G474RE | DAC→ADC loopback: real pulses, peak detect, real histogram | ✅ working |
| 3 | Target MCU board | OPAMP shaper, real PMT pulses, HV, Pt1000 compensation | ⏳ pending hardware |

On the Nucleo, stage 2 closes the loop entirely on-chip: a TIM7 Poisson scheduler
fires the DAC with realistic pulse shapes, a jumper feeds DAC (PA4/A2) back into the
ADC (PA0/A0) at 2.83 Msps, and the same DSP that will run on the target board builds
the histogram. Measured throughput ≈ 6000 cps accepted of ≈ 8500 generated.

## Pulse DSP (`dsp.c`)

For each ADC sample the detector:

1. **Tracks the baseline** with an EMA that only updates inside a `±threshold`
   corridor, so neither pulses nor undershoots drag the baseline.
2. **Triggers** when the deviation exceeds the threshold, with hysteresis: the
   trigger re-arms only after the signal falls below `threshold/2`.
3. **Rejects pile-up**: once a developed peak (`≥ 2×threshold`) starts falling,
   a renewed rise from the fall minimum marks the event as pile-up and discards it.
4. **Measures amplitude** by fitting a quadratic (parabola) by least squares over
   `2m+1` samples around the maximum and taking the vertex height — a sub-LSB,
   unbiased estimate that maps directly to a channel of the 8192-bin histogram.

Polarity is configurable (`+1` for the Nucleo loopback, `−1` for the target board,
where pulses go below baseline). The hot path is forced to `-O2` even in Debug
builds, because at 2.83 Msps an unoptimized ISR cannot keep up with the half-buffer
rate.

## Self-test subsystem

The test generator doubles as a built-in self-test, driven by the `-tst` command:

| Command | Action |
|---|---|
| `-tst off` | Generator off (default at boot) |
| `-tst spec` | Replays a real Cs-137 amplitude distribution (inverse-CDF sampled) |
| `-tst mono <amp> [t_us]` | Monoenergetic pulses at a fixed amplitude/period |
| `-tst stat` | Reports running mean / σ / FWHM of detected amplitudes |
| `-tst enc` | Automated ENC + linearity sweep (5 amplitudes × 1000 events) |

The `-tst enc` autotest returns one text frame with centroid, σ and FWHM at each
amplitude, plus the fitted slope and INL. A representative hardware run:
`k = 2.0048 ch/code`, `INL max = 0.025 %FS`, `FWHM @ Cs-equivalent = 0.66 %` —
confirming on real silicon that the electronics + DSP contribution is negligible
next to the ≈6.3 % PMT statistics (see [simulations.md](simulations.md)).

> The generator is **off at boot** and only runs when explicitly enabled. On the
> target board the ADC input is the real PMT signal, so the generator must never
> start on its own.

## Build & flash (headless)

```bash
# Build (no IDE needed; the -import path uses backslashes)
"…/stm32cubeidec.exe" --launcher.suppressErrors -nosplash \
  -application org.eclipse.cdt.managedbuilder.core.headlessbuild \
  -data /tmp/cube_hb_ws -import 'C:\gamma-spectrometer\firmware\gamma_stage1' \
  -build gamma_stage1/Debug

# Flash over SWD
STM32_Programmer_CLI.exe -c port=SWD mode=UR reset=HWrst \
  -w ".../Debug/gamma_stage1.elf" -v -rst
```

The newlib-nano `printf` needs the `-u _printf_float` linker flag for the
floating-point self-test reports.

See [`firmware/README.md`](../../firmware/README.md) for the step-by-step setup.
