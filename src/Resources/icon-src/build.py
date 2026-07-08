from __future__ import annotations

import io
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter


HERE = Path(__file__).resolve().parent
OUT = HERE / "out"
RESOURCES = HERE.parent
REPO = RESOURCES.parent.parent
SITE = REPO / "site"
# The desktop editor's in-app title bar renders its corner icon from this PNG
# (see web/apps/editor/src/components/TitleBar.tsx). It is the ramped 40px tile
# — the exact taskbar/exe artwork — so the in-app mark matches the taskbar. The
# title bar shows it at 20px CSS, so 40px covers 1x–2x DPI crisply.
WEB_ASSETS = REPO / "web" / "apps" / "editor" / "src" / "assets"

SIZES = (16, 20, 24, 32, 40, 48, 64, 128, 256)
ACCENT = "#4a8bff"
MARK_SCALE = 1.15
MARK_ANCHOR = (0.51, 0.50)
MARK_OFFSET = (-0.010, 0.0108)
ENDPOINT_FILL = "#10223a"

PLATE_TOP, PLATE_BOTTOM = "#172232", "#070a10"
# Lifted plate for taskbar/titlebar sizes: the standard plate is only
# ~1.0–1.25:1 contrast against Win11 dark chrome (~#1f2228) and the tile
# silhouette vanishes.
PLATE_TOP_LIFTED, PLATE_BOTTOM_LIFTED = "#2a3d5c", "#101827"

# Small-size legibility ramp. Below 48px the detail layers stop reading — at
# 16px the tube is ~1px, the highlight and endpoint-ring strokes are sub-pixel,
# and the glow only muddies the downscale — so ramped sizes draw plate +
# fattened tube + enlarged particle only. Multipliers taper upward: the smaller
# the render, the more the surviving shapes must be exaggerated. 20/24/40 exist
# so the common Windows cases (taskbar @100% DPI, titlebar @100–150%) get an
# exact entry instead of a resample of 16/32.  size: (tube_x, particle_x, plate_lift)
RAMP = {
    16: (2.2, 1.35, 1.0),
    20: (2.0, 1.32, 1.0),
    24: (1.8, 1.28, 1.0),
    32: (1.5, 1.20, 0.55),
    40: (1.35, 1.12, 0.35),
}


def hex_rgb(value: str) -> tuple[int, int, int]:
    value = value.strip().lstrip("#")
    return tuple(int(value[i : i + 2], 16) for i in (0, 2, 4))


def mix(a: str, b: str, t: float) -> tuple[int, int, int]:
    ar, ag, ab = hex_rgb(a)
    br, bg, bb = hex_rgb(b)
    return (
        round(ar + (br - ar) * t),
        round(ag + (bg - ag) * t),
        round(ab + (bb - ab) * t),
    )


def bezier(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
    steps: int = 220,
) -> list[tuple[float, float]]:
    points: list[tuple[float, float]] = []
    for index in range(steps + 1):
        t = index / steps
        u = 1 - t
        points.append(
            (
                u**3 * p0[0] + 3 * u * u * t * p1[0] + 3 * u * t * t * p2[0] + t**3 * p3[0],
                u**3 * p0[1] + 3 * u * u * t * p1[1] + 3 * u * t * t * p2[1] + t**3 * p3[1],
            )
        )
    return points


def split_bezier_second_half(
    p0: tuple[float, float],
    p1: tuple[float, float],
    p2: tuple[float, float],
    p3: tuple[float, float],
) -> tuple[tuple[float, float], tuple[float, float], tuple[float, float], tuple[float, float]]:
    def midpoint(a: tuple[float, float], b: tuple[float, float]) -> tuple[float, float]:
        return ((a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5)

    p01 = midpoint(p0, p1)
    p12 = midpoint(p1, p2)
    p23 = midpoint(p2, p3)
    p012 = midpoint(p01, p12)
    p123 = midpoint(p12, p23)
    p0123 = midpoint(p012, p123)
    return p0123, p123, p23, p3


def radial_circle(
    size: int,
    center: tuple[float, float],
    radius: float,
    colors: tuple[str, str, str] = ("#f4faff", "#5ea1ff", "#2f72e0"),
    alpha: int = 255,
) -> Image.Image:
    layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = layer.load()
    cx, cy = center
    for y in range(size):
        for x in range(size):
            distance = math.hypot(x + 0.5 - cx, y + 0.5 - cy) / max(radius, 1)
            if distance > 1:
                continue
            if distance < 0.58:
                rgb = mix(colors[0], colors[1], distance / 0.58)
            else:
                rgb = mix(colors[1], colors[2], (distance - 0.58) / 0.42)
            edge = max(0.0, 1.0 - max(0.0, distance - 0.72) * 1.85)
            px[x, y] = (*rgb, round(alpha * edge))
    return layer


def rounded_mask(size: int, radius: int) -> Image.Image:
    mask = Image.new("L", (size, size), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size - 1, size - 1), radius=radius, fill=255)
    return mask


def draw_plate(base: Image.Image, lift: float = 0.0) -> None:
    size = base.size[0]
    radius = round(size * 0.235)
    mask = rounded_mask(size, radius)

    top = "#%02x%02x%02x" % mix(PLATE_TOP, PLATE_TOP_LIFTED, lift)
    bottom = "#%02x%02x%02x" % mix(PLATE_BOTTOM, PLATE_BOTTOM_LIFTED, lift)
    plate = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    pixels = plate.load()
    for y in range(size):
        y_t = y / max(size - 1, 1)
        for x in range(size):
            x_t = x / max(size - 1, 1)
            t = min(1.0, max(0.0, y_t * 0.86 + (1 - x_t) * 0.14))
            pixels[x, y] = (*mix(top, bottom, t), 255)
    plate.putalpha(mask)

    shadow = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    shadow.putalpha(mask.filter(ImageFilter.GaussianBlur(max(3, round(size * 0.01)))).point(lambda v: round(v * 0.26)))
    base.alpha_composite(shadow, (0, max(1, round(size * 0.005))))
    base.alpha_composite(plate)

    draw = ImageDraw.Draw(base, "RGBA")
    inset = max(1, round(size * 0.005))
    rim_alpha = round(38 + (110 - 38) * lift)
    draw.rounded_rectangle(
        (inset, inset, size - 1 - inset, size - 1 - inset),
        radius=max(1, round(size * 0.22)),
        outline=(122, 165, 234, rim_alpha),
        width=max(1, round(size * 0.004)),
    )


def draw_tube(
    base: Image.Image,
    points: list[tuple[float, float]],
    radius: int,
    color: tuple[int, int, int, int],
    blur: int = 0,
) -> None:
    layer = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer, "RGBA")
    for x, y in points:
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=color)
    if blur:
        layer = layer.filter(ImageFilter.GaussianBlur(blur))
    base.alpha_composite(layer)


def draw_endpoint(draw: ImageDraw.ImageDraw, center: tuple[float, float], radius: int) -> None:
    x, y = center
    draw.ellipse(
        (x - radius, y - radius, x + radius, y + radius),
        fill=hex_rgb(ENDPOINT_FILL) + (238,),
        outline=(131, 190, 255, 235),
        width=max(2, round(radius * 0.22)),
    )


def draw_particle(base: Image.Image, center: tuple[float, float], radius: float) -> None:
    base.alpha_composite(radial_circle(base.size[0], center, radius))


def mark_point(size: int, x_ratio: float, y_ratio: float) -> tuple[float, float]:
    anchor_x = size * MARK_ANCHOR[0]
    anchor_y = size * MARK_ANCHOR[1]
    return (
        anchor_x + (size * x_ratio - anchor_x) * MARK_SCALE + size * MARK_OFFSET[0],
        anchor_y + (size * y_ratio - anchor_y) * MARK_SCALE + size * MARK_OFFSET[1],
    )


def svg_num(value: float) -> str:
    return f"{value:.2f}".rstrip("0").rstrip(".")


def draw_mark(base: Image.Image) -> None:
    size = base.size[0]
    draw = ImageDraw.Draw(base, "RGBA")

    p0 = mark_point(size, 0.245, 0.605)
    p1 = mark_point(size, 0.390, 0.365)
    p2 = mark_point(size, 0.615, 0.665)
    p3 = mark_point(size, 0.775, 0.375)
    points = bezier(p0, p1, p2, p3)

    draw_tube(
        base,
        points,
        max(3, round(size * 0.058 * MARK_SCALE)),
        (70, 145, 244, 76),
        blur=max(1, round(size * 0.003)),
    )
    draw_tube(base, points, max(2, round(size * 0.031 * MARK_SCALE)), (124, 185, 255, 250))
    draw_tube(base, points[len(points) // 2 :], max(1, round(size * 0.009 * MARK_SCALE)), (220, 240, 255, 155))

    draw_endpoint(draw, p0, max(5, round(size * 0.045 * MARK_SCALE)))
    draw_endpoint(draw, p3, max(5, round(size * 0.041 * MARK_SCALE)))
    draw_particle(base, mark_point(size, 0.525, 0.505), size * 0.142 * MARK_SCALE)


def draw_mark_ramped(base: Image.Image, tube_mult: float, particle_mult: float) -> None:
    """Small-size mark: spline + particle only. The highlight and endpoint-ring
    strokes are sub-pixel below 48px, and the glow only muddies the downscale."""
    size = base.size[0]
    p0 = mark_point(size, 0.245, 0.605)
    p1 = mark_point(size, 0.390, 0.365)
    p2 = mark_point(size, 0.615, 0.665)
    p3 = mark_point(size, 0.775, 0.375)
    points = bezier(p0, p1, p2, p3)
    draw_tube(base, points, max(2, round(size * 0.031 * MARK_SCALE * tube_mult)), (124, 185, 255, 250))
    draw_particle(base, mark_point(size, 0.525, 0.505), size * 0.142 * MARK_SCALE * particle_mult)


def scale_for(size: int) -> int:
    if size <= 64:
        return 6
    if size <= 256:
        return 4
    return 1


def render(size: int, plate: bool) -> Image.Image:
    scale = scale_for(size)
    large = size * scale
    image = Image.new("RGBA", (large, large), (0, 0, 0, 0))
    ramp = RAMP.get(size)
    if plate:
        draw_plate(image, lift=ramp[2] if ramp else 0.0)
    if ramp:
        draw_mark_ramped(image, ramp[0], ramp[1])
    else:
        draw_mark(image)
    return image.resize((size, size), Image.Resampling.LANCZOS) if scale != 1 else image


def png_bytes(image: Image.Image) -> bytes:
    out = io.BytesIO()
    image.save(out, format="PNG", optimize=True)
    return out.getvalue()


def build_ico(entries: list[tuple[int, bytes]]) -> bytes:
    count = len(entries)
    header = bytearray((0, 0, 1, 0, count, 0))
    directory = bytearray()
    offset = 6 + 16 * count
    for size, data in entries:
        directory.extend(bytes((0 if size >= 256 else size, 0 if size >= 256 else size, 0, 0, 1, 0, 32, 0)))
        directory.extend(len(data).to_bytes(4, "little"))
        directory.extend(offset.to_bytes(4, "little"))
        offset += len(data)
    return bytes(header + directory) + b"".join(data for _, data in entries)


def mark_svg(include_plate: bool) -> str:
    p0 = mark_point(256, 0.245, 0.605)
    p1 = mark_point(256, 0.390, 0.365)
    p2 = mark_point(256, 0.615, 0.665)
    p3 = mark_point(256, 0.775, 0.375)
    particle = mark_point(256, 0.525, 0.505)
    path = (
        f"M{svg_num(p0[0])} {svg_num(p0[1])} "
        f"C{svg_num(p1[0])} {svg_num(p1[1])} "
        f"{svg_num(p2[0])} {svg_num(p2[1])} "
        f"{svg_num(p3[0])} {svg_num(p3[1])}"
    )
    hp0, hp1, hp2, hp3 = split_bezier_second_half(p0, p1, p2, p3)
    highlight_path = (
        f"M{svg_num(hp0[0])} {svg_num(hp0[1])} "
        f"C{svg_num(hp1[0])} {svg_num(hp1[1])} "
        f"{svg_num(hp2[0])} {svg_num(hp2[1])} "
        f"{svg_num(hp3[0])} {svg_num(hp3[1])}"
    )
    glow_width = 256 * 0.058 * MARK_SCALE * 2
    tube_width = 256 * 0.031 * MARK_SCALE * 2
    highlight_width = 256 * 0.009 * MARK_SCALE * 2
    start_radius = 256 * 0.045 * MARK_SCALE
    end_radius = 256 * 0.041 * MARK_SCALE
    particle_radius = 256 * 0.142 * MARK_SCALE
    plate_defs = """
    <linearGradient id="plate" x1="48" y1="22" x2="210" y2="230" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#172232"/>
      <stop offset="1" stop-color="#070a10"/>
    </linearGradient>""" if include_plate else ""
    plate_shapes = """
  <rect x="8" y="8" width="240" height="240" rx="58" fill="url(#plate)"/>
  <rect x="10" y="10" width="236" height="236" rx="56" fill="none" stroke="#7aa5ea" stroke-opacity=".18" stroke-width="2"/>""" if include_plate else ""
    return f"""<svg xmlns="http://www.w3.org/2000/svg" width="256" height="256" viewBox="0 0 256 256">
  <defs>{plate_defs}
    <radialGradient id="particle" cx="42%" cy="36%" r="62%">
      <stop offset="0" stop-color="#f4faff"/>
      <stop offset=".58" stop-color="#5ea1ff"/>
      <stop offset="1" stop-color="#2f72e0"/>
    </radialGradient>
  </defs>{plate_shapes}
  <path d="{path}" fill="none" stroke="#4691f4" stroke-opacity=".3" stroke-width="{svg_num(glow_width)}" stroke-linecap="round"/>
  <path d="{path}" fill="none" stroke="#7cb9ff" stroke-width="{svg_num(tube_width)}" stroke-linecap="round"/>
  <path d="{highlight_path}" fill="none" stroke="#dcf0ff" stroke-opacity=".61" stroke-width="{svg_num(highlight_width)}" stroke-linecap="round"/>
  <circle cx="{svg_num(p0[0])}" cy="{svg_num(p0[1])}" r="{svg_num(start_radius)}" fill="{ENDPOINT_FILL}" stroke="#83beff" stroke-width="3"/>
  <circle cx="{svg_num(p3[0])}" cy="{svg_num(p3[1])}" r="{svg_num(end_radius)}" fill="{ENDPOINT_FILL}" stroke="#83beff" stroke-width="3"/>
  <circle cx="{svg_num(particle[0])}" cy="{svg_num(particle[1])}" r="{svg_num(particle_radius)}" fill="url(#particle)"/>
</svg>
"""


def preview_sheet(images: dict[int, Image.Image], mark: Image.Image) -> Image.Image:
    step = 224
    width = 80 + step * len(SIZES)
    sheet = Image.new("RGBA", (width, 820), hex_rgb("#0b0d12") + (255,))
    draw = ImageDraw.Draw(sheet)
    draw.text((60, 46), "logo.ico - no-handle spline particle mark (legibility ramp <= 40px)", fill="#eef1f6")
    for index, size in enumerate(SIZES):
        x = 80 + index * step
        icon = images[size].resize((160, 160), Image.Resampling.NEAREST)
        sheet.alpha_composite(icon, (x, 96))
        draw.text((x + 58, 284), f"{size}px", fill="#9aa1ad")
    draw.line((60, 336, width - 60, 336), fill="#222833", width=2)
    for index, size in enumerate((16, 20, 24, 32)):
        x = 140 + index * 315
        icon = images[size].resize((224, 224), Image.Resampling.NEAREST)
        sheet.alpha_composite(icon, (x, 392))
        draw.text((x + 60, 690), f"{size}px true pixels", fill="#cdd3dd")
    sheet.alpha_composite(mark.resize((224, 224), Image.Resampling.LANCZOS), (width - 370, 392))
    draw.text((width - 340, 690), "transparent mark", fill="#cdd3dd")
    return sheet.resize((width // 2, 410), Image.Resampling.LANCZOS)


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)

    tile_images = {size: render(size, plate=True) for size in SIZES}
    entries: list[tuple[int, bytes]] = []
    for size, image in tile_images.items():
        data = png_bytes(image)
        (OUT / f"size_{size}.png").write_bytes(data)
        entries.append((size, data))

    ico = build_ico(entries)
    (OUT / "logo.ico").write_bytes(ico)
    (RESOURCES / "logo.ico").write_bytes(ico)
    (SITE / "favicon.ico").write_bytes(ico)

    logo_svg = mark_svg(include_plate=True)
    transparent_svg = mark_svg(include_plate=False)
    (OUT / "logo.svg").write_text(logo_svg, encoding="utf-8")
    (HERE / "logo.svg").write_text(logo_svg, encoding="utf-8")
    (SITE / "favicon.svg").write_text(logo_svg, encoding="utf-8")
    (OUT / "mark.svg").write_text(transparent_svg, encoding="utf-8")
    (HERE / "mark.svg").write_text(transparent_svg, encoding="utf-8")

    master = render(1024, plate=True)
    mark = render(1024, plate=False)
    master.save(HERE / "master_1024.png", optimize=True)
    master.save(OUT / "master_1024.png", optimize=True)
    mark.save(HERE / "mark_1024.png", optimize=True)
    mark.save(OUT / "mark_1024.png", optimize=True)
    preview_sheet(tile_images, render(256, plate=False)).save(OUT / "preview_sheet.png", optimize=True)

    # In-app title-bar mark for the desktop editor — the ramped 40px tile, so
    # the corner icon matches the taskbar/exe icon exactly.
    WEB_ASSETS.mkdir(parents=True, exist_ok=True)
    tile_images[40].save(WEB_ASSETS / "app-mark.png", optimize=True)

    print(f"Wrote {RESOURCES / 'logo.ico'}")
    print(f"Wrote {SITE / 'favicon.ico'}")
    print(f"Wrote {SITE / 'favicon.svg'}")
    print(f"Wrote {HERE / 'logo.svg'}")
    print(f"Wrote {HERE / 'mark.svg'}")
    print(f"Wrote {HERE / 'master_1024.png'}")
    print(f"Wrote {HERE / 'mark_1024.png'}")
    print(f"Wrote {WEB_ASSETS / 'app-mark.png'}")
    print("Packed sizes:", ", ".join(str(size) for size in SIZES))


if __name__ == "__main__":
    main()
