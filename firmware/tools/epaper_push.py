#!/usr/bin/env python3
"""Push an image to the BLE e-paper display.

The board advertises continuously, but only every 1-1.5 s, so the scan
timeout needs some headroom.

    python firmware/tools/epaper_push.py photo.png
    python firmware/tools/epaper_push.py photo.png --colors 4
    python firmware/tools/epaper_push.py photo.png --dry-run --preview out.png
    python firmware/tools/epaper_push.py --clear
"""

from __future__ import annotations

import argparse
import asyncio
import struct
import sys
import time
import zlib

from PIL import Image, ImageOps

# Vendor service, matching src/config.h.
SVC_UUID = "69770000-b271-453a-a64d-cb31cec14bee"
CHR_CTRL = "69770001-b271-453a-a64d-cb31cec14bee"
CHR_DATA = "69770002-b271-453a-a64d-cb31cec14bee"
CHR_STATUS = "69770003-b271-453a-a64d-cb31cec14bee"
CHR_INFO = "69770004-b271-453a-a64d-cb31cec14bee"

PROTOCOL_VERSION = 1
MAGIC = b"EPD1"

OP_BEGIN, OP_COMMIT, OP_ABORT, OP_CLEAR = 1, 2, 3, 4
FLAG_INVERTED = 1 << 0
FLAG_GRAY4 = 1 << 1
FLAG_DEFLATE = 1 << 2

# Luminance of each 2bpp wire level, level 0 first. Measured off the panel with
# even the extremes are paper grey and grey ink, not 255 and 0. The greys are nowhere near
# evenly spaced, which is exactly why dithering has to work against these
# numbers rather than a linear ramp. Film-specific -- retune with --gray-levels
# rather than editing dithering code.
GRAY_LEVELS = (210, 180, 100, 40)

STATE_NAMES = {
    0: "IDLE",
    1: "RECEIVING",
    2: "VERIFYING",
    3: "RENDERING",
    4: "DONE",
    5: "ERROR",
}
ERROR_NAMES = {
    0: "NONE",
    1: "BAD_MAGIC",
    2: "BAD_VERSION",
    3: "BAD_GEOMETRY",
    4: "BAD_LENGTH",
    5: "CRC_MISMATCH",
    6: "OVERFLOW",
    7: "NO_SESSION",
    8: "NO_MEMORY",
    9: "INCOMPLETE",
}
STATE_DONE, STATE_ERROR = 4, 5

# Largest ATT payload the firmware will accept (MTU 256 minus the 3-byte
# write-request header), and the 4-byte offset each data chunk carries.
MAX_ATT_PAYLOAD = 253
DATA_HEADER_LEN = 4


# --------------------------------------------------------------------------
# Image conversion
# --------------------------------------------------------------------------


def _load_and_fit(path, size, fit, rotate):
    """Return a greyscale image of exactly `size`, letterboxed on white."""
    src = Image.open(path)
    # Flatten transparency onto white so alpha does not dither to noise.
    if src.mode in ("RGBA", "LA") or (src.mode == "P" and "transparency" in src.info):
        flat = Image.new("RGB", src.size, "white")
        flat.paste(src.convert("RGBA"), mask=src.convert("RGBA").split()[-1])
        src = flat
    src = src.convert("L")

    if rotate:
        src = src.rotate(rotate, expand=True, fillcolor=255)

    if fit == "stretch":
        src = src.resize(size, Image.LANCZOS)
    elif fit == "cover":
        src = ImageOps.fit(src, size, Image.LANCZOS)
    else:  # contain
        src = ImageOps.contain(src, size, Image.LANCZOS)
        canvas = Image.new("L", size, 255)
        canvas.paste(src, ((size[0] - src.width) // 2, (size[1] - src.height) // 2))
        src = canvas

    return src


def convert_image(path, size, fit, rotate, dither, threshold):
    """Return a mode-"1" image of exactly `size`, letterboxed on white."""
    src = _load_and_fit(path, size, fit, rotate)
    if dither == "none":
        return src.point(lambda v: 255 if v >= threshold else 0, mode="1")
    return src.convert("1")  # Floyd-Steinberg


def convert_gray4(path, size, fit, rotate, dither, levels, normalize=False):
    """Return one wire level (0..3) per pixel, as bytes, row-major.

    Quantising against the panel's real luminances is the whole point: error
    diffusion towards an evenly spaced ramp would put far too much ink in the
    lighter half of the image.

    `normalize` first rescales 0..255 onto the panel's real black and white,
    so a source value is read as a coverage fraction between the darkest and
    lightest ink this film can make rather than as an absolute luminance. It
    stops the unreachable ends -- everything below `levels[3]` and above
    `levels[0]` -- collapsing into two fat bands, at the cost of pulling the
    extremes off the rails: pure black is no longer guaranteed to stay pure
    black. Right for photographs, wrong for text, hence off by default.
    """
    src = _load_and_fit(path, size, fit, rotate)

    if normalize:
        black, white = levels[3], levels[0]
        src = src.point([round(black + v * (white - black) / 255) for v in range(256)])

    if dither == "none":
        # Nearest level per pixel, by the midpoints between adjacent levels.
        table = [min(range(4), key=lambda i: abs(v - levels[i])) for v in range(256)]
        return src.point(table, mode="L").tobytes()

    # Pillow needs a full 256-entry palette; repeating the four levels means a
    # duplicate entry can never win with the wrong luminance, and index % 4 is
    # always the level that entry stands for.
    palette = Image.new("P", (1, 1))
    palette.putpalette([c for _ in range(64) for v in levels for c in (v, v, v)])
    quantised = src.convert("RGB").quantize(
        palette=palette, dither=Image.Dither.FLOYDSTEINBERG
    )
    return bytes(b % 4 for b in quantised.tobytes())


def pack_1bpp(image, invert=False):
    """Pack a mode-"1" image MSB-first, one set bit per black pixel.

    Pillow packs mode "1" with a set bit meaning *white*, which is the
    opposite of the wire format, hence the complement.
    """
    raw = image.tobytes()
    if invert:
        return raw
    return bytes(b ^ 0xFF for b in raw)


def unpack_1bpp(packed, size, invert=False):
    """Inverse of pack_1bpp, for verifying a round trip."""
    raw = packed if invert else bytes(b ^ 0xFF for b in packed)
    return Image.frombytes("1", size, raw)


def pack_2bpp(levels):
    """Pack one wire level per pixel into 4 pixels per byte, MSB first."""
    if len(levels) % 4:
        raise ValueError(f"{len(levels)} pixels is not a whole number of bytes")
    return bytes(
        (levels[i] << 6) | (levels[i + 1] << 4) | (levels[i + 2] << 2) | levels[i + 3]
        for i in range(0, len(levels), 4)
    )


def unpack_2bpp(packed, size, levels=GRAY_LEVELS):
    """Inverse of pack_2bpp, painted with the measured luminances for preview."""
    return Image.frombytes(
        "L", size, bytes(levels[(b >> shift) & 3] for b in packed for shift in (6, 4, 2, 0))
    )


# --------------------------------------------------------------------------
# BLE transfer
# --------------------------------------------------------------------------


class StatusTracker:
    """Latest STATUS notification, plus an event for terminal states."""

    def __init__(self):
        self.state = None
        self.error = 0
        self.received = 0
        self.terminal = asyncio.Event()
        self.updated = asyncio.Event()

    def handle(self, _sender, data):
        if len(data) < 6:
            return
        self.state, self.error, self.received = struct.unpack("<BBI", data[:6])
        self.updated.set()
        if self.state in (STATE_DONE, STATE_ERROR):
            self.terminal.set()

    async def wait_update(self, timeout=2.0):
        """Give an in-flight notification a chance to land before we act on it."""
        try:
            await asyncio.wait_for(self.updated.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass

    def describe(self):
        return (
            f"state={STATE_NAMES.get(self.state, self.state)} "
            f"err={ERROR_NAMES.get(self.error, self.error)} "
            f"received={self.received}"
        )


async def find_device(name, address, scan_timeout):
    from bleak import BleakScanner

    if address:
        print(f"looking for {address} (up to {scan_timeout:.0f}s)...", file=sys.stderr)
        return await BleakScanner.find_device_by_address(address, timeout=scan_timeout)

    print(f"scanning for {name!r} (up to {scan_timeout:.0f}s)...", file=sys.stderr)

    def match(device, adv):
        if name and (adv.local_name == name or device.name == name):
            return True
        return SVC_UUID.lower() in [u.lower() for u in adv.service_uuids]

    return await BleakScanner.find_device_by_filter(match, timeout=scan_timeout)


async def connect_with_retry(name, address, scan_timeout):
    """Scan once, then hammer connect() until the deadline.

    Each connect() runs a full-duty initiator scan for the target, but the
    kernel aborts it after ~20s. Chaining attempts back-to-back keeps the
    radio listening until one of them catches an advert.
    """
    from bleak import BleakClient

    deadline = time.monotonic() + scan_timeout
    device = await find_device(name, address, scan_timeout)
    if device is None:
        raise SystemExit("could not connect: device not found")
    print(f"found {device.address}, connecting...", file=sys.stderr)
    client = BleakClient(device)
    last_error = "timed out"
    while time.monotonic() < deadline:
        started = time.monotonic()
        try:
            await client.connect()
            return client
        except Exception as exc:
            last_error = str(exc)
            print(f"connect failed ({exc}), retrying...", file=sys.stderr)
            if time.monotonic() - started < 1.0:
                await asyncio.sleep(1.0)  # adapter error, not a scan timeout
    raise SystemExit(f"could not connect: {last_error}")


async def read_info(client):
    raw = await client.read_gatt_char(CHR_INFO)
    if len(raw) < 8:
        raise SystemExit(f"INFO characteristic too short: {len(raw)} bytes")
    version, width, height, bpp, sleep_s = struct.unpack("<BHHBH", raw[:8])
    if version != PROTOCOL_VERSION:
        raise SystemExit(
            f"device speaks protocol v{version}, this client speaks v{PROTOCOL_VERSION}"
        )
    if bpp not in (1, 2):
        raise SystemExit(f"device reports {bpp} bpp, which this client cannot pack")
    return width, height, bpp, sleep_s


def build_begin(width, height, payload, crc, inverted, gray4=False, deflate=False):
    flags = (
        (FLAG_INVERTED if inverted else 0)
        | (FLAG_GRAY4 if gray4 else 0)
        | (FLAG_DEFLATE if deflate else 0)
    )
    return struct.pack(
        "<B4sBBHHII",
        OP_BEGIN,
        MAGIC,
        PROTOCOL_VERSION,
        flags,
        width,
        height,
        len(payload),
        crc,
    )


async def send_image(
    client, status, width, height, payload, chunk_size, inverted, corrupt, gray4=False, compress=True
):
    # Rendered text packs down to about a sixth of a frame, which is a sixth of
    # the chunks and of the awake burst that dominates the power budget. A
    # dithered photo is noise and comes out no smaller, so the flag is decided
    # per frame rather than configured: send whichever is shorter.
    raw = len(payload)
    deflate = False
    if compress:
        squeezed = zlib.compress(payload, 9)
        if len(squeezed) < raw:
            payload, deflate = squeezed, True

    crc = zlib.crc32(payload) & 0xFFFFFFFF
    how = f" (deflate, {100 * len(payload) // raw}% of {raw})" if deflate else ""
    print(
        f"{len(payload)} bytes{how}, crc32=0x{crc:08x}, chunk={chunk_size}",
        file=sys.stderr,
    )

    status.updated.clear()
    await client.write_gatt_char(
        CHR_CTRL,
        build_begin(width, height, payload, crc, inverted, gray4, deflate),
        response=True,
    )
    # The device acks the header with a status notification; if it rejected the
    # header there is no point streaming 15 kB at it.
    await status.wait_update()
    if status.state == STATE_ERROR:
        return status

    total = len(payload)
    started = time.monotonic()
    for offset in range(0, total, chunk_size):
        piece = payload[offset : offset + chunk_size]
        if corrupt and offset == 0:
            # Debug aid: flip one bit so the device's CRC check has to catch it.
            piece = bytes([piece[0] ^ 0x01]) + piece[1:]
        await client.write_gatt_char(CHR_DATA, struct.pack("<I", offset) + piece, response=True)
        if status.state == STATE_ERROR:
            print(file=sys.stderr)
            return status
        done = min(offset + len(piece), total)
        print(f"\r  uploading {done}/{total} ({100 * done // total}%)", end="", file=sys.stderr)
    elapsed = time.monotonic() - started
    print(f"\r  uploaded {total}/{total} in {elapsed:.1f}s", file=sys.stderr)

    status.terminal.clear()
    await client.write_gatt_char(CHR_CTRL, bytes([OP_COMMIT]), response=True)
    return status


async def await_terminal(status, timeout):
    """Wait for DONE/ERROR. A full e-paper refresh takes several seconds."""
    try:
        await asyncio.wait_for(status.terminal.wait(), timeout=timeout)
    except asyncio.TimeoutError:
        print(f"timed out waiting for the device ({status.describe()})", file=sys.stderr)
        return False
    return status.state == STATE_DONE


async def run(args, payload, width, height):
    client = await connect_with_retry(args.name, args.address, args.scan_timeout)
    try:
        info_width, info_height, info_bpp, sleep_s = await read_info(client)
        print(
            f"panel {info_width}x{info_height}, sleeps {sleep_s}s between adverts",
            file=sys.stderr,
        )
        if payload is not None and (info_width, info_height) != (width, height):
            raise SystemExit(
                f"image was built for {width}x{height} but the panel is "
                f"{info_width}x{info_height}; rerun without --width/--height"
            )
        if args.colors == 4 and info_bpp < 2:
            # Not fatal: firmware predating the 2 in INFO still accepts 2bpp
            # frames, so the only real answer comes from whether it takes BEGIN.
            print(
                "note: device advertises 1 bpp; sending 4 colours anyway",
                file=sys.stderr,
            )

        status = StatusTracker()
        await client.start_notify(CHR_STATUS, status.handle)

        if args.clear:
            status.terminal.clear()
            await client.write_gatt_char(CHR_CTRL, bytes([OP_CLEAR]), response=True)
        else:
            chunk_size = args.chunk or default_chunk_size(client)
            await send_image(
                client,
                status,
                width,
                height,
                payload,
                chunk_size,
                args.invert,
                args.corrupt,
                args.colors == 4,
                not args.no_compress,
            )

        ok = await await_terminal(status, args.render_timeout)
        print(status.describe(), file=sys.stderr)
        return 0 if ok else 1
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass  # the board may already have dropped the link


def default_chunk_size(client):
    # Not every backend exposes the negotiated MTU; fall back to the 23-byte
    # BLE default, which is slow but always works.
    try:
        mtu = client.mtu_size or 23
    except Exception:
        mtu = 23
    payload = min(mtu - 3, MAX_ATT_PAYLOAD) - DATA_HEADER_LEN
    return max(16, payload)


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("image", nargs="?", help="image file to display (any format Pillow reads)")
    parser.add_argument("--name", default="EPaper-BLE", help="advertised device name")
    parser.add_argument("--address", help="connect straight to this BLE address")
    parser.add_argument(
        "--scan-timeout",
        type=float,
        default=90.0,
        help="total seconds to spend looking for the device; must exceed one advert interval",
    )
    parser.add_argument(
        "--render-timeout",
        type=float,
        default=60.0,
        help="seconds to wait for the panel refresh to finish",
    )
    parser.add_argument("--width", type=int, default=400, help="panel width for --dry-run")
    parser.add_argument("--height", type=int, default=300, help="panel height for --dry-run")
    parser.add_argument("--fit", choices=("contain", "cover", "stretch"), default="contain")
    parser.add_argument("--rotate", type=int, choices=(0, 90, 180, 270), default=0)
    parser.add_argument(
        "--colors",
        type=int,
        choices=(2, 4),
        default=2,
        help="2 for black and white (15000 bytes), 4 for greyscale (30000 bytes)",
    )
    parser.add_argument(
        "--gray-levels",
        default=",".join(str(v) for v in GRAY_LEVELS),
        help="luminance of each 4-colour level, lightest first (default: measured)",
    )
    parser.add_argument(
        "--normalize",
        action="store_true",
        help="scale 0..255 onto the panel's real black and white before "
        "quantising, instead of clamping the unreachable ends (4 colours only)",
    )
    parser.add_argument("--dither", choices=("floyd", "none"), default="floyd")
    parser.add_argument("--threshold", type=int, default=128, help="cutoff for --dither none")
    parser.add_argument(
        "--invert",
        action="store_true",
        help="send with the inverted bit convention (a set bit means white)",
    )
    parser.add_argument("--chunk", type=int, help="bytes of image data per write (default: from MTU)")
    parser.add_argument("--clear", action="store_true", help="blank the panel instead of sending an image")
    parser.add_argument("--preview", help="save what the panel will show as a PNG")
    parser.add_argument("--dry-run", action="store_true", help="convert only, no radio")
    parser.add_argument(
        "--no-compress",
        action="store_true",
        help="send the raw frame even when it would deflate smaller",
    )
    parser.add_argument(
        "--corrupt",
        action="store_true",
        help="flip one bit before sending, to exercise the device's CRC check",
    )
    args = parser.parse_args(argv)

    if not args.clear and not args.image:
        parser.error("an image is required unless --clear is given")
    if args.clear and args.dry_run:
        parser.error("--clear has nothing to convert, so --dry-run does not apply")
    if args.colors != 4 and args.normalize:
        parser.error("--normalize applies to 4-colour mode only")
    if args.colors == 4 and args.invert:
        parser.error("--invert applies to the 2-colour bit convention only")
    try:
        args.gray_levels = tuple(int(v) for v in args.gray_levels.split(","))
    except ValueError:
        parser.error("--gray-levels wants four comma-separated numbers")
    if len(args.gray_levels) != 4 or not all(0 <= v <= 255 for v in args.gray_levels):
        parser.error("--gray-levels wants four values in 0..255")
    return args


def main(argv=None):
    args = parse_args(argv if argv is not None else sys.argv[1:])

    payload = None
    width, height = args.width, args.height
    if not args.clear:
        size = (width, height)
        if args.colors == 4:
            levels = convert_gray4(
                args.image,
                size,
                args.fit,
                args.rotate,
                args.dither,
                args.gray_levels,
                args.normalize,
            )
            payload = pack_2bpp(levels)
            expected = width // 4 * height
        else:
            image = convert_image(
                args.image, size, args.fit, args.rotate, args.dither, args.threshold
            )
            payload = pack_1bpp(image, args.invert)
            expected = (width + 7) // 8 * height
        if len(payload) != expected:
            raise SystemExit(f"packed {len(payload)} bytes, expected {expected}")
        print(
            f"packed {len(payload)} bytes for {width}x{height}, {args.colors} colours",
            file=sys.stderr,
        )

        if args.preview:
            if args.colors == 4:
                unpack_2bpp(payload, size, args.gray_levels).save(args.preview)
            else:
                unpack_1bpp(payload, size, args.invert).save(args.preview)
            print(f"preview written to {args.preview}", file=sys.stderr)

    if args.dry_run:
        return 0

    return asyncio.run(run(args, payload, width, height))


if __name__ == "__main__":
    sys.exit(main())
