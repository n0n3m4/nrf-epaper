#!/usr/bin/env bash
# Host-side checks for the image push protocol. No board and no BLE adapter
# needed: the Python client generates the fixtures, the firmware's own parser
# consumes them.
#
#   ./test/run.sh
#
# Needs g++ and Pillow; bleak is not imported on this path.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$root"

python3 - "$work" <<'PY'
import sys, zlib
sys.path.insert(0, "tools")
from PIL import Image, ImageDraw
from epaper_push import (
    GRAY_LEVELS,
    build_begin,
    convert_gray4,
    convert_image,
    pack_1bpp,
    pack_2bpp,
    unpack_1bpp,
    unpack_2bpp,
)

work = sys.argv[1]

# A source image that is deliberately the wrong aspect ratio and size, so the
# fit/letterbox path is exercised too.
img = Image.new("RGB", (800, 480), "white")
draw = ImageDraw.Draw(img)
for x in range(0, 800, 40):
    draw.line([(x, 0), (x, 480)], fill=(x // 4, 0, 255 - x // 4), width=3)
draw.ellipse([200, 100, 600, 380], fill="black")
src = f"{work}/src.png"
img.save(src)

size = (400, 300)
converted = convert_image(src, size, "contain", 0, "floyd", 128)
frame = pack_1bpp(converted)
assert len(frame) == 15000, len(frame)
assert unpack_1bpp(frame, size).tobytes() == converted.tobytes(), "round trip"

# All-black must pack to all ones, all-white to all zeros: a set bit is ink.
for fill, expect in ((0, 0xFF), (255, 0x00)):
    Image.new("L", size, fill).save(f"{work}/flat.png")
    flat = pack_1bpp(convert_image(f"{work}/flat.png", size, "stretch", 0, "none", 128))
    assert set(flat) == {expect}, (fill, sorted(set(flat))[:4])

open(f"{work}/frame.bin", "wb").write(frame)
open(f"{work}/begin.bin", "wb").write(
    build_begin(400, 300, frame, zlib.crc32(frame) & 0xFFFFFFFF, False)
)
print("  ok   client packs 15000 bytes, set bit = black, round trip lossless")

# --- 4 colours ---------------------------------------------------------------
levels = convert_gray4(src, size, "contain", 0, "floyd", GRAY_LEVELS)
gray = pack_2bpp(levels)
assert len(gray) == 30000, len(gray)
assert set(levels) == {0, 1, 2, 3}, sorted(set(levels))
assert unpack_2bpp(gray, size, GRAY_LEVELS).tobytes() == bytes(
    GRAY_LEVELS[v] for v in levels
), "2bpp round trip"

# Level 3 is black and level 0 white, in the same direction as the 1bpp path.
for fill, expect in ((0, 0xFF), (255, 0x00)):
    Image.new("L", size, fill).save(f"{work}/flat.png")
    flat = pack_2bpp(convert_gray4(f"{work}/flat.png", size, "stretch", 0, "none", GRAY_LEVELS))
    assert set(flat) == {expect}, (fill, sorted(set(flat))[:4])

# --normalize reads a source value as coverage between the panel's real black
# and white instead of as an absolute luminance, so the unreachable ends stop
# collapsing into two fat bands. Off by default: it lightens solid ink, which is
# wrong for text and is why render.py does not do it.
ramp = Image.new("L", (256, 8))
ramp.putdata([x for _ in range(8) for x in range(256)])
ramp.save(f"{work}/ramp.png")


def ramp_bands(normalize):
    row = convert_gray4(f"{work}/ramp.png", (256, 8), "stretch", 0, "none",
                        GRAY_LEVELS, normalize)[:256]
    out = []
    for v in row:
        if out and out[-1][0] == v:
            out[-1][1] += 1
        else:
            out.append([v, 1])
    return out


plain, normed = ramp_bands(False), ramp_bands(True)
assert [b[0] for b in plain] == [3, 2, 1, 0], plain
assert [b[0] for b in normed] == [3, 2, 1, 0], normed
assert normed[0][1] < plain[0][1] and normed[3][1] < plain[3][1], (plain, normed)
assert normed[1][1] > plain[1][1] and normed[2][1] > plain[2][1], (plain, normed)
# A no-op on a panel that really does reach 0 and 255, so it can only ever
# correct for the measured offset, never invent a mapping of its own.
true_ends = (255, GRAY_LEVELS[1], GRAY_LEVELS[2], 0)
assert convert_gray4(f"{work}/ramp.png", (256, 8), "stretch", 0, "none", true_ends, True) == \
       convert_gray4(f"{work}/ramp.png", (256, 8), "stretch", 0, "none", true_ends, False)
print(f"  ok   --normalize widens the interior {[b[1] for b in plain]}"
      f" -> {[b[1] for b in normed]}, no-op at true 0/255")

open(f"{work}/frame_gray4.bin", "wb").write(gray)
open(f"{work}/begin_gray4.bin", "wb").write(
    build_begin(400, 300, gray, zlib.crc32(gray) & 0xFFFFFFFF, False, True)
)
print("  ok   client packs 30000 bytes for 4 colours, all levels used, round trip lossless")

# --- deflate -----------------------------------------------------------------
# len and crc32 describe the compressed bytes; the uncompressed size is whatever
# FLAG_GRAY4 says. The firmware inflates from verify(), after the CRC passes.
for name, label, raw, gray4 in (("", "1bpp", frame, False), ("_gray4", "2bpp", gray, True)):
    squeezed = zlib.compress(raw, 9)
    assert len(squeezed) < len(raw) // 2, (name, len(squeezed))
    open(f"{work}/frame{name}_deflate.bin", "wb").write(squeezed)
    open(f"{work}/begin{name}_deflate.bin", "wb").write(
        build_begin(400, 300, squeezed, zlib.crc32(squeezed) & 0xFFFFFFFF, False, gray4, True)
    )
    print(f"  ok   client deflates {label} {len(raw)} bytes to {len(squeezed)}")

# A stream that survives the CRC but is not valid deflate: the device has to
# reject it rather than render whatever the inflate left behind.
broken = zlib.compress(frame, 9)[::-1]
open(f"{work}/frame_deflate_bad.bin", "wb").write(broken)
open(f"{work}/begin_deflate_bad.bin", "wb").write(
    build_begin(400, 300, broken, zlib.crc32(broken) & 0xFFFFFFFF, False, False, True)
)
PY

# -lz stands in for the vendored miniz on the host; image_receiver.cpp picks
# between them on __ZEPHYR__.
g++ -std=c++17 -Wall -Wextra -I"$root/src" -o "$work/protocol_test" \
    "$root/test/protocol_test.cpp" "$root/src/image_receiver.cpp" "$root/src/crc32.cpp" -lz

"$work/protocol_test" "$work/begin.bin" "$work/frame.bin" \
    "$work/begin_gray4.bin" "$work/frame_gray4.bin" \
    "$work/begin_deflate.bin" "$work/frame_deflate.bin" \
    "$work/begin_gray4_deflate.bin" "$work/frame_gray4_deflate.bin" \
    "$work/begin_deflate_bad.bin" "$work/frame_deflate_bad.bin"
