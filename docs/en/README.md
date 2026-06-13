# Documentation (English)

English documentation set for the NaI(Tl) gamma spectrometer.
For the project overview, see the root [README](../../README.en.md).

| Document | Contents |
|---|---|
| [hardware.md](hardware.md) | Board stack, signal chain, and a summary of all four boards |
| [firmware.md](firmware.md) | STM32G474 firmware: portable core, development stages, pulse DSP, self-test |
| [protocol.md](protocol.md) | shproto / AtomSpectra serial protocol reference |
| [simulations.md](simulations.md) | LTspice simulations (AD8000, CR-RC² shaper, noise) and the noise budget |

> The detailed engineering notes for each board (`docs/adc_mcu_board.md`,
> `docs/divider_board.md`, `docs/hv_board_schematic.md`,
> `docs/usb_power_board_schematic.md`) are maintained in Russian. The documents
> here are a curated English summary for an international audience.
