#!/usr/bin/env python3
"""Builds a translation patch (.wpatch) from a mod's replacement files.

The patch holds, for each file the mod replaces in VOLUME.DAT, a BPS delta
against the original entry: only what changed (the translated text) goes in,
never the game's own data.  The installer (src/setup) applies it to the
player's own disc to make the mod's files again.

    make_translation_patch.py VOLUME.DAT mods/ptbr/files out/ptbr.wpatch

Format (little-endian):
    "WS2PATCH"  u32 version (2)  u32 count
    count × { u8 kind, u16 path length, path (UTF-8, as in the archive: "Us/…"),
              [kind 2: u32 size, u32 CRC-32 of the file made]
              u32 BPS length, BPS }
kind 1: the BPS turns the archive entry into the mod's file.
kind 2: task/alltask, whose tasks are each LZSS-compressed: the BPS works on
    the tasks unpacked (for each task, u32 length or 0xFFFFFFFF for none, then
    its bytes), and the installer packs them again with the same compressor
    (lzss_compress below, as tools/repack_task_archive.py in the translation).
"""
import pathlib
import struct
import sys
import zlib


def name_hash(raw):
    value = 0
    for byte in raw:
        if byte == 0x2F:
            byte = 0x5C
        elif 0x61 <= byte <= 0x7A:
            byte -= 0x20
        if byte > 127:
            byte -= 256
        value = (byte + value * 19) & 0xFFFFFFFF
    return value


def read_volume(path):
    data = path.read_bytes()
    magic, count, _count2, data_start, _total = struct.unpack_from(">4sIIII", data, 0)
    if magic != b"\xfa\xde\xba\xbe":
        raise SystemExit(f"{path}: not a VOLUME.DAT")
    entries = {}
    for i in range(count):
        h, offset, size, flags, stored_end, name_len = struct.unpack_from(">6I", data, 20 + i * 24)
        if not (h or offset or size or flags or stored_end or name_len):
            continue
        entries[h] = (offset, size, flags, stored_end)
    return data, data_start, entries


def entry_bytes(volume, path):
    data, data_start, entries = volume
    e = entries.get(name_hash(path.encode("utf-8")))
    if e is None:
        return None
    offset, size, flags, stored_end = e
    if flags & 6:
        raise SystemExit(f"{path}: compressed in the archive (not supported by this tool)")
    return data[data_start + offset:data_start + offset + size]


def number(value):
    out = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value == 0:
            out.append(byte | 0x80)
            return bytes(out)
        out.append(byte)
        value -= 1


def signed(value):
    return number((-value) << 1 | 1 if value < 0 else value << 1)


KEY = 6


# ── task/alltask ──────────────────────────────────────────────────────────────
def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def lzss_decompress(chunk):
    packed_total, output_size = struct.unpack_from("<2I", chunk, 0)
    encoded = chunk[8:packed_total]
    out = bytearray()
    window = bytearray(0x1000)
    wpos, pos, flags = 0xFEE, 0, 0
    while len(out) < output_size:
        flags >>= 1
        if not flags & 0x100:
            flags = encoded[pos] | 0xFF00
            pos += 1
        if flags & 1:
            byte = encoded[pos]
            pos += 1
            out.append(byte)
            window[wpos] = byte
            wpos = (wpos + 1) & 0xFFF
        else:
            lo, hi = encoded[pos], encoded[pos + 1]
            pos += 2
            start = lo | ((hi & 0xF0) << 4)
            run = min((hi & 0x0F) + 3, output_size - len(out))
            for k in range(run):
                byte = window[(start + k) & 0xFFF]
                out.append(byte)
                window[wpos] = byte
                wpos = (wpos + 1) & 0xFFF
    return bytes(out)


def unpack_tasks(blob):
    """The tasks of an alltask, unpacked, in one block (see the format above)."""
    count, alignment, start, _reserved = struct.unpack_from("<4I", blob, 0)
    out = bytearray()
    offset = 0
    for i in range(count):
        size, = struct.unpack_from("<I", blob, 0x10 + i * 4)
        if size == 0:
            out += struct.pack("<I", 0xFFFFFFFF)
        else:
            at = start + offset
            data = lzss_decompress(blob[at:min(at + size + 16, len(blob))])
            out += struct.pack("<I", len(data)) + data
        offset += align_up(size, alignment)
    return bytes(out)


def bps(source, target):
    """A BPS patch: SourceRead where the bytes stay put, SourceCopy for what
    moved, TargetRead for what is new."""
    out = bytearray(b"BPS1")
    out += number(len(source)) + number(len(target)) + number(0)
    index = {}
    for i in range(len(source) - KEY + 1):
        index.setdefault(source[i:i + KEY], []).append(i)
    pos = 0
    literal_from = 0
    source_rel = 0
    n = len(target)

    def flush(upto):
        nonlocal literal_from
        if upto > literal_from:
            length = upto - literal_from
            out.extend(number((length - 1) << 2 | 1))
            out.extend(target[literal_from:upto])
        literal_from = upto

    while pos < n:
        # The same bytes in the same place.
        run = 0
        while pos + run < n and pos + run < len(source) and source[pos + run] == target[pos + run]:
            run += 1
        if run >= 4:
            flush(pos)
            out.extend(number((run - 1) << 2 | 0))
            pos += run
            literal_from = pos
            continue
        # The same bytes elsewhere in the original.
        best_len, best_at = 0, 0
        for cand in index.get(target[pos:pos + KEY], ())[-48:]:
            length = KEY
            while pos + length < n and cand + length < len(source) and source[cand + length] == target[pos + length]:
                length += 1
            if length > best_len or (length == best_len and abs(cand - source_rel) < abs(best_at - source_rel)):
                best_len, best_at = length, cand
        if best_len >= KEY:
            flush(pos)
            out.extend(number((best_len - 1) << 2 | 2))
            out.extend(signed(best_at - source_rel))
            source_rel = best_at + best_len
            pos += best_len
            literal_from = pos
            continue
        pos += 1
    flush(n)
    out += struct.pack("<II", zlib.crc32(source) & 0xFFFFFFFF, zlib.crc32(target) & 0xFFFFFFFF)
    out += struct.pack("<I", zlib.crc32(bytes(out)) & 0xFFFFFFFF)
    return bytes(out)


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    volume = read_volume(pathlib.Path(sys.argv[1]))
    root = pathlib.Path(sys.argv[2])
    files = sorted(p for p in root.rglob("*") if p.is_file())
    blob = bytearray(b"WS2PATCH" + struct.pack("<II", 2, len(files)))
    copied = new = 0
    for p in files:
        rel = p.relative_to(root).as_posix()
        target = p.read_bytes()
        source = entry_bytes(volume, rel)
        if source is None:
            raise SystemExit(f"{rel}: not in the archive (a patch only changes the game's own files)")
        path = rel.encode("utf-8")
        if rel.lower() == "us/task/alltask":
            patch = bps(unpack_tasks(source), unpack_tasks(target))
            blob += struct.pack("<BH", 2, len(path)) + path
            blob += struct.pack("<II", len(target), zlib.crc32(target) & 0xFFFFFFFF)
        else:
            patch = bps(source, target)
            blob += struct.pack("<BH", 1, len(path)) + path
        blob += struct.pack("<I", len(patch)) + patch
        new += len(patch)
        copied += len(target)
        print(f"{rel}: {len(target)} bytes -> patch {len(patch)}")
    out = pathlib.Path(sys.argv[3])
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(bytes(blob))
    print(f"{len(files)} files, {copied} bytes of mod files, patch {len(blob)} bytes -> {out}")


if __name__ == "__main__":
    main()
