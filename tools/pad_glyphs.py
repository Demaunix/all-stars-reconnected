r"""Controller button pictures in the online footers (the user's request 2026-10-27, choices 2026-10-29; re\pad-glyphs\FINDINGS.md,
FINDINGS2.md).

The footers draw button pictures as private-use characters of a font: F000 Enter, F001 Esc, F002 F1, F003 F2, F004 left/right,
F005 up/down, F006 all four arrows, F007 the A key (tools\footer_keys.py). This tool adds, in every scene that draws them, two more
sets of those characters with the console versions' own pictures, Xbox at F010-F017 and PlayStation at F018-F01F, and a copy of the
PC's keys at F020-F027; src\pad_glyphs.cpp copies a set's records over F000-F007 at run time while a controller player is online
(and the keys back afterwards, again by copying):
- the 7 navigator_info scenes (the lobby and game list footer, font ff7f8ec5; 8 characters, F007 = INVITE FRIENDS / VIEW PROFILE,
  the Back / View button on a pad), after footer_keys.build_scene;
- the 14 race results scenes (the online results and pause button bars; 16:9 fonts 3c005ba2 / 7ea1f827, 4:3 fonts 30f8365e /
  5e0a0003; 7 characters).
A glyph draws only a sprite of its own text element's scene (FINDINGS2 Q3), so each scene carries the pictures itself: one new DXT3
page per set (256x256) holding the cells of reference\pad-glyphs\cells.bin as they are (4x4 blocks, each cell at its own offset
inside its first block, so no texel changes), one sprite record per page (the whole page, named STEAMMP_PAD_<SET>_<KIND>_<LANG>),
merged like the game list's mod icons (codexui merge_ptex), and the glyph records inserted into each font before its 0xffff record
(the later fonts and kerning tables move, as in footer_keys.py). Each language takes its own console art where it exists, else
English (cells.json "aliases" and "notes"). reference\pad-glyphs is made once by a private extraction tool.

    import pad_glyphs; payload = pad_glyphs.add(entry, kind, lang)     (footer_keys.build: kind "nav", "wide" or "4_3")
    python tools\pad_glyphs.py                                          (build\pad_glyphs\preview.png: every set as extracted)
Run without -I (numpy, Pillow).
"""
import io
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools" / "codexui"))
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw  # noqa: E402
from build_ui import check_relos, merge_ptex, pack, path_hash, ptex_info  # noqa: E402
from decode_ui_texture import sections  # noqa: E402
from probe import chunks  # noqa: E402

LANGS = ("EN", "FR", "GE", "IT", "SP", "US", "JP")
COUNTS = {"nav": 8, "wide": 7, "4_3": 7}   # the characters F000.. each kind's footer uses
PC_FONTS = {"nav": (0xFF7F8EC5,), "wide": (0x3C005BA2, 0x7EA1F827), "4_3": (0x30F8365E, 0x5E0A0003)}
# The 14 race results scenes (name hashes of ingame\raceresults[_4_3]_<lang>.stz).
RESULTS = {0xD8A395BC: ("wide", "EN"), 0xB8463A89: ("wide", "FR"), 0x65F3A963: ("wide", "GE"), 0x48DA2752: ("wide", "IT"),
           0x92F629D0: ("wide", "SP"), 0xCB11729B: ("wide", "US"), 0xB69FFE07: ("wide", "JP"),
           0xFD0EE89D: ("4_3", "EN"), 0xC297CB7A: ("4_3", "FR"), 0x8B58DA74: ("4_3", "GE"), 0x6F719D13: ("4_3", "IT"),
           0x8979FAF1: ("4_3", "SP"), 0x42AB552C: ("4_3", "US"), 0x18603A58: ("4_3", "JP")}
SETS = (("x360", 0xF010), ("ps3", 0xF018))  # (art, first character): Xbox, PlayStation
KEYS = 0xF020                                # the copy of the PC's own key records
GLYPH = 0x18                                 # bytes per glyph record
PAGE = 256                                   # each set's page (DXT3, PAGE x PAGE)
REF = ROOT / "reference" / "pad-glyphs"
_cells = None


def set_name(platform: str, kind: str, lang: str) -> str:
    """The cells.json name of a set."""
    return f"{platform}/{kind}/{lang}"


def align64(n: int) -> int:
    """Next multiple of 64."""
    return (n + 63) // 64 * 64


def dds_dxt3(w: int, h: int, data: bytes) -> bytes:
    """A DXT3 DDS file of w x h."""
    hd = bytearray(128)
    hd[:4] = b"DDS "
    struct.pack_into("<7I", hd, 4, 124, 0x81007, h, w, len(data), 0, 1)
    struct.pack_into("<II4s", hd, 76, 32, 4, b"DXT3")
    struct.pack_into("<I", hd, 108, 0x1000)
    return bytes(hd) + data


def page_like(template: bytes, w: int, h: int, data: bytes) -> bytes:
    """A DXT3 page whose DDS header is the scene's own DXT3 page's (`template`, a whole DDS) with the size changed; its
    pitch / linear-size field follows the template's own rule (its data size, or as it is)."""
    hd = bytearray(template[:128])
    assert hd[:4] == b"DDS " and hd[84:88] == b"DXT3", "the template page is not DXT3"
    th, tw, tsize = struct.unpack_from("<3I", hd, 12)
    struct.pack_into("<2I", hd, 12, h, w)
    if tsize == tw * th:  # (a DXT3 page's data is w * h bytes)
        struct.pack_into("<I", hd, 20, len(data))
    assert len(data) == w * h
    return bytes(hd) + data


def decode_cell(cell: dict) -> np.ndarray:
    """A cell's pixels (h x w RGBA) from its blocks."""
    im = Image.open(io.BytesIO(dds_dxt3(cell["bw"] * 4, cell["bh"] * 4, cell["blocks"]))).convert("RGBA")
    a = np.asarray(im)
    return a[cell["py"]:cell["py"] + cell["h"], cell["px"]:cell["px"] + cell["w"]].copy()


def encode_dxt3(rgba: np.ndarray) -> bytes:
    """DXT3 blocks of an RGBA image (both sides multiples of 4), the alpha written exactly from the image (Pillow's encoder
    misses some edge alphas: footer_keys.exact_alpha)."""
    h, w = rgba.shape[:2]
    assert w % 4 == 0 and h % 4 == 0
    buf = io.BytesIO()
    Image.fromarray(rgba).save(buf, format="DDS", pixel_format="DXT3")
    enc = bytearray(buf.getvalue()[128:])
    assert len(enc) == w * h
    for by in range(h // 4):
        for bx in range(w // 4):
            nib = (rgba[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4, 3].astype(int) * 15 + 127) // 255
            rows = [int(nib[r, 0]) | int(nib[r, 1]) << 4 | int(nib[r, 2]) << 8 | int(nib[r, 3]) << 12 for r in range(4)]
            i = (by * (w // 4) + bx) * 16
            enc[i:i + 8] = struct.pack("<4H", *rows)
    return bytes(enc)


def cells() -> dict:
    """reference\\pad-glyphs\\cells.json with each cell's blocks read in (checked against the recorded hash)."""
    global _cells
    if _cells is None:
        import hashlib
        js = json.loads((REF / "cells.json").read_text(encoding="utf-8"))
        blob = (REF / "cells.bin").read_bytes()
        assert hashlib.sha256(blob).hexdigest() == js["cells_bin_sha256"], "cells.bin doesn't match cells.json"
        for rows in js["sets"].values():
            for c in rows:
                c["blocks"] = blob[c["at"]:c["at"] + c["size"]]
        _cells = js
    return _cells


def cells_for(platform: str, kind: str, lang: str) -> list:
    """A set's cells: the language's own, its alias, or English."""
    js = cells()
    name = set_name(platform, kind, lang)
    name = js["aliases"].get(name, name)
    if name not in js["sets"]:
        name = set_name(platform, kind, "EN")
    rows = js["sets"][name]
    assert len(rows) == COUNTS[kind], f"{name}: {len(rows)} cells"
    return rows


def layout(cs: list) -> tuple:
    """(the page's DXT3 blocks, [(u, v)] of each cell): the cells' block boxes in rows on a PAGE x PAGE page."""
    nb = PAGE // 4
    page = bytearray(nb * nb * 16)  # (zero blocks: transparent)
    uv, x, y, row_h = [], 0, 0, 0
    for c in cs:
        if x + c["bw"] > nb:
            x, y, row_h = 0, y + row_h, 0
        assert y + c["bh"] <= nb, "the set doesn't fit its page"
        for by in range(c["bh"]):
            src = c["blocks"][by * c["bw"] * 16:(by + 1) * c["bw"] * 16]
            at = ((y + by) * nb + x) * 16
            page[at:at + len(src)] = src
        uv.append((x * 4 + c["px"], y * 4 + c["py"]))
        x += c["bw"]
        row_h = max(row_h, c["bh"])
    return bytes(page), uv


# ---------------------------------------------------------------- the FONT chunk
def font_rows(c: bytes) -> list:
    """The FONT chunk's rows: (row offset, hash, glyph count, kern count, glyph offset, kern offset)."""
    n = struct.unpack_from("<I", c, 0x18)[0]
    out = []
    for i in range(n):
        at = 0x1C + 0x20 * i
        h, counts, goff, koff = struct.unpack_from("<4I", c, at)
        out.append((at, h, counts & 0xFFFF, counts >> 16, goff, koff))
    return out


def glyphs(c: bytes, font: int) -> list:
    """The glyph records of one font of a FONT chunk."""
    row = [r for r in font_rows(c) if r[1] == font]
    assert len(row) == 1, f"font {font:08x}: {len(row)} rows"
    _, _, n, _, goff, _ = row[0]
    return [struct.unpack_from("<IHHHHHhhhHH", c, 0x10 + goff + GLYPH * k) for k in range(n)]


def add_records(c: bytes, relo: bytes, font: int, new: list) -> bytes:
    """The FONT chunk with `new` glyph records (sorted, all between the font's last private-use key and 0xffff) inserted before
    the font's 0xffff record; every row offset at or past the insertion moves."""
    size, used, magic = struct.unpack_from("<3I", c, 4)
    assert c[:4] == b"FONT" and magic == 0x44332211 and size == align64(0x10 + used) and not any(c[0x10 + used:size]), "FONT header"
    rows = font_rows(c)
    mine = [r for r in rows if r[1] == font]
    assert len(mine) == 1
    at, _, n, _, goff, _ = mine[0]
    recs = glyphs(c, font)
    chars = [r[1] for r in recs]
    assert chars == sorted(chars) and chars[-1] == 0xFFFF, f"font {font:08x}: glyph order"
    assert chars[-2] < new[0][1] and [r[1] for r in new] == sorted(r[1] for r in new), f"font {font:08x}: the new characters' place"
    insert = 0x10 + goff + GLYPH * (n - 1)
    blob = b"".join(struct.pack("<IHHHHHhhhHH", *r) for r in new)
    # The relocated fields: only the rows' glyph / kern offsets (they sit before the arrays, so their places don't move).
    fields = {r[0] + 8 for r in rows} | {r[0] + 12 for r in rows}
    used_n = struct.unpack_from("<I", relo, 8)[0]
    listed = [struct.unpack_from("<2I", relo, p) for p in range(20, 16 + used_n, 8)]
    assert listed[-1] == (0, 0)
    for typ, p in listed[:-1]:
        assert typ == 1 and (p + 16 in fields or 0x10 + struct.unpack_from("<I", c, p + 16)[0] < insert), f"font chunk field {p:#x}"
        assert p + 16 < insert
    out = bytearray(c[:0x10 + used])
    for r in rows:
        if r[1] == font:
            struct.pack_into("<H", out, r[0] + 4, n + len(new))
        elif 0x10 + r[4] >= insert:
            struct.pack_into("<I", out, r[0] + 8, r[4] + len(blob))
        if r[3] and 0x10 + r[5] >= insert:
            struct.pack_into("<I", out, r[0] + 12, r[5] + len(blob))
    out[insert:insert] = blob
    new_used = used + len(blob)
    out += bytes(align64(0x10 + new_used) - len(out))
    struct.pack_into("<3I", out, 4, len(out), new_used, 0x44332211)
    return bytes(out)


# ---------------------------------------------------------------- one scene
def section_of(ss: list, font: int) -> int:
    """The section (0 or 3) whose FONT chunk holds the font."""
    hits = [si for si in (0, 3) for _, c in chunks(ss[si]) if c[:4] == b"FONT" and any(r[1] == font for r in font_rows(c))]
    assert len(hits) == 1, f"font {font:08x} in sections {hits}"
    return hits[0]


def add(entry: bytes, kind: str, lang: str) -> bytes:
    """A navigator / results scene container with the Xbox and PlayStation sets and the keys copy added (checked)."""
    fonts, n = PC_FONTS[kind], COUNTS[kind]
    ss = list(sections(entry, "<"))
    si = section_of(ss, fonts[0])
    cc = [c for _, c in chunks(ss[si])]
    rr = [c for _, c in chunks(ss[si + 1])]
    assert len(cc) == len(rr) and all(c[:4] == r[16:20] for c, r in zip(cc, rr)), "chunks and RELO chunks"
    pi = [i for i, c in enumerate(cc) if c[:4] == b"PTEX"]
    fi = [i for i, c in enumerate(cc) if c[:4] == b"FONT"]
    assert len(pi) == 1 and len(fi) == 1, f"{len(pi)} PTEX / {len(fi)} FONT chunks"
    old_sprites = {s[0] for s in ptex_info(cc[pi[0]])[0]}
    template = next(t for t in ptex_info(cc[pi[0]])[1] if t[84:88] == b"DXT3")  # (the new pages take its DDS header)
    newsp, newtex, sets = [], [], {}
    for k, (platform, first) in enumerate(SETS):
        cs = cells_for(platform, kind, lang)
        blocks, uv = layout(cs)
        sprite = path_hash(f"STEAMMP_PAD_{platform}_{kind}_{lang}")
        assert sprite not in old_sprites, f"sprite {sprite:08x} exists"
        newsp.append((sprite, 0.0, 0.0, 1.0, 0.0, 1.0, 1.0, 0.0, 1.0, k))
        newtex.append(page_like(template, PAGE, PAGE, blocks))
        sets[first] = [(sprite, first + i, u, v, c["w"], c["h"], c["xoff"], c["adv"], c["yoff"], c["f14"], c["f16"])
                       for i, (c, (u, v)) in enumerate(zip(cs, uv))]
    cc[pi[0]], rr[pi[0]], _ = merge_ptex(cc[pi[0]], rr[pi[0]], newsp, newtex)
    for font in fonts:
        recs = glyphs(cc[fi[0]], font)
        keys = sorted((r for r in recs if 0xF000 <= r[1] < 0xFFFF), key=lambda r: r[1])
        assert [r[1] for r in keys] == [0xF000 + i for i in range(n)], f"{kind}/{lang} font {font:08x}: keys {[hex(r[1]) for r in keys]}"
        assert all(r[0] in old_sprites for r in keys), f"{kind}/{lang} font {font:08x}: a key's sprite is not in this PTEX"
        copy = [(r[0], KEYS + i) + tuple(r[2:]) for i, r in enumerate(keys)]
        cc[fi[0]] = add_records(cc[fi[0]], rr[fi[0]], font, sets[SETS[0][1]] + sets[SETS[1][1]] + copy)
    ss[si] = b"".join(cc)
    ss[si + 1] = b"".join(rr)
    check_relos(ss)
    out = pack(ss)
    check(entry, out, kind, lang)
    return out


def page_pixels(c: bytes, sprite: int) -> np.ndarray:
    """The RGBA pixels of a sprite's page (a sprite of the whole page)."""
    sp, tex = ptex_info(c)
    rec = next(s for s in sp if s[0] == sprite)
    return np.asarray(Image.open(io.BytesIO(tex[rec[9]])).convert("RGBA"))


def check(entry: bytes, out: bytes, kind: str, lang: str) -> None:
    """The new scene reads back: every font's old records unchanged, the target fonts' new records in place, each new record's
    cell on its page with the extracted pixels; every other chunk of every section unchanged."""
    a, b = sections(entry, "<"), sections(out, "<")
    fonts, n = PC_FONTS[kind], COUNTS[kind]
    si = section_of(a, fonts[0])
    for s in range(6):
        if s not in (si, si + 1):
            assert a[s] == b[s], f"section {s} changed"
    ca, cb = [c for _, c in chunks(a[si])], [c for _, c in chunks(b[si])]
    assert [c[:4] for c in ca] == [c[:4] for c in cb]
    for x, y in zip(ca, cb):
        if x[:4] not in (b"PTEX", b"FONT"):
            assert x == y, f"chunk {x[:4]!r} changed"
    fa = next(c for c in ca if c[:4] == b"FONT")
    fb = next(c for c in cb if c[:4] == b"FONT")
    pb = next(c for c in cb if c[:4] == b"PTEX")
    sprites = {s[0] for s in ptex_info(pb)[0]}
    for _, h, *_ in font_rows(fa):
        old, new = glyphs(fa, h), glyphs(fb, h)
        if h not in fonts:
            assert old == new, f"font {h:08x} changed"
            continue
        assert [r for r in new if not 0xF010 <= r[1] < 0xF028] == old, f"font {h:08x}: an old record changed"
        added = {r[1]: r for r in new if 0xF010 <= r[1] < 0xF028}
        assert sorted(added) == [f + i for f in (0xF010, 0xF018, 0xF020) for i in range(n)], f"font {h:08x}: {sorted(added)}"
        keys = {r[1]: r for r in old}
        for i in range(n):
            assert added[KEYS + i][2:] == keys[0xF000 + i][2:] and added[KEYS + i][0] == keys[0xF000 + i][0]
        for platform, first in SETS:
            cs = cells_for(platform, kind, lang)
            for i, c in enumerate(cs):
                r = added[first + i]
                assert r[0] in sprites and (r[4], r[5], r[6], r[7], r[8]) == (c["w"], c["h"], c["xoff"], c["adv"], c["yoff"])
                px = page_pixels(pb, r[0])[r[3]:r[3] + r[5], r[2]:r[2] + r[4]]
                assert np.array_equal(px, decode_cell(c)), f"font {h:08x} char {r[1]:04x}: pixels"


# ---------------------------------------------------------------- preview
def preview(path: Path) -> None:
    """Every set's cells on the footers' dark blue, one row per set, labelled."""
    js = cells()
    names = sorted(js["sets"])
    cw, ch = 84, 96
    sheet = Image.new("RGBA", (180 + cw * 8, ch * len(names)), (20, 20, 20, 255))
    dr = ImageDraw.Draw(sheet)
    for row, name in enumerate(names):
        dr.text((4, row * ch + 40), name, fill=(230, 230, 120, 255))
        for i, c in enumerate(js["sets"][name]):
            bg = Image.new("RGBA", (c["w"], c["h"]), (40, 40, 100, 255))
            bg.alpha_composite(Image.fromarray(decode_cell(c)))
            sheet.paste(bg, (180 + i * cw, row * ch + 8))
    path.parent.mkdir(parents=True, exist_ok=True)
    sheet.save(path)


if __name__ == "__main__":
    out = ROOT / "build" / "pad_glyphs" / "preview.png"
    preview(out)
    print(out)
