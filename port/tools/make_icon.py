"""Generate original TTT2 community icon art using only the Python standard library."""
import struct
import sys

SIZES = (16, 24, 32, 48, 64, 128, 256)
FONT = {
    "T": ("11111", "00100", "00100", "00100", "00100", "00100", "00100"),
    "2": ("11111", "10001", "00001", "00110", "01000", "10000", "11111"),
}


def icon_pixels(size):
    pixels = bytearray(size * size * 4)
    scale = size / 256.0
    cx = cy = (size - 1) / 2.0
    radius = 45 * scale

    def blend(x, y, color):
        if not (0 <= x < size and 0 <= y < size):
            return
        dx = max(8 * scale - x, 0, x - (size - 1 - 8 * scale))
        dy = max(8 * scale - y, 0, y - (size - 1 - 8 * scale))
        if dx and dy and dx * dx + dy * dy > radius * radius:
            return
        at = (y * size + x) * 4
        a = color[3]
        for i in range(3):
            pixels[at + i] = (pixels[at + i] * (255 - a) + color[i] * a) // 255
        pixels[at + 3] = 255

    # Deep metallic charcoal base.
    for y in range(size):
        t = y / max(size - 1, 1)
        for x in range(size):
            dx = max(8 * scale - x, 0, x - (size - 1 - 8 * scale))
            dy = max(8 * scale - y, 0, y - (size - 1 - 8 * scale))
            if dx and dy and dx * dx + dy * dy > radius * radius:
                continue
            at = (y * size + x) * 4
            pixels[at:at + 4] = bytes((int(12 + 9*t), int(17 + 12*t), int(27 + 19*t), 255))

    # Red and blue energy streaks, clipped to the rounded tile.
    for y in range(size):
        for x in range(size):
            nx, ny = x / scale, y / scale
            red_dist = abs((nx - 14) - (183 - ny) * (214 / 159))
            blue_dist = abs((nx - 26) - (231 - ny) * (209 / 185))
            if 24 <= ny <= 228 and red_dist < 7:
                blend(x, y, (218, 47, 60, 205))
            if 48 <= ny <= 246 and blue_dist < 6:
                blend(x, y, (53, 158, 207, 215))

    # Pixel-grid T2 mark. At small icon sizes the heavy, clean glyph remains legible.
    glyph_scale = max(1, int(13 * scale))
    gap = max(1, int(7 * scale))
    total_w = glyph_scale * 11 + gap
    total_h = glyph_scale * 7
    left = int(cx - total_w / 2)
    top = int(cy - total_h / 2)
    for char_index, char in enumerate("T2"):
        for row, line in enumerate(FONT[char]):
            for col, bit in enumerate(line):
                if bit != "1":
                    continue
                x0 = left + char_index * (glyph_scale * 6 + gap) + col * glyph_scale
                y0 = top + row * glyph_scale
                for y in range(y0, y0 + glyph_scale):
                    for x in range(x0, x0 + glyph_scale):
                        blend(x + max(1, int(1 * scale)), y + max(1, int(2 * scale)), (0, 0, 0, 190))
                        blend(x, y, (246, 249, 252, 255))

    # Fine inset edge.
    edge = max(1, round(scale * 3))
    for x in range(size):
        for d in range(edge):
            blend(x, 8 + d, (225, 233, 244, 90))
            blend(x, size - 9 - d, (225, 233, 244, 90))
    for y in range(size):
        for d in range(edge):
            blend(8 + d, y, (225, 233, 244, 90))
            blend(size - 9 - d, y, (225, 233, 244, 90))
    return pixels


def dib_image(size):
    rgba = icon_pixels(size)
    xor = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            i = (y * size + x) * 4
            r, g, b, a = rgba[i:i + 4]
            xor.extend((b, g, r, a))
    mask_stride = ((size + 31) // 32) * 4
    and_mask = bytes(mask_stride * size)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, len(xor), 0, 0, 0, 0)
    return header + xor + and_mask


def main():
    out = sys.argv[1]
    images = [(s, dib_image(s)) for s in SIZES]
    directory = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        directory.extend(struct.pack("<BBBBHHII", size if size < 256 else 0, size if size < 256 else 0,
                                     0, 0, 1, 32, len(data), offset))
        offset += len(data)
    with open(out, "wb") as f:
        f.write(directory)
        for _, data in images:
            f.write(data)
    print("wrote", out)


if __name__ == "__main__":
    main()
