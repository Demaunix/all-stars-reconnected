r"""Footer key pictures: a new key cap (the A key: INVITE FRIENDS in the lobby, VIEW PROFILE on the game list, the consoles'
triangle / Y button) in the game's navigator_info scenes.

The footer (fe_navigator) draws key pictures as characters of the font ff7f8ec5 in the navigator_info scene of
SumoToolResources.xpac (one copy per language, 7 scenes): icon 9 is the private-use character F007, which no scene
defines, so a footer entry with icon 9 shows the font's missing-glyph box. This tool adds that glyph to each scene:
- the key row (sprite e484cf68, page 1, seven 73x73 cells) is re-laid into eight cells: each cap keeps its place in its
  cell, only the empty right margins shrink (re\m7-keys\FINDINGS.md: the caps are ~35 px wide in 73 px cells);
- the new cap is painted in the eighth cell: the F1 cap with its label replaced by NEW_LABEL in a bold sans at the label's size;
- the changed DXT3 blocks of page 1 are re-encoded (tools\steam_tiles.py);
- the glyph record {sprite, char F007, u, v, w, h, xoff, adv, yoff, ...} is inserted into the font's sorted array before
  its 0xffff record; the fonts after it and every kern table move by one record (0x18 bytes), the FONT chunk grows.
Bases: the stock scenes (this PC's game archive) and any other mod's navigator payloads given as a base folder under
reference\navigator-bases\<mod>\<KEY>.dat (the same change on top of that mod's scene, for that mod's update: the loader
allows one mod per archive member, so a mod that replaces these scenes has to carry the keys itself).

    python tools\footer_keys.py <repo root>     (writes build\keys\<base>\<KEY>.dat, previews and keys.json)
Run without -I: needs numpy, Pillow and OpenCV (as steam_tiles.py).
"""
import hashlib
import json
import os
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import numpy as np  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402
import dialog_icons  # noqa: E402
import lobby_drift_row  # noqa: E402
import egg_lobby  # noqa: E402
import lobby_bars  # noqa: E402
import mod_icons  # noqa: E402
import steam_tiles as st  # noqa: E402
import text_tables  # noqa: E402
from scene_container import packer_for, sections6  # noqa: E402

SCENES = {0x0720C59E: "EN", 0x08538F72: "SP", 0x0B11E779: "JP", 0x11C1615B: "FR", 0x5706E9CD: "US", 0xA4877A54: "IT", 0xDFB2DA15: "GE"}
FONT_HASH = 0xFF7F8EC5     # the footer's key/label font
NEW_CHAR = 0xF007          # fe_navigator's icon 9
NEW_LABEL = "A"            # the key for INVITE FRIENDS (lobby) and VIEW PROFILE (game list): the consoles' triangle / Y button
CELL = 73                  # the key row: 7 cells of 73 px in a 512-px sprite (EN: e484cf68, page 1 at (0,464); each language has its own)
MARGIN = 4                 # transparent pixels kept right of each cap
GLYPH = 0x18               # bytes per glyph record
BASES = "reference/navigator-bases"
FONT_FILES = (r"C:\Windows\Fonts\arialbd.ttf", r"C:\Windows\Fonts\segoeuib.ttf", r"C:\Windows\Fonts\tahomabd.ttf")


def sha(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def align64(n: int) -> int:
    return (n + 63) // 64 * 64


# ---------------------------------------------------------------- the PTEX's FONT chunk
def font_chunk(ptex: bytes) -> dict:
    """The FONT chunk: its offset, sizes, font table and the key font's glyph array."""
    f = ptex.find(b"FONT")
    assert f > 0 and struct.unpack_from("<I", ptex, f + 12)[0] == 0x44332211, "FONT chunk not found"
    size, payload = struct.unpack_from("<2I", ptex, f + 4)
    # The payload follows the 16-byte header; the chunk is padded to 64 bytes (the padding differs per language).
    assert size == align64(0x10 + payload) and not any(ptex[f + 0x10 + payload:f + size]), "FONT chunk header"
    count = struct.unpack_from("<I", ptex, f + 0x18)[0]
    table = f + 0x1C
    fonts = [list(struct.unpack_from("<8I", ptex, table + 0x20 * i)) for i in range(count)]
    index = next(i for i, e in enumerate(fonts) if e[0] == FONT_HASH)
    kerns, glyphs = fonts[index][1] >> 16, fonts[index][1] & 0xFFFF
    base = f + 0x10  # offsets in the table count from here
    return {"at": f, "size": size, "payload": payload, "count": count, "table": table, "fonts": fonts, "index": index,
            "glyphs_at": base + fonts[index][2], "glyphs": glyphs, "kerns": kerns, "base": base}


def glyph_records(ptex: bytes, fc: dict) -> list:
    """(char, offset, fields) of every glyph of the key font; fields = (sprite, char, u, v, w, h, xoff, adv, yoff, x1, x2)."""
    out = []
    for i in range(fc["glyphs"]):
        at = fc["glyphs_at"] + GLYPH * i
        fields = struct.unpack_from("<IHHHHHhhhHH", ptex, at)
        out.append((fields[1], at, fields))
    return out


# ---------------------------------------------------------------- the caps
def cap_extent(row: np.ndarray, u: int) -> tuple:
    """The visible pixels of the cap in the 73-px cell at u: (left, right) columns within the cell."""
    al = row[:, u:u + CELL, 3]
    xs = np.nonzero(al.max(axis=0) > 0)[0]
    assert len(xs), "an empty key cell"
    return int(xs.min()), int(xs.max())


def label_box(f1: np.ndarray, f2: np.ndarray) -> tuple:
    """Where the digit of F1/F2 sits: (x0, x1, y0, y1) of the "2"'s ink right of the F (the F ends at x 32), and the box the
    digits and their anti-aliasing cover."""
    diff = np.abs(f1.astype(int) - f2.astype(int)).max(axis=2) > 12
    ys, xs = np.nonzero(diff)
    bx0, bx1, by0, by1 = int(xs.min()), int(xs.max()) + 1, int(ys.min()), int(ys.max()) + 1
    assert 20 <= bx0 <= 30 and 40 <= bx1 <= 50 and 20 <= by0 <= 28 and 32 <= by1 <= 40, f"the F1/F2 labels are not where expected: {bx0, bx1, by0, by1}"
    two = f2[by0:by1, 33:bx1, :3].min(axis=2) > 150
    ty, tx = np.nonzero(two)
    return int(tx.min()) + 33, int(tx.max()) + 34, by0 + int(ty.min()), by0 + int(ty.max()) + 1, (bx0, bx1, by0, by1)


def digit_image(text: str, target: tuple) -> Image.Image:
    """White bold text sized like the target (w, h): rendered 8x and scaled down (RGBA, cropped to its ink)."""
    path = next((p for p in FONT_FILES if os.path.exists(p)), None)
    assert path, "no bold sans font found in C:\\Windows\\Fonts"
    best = None
    for px in range(5, 20):
        font = ImageFont.truetype(path, px * 8)
        im = Image.new("L", (px * 24, px * 24), 0)
        ImageDraw.Draw(im).text((px * 8, px * 8), text, fill=255, font=font)
        im = im.crop(im.getbbox())
        small = im.resize((max(1, round(im.width / 8)), max(1, round(im.height / 8))), Image.LANCZOS)
        score = abs(small.height - target[1]) * 2 + abs(small.width - target[0])
        if best is None or score < best[0]:
            best = (score, small)
    out = Image.new("RGBA", best[1].size, (255, 255, 255, 0))
    out.putalpha(best[1])
    return out


def new_cap(row: np.ndarray, u_f1: int, u_f2: int) -> np.ndarray:
    """The new cap: the F1 cap with its whole "F1" label erased (the label's pixels in-painted from the face around them, as
    the STEAM tiles erased the old text) and NEW_LABEL drawn in a bold sans at the label's height, starting where the "F"
    started. (For a label that starts with F and a digit, e.g. "F3", only the digit's columns are replaced, keeping the game's
    own "F".)"""
    f1, f2 = row[:, u_f1:u_f1 + CELL].copy(), row[:, u_f2:u_f2 + CELL]
    dx0, dx1, dy0, dy1, (bx0, bx1, by0, by1) = label_box(f1, f2)
    cap = f1.copy()
    if NEW_LABEL[0] == "F" and NEW_LABEL[1:].isdigit():
        cap[by0 - 1:by1 + 1, 33:40] = f1[by0 - 1:by1 + 1, 40:47]  # the digit's columns take the plain face 7 px to their right
        glyph = digit_image(NEW_LABEL[1:], (dx1 - dx0, dy1 - dy0))
        at = (dx0, dy0)
    else:
        # The label's columns (the "F" starts at x 27, the "1" ends at 35, its anti-aliasing at 38) take, row by row, the plain
        # face right of the label (x 40..41 of the same row): the face shades from top to bottom and is nearly flat across, and
        # the cap's bevel (x <= 24) and its edge (x 25) are left alone, so nothing dark leaks in. The label area then sits in
        # the face's own brightness range (checked).
        assert (f1[by0 - 1:by1 + 1, 40:47, :3].min(axis=2) <= 130).all(), "the label reaches its fill's source"
        for y in range(by0 - 1, by1 + 1):
            cap[y, 26:40, :3] = np.round(f1[y, 40:42, :3].mean(axis=0)).astype(np.uint8)
        face = f1[by1 + 1:by1 + 4, 26:40, :3].mean()  # the face just below the label
        filled = cap[by0 - 1:by1 + 1, 26:40, :3].mean()
        assert abs(filled - face) < 20, f"the filled label area ({filled:.0f}) is off the face's brightness ({face:.0f})"
        glyph = digit_image(NEW_LABEL, (int(round((dx1 - dx0) * 1.6)) * len(NEW_LABEL), dy1 - dy0))
        at = (bx0 + 2, dy0)
    im = Image.fromarray(cap)
    im.alpha_composite(glyph, at)
    return np.asarray(im).copy()


# ---------------------------------------------------------------- one scene
def build_scene(entry: bytes, name: str, preview: Path = None) -> tuple:
    """A navigator_info scene container with the F007 glyph added: (payload bytes, facts dict)."""
    secs = sections6(entry)
    a, d, s = secs[3]
    stream = entry[a:a + s]
    packer = packer_for(entry)  # (asserts that the container's layout rule reproduces it)
    ptex = bytearray(zlib.decompress(stream))
    assert len(ptex) == d
    fc = font_chunk(bytes(ptex))
    records = glyph_records(bytes(ptex), fc)
    chars = [c for c, _, _ in records]
    assert chars == sorted(chars) and chars[-1] == 0xFFFF and NEW_CHAR not in chars, f"{name}: unexpected glyph order"
    keys = {c: (at, f) for c, at, f in records if 0xF000 <= c < 0xF007}
    assert len(keys) == 7, f"{name}: {len(keys)} key glyphs"
    sprite = keys[0xF000][1][0]  # the key row's sprite: its hash differs per language (EN: e484cf68)
    for c, (at, f) in keys.items():
        assert f[0] == sprite and f[3] == 0 and f[4] == CELL and f[5] == CELL and f[2] % CELL == 0 and f[2] < 7 * CELL, f"{name}: key glyph {c:x} {f}"
    pl = st.pages(bytes(ptex))
    # the sprite's record: {hash, 8 floats u0 v0 u1 v0 u0 v1 u1 v1 on its page (0..1), page}; the row is its top 73 rows
    srec = ptex.find(struct.pack("<I", sprite))
    assert 0 < srec < pl[0][0], f"{name}: key sprite record not found"
    sf = struct.unpack_from("<I8fI", bytes(ptex), srec)
    page_index = sf[9]
    page = pl[page_index]
    assert page[3] == b"DXT3" and page[4] == 0, f"{name}: key page {page_index} is {page[3]!r}"
    pw, ph = page[1], page[2]
    row_x, row_y = round(sf[1] * pw), round(sf[2] * ph)
    assert abs(sf[1] * pw - row_x) < 0.01 and abs(sf[2] * ph - row_y) < 0.01 and round((sf[3] - sf[1]) * pw) == 512 and (sf[6] - sf[2]) * ph >= CELL, f"{name}: key sprite rect {sf}"
    page1 = st.decode(bytes(ptex), page)
    row = page1[row_y:row_y + CELL, row_x:row_x + 512].copy()
    # The new layout: the caps in their old left-to-right order, each cell cut to its cap plus a margin, 4-px aligned.
    order = sorted(keys.items(), key=lambda kv: kv[1][1][2])
    layout, x = [], 0
    for c, (at, f) in order:
        left, right = cap_extent(row, f[2])
        w = (right + 1 + MARGIN + 3) // 4 * 4
        layout.append((c, at, f[2], x, w))
        x += w
    u_f1, u_f2 = keys[0xF002][1][2], keys[0xF003][1][2]
    new_w = min(CELL, 512 - x)
    assert new_w >= 56, f"{name}: no room for the new cap ({new_w} px left)"
    new_row = np.zeros_like(row)
    for c, at, old_u, new_u, w in layout:
        new_row[:, new_u:new_u + w] = row[:, old_u:old_u + w]
    cap = new_cap(row, u_f1, u_f2)
    new_row[:, x:x + new_w] = cap[:, :new_w]
    painted = page1.copy()
    painted[row_y:row_y + CELL, row_x:row_x + 512] = new_row
    blocks = st.reencode(ptex, page, page1, painted)
    exact_alpha(ptex, page, painted, blocks)
    got = st.decode(bytes(ptex), page).astype(int)
    mask = painted[:, :, 3] > 0
    err = np.abs(got - painted.astype(int))[:, :, :3][mask].mean()
    alpha_err = np.abs(got[:, :, 3] - painted[:, :, 3].astype(int)).max()
    assert err < 4 and alpha_err <= 9, f"{name}: page 1 re-encode error {err}, alpha {alpha_err}"
    # The font: the seven records' u (+6) and w (+10; v at +8 and h at +12 stay), then the new record before 0xffff; offsets
    # after it move by one record.
    for c, at, old_u, new_u, w in layout:
        struct.pack_into("<H", ptex, at + 6, new_u)
        struct.pack_into("<H", ptex, at + 10, w)
        assert struct.unpack_from("<IHHHHH", bytes(ptex), at) == (sprite, c, new_u, 0, w, CELL), f"{name}: key glyph {c:x} after the patch"
    ffff_at = records[-1][1]
    template = list(keys[0xF002][1])
    template[1], template[2], template[4] = NEW_CHAR, x, new_w
    new_rec = struct.pack("<IHHHHHhhhHH", *template)
    ptex[ffff_at:ffff_at] = new_rec
    f = fc["at"]
    # The chunk grows by one record; its 64-byte padding before the next chunk is made up again.
    new_payload = fc["payload"] + GLYPH
    new_size = align64(0x10 + new_payload)
    text_at = f + fc["size"] + GLYPH  # where the next chunk sits after the insertion
    delta = new_size - fc["size"] - GLYPH
    if delta > 0:
        ptex[text_at:text_at] = bytes(delta)
    elif delta < 0:
        assert not any(ptex[text_at + delta:text_at]), f"{name}: FONT padding is not zero"
        del ptex[text_at + delta:text_at]
    struct.pack_into("<2I", ptex, f + 4, new_size, new_payload)
    for i, e in enumerate(fc["fonts"]):
        at = fc["table"] + 0x20 * i
        glyph_off, kern_off = e[2], e[3]
        if i == fc["index"]:
            struct.pack_into("<I", ptex, at + 4, e[1] + 1)
        elif glyph_off > fc["fonts"][fc["index"]][2]:
            struct.pack_into("<I", ptex, at + 8, glyph_off + GLYPH)
        if kern_off > fc["fonts"][fc["index"]][2]:
            struct.pack_into("<I", ptex, at + 12, kern_off + GLYPH)
    # Check: the new chunk parses, the new glyph is found, every other font's first record is where it was.
    fc2 = font_chunk(bytes(ptex))
    recs2 = glyph_records(bytes(ptex), fc2)
    assert fc2["glyphs"] == fc["glyphs"] + 1 and fc2["size"] == new_size and fc2["payload"] == new_payload and recs2[-2][0] == NEW_CHAR and recs2[-1][0] == 0xFFFF
    for i, e in enumerate(fc["fonts"]):
        if i == fc["index"]:
            continue
        old_first = bytes(stream_glyph(zlib.decompress(stream), fc["base"] + e[2]))
        new_first = bytes(ptex[fc2["base"] + fc2["fonts"][i][2]:fc2["base"] + fc2["fonts"][i][2] + GLYPH])
        assert old_first == new_first, f"{name}: font {e[0]:08x} moved wrongly"
        old_kern = zlib.decompress(stream)[fc["base"] + e[3]:fc["base"] + e[3] + 16]
        assert bytes(ptex[fc2["base"] + fc2["fonts"][i][3]:fc2["base"] + fc2["fonts"][i][3] + 16]) == old_kern, f"{name}: kerns of {e[0]:08x} moved wrongly"
    tail_old = zlib.decompress(stream)[fc["at"] + fc["size"]:]
    assert bytes(ptex[fc2["at"] + fc2["size"]:]) == tail_old, f"{name}: the chunk after FONT changed"
    payload = packer(zlib.compress(bytes(ptex), 9), len(ptex))
    s3 = sections6(payload)[3]
    assert zlib.decompress(payload[s3[0]:s3[0] + s3[2]]) == bytes(ptex) and s3[1] == len(ptex)
    if preview is not None:
        bg = Image.new("RGBA", (512, CELL), (60, 60, 120, 255))
        bg.alpha_composite(Image.fromarray(st.decode(bytes(ptex), page)[row_y:row_y + CELL, row_x:row_x + 512]))
        bg.resize((512 * 2, CELL * 2), Image.NEAREST).save(preview)
    facts = {"glyphs": fc["glyphs"] + 1, "blocks": len(blocks), "reencode_error": round(float(err), 3), "sprite": f"{sprite:08x}",
             "row": [page_index, row_x, row_y], "cells": [(f"{c:04x}", new_u, w) for c, at, old_u, new_u, w in layout] + [(f"{NEW_CHAR:04x}", x, new_w)]}
    return payload, facts


def stream_glyph(ptex: bytes, at: int) -> bytes:
    return ptex[at:at + GLYPH]


def exact_alpha(ptex: bytearray, page: tuple, painted: np.ndarray, blocks: list) -> None:
    """Pillow's DXT3 encoder gets some edge alphas wrong (errors up to 200 on the caps' anti-aliased edges); DXT3 stores alpha
    as a plain 4-bit value per pixel, so the alpha half of every re-encoded block is rewritten from the painting itself."""
    q, w = page[0], page[1]
    bw = w // 4
    for i, old, new in blocks:
        b = (i - 128) // 16
        by, bx = divmod(b, bw)
        a = painted[by * 4:by * 4 + 4, bx * 4:bx * 4 + 4, 3].astype(int)
        nib = (a * 15 + 127) // 255
        rows = [int(nib[r, 0]) | int(nib[r, 1]) << 4 | int(nib[r, 2]) << 8 | int(nib[r, 3]) << 12 for r in range(4)]
        ptex[q + i:q + i + 8] = struct.pack("<4H", *rows)


# ---------------------------------------------------------------- all scenes, every base
# The tools whose change rebuilds the scenes (tools/package.py checks the same list).
INPUTS = ("steam_tiles.py", "text_tables.py", "dialog_icons.py", "scene_container.py", "lobby_bars.py", "egg_lobby.py", "mod_icons.py", "lobby_drift_row.py", "codexui/build_ui.py",
          "codexui/decode_ui_texture.py", "codexui/probe.py")
METAL_PORTRAIT_SCENE = "metal-sonic-1.1.0-beta.3/E01CF5DC.dat"  # (under BASES) Metal Sonic's racer portrait for the game list's mod icons


def build(root: Path) -> dict:
    """Writes build/keys/<base>/<KEY>.dat (+ previews) and build/keys/keys.json; returns the json. Beside the 7 navigator scenes,
    each base gets the text tables of tools\\text_tables.py (the lobby popup's VIEW STEAM PROFILE, UNMUTE) and the 7 lobby scenes
    with the yellow bar of tools\\lobby_bars.py (M8): replaced members Metal Sonic replaces too, so each carries a variant made on
    Metal's version, for the same reason."""
    out = root / "build" / "keys"
    out.mkdir(parents=True, exist_ok=True)
    path = st.GAME / st.ARCHIVE
    _, rows = st.read_rows(path)
    members = list(SCENES) + list(lobby_bars.SCENES) + list(text_tables.EDITS) + list(egg_lobby.POPUP_SCENES)
    bases = {"stock": {key: st.member(path, rows, key) for key in members}}
    for folder in sorted((root / BASES).glob("*")) if (root / BASES).exists() else []:
        files = {key: folder / ("%08X.dat" % key) for key in members}
        if all(p.exists() for p in files.values()):
            bases[folder.name] = {key: p.read_bytes() for key, p in files.items()}
        else:
            print(f"{folder.name}: skipped, not all of {[('%08X' % k) for k in members]} are there")
    tools = Path(__file__).resolve().parent
    inputs = {name: sha((tools / name).read_bytes()) for name in INPUTS}
    for base, entries in bases.items():
        for key, entry in entries.items():
            inputs[f"{base}/{key:08X}"] = sha(entry)
    result = {"tool_sha256": sha(Path(__file__).read_bytes()), "inputs": inputs, "bases": {}}
    for base, entries in bases.items():
        folder = out / base
        folder.mkdir(exist_ok=True)
        for old in folder.glob("*"):
            old.unlink()
        ops = []
        for key, entry in entries.items():
            if key in SCENES:
                name = f"{base}/{SCENES[key]}"
                payload, facts = build_scene(entry, name, folder / ("%08X-row.png" % key))
                op_name = f"navigator_info scene ({SCENES[key]}): the {NEW_LABEL} key picture (INVITE FRIENDS / VIEW PROFILE)"
                print(f"{name}: {facts['glyphs']} glyphs, {facts['blocks']} blocks re-encoded (error {facts['reencode_error']}), cells {facts['cells']}")
            elif key in lobby_bars.SCENES:
                name = f"{base}/lobby {lobby_bars.SCENES[key]}"
                payload, facts = lobby_bars.build_scene(entry, name)
                payload = egg_lobby.add_card(payload, lobby_bars.SCENES[key])
                payload = mod_icons.add_icons(payload, (root / BASES / METAL_PORTRAIT_SCENE).read_bytes(), lobby_bars.SCENES[key])
                payload, row = lobby_drift_row.add_row(payload, name)
                facts["drift_row"] = row
                op_name = (f"lobby scene ({lobby_bars.SCENES[key]}): the yellow bar of a muted player, Egg Hangar's course card, "
                           "the game list's mod icons, the rules panel's DRIFT+ row")
                print(f"{name}: record {facts['index']}, {facts['blocks']} blocks re-encoded (error {facts['reencode_error']}), "
                      f"{facts['pointers_moved']} pointers / {facts['relo_fields_moved']} RELO fields moved")
            elif key in egg_lobby.POPUP_SCENES:
                lang = egg_lobby.POPUP_SCENES[key]
                payload = egg_lobby.add_popup_name(entry, lang)
                facts = {"text": "%08X" % egg_lobby.NAME}
                op_name = f"lobby Settings popup scene ({lang}): Egg Hangar's course name"
                print(f"{base}/popups {lang}: Egg Hangar's name ({len(entry)} -> {len(payload)} stored bytes)")
            else:
                payload = text_tables.patch_entry(entry, text_tables.EDITS[key])
                # 1.1: the join refusal pop-up's mod badges (F001 / F002 in the dialog fonts; the tick from this base's English lobby scene)
                payload = dialog_icons.add_icons(payload, mod_icons.sprite_pixels(entries[0xE01CF5DC], mod_icons.TICK))
                dialog_icons.check(payload)
                facts = {"strings": {"%08X" % k: v for k, v in text_tables.EDITS[key].items()}, "glyphs": ["F001 mod X", "F002 mod dash"]}
                op_name = text_tables.NAMES[key] + "; the mod badges (F001 / F002) in the dialog fonts"
                print(f"{base}/{key:08X}: {op_name} ({len(entry)} -> {len(payload)} stored bytes)")
            (folder / ("%08X.dat" % key)).write_bytes(payload)
            ops.append({"key": "%08X" % key, "kind": "replace", "name": op_name, "payload": "%s/%08X.dat" % (st.PAYLOADS, key),
                        "payload_sha256": sha(payload), "stored_size": len(payload), "entry_flags": 0, "base_stored_sha256": sha(entry),
                        "facts": facts})
        result["bases"][base] = {"operations": ops}
    (out / "keys.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    return result


if __name__ == "__main__":
    r = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[1])
    build(r)
