# shproto / AtomSpectra Protocol

The instrument speaks the **shproto** serial protocol, compatible with
[BecqMoni](https://github.com/Am6er/BecqMoni) and [nanopro](https://github.com/Am6er/nanopro).
It enumerates as a virtual COM port (FT231X) at **600 000 bps, 8N1**.

## Frame format

```
0xFF  0xFE  [CMD][PAYLOAD …][CRC_LSB][CRC_MSB]  0xA5
 │     │     └──────────── stuffed body ───────┘   │
 │     └ START                                      └ FINISH
 └ keep-alive / lead-in byte
```

- **`0xFE`** — start of frame.
- **`0xA5`** — end of frame.
- **`0xFD`** — escape byte.
- **Byte stuffing:** any `0xFE`, `0xFD`, or `0xA5` occurring inside the body is sent
  as `0xFD` followed by the bitwise-NOT (`~byte`) of the original.
- **CRC16-MODBUS** (`init = 0xFFFF`, `poly = 0xA001`) is computed over the
  *unstuffed* `CMD + PAYLOAD`, then appended little-endian and itself stuffed.

## Commands (device → host)

| CMD | Meaning | Payload |
|---|---|---|
| `0x01` | Spectrum chunk | `[offset u16 LE][channel u32 LE × N]` (each value masked with `0x7FFFFFF`) |
| `0x03` | Text | UTF-8/ASCII text (command replies, reports) |
| `0x04` | Status | `[elapsed u32][cpu_load u16][cps u32][invalid u32]` |

Notes:

- The full 8192-channel spectrum is sent as a series of `0x01` chunks (512 channels each).
- A **`0x04` status frame triggers the spectrum redraw** in BecqMoni — send it after the chunks.
- The `elapsed` field is in **seconds** (BecqMoni interprets it via `TimeSpan.FromSeconds`),
  not milliseconds.

## Host → device

- Commands are sent as `0x03` text frames, e.g. `-sta`, `-sto`, `-rst`, `-inf`, `-cal`, `-tst …`.
- The host sends a bare **`0xFF` keep-alive** roughly once per second.

| Command | Effect |
|---|---|
| `-sta` | Start acquisition |
| `-sto` | Stop acquisition |
| `-rst` | Reset the spectrum |
| `-inf` | Report device info |
| `-cal` | Report calibration / serial (used for the connection check) |
| `-tst …` | Built-in self-test (see [firmware.md](firmware.md)) |

## BecqMoni connection handshake

A couple of BecqMoni behaviors that the firmware must satisfy:

- **Connection check:** BecqMoni sends `-cal` and expects exactly one text frame whose
  content, split on `\r\n`, has more than two lines; the second-to-last line is taken as
  the serial number and the device turns green ("Connected").
- **Startup:** on connect BecqMoni issues `-sto` → `-rst` → `-sta` and expects an `-ok`
  reply to each.
- Arbitrary commands (including `-tst …`) can be typed into the command line on BecqMoni's
  device tab.

A reference host implementation lives in `firmware/tools/becqmoni_sim.py`.
