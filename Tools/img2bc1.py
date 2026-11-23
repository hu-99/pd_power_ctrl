#!/usr/bin/env python3
"""
img2bc1.py - Offline BC1/DXT1 image encoder for PY32F003 0.96" SPI TFT (160x80).

Encodes any PNG/JPG/BMP (or a built-in --demo dark instrument wallpaper) into a
fixed-ratio 4:1 BC1/DXT1 C header compatible with Logic_PureC/bc1_decode.h.
For 160x80 RGB565, raw size is 25,600 B -> BC1 encoded size is exactly 6,400 B (6.25 KB).
"""

import argparse
import math
import os
import struct
from typing import List, Tuple

from PIL import Image, ImageOps


def clamp_u8(val: float) -> int:
    if val < 0.0:
        return 0
    if val > 255.0:
        return 255
    return int(round(val))


def rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return (r5 << 11) | (g6 << 5) | b5


def rgb565_to_rgb888(c: int) -> Tuple[int, int, int]:
    r5 = (c >> 11) & 0x1F
    g6 = (c >> 5) & 0x3F
    b5 = c & 0x1F
    r8 = (r5 << 3) | (r5 >> 2)
    g8 = (g6 << 2) | (g6 >> 4)
    b8 = (b5 << 3) | (b5 >> 2)
    return (r8, g8, b8)


def blend_2_1_565(c_major: int, c_minor: int) -> int:
    r0, g0, b0 = (c_major >> 11) & 0x1F, (c_major >> 5) & 0x3F, c_major & 0x1F
    r1, g1, b1 = (c_minor >> 11) & 0x1F, (c_minor >> 5) & 0x3F, c_minor & 0x1F
    r = (2 * r0 + r1 + 1) // 3
    g = (2 * g0 + g1 + 1) // 3
    b = (2 * b0 + b1 + 1) // 3
    return (r << 11) | (g << 5) | b


def expand_palette_565(c0: int, c1: int) -> List[int]:
    if c0 > c1:
        return [c0, c1, blend_2_1_565(c0, c1), blend_2_1_565(c1, c0)]
    r0, g0, b0 = (c0 >> 11) & 0x1F, (c0 >> 5) & 0x3F, c0 & 0x1F
    r1, g1, b1 = (c1 >> 11) & 0x1F, (c1 >> 5) & 0x3F, c1 & 0x1F
    mid = (((r0 + r1) >> 1) << 11) | (((g0 + g1) >> 1) << 5) | ((b0 + b1) >> 1)
    return [c0, c1, mid, 0x0000]


def dist_sq(a: Tuple[int, int, int], b: Tuple[int, int, int]) -> int:
    dr = a[0] - b[0]
    dg = a[1] - b[1]
    db = a[2] - b[2]
    # Perceptual green weighting (2*R^2 + 4*G^2 + 1*B^2 approx)
    return (2 * dr * dr) + (4 * dg * dg) + (db * db)


def principal_axis(pixels: List[Tuple[int, int, int]]) -> Tuple[float, float, float]:
    n = float(len(pixels))
    mr = sum(p[0] for p in pixels) / n
    mg = sum(p[1] for p in pixels) / n
    mb = sum(p[2] for p in pixels) / n

    crr = crg = crb = cgg = cgb = cbb = 0.0
    for r, g, b in pixels:
        dr, dg, db = r - mr, g - mg, b - mb
        crr += dr * dr
        crg += dr * dg
        crb += dr * db
        cgg += dg * dg
        cgb += dg * db
        cbb += db * db

    vx, vy, vz = 0.299, 0.587, 0.114
    for _ in range(8):
        nx = crr * vx + crg * vy + crb * vz
        ny = crg * vx + cgg * vy + cgb * vz
        nz = crb * vx + cgb * vy + cbb * vz
        norm = math.sqrt(nx * nx + ny * ny + nz * nz)
        if norm < 1e-9:
            break
        vx, vy, vz = nx / norm, ny / norm, nz / norm
    return (vx, vy, vz)


def encode_block(pixels: List[Tuple[int, int, int]]) -> bytes:
    """Encode 16 RGB888 pixels (4x4 row-major) into an 8-byte BC1 block."""
    # Check if all pixels map to the exact same RGB565 color
    first_565 = rgb888_to_rgb565(*pixels[0])
    if all(rgb888_to_rgb565(*p) == first_565 for p in pixels):
        return struct.pack("<HHI", first_565, first_565, 0)

    # Initial endpoints from projection onto principal color axis
    ax = principal_axis(pixels)
    projs = [p[0] * ax[0] + p[1] * ax[1] + p[2] * ax[2] for p in pixels]
    e0 = list(pixels[projs.index(max(projs))])
    e1 = list(pixels[projs.index(min(projs))])

    # 3 rounds of least-squares endpoint refinement for 4-level linear palette
    weights = (0.0, 1.0, 1.0 / 3.0, 2.0 / 3.0)
    best_err = None
    best_pack = None

    for _ in range(4):
        c0 = rgb888_to_rgb565(clamp_u8(e0[0]), clamp_u8(e0[1]), clamp_u8(e0[2]))
        c1 = rgb888_to_rgb565(clamp_u8(e1[0]), clamp_u8(e1[1]), clamp_u8(e1[2]))
        if c0 < c1:
            c0, c1 = c1, c0
            e0, e1 = e1, e0
        if c0 == c1:
            # Nudge to unlock 4-color mode if pixels actually vary
            if c0 < 0xFFFF:
                c0 += 1
            else:
                c1 -= 1

        pal_565 = expand_palette_565(c0, c1)
        pal_888 = [rgb565_to_rgb888(c) for c in pal_565]

        indices = 0
        total_err = 0
        chosen = []
        for i, p in enumerate(pixels):
            best_k = 0
            best_d = dist_sq(p, pal_888[0])
            for k in (1, 2, 3):
                d = dist_sq(p, pal_888[k])
                if d < best_d:
                    best_d = d
                    best_k = k
            indices |= (best_k << (2 * i))
            total_err += best_d
            chosen.append(best_k)

        if best_err is None or total_err < best_err:
            best_err = total_err
            best_pack = struct.pack("<HHI", c0, c1, indices)

        # Solve 2x2 least squares for refined endpoints e0 (w=0) and e1 (w=1)
        aa = ab = bb = 0.0
        ar = ag = ab_c = 0.0
        br = bg = bb_c = 0.0
        for p, k in zip(pixels, chosen):
            w1 = weights[k]
            w0 = 1.0 - w1
            aa += w0 * w0
            ab += w0 * w1
            bb += w1 * w1
            ar += w0 * p[0]
            ag += w0 * p[1]
            ab_c += w0 * p[2]
            br += w1 * p[0]
            bg += w1 * p[1]
            bb_c += w1 * p[2]

        det = aa * bb - ab * ab
        if abs(det) < 1e-6:
            break
        inv = 1.0 / det
        e0 = [(bb * ar - ab * br) * inv,
              (bb * ag - ab * bg) * inv,
              (bb * ab_c - ab * bb_c) * inv]
        e1 = [(aa * br - ab * ar) * inv,
              (aa * bg - ab * ag) * inv,
              (aa * bb_c - ab * ab_c) * inv]

    assert best_pack is not None
    return best_pack


def encode_image(img: Image.Image) -> bytes:
    w, h = img.size
    if w == 0 or h == 0 or (w % 4) != 0 or (h % 4) != 0:
        raise ValueError(f"Image dimensions ({w}x{h}) must be non-zero multiples of 4.")
    rgb = img.convert("RGB")
    px = rgb.load()
    out = bytearray()
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            block_pixels = [px[bx + lx, by + ly] for ly in range(4) for lx in range(4)]
            out.extend(encode_block(block_pixels))
    return bytes(out)


def decode_image(data: bytes, w: int, h: int) -> Image.Image:
    out = Image.new("RGB", (w, h))
    px = out.load()
    offset = 0
    for by in range(0, h, 4):
        for bx in range(0, w, 4):
            c0, c1, bits = struct.unpack_from("<HHI", data, offset)
            offset += 8
            pal = [rgb565_to_rgb888(c) for c in expand_palette_565(c0, c1)]
            for ly in range(4):
                for lx in range(4):
                    idx = (bits >> (2 * (ly * 4 + lx))) & 3
                    px[bx + lx, by + ly] = pal[idx]
    return out


def compute_psnr(img_a: Image.Image, img_b: Image.Image) -> float:
    a = img_a.convert("RGB").tobytes()
    b = img_b.convert("RGB").tobytes()
    mse = sum((x - y) ** 2 for x, y in zip(a, b)) / float(len(a))
    if mse <= 1e-12:
        return 99.99
    return 10.0 * math.log10((255.0 * 255.0) / mse)


def quantize_rgb565_image(img: Image.Image) -> Image.Image:
    w, h = img.size
    src = img.convert("RGB")
    dst = Image.new("RGB", (w, h))
    sp = src.load()
    dp = dst.load()
    for y in range(h):
        for x in range(w):
            dp[x, y] = rgb565_to_rgb888(rgb888_to_rgb565(*sp[x, y]))
    return dst


def generate_demo_wallpaper(w: int, h: int) -> Image.Image:
    """Generate a dark cyber-instrument gradient wallpaper with subtle grid & glow."""
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        ny = y / float(max(1, h - 1))
        for x in range(w):
            nx = x / float(max(1, w - 1))
            # Deep navy-slate background + subtle cyan/amber radial accents
            r = 10.0 + 28.0 * (nx ** 1.6) * (1.0 - ny * 0.5)
            g = 18.0 + 42.0 * (1.0 - abs(nx - 0.35)) * (1.0 - ny * 0.4)
            b = 34.0 + 68.0 * (1.0 - nx * 0.4) * (0.4 + 0.6 * ny)

            # Soft radial glow in upper-right corner
            dx = (nx - 0.82) * 1.6
            dy = (ny - 0.22)
            glow = math.exp(-(dx * dx + dy * dy) * 7.5)
            r += 95.0 * glow
            g += 48.0 * glow
            b += 18.0 * glow

            # Subtle instrument grid lines
            if (x % 20 == 0) or (y % 20 == 0):
                r += 10.0
                g += 16.0
                b += 22.0

            # Thin top accent bar
            if y < 2:
                r = 20.0 + 180.0 * nx
                g = 200.0 - 90.0 * nx
                b = 240.0 - 120.0 * nx

            px[x, y] = (clamp_u8(r), clamp_u8(g), clamp_u8(b))
    return img


def write_c_header(path: str, symbol: str, w: int, h: int, data: bytes,
                   psnr_888: float, psnr_565: float) -> None:
    guard = f"{symbol.upper()}_H"
    raw_bytes = w * h * 2
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        '#include "bc1_decode.h"',
        "",
        f"/* Auto-generated by Tools/img2bc1.py */",
        f"/* Resolution: {w}x{h} | Raw RGB565: {raw_bytes} B -> BC1: {len(data)} B (4:1) */",
        f"/* Quality: PSNR vs RGB888 = {psnr_888:.2f} dB, vs RGB565 = {psnr_565:.2f} dB */",
        "",
        f"static const uint8_t {symbol}_blocks[{len(data)}] = {{",
    ]
    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02X}U" for b in data[i:i + 16])
        comma = "," if (i + 16) < len(data) else ""
        lines.append(f"    {chunk}{comma}")
    lines.extend([
        "};",
        "",
        f"static const bc1_image_t {symbol} = {{",
        f"    {w}U,",
        f"    {h}U,",
        f"    {symbol}_blocks",
        "};",
        "",
        f"#endif /* {guard} */",
        "",
    ])
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser(description="Encode image to BC1/DXT1 C header for PY32 TFT.")
    parser.add_argument("input", nargs="?", help="Input image path (PNG/JPG/BMP). Omit if using --demo.")
    parser.add_argument("--demo", action="store_true", help="Generate built-in dark cyber wallpaper.")
    parser.add_argument("--width", type=int, default=160, help="Target width (multiple of 4, default 160).")
    parser.add_argument("--height", type=int, default=80, help="Target height (multiple of 4, default 80).")
    parser.add_argument("--dim", type=float, default=1.0,
                        help="Brightness factor (e.g. 0.45 for dark background behind bright text).")
    parser.add_argument("--stretch", action="store_true",
                        help="Resize directly to WxH without cropping (ideal for UI bezel templates).")
    parser.add_argument("--symbol", default="g_bg_wallpaper", help="C symbol name (default g_bg_wallpaper).")
    parser.add_argument("--out-c", required=True, help="Output C header path.")
    parser.add_argument("--preview", help="Optional side-by-side PNG preview (Original | BC1 Decoded).")
    args = parser.parse_args()

    if args.demo:
        img = generate_demo_wallpaper(args.width, args.height)
    else:
        if not args.input:
            parser.error("Provide an input image path or pass --demo.")
        img = Image.open(args.input).convert("RGB")
        if args.stretch:
            img = img.resize((args.width, args.height), Image.Resampling.LANCZOS)
        else:
            img = ImageOps.fit(img, (args.width, args.height), method=Image.Resampling.LANCZOS)

    if args.dim != 1.0:
        img = img.point(lambda v: clamp_u8(v * args.dim))

    bc1_bytes = encode_image(img)
    decoded = decode_image(bc1_bytes, args.width, args.height)
    ref_565 = quantize_rgb565_image(img)

    psnr_888 = compute_psnr(img, decoded)
    psnr_565 = compute_psnr(ref_565, decoded)

    write_c_header(args.out_c, args.symbol, args.width, args.height, bc1_bytes, psnr_888, psnr_565)

    if args.preview:
        os.makedirs(os.path.dirname(os.path.abspath(args.preview)), exist_ok=True)
        # Upscale 3x nearest-neighbor so the 160x80 comparison is easy to inspect on PC
        scale = 3
        comp = Image.new("RGB", (args.width * 2 * scale + 12, args.height * scale), (24, 24, 24))
        comp.paste(img.resize((args.width * scale, args.height * scale), Image.Resampling.NEAREST), (0, 0))
        comp.paste(decoded.resize((args.width * scale, args.height * scale), Image.Resampling.NEAREST),
                   (args.width * scale + 12, 0))
        comp.save(args.preview)

    raw_size = args.width * args.height * 2
    print(f"[BC1] {args.width}x{args.height}: Raw RGB565={raw_size}B -> BC1={len(bc1_bytes)}B "
          f"({len(bc1_bytes)/1024.0:.2f}KB, 4.00x)")
    print(f"[BC1] PSNR vs RGB888 = {psnr_888:.2f} dB | PSNR vs RGB565 = {psnr_565:.2f} dB")
    print(f"[BC1] Header written: {args.out_c}")
    if args.preview:
        print(f"[BC1] Preview written: {args.preview}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
