#!/usr/bin/env python3
"""
t3s_to_atlas.py - gd3ds SDL2 port, phase 2 asset converter.

Reads every `.t3s` source list under gfx/, packs the referenced PNG files
into portable raw-atlas containers, and writes them to the assets dir.

Index semantics (verified against the game data):
  entry i of <name>.t3s (lines starting with '-' are tex3ds option flags)
  maps to subtexture index i, exactly the value the game passes to
  C2D_SpriteFromSheet(sheet, i). Evidence:
    - romfs/gfx/<name>.t3x first u16 (little endian) == number of PNG lines
      (sprites 819==819, ui 428==428, chatFont 94==94, bigFont 95==95,
       goldFont 95==95)
    - fonts/bigFont.c & fonts/chatFont.c glyph tables carry an explicit
      `spriteIndex`; ascii 33 ('!') -> spriteIndex 0 and their .t3s list
      `*_glyph_33.png` first.

Output container (little endian, one file per sheet):
  u32 magic 'T3SA'
  u32 version (1)
  u32 numPages
  u32 numEntries
  per page:  u32 pageWidth, u32 pageHeight      (used bounding box)
  per entry: u16 page, u16 x, u16 y, u16 w, u16 h   (10 bytes)
  then raw RGBA8888 rows, pages concatenated in page order.

Run:
  python3 platform/sdl/tools/t3s_to_atlas.py [--dest DIR] [--force]
Incremental: a sheet rebuilds only if its .t3s (or a referenced PNG) is
newer than the generated .atlas.
"""

import argparse
import glob
import os
import struct
import sys
import zlib

MAX_PAGE = 2048          # working page side; largest source PNG is 512x512
THE_MAGIC = 0x41533354   # bytes 'T','3','S','A'
VERSION = 1

# ---------------------------------------------------------------- PNG decode

_PNG_SIG = b"\x89PNG\r\n\x1a\n"
_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


class PngError(Exception):
    pass


def _paeth(a, b, c):
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def png_decode(path):
    """Decode a PNG into (width, height, bytes RGBA8888 row-major)."""
    with open(path, "rb") as f:
        data = f.read()

    if data[:8] != _PNG_SIG:
        raise PngError(f"{path}: not a PNG")

    ihdr = plte = trns = None
    idat = bytearray()
    pos = 8
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        payload = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            ihdr = payload[:13]
        elif ctype == b"PLTE":
            plte = payload
        elif ctype == b"tRNS":
            trns = payload
        elif ctype == b"IDAT":
            idat += payload
        elif ctype == b"IEND":
            break

    if ihdr is None or not idat:
        raise PngError(f"{path}: missing IHDR/IDAT")

    w, h, depth, color, _comp, _filt, interlace = struct.unpack(">IIBBBBB", ihdr)
    if w == 0 or h == 0:
        raise PngError(f"{path}: bad size {w}x{h}")
    if interlace != 0:
        raise PngError(f"{path}: interlaced PNG not supported")
    channels = _CHANNELS.get(color)
    if channels is None:
        raise PngError(f"{path}: unsupported color type {color}")

    raw = zlib.decompress(bytes(idat))
    bpp = max(1, channels * depth // 8)
    stride = (w * channels * depth + 7) // 8
    if len(raw) < h * (stride + 1):
        raise PngError(f"{path}: truncated pixel data")

    out = bytearray(w * h * 4)
    prev = bytearray(stride)
    off = 0
    for y in range(h):
        ftype = raw[off]
        row = bytearray(raw[off + 1:off + 1 + stride])
        off += stride + 1

        if ftype == 1:    # Sub
            for i in range(bpp, stride):
                row[i] = (row[i] + row[i - bpp]) & 0xFF
        elif ftype == 2:  # Up
            if y != 0:
                for i in range(stride):
                    row[i] = (row[i] + prev[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up = prev[i] if y != 0 else 0
                row[i] = (row[i] + ((left + up) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                left = row[i - bpp] if i >= bpp else 0
                up = prev[i] if y != 0 else 0
                ul = prev[i - bpp] if (y != 0 and i >= bpp) else 0
                row[i] = (row[i] + _paeth(left, up, ul)) & 0xFF
        elif ftype != 0:
            raise PngError(f"{path}: unknown filter {ftype}")

        prev = row
        _extract_row(row, y, w, depth, color, channels, plte, trns, out)

    return w, h, bytes(out)


def _extract_row(row, y, w, depth, color, channels, plte, trns, out):
    """Write one unfiltered scanline into the RGBA output buffer."""
    o = y * w * 4
    ps = channels if depth == 8 else (channels * 2 if depth == 16 else None)

    def sample(idx, chan):
        if ps is not None:
            return row[idx * ps + chan * (ps // channels)]
        per = 8 // depth
        byte = row[(idx * depth) >> 3]
        shift = 8 - depth * ((idx % per) + 1)
        return (byte >> shift) & ((1 << depth) - 1)

    for i in range(w):
        o1 = o + i * 4
        if color == 6:              # RGBA
            if depth == 8:
                out[o1:o1 + 4] = row[i * 4:i * 4 + 4]
            elif depth == 16:       # high byte of each channel
                p = i * 8
                out[o1] = row[p]
                out[o1 + 1] = row[p + 2]
                out[o1 + 2] = row[p + 4]
                out[o1 + 3] = row[p + 6]
            else:
                raise PngError(f"depth {depth} unsupported for RGBA")
        elif color == 4:            # LA
            lum = row[i * 4] if depth == 16 else sample(i, 0)
            alp = row[i * 4 + 2] if depth == 16 else sample(i, 1)
            out[o1] = out[o1 + 1] = out[o1 + 2] = lum
            out[o1 + 3] = alp
        elif color == 0:            # gray (optional tRNS key)
            g = row[i * 2] if depth == 16 else sample(i, 0)
            a = 255
            if trns:
                key = (trns[0] << 8 | trns[1]) if depth == 16 else trns[0]
                if g == key:
                    a = 0
            out[o1] = out[o1 + 1] = out[o1 + 2] = g
            out[o1 + 3] = a
        elif color == 2:            # RGB (optional 16-bit tRNS key)
            if depth == 16:
                r, g, b = row[i * 6], row[i * 6 + 2], row[i * 6 + 4]
            else:
                r, g, b = sample(i, 0), sample(i, 1), sample(i, 2)
            a = 255
            if trns and len(trns) >= 6:
                keys = struct.unpack(">HHH", trns[:6])
                if (r, g, b) == keys:
                    a = 0
            out[o1] = r
            out[o1 + 1] = g
            out[o1 + 2] = b
            out[o1 + 3] = a
        elif color == 3:            # palette
            idx = sample(i, 0)
            base = idx * 3
            out[o1] = plte[base]
            out[o1 + 1] = plte[base + 1]
            out[o1 + 2] = plte[base + 2]
            out[o1 + 3] = trns[idx] if trns and idx < len(trns) else 255
        else:
            raise PngError(f"unhandled color type {color}")


# ---------------------------------------------------------------- atlas

def build_sheet(t3s_path, out_path, force=False):
    """Pack one .t3s. Returns 'built' | 'fresh' | 'error'."""
    base_dir = os.path.dirname(t3s_path)

    files = []
    with open(t3s_path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("-") or line.startswith("#"):
                continue
            files.append(line)

    if not files:
        return "error"

    if os.path.exists(out_path) and not force:
        newest = os.path.getmtime(t3s_path)
        for rel in files:
            p = os.path.join(base_dir, rel)
            if not os.path.exists(p):
                print(f"ERROR: {t3s_path}: missing PNG '{rel}'", file=sys.stderr)
                return "error"
            newest = max(newest, os.path.getmtime(p))
        if newest <= os.path.getmtime(out_path):
            return "fresh"

    # -- packing: shelf packer, entries appended strictly in file order ----
    # working page state: [RGBA rows buffer (stride MAX_PAGE), cur_x, cur_y,
    #                      cur_row_height]
    pages = []
    entries = []
    for idx, rel in enumerate(files):
        w, h, rgba = png_decode(os.path.join(base_dir, rel))
        if w > MAX_PAGE or h > MAX_PAGE:
            print(f"ERROR: {t3s_path}: '{rel}' is {w}x{h} (limit {MAX_PAGE})",
                  file=sys.stderr)
            return "error"

        if not pages:
            pages.append([bytearray(MAX_PAGE * MAX_PAGE * 4), 0, 0, 0])
        cur = pages[-1]
        px, py, row_h = cur[1], cur[2], cur[3]

        if px + w > MAX_PAGE:                     # wrap to next row
            px, py, row_h = 0, py + row_h, 0
        if py + h > MAX_PAGE:                     # page full -> new page
            cur = [bytearray(MAX_PAGE * MAX_PAGE * 4), 0, 0, 0]
            pages.append(cur)
            px, py, row_h = 0, 0, 0

        buf = cur[0]
        for r in range(h):
            dst = (py + r) * MAX_PAGE * 4 + px * 4
            src = r * w * 4
            buf[dst:dst + w * 4] = rgba[src:src + w * 4]

        entries.append((len(pages) - 1, px, py, w, h))
        cur[1] = px + w
        cur[2] = py
        cur[3] = max(row_h, h)

    # -- compaction: crop every working page to its used bounding box -----
    page_blobs = []
    page_dims = []
    for p, page in enumerate(pages):
        page_entries = [e for e in entries if e[0] == p]
        used_w = max(x + w for (_, x, y, w, h) in page_entries)
        used_h = max(y + h for (_, x, y, w, h) in page_entries)
        compact = bytearray(used_w * used_h * 4)
        for (_, x, y, w, h) in page_entries:
            for r in range(h):
                dst = (y + r) * used_w * 4 + x * 4
                src = (y + r) * MAX_PAGE * 4 + x * 4
                compact[dst:dst + w * 4] = page[0][src:src + w * 4]
        page_blobs.append(bytes(compact))
        page_dims.append((used_w, used_h))

    # -- write ------------------------------------------------------------
    with open(out_path, "wb") as f:
        f.write(struct.pack("<IIII", THE_MAGIC, VERSION,
                            len(page_blobs), len(entries)))
        for (pw, ph) in page_dims:
            f.write(struct.pack("<II", pw, ph))
        for (pg, x, y, w, h) in entries:
            f.write(struct.pack("<HHHHH", pg, x, y, w, h))
        for blob in page_blobs:
            f.write(blob)
    return "built"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dest", default="platform/sdl/assets_build")
    ap.add_argument("--force", action="store_true")
    args = ap.parse_args()

    if not os.path.isdir("gfx"):
        print("ERROR: run from the repo root (gfx/ not found)", file=sys.stderr)
        return 1

    os.makedirs(args.dest, exist_ok=True)
    built = fresh = failed = 0
    for t3s in sorted(glob.glob("gfx/**/*.t3s", recursive=True)):
        name = os.path.splitext(os.path.basename(t3s))[0]
        out_path = os.path.join(args.dest, name + ".atlas")
        try:
            status = build_sheet(t3s, out_path, args.force)
        except Exception as exc:                                    # noqa
            print(f"ERROR: {t3s}: {exc}", file=sys.stderr)
            status = "error"
        if status == "fresh":
            fresh += 1
        elif status == "built":
            built += 1
        else:
            failed += 1
    print(f"assets: built={built} fresh={fresh} failed={failed} -> {args.dest}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
