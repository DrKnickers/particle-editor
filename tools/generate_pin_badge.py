"""Generate a 24×48 24-bit BMP for the texture-palette hover-pin badge.

The bitmap is a vertical strip with two stacked 24×24 frames:
  - top half (y=0..23):   "empty"  — hollow pin, hover-but-not-pinned
  - bottom half (y=24..47): "filled" — solid pin, pinned state

Drawn as a 5-pointed star silhouette (yellow fill on filled, white on
empty) with a dark outline. The shape reads as a star/pin badge
regardless of state. Background colour is COLOR_BTNFACE-grey so the
non-shape pixels blend with the cell background when blitted; the
caller can replace this with a colour-key if needed.

Run: python tools/generate_pin_badge.py
Writes: src/Resources/pin_badge.bmp
"""

import struct
from pathlib import Path

# Colour palette (BGR for BMP).
BG  = (240, 240, 240)   # button-face grey — invisible on cell bg
OUT = ( 40,  40,  40)   # near-black outline — readable on any thumbnail
FIL = ( 50,  50, 220)   # canonical red pushpin (BGR) for the pinned state
EMP = (250, 250, 250)   # near-white for the hover-but-not-pinned state

# Two 24×24 frames. Each row is exactly 24 chars.
#   .  background (button-face grey, invisible on cell bg)
#   O  outline (dark)
#   F  state-dependent fill (yellow on filled half, white on empty half)
#
# A stylised thumbtack: round head at the top with a tapered needle
# going down. Recognisable as a pin even at small sizes.
FRAME_TEMPLATE = [
    "........................",
    ".......OOOOOOOOOO.......",
    ".....OOFFFFFFFFFFOO.....",
    "....OFFFFFFFFFFFFFFO....",
    "....OFFFFFFFFFFFFFFO....",
    "....OFFFFFFFFFFFFFFO....",
    "....OFFFFFFFFFFFFFFO....",
    "....OFFFFFFFFFFFFFFO....",
    ".....OOFFFFFFFFFFOO.....",
    ".......OOOOOOOOOO.......",
    "..........OFFO..........",
    "..........OFFO..........",
    "..........OFFO..........",
    "..........OFFO..........",
    "..........OFFO..........",
    "...........OO...........",
    "...........OO...........",
    "............O...........",
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
    "........................",
]

CHAR_TO_BGR = {
    ".": BG,
    "O": OUT,
}


def encode_bmp(width, height, pixels_top_first):
    row_bytes = width * 3
    # Each row must be a multiple of 4 bytes. 24*3=72 → already aligned.
    assert row_bytes % 4 == 0, "row padding needed; not implemented"
    pixel_data_size = row_bytes * height

    rows_bottom_first = list(reversed(pixels_top_first))
    pixel_bytes = bytearray()
    for row in rows_bottom_first:
        assert len(row) == width, f"row length {len(row)} != width {width}"
        for ch in row:
            if ch in CHAR_TO_BGR:
                b, g, r = CHAR_TO_BGR[ch]
            elif ch == "F":
                # Should never reach here — caller substitutes F per half.
                raise ValueError("unresolved F in pixel row")
            elif ch == "Y":
                b, g, r = FIL   # yellow fill
            elif ch == "W":
                b, g, r = EMP   # white fill
            else:
                raise ValueError(f"unknown pixel char '{ch}'")
            pixel_bytes.extend((b, g, r))
    assert len(pixel_bytes) == pixel_data_size

    file_header_size = 14
    info_header_size = 40
    pixel_offset = file_header_size + info_header_size
    file_size = pixel_offset + pixel_data_size

    file_header = struct.pack(
        "<2sIHHI", b"BM", file_size, 0, 0, pixel_offset,
    )
    info_header = struct.pack(
        "<IiiHHIIiiII",
        info_header_size,
        width,
        height,
        1, 24, 0,
        pixel_data_size,
        2835, 2835,
        0, 0,
    )
    return file_header + info_header + bytes(pixel_bytes)


def main():
    # Top frame (rows 0..23): "F" → "W" (empty/hover state).
    empty_frame = [row.replace("F", "W") for row in FRAME_TEMPLATE]
    # Bottom frame (rows 24..47): "F" → "Y" (filled/pinned state).
    filled_frame = [row.replace("F", "Y") for row in FRAME_TEMPLATE]

    # Strip layout: empty on TOP, filled on BOTTOM (matches caller's
    # 0=empty, 1=filled state convention).
    combined = empty_frame + filled_frame
    assert len(combined) == 48

    out = Path(__file__).resolve().parent.parent / "src" / "Resources" / "pin_badge.bmp"
    out.write_bytes(encode_bmp(24, 48, combined))
    print(f"wrote {out} ({out.stat().st_size} bytes)")


if __name__ == "__main__":
    main()
