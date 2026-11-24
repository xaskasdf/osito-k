#!/usr/bin/env python3
# Minimal PPM (P6) -> PNG converter, pure stdlib (zlib + struct), no PIL.
# Usage: ppm2png.py in.ppm out.png [max_w]
import sys, zlib, struct

def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    assert data[:2] == b"P6", "not a P6 PPM"
    # parse header: P6 <w> <h> <maxval>, whitespace-separated, then one byte then pixels
    idx = 2
    vals = []
    while len(vals) < 3:
        # skip whitespace
        while data[idx:idx+1].isspace():
            idx += 1
        if data[idx:idx+1] == b"#":
            while data[idx:idx+1] not in (b"\n", b""):
                idx += 1
            continue
        start = idx
        while not data[idx:idx+1].isspace():
            idx += 1
        vals.append(int(data[start:idx]))
    w, h, maxv = vals
    idx += 1  # single whitespace after maxval
    pix = data[idx:idx + w*h*3]
    return w, h, pix

def write_png(path, w, h, pix, stride_div=1):
    # optional downscale by integer factor stride_div (nearest)
    if stride_div > 1:
        nw, nh = w // stride_div, h // stride_div
        out = bytearray()
        for y in range(nh):
            sy = y * stride_div
            row = bytearray()
            for x in range(nw):
                sx = x * stride_div
                o = (sy*w + sx)*3
                row += pix[o:o+3]
            out += b"\x00" + row
        w, h = nw, nh
        raw = bytes(out)
    else:
        out = bytearray()
        for y in range(h):
            o = y*w*3
            out += b"\x00" + pix[o:o+w*3]
        raw = bytes(out)
    def chunk(typ, body):
        c = typ + body
        return struct.pack(">I", len(body)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)

if __name__ == "__main__":
    inp, outp = sys.argv[1], sys.argv[2]
    div = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    w, h, pix = read_ppm(inp)
    write_png(outp, w, h, pix, div)
    print(f"{inp}: {w}x{h} -> {outp} (div={div})")
