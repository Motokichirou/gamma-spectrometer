# shproto / AtomSpectra Protocol

<!-- README-I18N:START -->
[Русский](../ru/protocol.md) | **English**
<!-- README-I18N:END -->

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
| `-cal` | Energy calibration in BecqMoni format (see below); a bare `-cal` also serves as the connection check |
| `-cal <i> <hex>` | Write calibration word `i` (0..10), replies `-ok` |
| `-tst …` | Built-in self-test (see [firmware.md](firmware.md)) |
| `-thr <channels>` | DSP threshold in channels (no argument = query); replies `-ok thr <channels>` |
| `-wcal <order> <c0..cN>` | Write the energy calibration in human-readable form (our format), `order = 1..4` |
| `-rcal` | Read the calibration in our format: `CAL: <order> c0 c1 …\r\n<serial>\r\n` |
| `-calclr` | Erase the stored calibration |

> `-thr`/`-wcal`/`-rcal`/`-calclr` are **application-level** commands of our device and
> the `gammapult` service tool. They do not change the shproto frame format. `-cal` is
> BecqMoni-compatible (see below).

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

## BecqMoni energy calibration (`-cal`)

BecqMoni can read and write the energy calibration (channel→keV polynomial) directly
to the device. The protocol was recovered by reversing the real exchange and is
implemented in firmware (`cal_store.c`) — the "Write to device" / "Read from device"
buttons work.

**Calibration model:** five IEEE-754 `double` coefficients `c0..c4`,
`E(ch) = c0 + c1·ch + c2·ch² + c3·ch³ + c4·ch⁴`.

**Write** ("Write to device") — 11 `-cal <i> <hex32>` commands, `i = 0..10`, each
expecting an `-ok` reply:

| Words | Content |
|---|---|
| `0,1` | `c0` as a double: even word = high 32 bits, odd word = low 32 bits (big-endian) |
| `2,3` | `c1` |
| `4,5` | `c2` |
| `6,7` | `c3` |
| `8,9` | `c4` |
| `10` | checksum |

Reconstruct the double as `u64 = (word[2k] << 32) | word[2k+1]`, then reinterpret the
bits as `double`.

**Checksum (word 10):** standard **CRC-32** (polynomial `0x04C11DB7`, reflected
`0xEDB88320`, `init = xorout = 0xFFFFFFFF` — plain zip / `zlib.crc32`), computed over the
**ASCII string** of words `0..9` formatted as `%08X` (uppercase, 8 digits, no separators —
80 characters total).

**Read** ("Read from device") — the same bare `-cal`. The device replies with 11 `%08X`
lines (words 0..10) followed by the serial line:

```
40240000\r\n00000000\r\n3FE00000\r\n…\r\n<CRC32>\r\nGS474-0001\r\n
```

BecqMoni parses the 11 words and verifies the CRC-32. The serial is the second-to-last
line, so the same reply also satisfies the connection check.

> ⚠ If the device holds a calibration with a bad CRC, BecqMoni throws during its
> pre-read and closes the port. The firmware stores the words verbatim (BecqMoni write)
> or computes the CRC itself (`-wcal`), so the checksum is always valid.
