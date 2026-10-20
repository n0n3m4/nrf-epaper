# E-paper display firmware

A BLE e-paper display: a **Pro Micro nRF52840** (nice!nano v2 pinout, Adafruit
UF2 bootloader) driving a 400×300 monochrome SSD1683 panel — a WeAct 4.2", the
GDEY042T81, which also covers the Waveshare 4.2" V2 — on **Zephyr**, built with
the nRF Connect SDK v3.3.0 at `~/ncs/v3.3.0`. The board advertises a vendor GATT
service continuously; a client connects and pushes a length- and CRC-checked
1bpp or 2bpp frame, optionally zlib-compressed, which the board renders. E-paper
keeps the image with no power.

## Wiring

WeAct / Waveshare 4.2" SSD1683 panel to the Pro Micro header:

| Panel | nRF pin | Pro Micro label |
|---|---|---|
| SCK  | P1.13 | D15 / SCK |
| DIN (MOSI) | P0.10 | D16 / MOSI |
| CS   | P0.17 | D2 |
| DC   | P0.20 | D3 |
| RST  | P0.22 | D4 |
| BUSY | P0.24 | D5 |
| VCC  | VCC pin | switched on by P0.13 at boot |
| GND  | GND | |

These avoid everything the bootloader uses (the LED on P0.15, RST on P0.18)
and leave the board's own UART pins (D0/D1) free. The `VCC` pin on nice!nano
v2 clones is behind a switch on P0.13; the firmware turns it on at boot through
a devicetree regulator, so the panel can hang off that pin. Pins live in
`app.overlay`.

## Build and flash

Needs the nRF Connect SDK v3.3.0 at `~/ncs/v3.3.0` with its toolchain bundle,
as installed by `nrfutil toolchain-manager`.

```sh
./build.sh              # -> epaper.uf2, USB off
./build.sh debug        # with a console on the USB CDC ACM port
./flash.sh              # copies the UF2 onto the bootloader volume
```

`epaper.uf2` is committed: a ready-made image matching the sources, so you can
also just copy it onto the bootloader drive.

To flash, put the board into the bootloader: short RST to GND twice quickly.
The LED starts breathing and a USB drive (`NICENANO` or similar) mounts;
`./flash.sh` copies the image onto it and the board reboots into the new
firmware. The bootloader volume's `INFO_UF2.TXT` says `SoftDevice: not found` on this
board, so the application is linked at 0x1000, straight after the MBR. A
board whose bootloader carries S140 6.x instead needs the `/uf2` variant:
`BOARD=promicro_nrf52840/nrf52840/uf2 ./build.sh` (application at 0x26000).

A board that has never been sent an image shows a "ready" splash; after the
first push the panel keeps whatever it was last given, across resets and
reflashes.

## Pushing an image without Home Assistant

```sh
pip install -r tools/requirements.txt
python tools/epaper_push.py photo.png
python tools/epaper_push.py photo.png --colors 4                   # 4-level greyscale
python tools/epaper_push.py photo.png --dry-run --preview out.png  # no radio
python tools/epaper_push.py photo.png --fit cover --dither none
python tools/epaper_push.py --clear
python tools/epaper_push.py photo.png --corrupt   # prove the CRC check works
```

Host-side checks, no board and no BLE adapter needed:

```sh
./test/run.sh
```

## Power and advertising

The board advertises all the time, slowly, at 1–1.5 s, and never sleeps the
radio. The SoftDevice Controller sleeps the CPU and the crystal between advert
events, so an advert costs only its ~25 µC and the rest is System ON idle at a
few µA: ~25 µA average, and the panel can be connected at any moment. The
interval is `ADV_INTERVAL_*` in `src/config.h`. INFO still carries a
`sleep_seconds` field (`DEEP_SLEEP_SECONDS`) for clients that use it as a hint.

- **USB is off in the release build.** Zephyr's USB console enables the USB
  peripheral at boot, which keeps the 32 MHz crystal running even on battery.
  `./build.sh debug` turns the console back on for the bench; `printk` output
  appears on the CDC ACM port. Opening that port does not reset the board.
- **Radio tops out at +8 dBm.** `CONFIG_BT_CTLR_TX_PWR_PLUS_8` is the ceiling
  and the SoftDevice Controller does apply it (`sdc_default_tx_power_set`).
- A connection holds a session open until the transfer finishes or
  `SESSION_TIMEOUT_MS` elapses, so a client that connects and stalls cannot
  wedge it.

## Protocol

Service `69770000-b271-453a-a64d-cb31cec14bee`, all values little-endian.

| Characteristic | UUID suffix | Properties |
|---|---|---|
| CTRL | `…0001` | write |
| DATA | `…0002` | write |
| STATUS | `…0003` | read, notify |
| INFO | `…0004` | read |

**INFO** (8 bytes) — read this first rather than hardcoding geometry:
`ver u8 | width u16 | height u16 | bpp u8 | sleep_seconds u16`

`bpp` is the deepest mode the firmware accepts (2), not a demand that a client
use it; the depth is chosen per frame.

**CTRL** — first byte is the opcode:

- `0x01` BEGIN, 19 bytes:
  `0x01 | "EPD1" | ver u8 | flags u8 | w u16 | h u16 | len u32 | crc32 u32`.
  `flags` bit 0 inverts the bit convention (1bpp only); bit 1 says the frame is
  4-colour 2bpp rather than 2-colour 1bpp; bit 2 says the bytes on the wire are
  a zlib stream of the frame rather than the frame itself. The flag and `len`
  have to agree — 15000 bytes without it, 30000 with — or BEGIN fails with
  `BAD_LENGTH`. With bit 2 set, `len` is instead the length of the compressed
  stream, which must be shorter than the frame it encodes; the uncompressed
  size is still whatever bit 1 implies, so the header needs no second length.
- `0x02` COMMIT — check the byte count, verify the CRC, render on match.
- `0x03` ABORT — drop the session.
- `0x04` CLEAR — blank the panel to white.

**DATA** — `offset u32 || bytes`. Bounds-checked, so a chunk may be re-sent;
skipping ahead is rejected rather than left for the CRC to discover. Written
with response, which rules out silent drops; ATT MTU is 256, so chunks are up
to 249 bytes.

**STATUS** notify (6 bytes) — `state u8 | error u8 | received u32`.
States `IDLE, RECEIVING, VERIFYING, RENDERING, DONE, ERROR`; errors
`NONE, BAD_MAGIC, BAD_VERSION, BAD_GEOMETRY, BAD_LENGTH, CRC_MISMATCH,
OVERFLOW, NO_SESSION, NO_MEMORY, INCOMPLETE, INFLATE`.

CRC-32 is the reflected zlib/Ethernet variant, so `zlib.crc32` on the client
and the firmware's nibble-table implementation agree byte for byte —
`test/run.sh` asserts it. It always covers the bytes that actually crossed the
air, so on a compressed frame it is checked before the inflate rather than
after: radio corruption still reports `CRC_MISMATCH`, and only a stream that
survives the CRC yet will not inflate to exactly one frame reports `INFLATE`.

**Image format** — row-major, MSB first, rows padded to a byte. 1bpp at 400×300
is exactly 50 × 300 = 15000 bytes, and a set bit is black ink unless
`FLAG_INVERTED` is set. 2bpp packs four pixels per byte, 100 × 300 = 30000
bytes, each pair a level from 0 (white) to 3 (black) — the same direction, more
bits meaning more ink. Only full-panel frames are accepted; the client
letterboxes to fit.

## Compression

The client deflates the frame, keeps the result if it came out smaller, and
sets `FLAG_DEFLATE` to say so. Nothing negotiates: it is decided per frame from
the two byte counts, and `--no-compress` forces the raw path for comparison.

A frame of rendered text is mostly runs of white: a page of Markdown from the
Home Assistant integration deflates to a sixth or less. A dithered photo is
noise and comes out no smaller, which is exactly when the client leaves the
flag clear.

Inflate is miniz's `tinfl` (miniz 3.1.2, vendored in `src/miniz/`, ~4 kB of
flash). `src/image_receiver.cpp` calls the low-level `tinfl_decompress()`
rather than the one-line `tinfl_decompress_mem_to_mem()`, because that helper
puts an ~11 kB `tinfl_decompressor` on the stack. The frame buffer is larger
than any back-reference can reach, so no 32 kB dictionary is needed either.
The host test build uses plain zlib instead — the `#ifdef __ZEPHYR__` is why
`test/run.sh` links `-lz`.

## Grayscale

This panel really does drive the SSD1683's 4-level greyscale waveform, so a
client can send either 2 colours (1bpp, 15000 bytes) or 4 (2bpp, 30000 bytes).

Levels run 0 white, 1 light, 2 dark, 3 black. Measured off the panel, ink and
background included, they are **210 / 180 / 100 / 40**. They are
nowhere near evenly spaced: level 1 sits much closer to white than level 2 does
to black, so the client dithers against those measured luminances rather than a
linear ramp. It is a per-panel, per-film calibration — retune it with
`--gray-levels` rather than by editing the dithering.

Because those ends are 40 and 210 rather than 0 and 255, everything darker or
lighter than the panel can reproduce collapses into the two end levels — on a
0→255 ramp they come out the widest bands. `--normalize` rescales the source
onto the real black and white first, reading a pixel as coverage between them
instead of as an absolute luminance, which spends that clipped range on tone.
It is off by default and applies to photographs: it lightens solid ink, so on
text it erodes stems and hazes the paper. (The Home Assistant renderer does the
same mapping for antialiased text, where a pixel *is* coverage.)

The recipe is Waveshare's, from their `epd4in2_V2` demo:

- upload the 233-byte greyscale waveform with `0x32`, plus `0x3F`, `0x03`,
  `0x04` and `0x2C`, which replaces the panel's built-in monochrome waveform;
- split the 2bpp frame into two 1bpp planes — the low bit of each pixel's level
  and the high bit — and write them to `0x24` and `0x26`. The waveform then
  reads the bit pair as white / gray1 / gray2 / black;
- refresh with `0x22 0xCF`, `0x20`.

**Which plane takes which bit is the trap.** Waveshare's Arduino demo and their
Raspberry Pi demo map the two middle codes to the planes in opposite
directions, and only the Raspberry Pi one gives levels that darken
monotonically here — the other paints light grey where dark belongs. This is
settled by measurement, not by reading; `pack_plane()` in `src/epd.c` says so.

A hardcoded greyscale waveform has no temperature compensation, so greyscale
quality drifts with ambient temperature in a way the panel's OTP waveform does
not. Partial refresh is unavailable in 4-grey; it is a full refresh.

## Layout

| Path | |
|---|---|
| `app.overlay` | pins, SPI child node, panel-power regulator, disabled peripherals |
| `prj.conf`, `debug.conf` | Kconfig; release has no USB, debug adds the CDC console |
| `dts/bindings/` | the binding for the SSD1683 SPI child node |
| `src/config.h` | UUIDs, geometry, timings, protocol constants |
| `src/main.cpp` | main loop, session watchdog, painted flag; wraps `ImageReceiver` |
| `src/image_receiver.*` | BEGIN/DATA/COMMIT state machine and validation |
| `src/crc32.*` | zlib-compatible CRC-32 |
| `src/ble.c` | GATT service and advertising |
| `src/epd.c` | SSD1683 over SPI: 1bpp, 2bpp, clear, splash, sleep |
| `src/epd_gray_lut.h` | the greyscale waveform and plane packing |
| `src/font5x7.h` | Adafruit GFX classic 5x7 font for the splash |
| `src/miniz/` | vendored miniz 3.1.2, tinfl only |
| `tools/epaper_push.py` | client: converts, dithers, uploads |
| `test/` | host-side protocol tests |

GATT callbacks run on the Bluetooth RX thread and must return promptly, so they
do no more than hand the bytes to the receiver and give a semaphore; the CRC
pass, the inflate and the SPI transfer all happen in `main()`.

## Security

The GATT writes are open — anyone in range can redraw the panel. Add
encryption permissions and passkey pairing in `src/ble.c` if that matters for
your deployment.
