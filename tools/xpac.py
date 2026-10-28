"""The game's xpac archives, with nothing but the standard library (for the packaging steps that run with python -I).

An xpac: a 24-byte header {0, 0, directory end, member count, 0, 0}, 20-byte rows {key, offset, size, size, 0} and the members at
2048-byte aligned offsets. The loader's virtual archive (ChaoGarage virtual_xpac.h) lays the composed archive out the same way:
header, rows in key order, members 2048-aligned in key order; its expected_composed_* plan fields are computed here.
"""
import hashlib
import struct
from pathlib import Path

GAME = Path(r"A:\SteamLibrary\steamapps\common\Sonic and SEGA All Stars Racing")
ARCHIVE = "Resource/SumoToolResources.xpac"
BASE_SHA256 = "f540d0d9927caa9ce26f8879c5be01d7fefc82947382466c8b8acb6ec650e447"  # the Steam build's SumoToolResources.xpac


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align2048(n: int) -> int:
    return (n + 2047) // 2048 * 2048


def read_rows(path: Path) -> tuple:
    """The archive's header and {key: (offset, stored)}."""
    with open(path, "rb") as f:
        h = struct.unpack("<6I", f.read(24))
        rows = f.read(20 * h[3])
    assert h[0] == h[1] == h[4] == h[5] == 0
    out = {}
    for i in range(h[3]):
        key, off, size2, stored, flags = struct.unpack_from("<5I", rows, 20 * i)
        assert size2 == stored and flags == 0 and key not in out
        out[key] = (off, stored)
    return h, out


def member(path: Path, rows: dict, key: int) -> bytes:
    """A member's stored bytes."""
    off, n = rows[key]
    with open(path, "rb") as f:
        f.seek(off)
        return f.read(n)


def compose(path: Path, rows: dict, members: dict) -> tuple:
    """(size, count, sha256) of the loader's virtual archive: `path` with `members` {key: bytes} replacing or adding members."""
    keys = sorted(set(rows) | set(members))
    meta = bytearray(align2048(24 + 20 * len(keys)))
    struct.pack_into("<6I", meta, 0, 0, 0, len(meta), len(keys), 0, 0)
    cursor, layout = len(meta), []
    for i, key in enumerate(keys):
        length = len(members[key]) if key in members else rows[key][1]
        struct.pack_into("<5I", meta, 24 + 20 * i, key, cursor, length, length, 0)
        layout.append((key, length))
        cursor = align2048(cursor + length)
    h = hashlib.sha256(bytes(meta))
    with open(path, "rb") as f:
        for key, length in layout:
            if key in members:
                data = members[key]
            else:
                f.seek(rows[key][0])
                data = f.read(length)
                assert len(data) == length
            h.update(data)
            h.update(bytes(align2048(length) - length))
    return cursor, len(keys), h.hexdigest()
