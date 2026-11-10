# shproto / AtomSpectra Protocol

<!-- README-I18N:START -->
[Русский](../ru/protocol.md) | **English**
<!-- README-I18N:END -->

The device communicates via the serial protocol **shproto**, compatible with
[BecqMoni](https://github.com/Am6er/BecqMoni) and [nanopro](https://github.com/Am6er/nanopro).
It enumerates as a virtual COM port (FT231X) at **600 000 bit/s, 8N1**.

## Frame Format

```
0xFF  0xFE  [CMD][PAYLOAD …][CRC_LSB][CRC_MSB]  0xA5
 │     │     └──────────── body with stuffing ──┘   │
 │     └ START                                      └ FINISH
 └ keep-alive / intro byte
```

- **`0xFE`** — start of frame.
- **`0xA5`** — end of frame.
- **`0xFD`** — escape byte.
- **Byte stuffing:** any `0xFE`, `0xFD`, or `0xA5` inside the body is transmitted as
  `0xFD` followed by the bitwise NOT (`~byte`) of the original.
- **CRC16-MODBUS** (`init = 0xFFFF`, `poly = 0xA001`) is calculated over the *unstuffed*
  `CMD + PAYLOAD`, then appended little-endian and itself stuffed.

## Commands (Device → Host)

| CMD | Meaning | Payload |
|---|---|---|
| `0x01` | Spectrum chunk | `[offset u16 LE][channel u32 LE × N]` (each value masked with `0x7FFFFFF`) |
| `0x02` | Pulse shape / raw samples | `[offset u16][total u16][sample u16 × N]` — response to `-osc` |
| `0x03` | Text | UTF-8/ASCII text (command responses, reports) |
| `0x04` | Status | `[elapsed u32][cpu_load u16][cps u32][invalid u32]` |

Notes:

- A full spectrum of 8192 channels is sent as a series of `0x01` chunks (512 channels each).
- **The status frame `0x04` triggers a spectrum redraw** in BecqMoni — send it after the chunks.
- The `elapsed` field is in **seconds** (BecqMoni interprets it via `TimeSpan.FromSeconds`),
  not milliseconds.

## Host → Device

- Commands are sent as text frames `0x03`, e.g., `-sta`, `-sto`, `-rst`, `-inf`, `-cal`, `-gst`.
- The host sends a bare **keep-alive `0xFF`** approximately once per second.

| Command | Effect |
|---|---|
| `-sta` | Start acquisition · response `-ok\r\n` |
| `-sto` | Stop acquisition · response `-ok\r\n` |
| `-rst` | Reset spectrum · response `-ok\r\n` |
| `-stt` | Status query (AtomSpectra): `-ok collecting\r\n` if acquiring, otherwise `-ok\r\n` |
| `-mode <n>` | Device mode (AtomSpectra sends `-mode 0` on connection) · response `-ok\r\n` |
| `-inf` | Device report (contains `VERSION <n>` for AtomSpectra) |
| `-cal` | Energy calibration, 40 registers in BecqMoni/AtomSpectra format (see below); bare `-cal` is also a link check |
| `-cal <i> <hex>` | Write calibration word `i` (0..10), response `-ok\r\n` |
| `-sho` | AtomSpectra compatibility · response `-ok collecting\r\n` |
| `-thr [codes]` | Detector threshold in **ADC codes** (0 — auto 5σ; no argument — query); response `-ok thr <N>\r\n` |
| `-hv`, `-gst`, `-diag`, `-osc`, `-dac`, `-opa`, `-tplw` … | Device service commands — full list in [firmware.md](firmware.md#commands) |
| `!boot` | Turn off HV and enter system bootloader (firmware update) |

> **Command acks are strictly `-ok\r\n`** (or `-err\r\n`): the AtomSpectra app checks
> the response EXACTLY, BecqMoni truncates at the first `\r` — the format is compatible with both.

> `-thr` and service commands are at the **application level** of our device. They do not
> change the shproto frame format. `-cal` is compatible with BecqMoni (see below). Unknown command — `-err unknown\r\n`.

## BecqMoni Connection Handshake

Two BecqMoni specifics that the firmware must satisfy:

- **Connection check:** BecqMoni sends `-cal` and waits for exactly one text frame whose
  content, when split by `\r\n`, yields more than two lines; the second-to-last line is taken
  as the serial number, and the device turns green ("Connected").
- **Start:** on connection, BecqMoni issues `-sto` → `-rst` → `-sta` and waits for `-ok`
  for each.
- Arbitrary commands (e.g., `-gst` or `-hv 750`) can be entered in the command line on the
  device tab in BecqMoni.

Reference host implementation — `firmware/gamma_fw/tools/gfw.py`.

## BecqMoni Energy Calibration (`-cal`)

BecqMoni can read and write energy calibration (channel→keV polynomial) directly to
the device. The protocol was reconstructed by reversing real traffic and implemented in the firmware
(`becq_cal.c`) — the "Write to device" / "Read from device" buttons work.

**Calibration model:** five coefficients `c0..c4` of type IEEE-754 double,
`E(ch) = c0 + c1·ch + c2·ch² + c3·ch³ + c4·ch⁴`.

**Write** ("Write to device") — 11 commands `-cal <i> <hex32>`, `i = 0..10`,
each waiting for response `-ok`:

| Words | Content |
|---|---|
| `0,1` | `c0` as double: even word — upper 32 bits, odd — lower (big-endian) |
| `2,3` | `c1` |
| `4,5` | `c2` |
| `6,7` | `c3` |
| `8,9` | `c4` |
| `10` | checksum |

Double reconstruction: `u64 = (word[2k] << 32) | word[2k+1]`, then bits as `double`.

**Checksum (word 10):** standard **CRC-32** (polynomial `0x04C11DB7`,
reflected `0xEDB88320`, `init = xorout = 0xFFFFFFFF` — standard zip / `zlib.crc32`),
calculated over the **ASCII string** of words `0..9`, formatted as `%08X`
(uppercase, 8 digits, no separators — total 80 characters).

**Read** ("Read from device") — the same bare command `-cal`. The device responds with
**40 registers** (like a real AtomSpectra device), one line per register via `\r\n`:

| Registers | Content |
|---|---|
| `0..9` | 5 coefficients (hi,lo pairs) |
| `10` | CRC-32 |
| `11..38` | other registers (fillers `00000000`, not used by hosts) |
| `39` | device identifier (serial number) |

```
40240000\r\n00000000\r\n3FE00000\r\n…\r\n<CRC32>\r\n00000000\r\n…(×28)…\r\n04740001\r\n
```

Satisfies two consumers at once: **BecqMoni** (link check — second-to-last
line = serial number; calibration read — words 0..10), **AtomSpectra** (startup
metadata — exactly 40 tokens, register 39 = device; calibration read — words 0..10).

> ⚠ If the device contains a calibration with an incorrect CRC, BecqMoni throws
> an exception on pre-read and closes the port. The firmware accepts words only with the correct checksum and stores them
> verbatim, so the checksum in the device is always correct.