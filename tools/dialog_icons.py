r"""The join refusal pop-up's mod icons (1.1 ENABLE MODIFICATIONS; the user's request 2026-10-24; re\m9-modifications\JOIN-POPUP-FINDINGS.md).

When a join is refused for mods, the dialog box names each mod with the game list's marks: the red X (not installed here) and the
orange dash (another version here), drawn inline in the body text. The dialog fonts (fill af95e8ed, border 77f11b09) are in the
dialogs text-table members (section 3's PTEX, beside the strings tools\text_tables.py edits) and already define the private-use
characters F001 (a red circle) and F002 (a blue cross): the consoles' button pictures, used by no PC string. This tool repaints
those two cells: F001 = the X badge, F002 = the dash badge (tools\mod_icons.py badge(), from the lobby scenes' tick 9309f511),
scaled to the capitals' height; the border font gets a dark disc one pixel wider (its outline). Each glyph record's size,
advance and y offset are set to match: the capitals' cells are 25 px tall with yoff 18 (the cell's top above the font's
reference line), so a badge cell of SIZE px is centred on them. The changed DXT3 blocks are re-encoded (tools\steam_tiles.py,
alpha exact as tools\footer_keys.py). The body strings use U+F001 / U+F002 (UTF-8 EF 80 81 / EF 80 82; the text path is UTF-8).

    import dialog_icons; payload = dialog_icons.add_icons(entry, tick)   (tick: mod_icons.sprite_pixels(lobby scene, mod_icons.TICK))
"""
import struct
import zlib

import numpy as np
from PIL import Image, ImageDraw

import footer_keys as fk
import mod_icons
import steam_tiles as st
from scene_container import packer_for, sections6

FILL, BORDER = 0xAF95E8ED, 0x77F11B09
X_CHAR, DASH_CHAR = 0xF001, 0xF002
X_TEXT, DASH_TEXT = "", ""
OLD_CELL = 59          # the stock F001 / F002 cells (59x59)
SIZE = 28              # the new cell: a 26-px badge with a 1-px margin
BADGE = 26
CAP_YOFF, CAP_H = 18, 25  # the capitals' cells (A, X): their yoff and height, the same in every language checked
YOFF = round(CAP_YOFF - CAP_H / 2 + SIZE / 2)
ADVANCE = SIZE + 2


def font_index(ptex: bytes, font: int) -> dict:
    """footer_keys.font_chunk for any font of the chunk."""
    saved = fk.FONT_HASH
    fk.FONT_HASH = font
    try:
        return fk.font_chunk(ptex)
    finally:
        fk.FONT_HASH = saved


def cells(tick: np.ndarray) -> dict:
    """{(font, char): SIZE x SIZE RGBA}: the badges for the fill font, their dark discs for the border font."""
    out = {}
    for char, (hue, glyph, value) in ((X_CHAR, (mod_icons.X_HUE, "X", mod_icons.X_VALUE)), (DASH_CHAR, (mod_icons.DASH_HUE, "-", mod_icons.DASH_VALUE))):
        # The game list's badge shrunk to text size blurs its thin strokes: drawn again at 4x in its colours (its disc's middle
        # colour), with strokes in proportion, then scaled down.
        big = mod_icons.badge(tick, hue, glyph, value)
        disc = tuple(int(v) for v in big[big.shape[0] // 2 - 8, big.shape[1] // 2 - 1][:3])
        k, c = 4, SIZE * 2
        im = Image.new("RGBA", (SIZE * k, SIZE * k), (0, 0, 0, 0))
        dr = ImageDraw.Draw(im)
        r = BADGE / 2 * k
        dr.ellipse((c - r, c - r, c + r, c + r), fill=(30, 20, 40, 255))
        dr.ellipse((c - r + 1.5 * k, c - r + 1.5 * k, c + r - 1.5 * k, c + r - 1.5 * k), fill=disc + (255,))
        if glyph == "X":
            m = r * 0.42
            for width, colour in ((5 * k, (30, 20, 40, 255)), (3 * k, (255, 255, 255, 255))):
                dr.line((c - m, c - m, c + m, c + m), fill=colour, width=width)
                dr.line((c - m, c + m, c + m, c - m), fill=colour, width=width)
        else:
            m = r * 0.5
            dr.rectangle((c - m - k, c - 2.5 * k, c + m + k, c + 2.5 * k), fill=(30, 20, 40, 255))
            dr.rectangle((c - m, c - 1.5 * k, c + m, c + 1.5 * k), fill=(255, 255, 255, 255))
        out[(FILL, char)] = np.array(im.resize((SIZE, SIZE), Image.LANCZOS))
        border = Image.new("RGBA", (SIZE * 4, SIZE * 4), (0, 0, 0, 0))  # (drawn 4x, scaled down: a smooth edge)
        r = (BADGE / 2 + 1) * 4
        c = SIZE * 2
        ImageDraw.Draw(border).ellipse((c - r, c - r, c + r, c + r), fill=(30, 20, 40, 255))
        out[(BORDER, char)] = np.array(border.resize((SIZE, SIZE), Image.LANCZOS))
    return out


def add_icons(entry: bytes, tick: np.ndarray) -> bytes:
    """A dialogs text-table member (any language, stock or Metal Sonic's layout) with F001 / F002 repainted as the mod badges."""
    packer = packer_for(entry)
    a, d, s = sections6(entry)[3]
    ptex = bytearray(zlib.decompress(entry[a:a + s]))
    assert len(ptex) == d
    pl = st.pages(bytes(ptex))
    art = cells(tick)
    painted, originals = {}, {}
    records = []
    for font in (FILL, BORDER):
        fc = font_index(bytes(ptex), font)
        recs = {c: (at, f) for c, at, f in fk.glyph_records(bytes(ptex), fc)}
        for cap in (0x41, 0x58):
            f = recs[cap][1]
            assert (f[5], f[8]) == (CAP_H, CAP_YOFF), f"font {font:08x}: capital {cap:02x} is {f}"
        for char in (X_CHAR, DASH_CHAR):
            at, f = recs[char]
            assert f[4] == f[5] == OLD_CELL and f[3] == 0, f"font {font:08x}: glyph {char:x} is {f}"
            sprite = f[0]
            srec = ptex.find(struct.pack("<I", sprite))
            assert 0 < srec < pl[0][0], f"font {font:08x}: sprite {sprite:08x} not found"
            sf = struct.unpack_from("<I8fI", bytes(ptex), srec)
            page = pl[sf[9]]
            assert page[3] == b"DXT3" and page[4] == 0
            sx, sy = round(sf[1] * page[1]), round(sf[2] * page[2])
            assert abs(sf[1] * page[1] - sx) < 0.01 and abs(sf[2] * page[2] - sy) < 0.01
            if sf[9] not in painted:
                originals[sf[9]] = st.decode(bytes(ptex), page)
                painted[sf[9]] = originals[sf[9]].copy()
            img = painted[sf[9]]
            x, y = sx + f[2], sy + f[3]
            img[y:y + OLD_CELL, x:x + OLD_CELL] = 0
            img[y:y + SIZE, x:x + SIZE] = art[(font, char)]
            records.append((at, f))
    for index, img in painted.items():
        page = pl[index]
        blocks = st.reencode(ptex, page, originals[index], img)
        fk.exact_alpha(ptex, page, img, blocks)
        got = st.decode(bytes(ptex), page).astype(int)
        mask = img[:, :, 3] > 0
        err = np.abs(got - img.astype(int))[:, :, :3][mask].mean()
        assert err < 6 and np.abs(got[:, :, 3] - img[:, :, 3].astype(int)).max() <= 9, f"page {index}: re-encode error {err}"
    for at, f in records:  # {sprite, char, u, v, w, h, xoff, adv, yoff, x1, x2}: u, v kept
        new = (f[0], f[1], f[2], f[3], SIZE, SIZE, 0, ADVANCE, YOFF, f[9], f[10])
        struct.pack_into("<IHHHHHhhhHH", ptex, at, *new)
        assert struct.unpack_from("<IHHHHHhhhHH", bytes(ptex), at) == new
    assert len(ptex) == d
    return packer(zlib.compress(bytes(ptex), 9), len(ptex))


def check(payload: bytes) -> None:
    """The two glyphs of both fonts have the new cells."""
    a, d, s = sections6(payload)[3]
    ptex = zlib.decompress(payload[a:a + s])
    for font in (FILL, BORDER):
        recs = {c: f for c, at, f in fk.glyph_records(ptex, font_index(ptex, font))}
        for char in (X_CHAR, DASH_CHAR):
            assert recs[char][4:9] == (SIZE, SIZE, 0, ADVANCE, YOFF), f"{font:08x} {char:x}: {recs[char]}"
