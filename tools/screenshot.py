#!/usr/bin/env python3
"""Grab a frame from a WikiStats panel and write it as a PNG.

    python tools/screenshot.py 192.168.1.34 panel.png

The panel streams the frame straight out of its LVGL flush callback (see
firmware/src/net/screenshot.cpp) because a full 320x240 RGB565 frame is 150 KiB and
the board has nothing like that free. The wire format is a tiny header followed by
raw pixels:

    "WSS1"  width:u16  height:u16  pixels:u16[width*height]   (all little-endian)

Only the standard library is used - zlib and struct are enough to write a PNG, and
this has to run anywhere the firmware is being developed.
"""

from __future__ import annotations

import argparse
import socket
import struct
import sys
import zlib

MAGIC = b"WSS1"
DEFAULT_PORT = 24
HEADER_BYTES = 8


def receive_exact(sock: socket.socket, count: int) -> bytes:
    chunks: list[bytes] = []
    remaining = count
    while remaining > 0:
        chunk = sock.recv(min(remaining, 65536))
        if not chunk:
            raise ConnectionError(f"connection closed with {remaining} bytes still expected")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def capture(host: str, port: int, timeout: float) -> tuple[int, int, bytes]:
    with socket.create_connection((host, port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        header = receive_exact(sock, HEADER_BYTES)
        if header[:4] == b"BUSY":
            raise RuntimeError("the panel is already serving another capture")
        if header[:4] != MAGIC:
            raise RuntimeError(f"unexpected header {header[:4]!r}")
        width, height = struct.unpack_from("<HH", header, 4)
        pixels = receive_exact(sock, width * height * 2)
    return width, height, pixels


def rgb565_to_rgb888(pixels: bytes, width: int, height: int) -> bytes:
    """Expand to 8 bits per channel, replicating high bits so white stays white."""
    rows: list[bytes] = []
    for y in range(height):
        row = bytearray()
        row.append(0)  # PNG per-scanline filter: none
        base = y * width * 2
        for x in range(width):
            value = pixels[base + x * 2] | (pixels[base + x * 2 + 1] << 8)
            red = (value >> 11) & 0x1F
            green = (value >> 5) & 0x3F
            blue = value & 0x1F
            row.append((red << 3) | (red >> 2))
            row.append((green << 2) | (green >> 4))
            row.append((blue << 3) | (blue >> 2))
        rows.append(bytes(row))
    return b"".join(rows)


def write_png(path: str, width: int, height: int, raw_rgb: bytes) -> None:
    def chunk(kind: bytes, payload: bytes) -> bytes:
        return (
            struct.pack(">I", len(payload))
            + kind
            + payload
            + struct.pack(">I", zlib.crc32(kind + payload) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit truecolour
    with open(path, "wb") as handle:
        handle.write(b"\x89PNG\r\n\x1a\n")
        handle.write(chunk(b"IHDR", header))
        handle.write(chunk(b"IDAT", zlib.compress(raw_rgb, 9)))
        handle.write(chunk(b"IEND", b""))


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture a WikiStats panel screen as PNG")
    parser.add_argument("host", help="panel address or hostname")
    parser.add_argument("output", nargs="?", default="panel.png", help="PNG file to write")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=15.0)
    args = parser.parse_args()

    try:
        width, height, pixels = capture(args.host, args.port, args.timeout)
    except (OSError, RuntimeError) as exc:
        print(f"capture failed: {exc}", file=sys.stderr)
        return 1

    write_png(args.output, width, height, rgb565_to_rgb888(pixels, width, height))
    print(f"wrote {args.output} ({width}x{height})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
