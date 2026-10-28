r"""The lobby's YELLOW player bar (a muted player's, the user's design 2026-10-23; re\m8-yellow\FINDINGS.md): a new sprite in the
7 lobby scenes of SumoToolResources.xpac, which the C++ sets as a muted player's bar art (0x47f400 on the bar's base element).

A bar is two stacked sprites: the blue base 5da8c97e and the red cursor overlay bcf09c8c (only its alpha is animated, so the red
still covers a yellow bar under the cursor). No yellow exists in the lobby's art, and the keyframe colours multiply (white only),
so the yellow is painted: the blue bar's pixels (page 3, (0,232)-(640,276)) with their hue moved to YELLOW_HUE and their value
scaled (the white border and its anti-aliasing, low saturation, stay as they are), at page 3 (0,320)-(640,364), which is fully
transparent in every lobby scene (stock and Metal Sonic's; checked). Only the changed DXT3 blocks are re-encoded
(steam_tiles.reencode), their alpha written exactly (as footer_keys.exact_alpha).

The sprite record {hash, u0 v0 u1 v0 u1 v1 u0 v1, page} is inserted into the PTEX chunk's hash-sorted record array. Everything
after it moves 0x28 bytes, so the chunk's pointers move with it: every PTEX pointer field is listed in section 4's RELO chunk
for PTEX (offsets from the chunk's +0x10, as the pointers' values): a pointer whose value points past the insertion and a listed
field past it both move. That covers the stock layout (page table right after the records) and Metal Sonic's (page table at the
chunk end). The chunk's used size grows by 0x28, its size is align64(0x10 + used) again, and the later chunks move with it
(they are walked by size; their own RELO offsets are relative to them). Section 4 is recompressed.

    import lobby_bars; payload, facts = lobby_bars.build_scene(entry, name)
Needs numpy and Pillow (run without -I, as footer_keys.py).
"""
import colorsys
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402
import steam_tiles as st  # noqa: E402
from scene_container import packer_for, sections6  # noqa: E402

# The 7 lobby scenes (fe_network_game's), one per language; JP and US are placeholders, the same art.
SCENES = {0x1A87C210: "SP", 0x1D952462: "IT", 0x3943D953: "JP", 0x7F94F89F: "US", 0x841A31F1: "FR", 0x9BDCF15B: "GE", 0xE01CF5DC: "EN"}
BLUE = 0x5DA8C97E          # the bars' blue base sprite
MUTED = 0xDD4E2BB1         # name_hash("STEAMMP_LOBBY_BAR_MUTED"): free in every scene
PAGE = 3
SRC = (0, 232, 640, 276)   # the blue bar on page 3 (x0, y0, x1, y1)
DST_Y = 320                # the yellow bar: (0, 320)-(640, 364), rows 320..512 transparent
YELLOW_HUE = 48 / 360
VALUE_SCALE = 0.92
RECORD = 40                # {u32 hash, 8 floats, u32 page}
RECORDS_AT = 0x1C          # the record array in the PTEX chunk


def align64(n: int) -> int:
    """Next multiple of 64."""
    return (n + 63) // 64 * 64


def recolour(bar: np.ndarray) -> np.ndarray:
    """The bar (RGBA) with its coloured pixels moved to YELLOW_HUE and their value scaled; the rest as it is."""
    out = bar.copy()
    for y in range(bar.shape[0]):
        for x in range(bar.shape[1]):
            r, g, b, a = (int(v) for v in bar[y, x])
            h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
            if s > 0.25:
                r2, g2, b2 = colorsys.hsv_to_rgb(YELLOW_HUE, s, min(1.0, v * VALUE_SCALE))
                out[y, x] = (round(r2 * 255), round(g2 * 255), round(b2 * 255), a)
    return out


def relo_chunks(s4: bytes) -> dict:
    """{tag: (offset of the RELO chunk in section 4, [(entry offset, type, field offset)])}."""
    out, q = {}, 0
    while q + 16 <= len(s4) and s4[q:q + 4] == b"RELO":
        size, used = struct.unpack_from("<II", s4, q + 4)
        assert size and struct.unpack_from("<I", s4, q + 12)[0] == 0x44332211, "RELO chunk header"
        tag = s4[q + 16:q + 20]
        n = (used - 4) // 8
        out[tag] = (q, [(q + 20 + 8 * i,) + struct.unpack_from("<II", s4, q + 20 + 8 * i) for i in range(n)])
        q += size
    assert not any(s4[q:]), "data after the RELO chunks"
    return out


def build_scene(entry: bytes, name: str) -> tuple:
    """A lobby scene container with the yellow bar sprite added: (payload bytes, facts dict)."""
    secs = sections6(entry)
    a3, d3, s3 = secs[3]
    a4, d4, s4 = secs[4]
    pack = packer_for(entry)
    assert pack(entry[a3:a3 + s3], d3, entry[a4:a4 + s4], d4) == entry, f"{name}: the layout with a given section 4"
    sec3 = bytearray(zlib.decompress(entry[a3:a3 + s3]))
    sec4 = bytearray(zlib.decompress(entry[a4:a4 + s4]))
    assert len(sec3) == d3 and len(sec4) == d4, f"{name}: section sizes"
    assert sec3[:4] == b"PTEX" and struct.unpack_from("<I", sec3, 12)[0] == 0x44332211, f"{name}: PTEX first"
    size, used = struct.unpack_from("<II", sec3, 4)
    count = struct.unpack_from("<I", sec3, 0x10)[0]
    assert size == align64(0x10 + used) and not any(sec3[0x10 + used:size]), f"{name}: PTEX size/padding"
    hashes = [struct.unpack_from("<I", sec3, RECORDS_AT + RECORD * i)[0] for i in range(count)]
    assert hashes == sorted(hashes) and MUTED not in hashes and BLUE in hashes, f"{name}: records"
    blue = struct.unpack_from("<I8fI", sec3, RECORDS_AT + RECORD * hashes.index(BLUE))
    pl = st.pages(bytes(sec3))
    page = pl[PAGE]
    q, pw, ph, fourcc, mips = page
    assert fourcc == b"DXT3" and mips == 0 and (pw, ph) == (1024, 512), f"{name}: page 3 is {page[1:]}"
    x0, y0, x1, y1 = SRC
    assert blue[9] == PAGE and [round(blue[1] * pw), round(blue[2] * ph), round(blue[5] * pw), round(blue[6] * ph)] == [x0, y0, x1, y1], f"{name}: blue bar {blue}"
    # The yellow bar painted on page 3, only the changed blocks re-encoded.
    old = st.decode(bytes(sec3), page)
    dy0, dy1 = DST_Y, DST_Y + (y1 - y0)
    assert not old[dy0:, :, 3].any(), f"{name}: page 3 is not transparent below row {dy0}"
    painted = old.copy()
    painted[dy0:dy1, x0:x1] = recolour(old[y0:y1, x0:x1])
    blocks = st.reencode(sec3, page, old, painted)
    exact_alpha(sec3, page, painted, blocks)
    got = st.decode(bytes(sec3), page).astype(int)
    mask = painted[:, :, 3] > 0
    err = np.abs(got - painted.astype(int))[:, :, :3][mask].mean()
    alpha_err = np.abs(got[:, :, 3] - painted[:, :, 3].astype(int)).max()
    assert err < 4 and alpha_err <= 9, f"{name}: page 3 re-encode error {err}, alpha {alpha_err}"
    assert all(dy0 // 4 * 1024 // 4 <= (i - 128) // 16 < dy1 // 4 * 1024 // 4 for i, _, _ in blocks), f"{name}: a block outside the yellow bar changed"
    # The record, sorted in; the pointers past it move.
    index = next((i for i, h in enumerate(hashes) if h > MUTED), count)
    at = RECORDS_AT + RECORD * index
    u0, v0, u1, v1 = x0 / pw, dy0 / ph, x1 / pw, dy1 / ph
    record = struct.pack("<I8fI", MUTED, u0, v0, u1, v0, u1, v1, u0, v1, PAGE)
    relos = relo_chunks(bytes(sec4))
    rq, entries = relos[b"PTEX"]
    moved_values = moved_fields = 0
    for e_at, kind, field in entries:
        if not kind:
            continue
        value = struct.unpack_from("<I", sec3, 0x10 + field)[0]
        if 0x10 + value >= at:
            struct.pack_into("<I", sec3, 0x10 + field, value + RECORD)
            moved_values += 1
        if 0x10 + field >= at:
            struct.pack_into("<I", sec4, e_at + 4, field + RECORD)
            moved_fields += 1
    sec3[at:at] = record
    new_used = used + RECORD
    new_size = align64(0x10 + new_used)
    pad_at = 0x10 + new_used  # (the old padding now starts RECORD bytes later; it is laid out again)
    del sec3[pad_at:size + RECORD]
    sec3[pad_at:pad_at] = bytes(new_size - pad_at)
    struct.pack_into("<3I", sec3, 4, new_size, new_used, 0x44332211)
    struct.pack_into("<I", sec3, 0x10, count + 1)
    # Checks: the records, the pages (same pixels, the yellow bar added), the later chunks unchanged.
    check_scene(bytes(sec3), bytes(sec4), name, count + 1, index, record)
    pl2 = st.pages(bytes(sec3))
    assert [p[1:] for p in pl2] == [p[1:] for p in pl] and all(p2[0] == p[0] + RECORD for p, p2 in zip(pl, pl2)), f"{name}: pages moved wrongly"
    old_tail = zlib.decompress(entry[a3:a3 + s3])[size:]
    assert bytes(sec3[new_size:]) == old_tail, f"{name}: the chunks after PTEX changed"
    payload = pack(zlib.compress(bytes(sec3), 9), len(sec3), zlib.compress(bytes(sec4), 9), len(sec4))
    n3, n4 = sections6(payload)[3], sections6(payload)[4]
    assert zlib.decompress(payload[n3[0]:n3[0] + n3[2]]) == bytes(sec3) and n3[1] == len(sec3)
    assert zlib.decompress(payload[n4[0]:n4[0] + n4[2]]) == bytes(sec4) and n4[1] == len(sec4)
    facts = {"index": index, "blocks": len(blocks), "reencode_error": round(float(err), 3), "pointers_moved": moved_values,
             "relo_fields_moved": moved_fields, "ptex": [hex(new_size), hex(new_used)]}
    return payload, facts


def check_scene(sec3: bytes, sec4: bytes, name: str, count: int, index: int, record: bytes) -> None:
    """The patched PTEX chunk reads back: sorted records with ours at `index`, every RELO-listed pointer inside the chunk, the page
    table's pages where the DDS headers are."""
    size, used = struct.unpack_from("<II", sec3, 4)
    assert struct.unpack_from("<I", sec3, 0x10)[0] == count and size == align64(0x10 + used)
    hashes = [struct.unpack_from("<I", sec3, RECORDS_AT + RECORD * i)[0] for i in range(count)]
    assert hashes == sorted(hashes) and hashes[index] == MUTED and sec3[RECORDS_AT + RECORD * index:RECORDS_AT + RECORD * (index + 1)] == record
    _, entries = relo_chunks(sec4)[b"PTEX"]
    for _, kind, field in entries:
        if kind:
            value = struct.unpack_from("<I", sec3, 0x10 + field)[0]
            assert 0x10 + field < 0x10 + used and 0x10 + value <= 0x10 + used, f"{name}: a pointer outside PTEX"
    pages_n = struct.unpack_from("<I", sec3, 0x14)[0]
    table = 0x10 + struct.unpack_from("<I", sec3, 0x18)[0]
    dds = [p[0] for p in st.pages(sec3)]
    listed = [0x10 + struct.unpack_from("<I", sec3, table + 8 * k)[0] for k in range(pages_n)]
    assert sorted(listed) == dds[:pages_n], f"{name}: the page table doesn't point at the pages"


def exact_alpha(ptex: bytearray, page: tuple, painted: np.ndarray, blocks: list) -> None:
    """The alpha half of every re-encoded DXT3 block written from the painting (Pillow's encoder misses some edge alphas)."""
    q, w = page[0], page[1]
    bw = w // 4
    for i, old, new in blocks:
        b = (i - 128) // 16
        by, bx = divmod(b, bw)
        a = painted[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4, 3].astype(int)
        nib = (a * 15 + 127) // 255
        rows = [int(nib[r, 0]) | int(nib[r, 1]) << 4 | int(nib[r, 2]) << 8 | int(nib[r, 3]) << 12 for r in range(4)]
        ptex[q + i:q + i + 8] = struct.pack("<4H", *rows)
