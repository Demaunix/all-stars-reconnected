"""The Multiplayer menu's "Xbox LIVE" tiles repainted to say STEAM, as a ChaoGarage xpac replace.

The main-menu scene has one copy per language in SumoToolResources.xpac (SCENES; B888ED97 is the reference,
the Spanish and French ones translate only the Wi-Fi tile). Each has three tiles with "Xbox LIVE" painted
in: page 3 (balloons, heart) and page 4 (globe). Page 1 holds text-free twins of the heart and globe tiles,
pixel-identical except the text; the balloons tile has none, so its old text is inpainted. The word is set
in the game's own big font (scene 0720c59e page 1, fill layer), white with a black outline and a soft
shadow like the original. Only the 4x4 DXT3 blocks that change are re-encoded, on the reference, and the
same blocks are written into every copy after checking each copy's block is the reference's original; the
rest of each scene is byte-identical. The loader's composed-archive values are computed as ChaoGarage does
(virtual_xpac.h; port of Codex's compose_xpac.py).

usage: python tools/steam_tiles.py [<repo root>]   (writes build/tiles/, prints the plan group)
"""
import hashlib
import io
import json
import os
import struct
import sys
import zlib
from pathlib import Path

import cv2
import numpy as np
from PIL import Image, ImageFilter

GAME = Path(os.environ.get("ASR_GAME_DIR", r"A:\SteamLibrary\steamapps\common\Sonic and SEGA All Stars Racing"))
ARCHIVE = "Resource/SumoToolResources.xpac"
BASE_SHA256 = "f540d0d9927caa9ce26f8879c5be01d7fefc82947382466c8b8acb6ec650e447"  # as Egg Hangar's and Metal Sonic's plans
SCENE, FONT_SCENE = 0xB888ED97, 0x0720C59E
# Every language's copy of the main-menu scene (same 7 pages; the game loads one by its language).
SCENES = [0xB888ED97, 0x7D3A769A, 0xB912575B, 0xBA471B1C, 0xF7D266EF, 0x1A029B56, 0x93F9F421]
PAYLOADS = "payloads/SumoToolResources"
WORD = "STEAM"
# The big font's fill glyphs on the font scene's page 1: (x0, x1, y0, y1) boxes (rows split by alpha gaps).
GLYPHS = {"S": (178, 200, 229, 260), "T": (254, 281, 304, 333), "E": (166, 191, 340, 369),
          "A": (251, 287, 340, 369), "M": (361, 399, 340, 369)}
CAP = 25        # cap height in tile pixels ("LIVE" in the original)
OUTLINE = 3     # black outline, tile pixels
GAP = 4         # between the letters' fills
# Each tile: (page, clean twin's page or None, twin = tile + (dx, dy), old text box x0, y0, x1, y1, word's right and bottom edge).
TILES = [
    (3, 1, (880, 260), (656, 668, 848, 708), (843, 703)),   # heart: Play with Friends
    (4, 1, (880, 0), (216, 148, 408, 188), (403, 183)),     # globe: Play with Anyone
    (3, None, None, (216, 668, 408, 708), (403, 703)),      # balloons: Party (no twin: inpainted)
]


def sha(data: bytes) -> str:
    """SHA-256 hex."""
    return hashlib.sha256(data).hexdigest()


def align2048(n: int) -> int:
    """Next multiple of 2048."""
    return (n + 2047) & ~2047


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


def compose(path: Path, rows: dict, replace: dict) -> tuple:
    """(size, members, sha256) of the loader's virtual archive with `replace` {key: bytes} applied."""
    n = len(rows)
    meta = bytearray(align2048(24 + 20 * n))
    struct.pack_into("<6I", meta, 0, 0, 0, len(meta), n, 0, 0)
    cursor, layout = len(meta), []
    for i, key in enumerate(sorted(rows)):
        length = len(replace[key]) if key in replace else rows[key][1]
        struct.pack_into("<5I", meta, 24 + 20 * i, key, cursor, length, length, 0)
        layout.append((key, length))
        cursor = align2048(cursor + length)
    h = hashlib.sha256(bytes(meta))
    with open(path, "rb") as f:
        for key, length in layout:
            if key in replace:
                data = replace[key]
            else:
                f.seek(rows[key][0])
                data = f.read(length)
                assert len(data) == length
            h.update(data)
            h.update(bytes(align2048(length) - length))
    return cursor, n, h.hexdigest()


# ---------------------------------------------------------------- the scene container
# 0x48-byte table of 6 sections {offset, decoded size, stored size}; section 3 is the PTEX zlib stream,
# section 4 a small zlib block; each section starts at an offset = 8 (mod 64), zero-padded; 24 zero bytes end it.
def sections(entry: bytes) -> list:
    """The container's 6 (offset, decoded, stored) sections."""
    t = struct.unpack_from("<18I", entry, 0)
    secs = [t[k:k + 3] for k in range(0, 18, 3)]
    assert secs[0] == secs[1] == secs[2] == (0x48, 0, 0) and secs[3][0] == 0x48 and secs[5][1:] == (0, 0)
    return secs


def next_section(n: int) -> int:
    """The first offset >= n that is 8 (mod 64)."""
    return n + (8 - n) % 64


def pack(entry: bytes, stream: bytes, decoded: int) -> bytes:
    """`entry` with its PTEX stream replaced (section 4 copied as is)."""
    secs = sections(entry)
    a4, d4, s4 = secs[4]
    o4 = next_section(0x48 + len(stream))
    o5 = next_section(o4 + s4)
    out = bytearray(o5 + 0x18)
    struct.pack_into("<18I", out, 0, 0x48, 0, 0, 0x48, 0, 0, 0x48, 0, 0, 0x48, decoded, len(stream), o4, d4, s4, o5, 0, 0)
    out[0x48:0x48 + len(stream)] = stream
    out[o4:o4 + s4] = entry[a4:a4 + s4]
    return bytes(out)


def pages(ptex: bytes) -> list:
    """(offset, width, height) of each DDS page; all DXT3 without mipmaps."""
    out, q = [], ptex.find(b"DDS ")
    while q >= 0:
        hgt, wid = struct.unpack_from("<2I", ptex, q + 12)
        mips = struct.unpack_from("<I", ptex, q + 28)[0]
        out.append((q, wid, hgt, ptex[q + 84:q + 88], mips))
        q = ptex.find(b"DDS ", q + 4)
    return out


def decode(ptex: bytes, page: tuple) -> np.ndarray:
    """A DDS page as RGBA uint8."""
    q, w, h, fourcc, _ = page
    im = Image.open(io.BytesIO(ptex[q:q + 128 + w * h * (16 if fourcc == b"DXT3" else 16) // 16]))
    return np.asarray(im.convert("RGBA")).copy()


# ---------------------------------------------------------------- painting
def word_image(font_page: np.ndarray) -> Image.Image:
    """STEAM in the game's big font: white fill, black outline and shadow, at tile scale (RGBA)."""
    k = 4  # drawn 4x and scaled down for smooth edges
    letters = []
    for ch in WORD:
        x0, x1, y0, y1 = GLYPHS[ch]
        a = font_page[y0:y1, x0:x1, 3]
        ys, xs = np.nonzero(a > 40)
        a = a[ys.min():ys.max() + 1, xs.min():xs.max() + 1]
        letters.append(a)
    src_cap = max(a.shape[0] for a in letters)
    scale = CAP * k / src_cap
    glyphs = [cv2.resize(a, (max(1, round(a.shape[1] * scale)), max(1, round(a.shape[0] * scale))), interpolation=cv2.INTER_CUBIC)
              for a in letters]
    pad = (OUTLINE + 4) * k
    width = sum(g.shape[1] for g in glyphs) + GAP * k * (len(glyphs) - 1) + 2 * pad
    height = CAP * k + 2 * pad
    fill = np.zeros((height, width), np.uint8)
    x = pad
    for g in glyphs:
        y = pad + CAP * k - g.shape[0]  # bottoms on the baseline
        fill[y:y + g.shape[0], x:x + g.shape[1]] = np.maximum(fill[y:y + g.shape[0], x:x + g.shape[1]], g)
        x += g.shape[1] + GAP * k
    r = OUTLINE * k
    disk = cv2.getStructuringElement(cv2.MORPH_ELLIPSE, (2 * r + 1, 2 * r + 1))
    outline = cv2.dilate(fill, disk)
    size = (width // k, height // k)
    fill_s = Image.fromarray(fill).resize(size, Image.LANCZOS)
    out_s = Image.fromarray(outline).resize(size, Image.LANCZOS)
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    shadow = Image.new("RGBA", size, (0, 0, 0, 0))
    shadow.putalpha(out_s.point(lambda v: v * 170 // 255))
    shadow = shadow.transform(size, Image.AFFINE, (1, 0, -2, 0, 1, -2)).filter(ImageFilter.GaussianBlur(1.2))
    img.alpha_composite(shadow)
    black = Image.new("RGBA", size, (0, 0, 0, 255))
    black.putalpha(out_s)
    img.alpha_composite(black)
    white = Image.new("RGBA", size, (255, 255, 255, 255))
    white.putalpha(fill_s)
    img.alpha_composite(white)
    bbox = img.getbbox()
    return img.crop(bbox)


def paint(pg: dict, word: Image.Image) -> dict:
    """The repainted pages {index: RGBA} for every tile in TILES."""
    out = {i: a.copy() for i, a in pg.items()}
    heart = TILES[0]
    for page, twin, off, box, (right, bottom) in TILES:
        x0, y0, x1, y1 = box
        a = out[page]
        if twin is not None:
            dx, dy = off
            src = pg[twin]
            # The twin must match the tile exactly outside the text (a band above it).
            assert np.array_equal(src[y0 - 40 + dy:y0 - 4 + dy, x0 + dx:x1 + dx], pg[page][y0 - 40:y0 - 4, x0:x1]), f"tile twin moved (page {page})"
            a[y0:y1, x0:x1] = src[y0 + dy:y1 + dy, x0 + dx:x1 + dx]
        else:
            # The old text's shape: where the heart tile differs from its twin, moved onto this tile.
            hp, _, (hdx, hdy), (hx0, hy0, hx1, hy1), _ = heart
            diff = np.abs(pg[1][hy0 + hdy:hy1 + hdy, hx0 + hdx:hx1 + hdx].astype(int) - pg[hp][hy0:hy1, hx0:hx1].astype(int)).max(axis=2) > 0
            mask = np.zeros(a.shape[:2], np.uint8)
            mask[y0:y1, x0:x1] = diff * 255
            mask = cv2.dilate(mask, np.ones((3, 3), np.uint8))
            rgb = cv2.inpaint(np.ascontiguousarray(a[:, :, :3]), mask, 5, cv2.INPAINT_TELEA)
            a[:, :, :3] = np.where(mask[:, :, None] > 0, rgb, a[:, :, :3])
        im = Image.fromarray(a)
        im.alpha_composite(word, (right - word.width, bottom - word.height))
        out[page] = np.asarray(im).copy()
    return out


def reencode(ptex: bytearray, page: tuple, old: np.ndarray, new: np.ndarray) -> list:
    """Write the DXT3 blocks of `page` whose pixels changed; returns them as (offset in the page's data, old, new)."""
    q, w, h, fourcc, mips = page
    assert fourcc == b"DXT3" and mips == 0
    buf = io.BytesIO()
    Image.fromarray(new).save(buf, format="DDS", pixel_format="DXT3")
    enc = buf.getvalue()
    assert enc[84:88] == b"DXT3" and len(enc) == 128 + w * h
    changed = []
    for by in range(h // 4):
        for bx in range(w // 4):
            if np.array_equal(old[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4], new[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4]):
                continue
            i = 128 + (by * (w // 4) + bx) * 16
            changed.append((i, bytes(ptex[q + i:q + i + 16]), enc[i:i + 16]))
            ptex[q + i:q + i + 16] = enc[i:i + 16]
    return changed


def repaint_copy(entry: bytes, ref_pages: list, blocks: dict) -> bytes:
    """Another language's scene with the reference's changed blocks; each must hold the reference's original."""
    secs = sections(entry)
    a, d, s = secs[3]
    ptex = bytearray(zlib.decompress(entry[a:a + s]))
    pl = pages(bytes(ptex))
    assert [p[1:] for p in pl] == [p[1:] for p in ref_pages], "a scene copy's pages differ from the reference's"
    for page, changed in blocks.items():
        q = pl[page][0]
        for i, old, new in changed:
            assert ptex[q + i:q + i + 16] == old, f"a scene copy's tile art differs (page {page})"
            ptex[q + i:q + i + 16] = new
    return pack(entry, zlib.compress(bytes(ptex), 9), len(ptex))


def build(root: Path) -> dict:
    """Writes build/tiles/B888ED97.dat and group.json; returns the plan group."""
    path = GAME / ARCHIVE
    data_sha = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 24), b""):
            data_sha.update(chunk)
    assert data_sha.hexdigest() == BASE_SHA256, "SumoToolResources.xpac is not the Steam build's"
    _, rows = read_rows(path)
    entry = member(path, rows, SCENE)
    secs = sections(entry)
    a, d, s = secs[3]
    stream = entry[a:a + s]
    assert pack(entry, stream, d) == entry, "the scene's layout rule doesn't reproduce it"
    ptex = zlib.decompress(stream)
    assert len(ptex) == d
    pl = pages(ptex)
    assert [(w, h) for _, w, h, _, _ in pl] == [(512, 512), (2048, 2048), (1024, 1024), (1024, 1024), (512, 512), (512, 512), (512, 512)]
    pg = {i: decode(ptex, pl[i]) for i in (1, 3, 4)}
    font_entry = member(path, rows, FONT_SCENE)
    fa, fd, fs = sections(font_entry)[3]
    font_ptex = zlib.decompress(font_entry[fa:fa + fs])
    font_page = decode(font_ptex, pages(font_ptex)[1])
    word = word_image(font_page)
    painted = paint(pg, word)
    new_ptex = bytearray(ptex)
    blocks = {i: reencode(new_ptex, pl[i], pg[i], painted[i]) for i in (3, 4)}
    for i in (3, 4):  # the result decodes to the painting, within DXT3's error
        got = decode(bytes(new_ptex), pl[i]).astype(int)
        err = np.abs(got - painted[i].astype(int))[:, :, :3][painted[i][:, :, 3] > 0].mean()
        assert err < 4, f"page {i} re-encode error {err}"
    payloads = {}
    for key in SCENES:
        orig = entry if key == SCENE else member(path, rows, key)
        payload = repaint_copy(orig, pl, blocks)
        s3, s4 = sections(payload)[3], sections(payload)[4]
        o4 = sections(orig)[4]
        assert zlib.decompress(payload[0x48:0x48 + s3[2]]) != zlib.decompress(orig[0x48:0x48 + sections(orig)[3][2]])
        assert payload[s4[0]:s4[0] + s4[2]] == orig[o4[0]:o4[0] + o4[2]]
        if key == SCENE:
            assert zlib.decompress(payload[0x48:0x48 + s3[2]]) == bytes(new_ptex)
        payloads[key] = (orig, payload)
    # (The text tables of tools/text_tables.py are NOT in this group: Metal Sonic replaces the same member, so they ride with the
    # key pictures in the Keys folder and in Metal's update; tools/footer_keys.py.)
    TEXT_NAMES = {}
    size, count, composed = compose(path, rows, {k: p for k, (_, p) in payloads.items()})
    out = root / "build" / "tiles"
    out.mkdir(parents=True, exist_ok=True)
    for old in out.glob("*.dat"):
        old.unlink()
    ops = []
    for key, (orig, payload) in payloads.items():
        (out / ("%08X.dat" % key)).write_bytes(payload)
        ops.append({
            "key": "%08X" % key,
            "kind": "replace",
            "name": TEXT_NAMES.get(key, "main-menu scene (a language's copy): the Multiplayer tiles say STEAM instead of Xbox LIVE"),
            "payload": "%s/%08X.dat" % (PAYLOADS, key),
            "payload_sha256": sha(payload),
            "stored_size": len(payload),
            "entry_flags": 0,
            "base_stored_sha256": sha(orig),
        })
    Image.fromarray(painted[3]).save(out / "page3.png")
    Image.fromarray(painted[4]).save(out / "page4.png")
    group = {
        "path": ARCHIVE,
        "format": "xpac",
        "base_sha256": BASE_SHA256,
        "operations": ops,
        "expected_composed_sha256": composed,
        "expected_composed_size": size,
        "expected_member_count": count,
    }
    stamp = {"tool_sha256": sha(Path(__file__).read_bytes()), "blocks": {p: len(c) for p, c in blocks.items()}, "group": group}
    (out / "group.json").write_text(json.dumps(stamp, indent=2) + "\n", encoding="utf-8")
    return group


if __name__ == "__main__":
    r = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
    print(json.dumps(build(r), indent=2))
    print(json.loads((r / "build" / "tiles" / "group.json").read_text())["blocks"], "blocks re-encoded (page: count)")
