# Simulations & Noise Budget

<!-- README-I18N:START -->
[Русский](../ru/simulations.md) | **English**
<!-- README-I18N:END -->

The analog chain was validated in LTspice before layout. All three simulations are
complete; the source decks and Python analysis scripts live in `simulation/`.

| Sim | Deck | Script | Subject |
|---|---|---|---|
| 1 | `sim1_ad8000.cir` | `analyze_sim1.py` | AD8000 front-end stability & bandwidth |
| 2 | `sim2_shaper.cir` | `analyze_sim2.py` | CR-RC² shaper response & energy calibration |
| 3 | `sim3_noise.cir` | `analyze_sim3.py` | Full electronics noise budget |

## Sim 1 — AD8000 front end

With `Rfb = 1.5 kΩ`, `Cfb = 1 pF`, `Cpar = 1 pF`:

- bandwidth ≈ **99 MHz**, peaking < 0.5 dB.
- **`Cfb = 1 pF` is mandatory** on the divider PCB for a stable, well-damped response.

## Sim 2 — CR-RC² shaper

PGA ×4, with the DC-bias divider giving `V_out_DC = 2.13 V` baseline.
The ballistic-deficit-corrected peaking factor is `PF = 0.202` (the G474 OPAMP
GBW of 13 MHz slightly reduces the ideal-opamp 0.209; the earlier "0.36" was an analytic-calc error).

Resulting energy → channel mapping (of 8192):

| Source | Energy | Channel |
|---|---|---|
| Am-241 | 59.5 keV | 101 |
| Cs-137 | 662 keV | 1138 |
| Co-60 | 1.33 MeV | 2277 |
| Tl-208 | 2.6 MeV | 4470 |
| Ceiling | 3.5 MeV | 6021 |

## Sim 3 — Noise

Total input-referred electronics noise:

- `σ_total = 94.4 µV rms`
- `σ_baseline = 0.31 channel rms`

### Noise budget

| Contribution | FWHM |
|---|---|
| PMT R1307 + NaI(Tl) statistics | 6.3 % |
| HV ripple (typical) | 0.3–0.5 % |
| ADC nonlinearity (12-bit, ENOB ~10.5) | 0.1 % |
| Electronics noise (σ = 94.4 µV rms) | **0.064 %** |
| **Total** | **~6.32 %** |

The electronics noise is **two orders of magnitude below** the PMT statistics, so
the energy resolution is PMT-limited, not electronics-limited. This was later
confirmed on hardware: the firmware ENC self-test measured a Cs-equivalent FWHM of
0.66 % from the digital chain alone (see [firmware.md](firmware.md)).

## Running a simulation

```powershell
& "C:\Program Files\ADI\LTspice\LTspice.exe" -b "C:\gamma-spectrometer\simulation\sim2_shaper.cir"
python simulation/scripts/analyze_sim2.py simulation/sim2_shaper.raw
```
