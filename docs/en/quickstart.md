# Quick Start

<!-- README-I18N:START -->
[Русский](../ru/quickstart.md) | **English**
<!-- README-I18N:END -->

From an assembled instrument to the first spectrum in BecqMoni, in about half an hour.

## What you need

- An assembled instrument: board stack, divider with PMT, crystal in its housing.
- A USB-C cable **with data lines** (a charge-only cable will not work).
- A Windows 10/11 PC.
- A **Cs-137** check source (662 keV line). For an accurate scale, more lines help: background K-40 (1461 keV), Am-241, Co-60, etc.
- The **gammapult** service tool: `gammapult-vX.Y.Z-win64.zip` from the [Releases](https://github.com/Motokichirou/gamma-spectrometer/releases) page. No installation, it is a single executable.
- [BecqMoni](https://github.com/Am6er/BecqMoni) for spectra.

> ⚠️ **High voltage.** Up to −1200 V inside the instrument. Open the housing only with HV off and the cable unplugged.
> In gammapult, the red **"Turn off HV"** button in the top bar switches HV off at any time.
> The firmware turns HV off by itself before jumping to the bootloader.

## 1. Connect

1. Plug the instrument in. Device Manager → "Ports (COM & LPT)" shows **USB Serial Port (COMx)** (FTDI FT231X bridge).
   Windows usually installs the driver itself; if no port appears, install the VCP driver from FTDI.
2. Unpack the archive and run **`gammapult.exe`**.
3. Choose the port and press **"Connect"**.

Only one program can hold the port: if BecqMoni is open, disconnect it there first.

## 2. Setup wizard

A new instrument shows "The device isn't set up yet" on the home screen. Press **"Start setup"**. The wizard walks
through nine steps and tells you what to do on each:

| Step | What happens | What you do |
|---|---|---|
| 1. Firmware | Writes the built-in firmware through the STM32 system bootloader | Nothing (bare MCU — see below) |
| 2. Electronics check | Reference, baseline noise, Pt1000, linearity self-test, HV off | — |
| 3. HV monitor zero | Calibrates the zero of the HV measurement | — |
| 4. HV tuning | Raises HV until the Cs-137 peak sits on the target channel | Cs-137 source against the detector face |
| 5. Pulse shape | Learns the shape template and the tail table for pile-up peeling | Source in place |
| 6. HV tuning again | The peak shifts slightly with the new shape | Source in place |
| 7. Resolution | Acquires a spectrum and fits the photopeak FWHM | Source in place |
| 8. Energy scale | Rough one-peak scale: E = k · channel | — (accurate one in BecqMoni, step 3 below) |
| 9. Save and report | Writes settings to flash, offers HV autostart, saves a report | Decide on autostart |

You can stop the wizard and continue later: finished steps stay marked. **"Home"** returns to the home screen.

**Bare MCU without firmware:** tie BOOT0 (pin 45 of LQFP-48) to +3.3 V through 10 kΩ and replug USB. The MCU starts in
the bootloader and the wizard flashes it. Return BOOT0 to GND afterwards. An instrument that already has firmware enters
the bootloader by itself, no jumper needed.

The commissioning report is saved next to the tool: `gammapult_ввод_<serial>_<date>.txt`.

## 3. Working in BecqMoni

1. In gammapult press **"Hand the port to BecqMoni"** on the home screen (or **"Disconnect"** in the top bar).
2. In BecqMoni add an AtomSpectra device (shproto protocol), choose the same COM port, speed **600 000**.
   The device turns green and acquisition starts.
3. **Energy scale.** Acquire a spectrum with several known lines, calibrate on them in BecqMoni and press
   **"Write to device"**. The calibration is stored in the instrument and read back with "Read from device".

Reference figures on the standard detector (NaI 63×63 + R1307): Cs-137 near channel 1190, resolution 6.7–6.9 % FWHM,
range about 20 keV to 3.8 MeV, up to ~30 000 cps.

## What else the tool does

- **HV autostart** (step 9 or the "High voltage" page): a closed instrument switches HV on by itself ~5 s after power-up
  if the reference and the monitor are fine. Handy when you only use BecqMoni.
- **Standalone logging**: the logger writes spectra and telemetry to the instrument's flash, gammapult downloads the
  journal later (home screen → tasks, or advanced mode → "Logger").
- **Temperature compensation**: the peak drifts with crystal temperature. "Temperature compensation" page in advanced
  mode; needs a 5–10 °C warm-up or cool-down.
- **Firmware update**: a new gammapult release carries new firmware; update from home screen → tasks.
- **Save settings**: anything changed by hand lives in RAM until you press **"Save"** (the button is yellow while
  something is unsaved).

## Troubleshooting

| Symptom | What to do |
|---|---|
| No port in the list | Another cable (with data), FTDI VCP driver, "Refresh list" |
| "The port is in use by another program" | Disconnect in BecqMoni or close it |
| "The device isn't responding" | Wrong port selected; replug the cable |
| Peak moved after transport or heating | Repeat "HV tuning" or set up temperature compensation |
| Everything reset after power-off | "Save settings" was not pressed |
| Lots of counts at the very bottom of the spectrum | Raise the threshold (advanced mode → "Threshold and detector") |

More: [PC software](../../host/README.md) (Russian) · [firmware and commands](firmware.md) · [protocol](protocol.md) ·
[hardware](hardware.md).
