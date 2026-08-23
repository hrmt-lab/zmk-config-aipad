#!/usr/bin/env python3
"""Generate the 96x96 Claude Code ScreenKey logo as an LVGL RGB565 image.

Mirrors the output contract of tools/generate-codex-logo.ps1: the source is
composited over black, mapped to the ScreenKey's upright orientation (90 degrees
counter-clockwise from the source asset), emitted as little-endian RGB565 and
wrapped in an lv_image_dsc_t.

The Codex asset is 640x640 and needs the resampler from the PowerShell script;
this generator refuses anything that is not already 96x96 so no second
resampling implementation enters the repository.
"""

import pathlib
import struct
import sys
import zlib

SIZE = 96
BYTES_PER_LINE = 16


def read_rgba_png(path):
    """Decode an 8-bit RGBA non-interlaced PNG into a flat bytearray."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG file")

    header = None
    idat = bytearray()
    offset = 8
    while offset < len(data):
        (length,) = struct.unpack(">I", data[offset:offset + 4])
        chunk_type = data[offset + 4:offset + 8]
        payload = data[offset + 8:offset + 8 + length]
        offset += 12 + length

        if chunk_type == b"IHDR":
            header = struct.unpack(">IIBBBBB", payload)
        elif chunk_type == b"IDAT":
            idat += payload
        elif chunk_type == b"IEND":
            break

    if header is None:
        raise ValueError(f"{path}: missing IHDR")

    width, height, depth, color_type, compression, filter_method, interlace = header
    if (depth, color_type, compression, filter_method, interlace) != (8, 6, 0, 0, 0):
        raise ValueError(
            f"{path}: expected 8-bit RGBA non-interlaced PNG, got depth={depth} "
            f"color_type={color_type} interlace={interlace}"
        )
    if (width, height) != (SIZE, SIZE):
        raise ValueError(f"{path}: expected {SIZE}x{SIZE} source, got {width}x{height}")

    raw = zlib.decompress(bytes(idat))
    stride = width * 4
    if len(raw) != (stride + 1) * height:
        raise ValueError(f"{path}: unexpected decompressed size {len(raw)}")

    pixels = bytearray(stride * height)
    previous = bytearray(stride)
    pos = 0
    for row in range(height):
        filter_type = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride

        for index in range(stride):
            left = line[index - 4] if index >= 4 else 0
            up = previous[index]
            up_left = previous[index - 4] if index >= 4 else 0

            if filter_type == 0:
                value = line[index]
            elif filter_type == 1:
                value = line[index] + left
            elif filter_type == 2:
                value = line[index] + up
            elif filter_type == 3:
                value = line[index] + (left + up) // 2
            elif filter_type == 4:
                estimate = left + up - up_left
                distance_left = abs(estimate - left)
                distance_up = abs(estimate - up)
                distance_up_left = abs(estimate - up_left)
                if distance_left <= distance_up and distance_left <= distance_up_left:
                    predictor = left
                elif distance_up <= distance_up_left:
                    predictor = up
                else:
                    predictor = up_left
                value = line[index] + predictor
            else:
                raise ValueError(f"{path}: unsupported filter type {filter_type}")

            line[index] = value & 0xFF

        pixels[row * stride:(row + 1) * stride] = line
        previous = line

    return pixels


def to_rgb565_bytes(pixels):
    """Composite over black, rotate 90 degrees CCW and pack little-endian RGB565."""
    out = bytearray()
    for y in range(SIZE):
        for x in range(SIZE):
            # The ScreenKey's physical upright orientation is 90 degrees
            # counter-clockwise from the source asset. Map each output pixel
            # back to the source image.
            index = (x * SIZE + (SIZE - 1 - y)) * 4
            red, green, blue, alpha = pixels[index:index + 4]

            # Source-over composite against a black background.
            red = (red * alpha + 127) // 255
            green = (green * alpha + 127) // 255
            blue = (blue * alpha + 127) // 255

            rgb565 = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
            out.append(rgb565 & 0xFF)
            out.append((rgb565 >> 8) & 0xFF)

    return out


def render_source(data):
    lines = ['#include "claude_code_logo.h"', "", "static const uint8_t claude_code_logo_map[] = {"]
    for offset in range(0, len(data), BYTES_PER_LINE):
        chunk = data[offset:offset + BYTES_PER_LINE]
        lines.append("    " + ", ".join(f"0x{byte:02x}" for byte in chunk) + ",")
    lines += [
        "};",
        "",
        "const lv_image_dsc_t screenkey_claude_code_logo = {",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.cf = LV_COLOR_FORMAT_RGB565,",
        "    .header.flags = 0,",
        f"    .header.w = {SIZE},",
        f"    .header.h = {SIZE},",
        f"    .header.stride = {SIZE * 2},",
        "    .header.reserved_2 = 0,",
        "    .data_size = sizeof(claude_code_logo_map),",
        "    .data = claude_code_logo_map,",
        "};",
    ]
    return "\n".join(lines) + "\n"


def main():
    repository_root = pathlib.Path(__file__).resolve().parent.parent
    source_path = repository_root / "assets/claude_code_screenkey_96.png"
    output_path = (
        repository_root / "boards/shields/aipad/src/claude_code_logo.c"
    )

    if not source_path.is_file():
        raise SystemExit(f"Logo source not found: {source_path}")

    data = to_rgb565_bytes(read_rgba_png(source_path))
    output_path.write_text(render_source(data), encoding="utf-8", newline="\n")
    print(f"Generated: {output_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
