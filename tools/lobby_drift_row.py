r"""The lobby panel's DRIFT+ row: an ADDITION (the consoles had none; the user's request 2026-10-27, with a screenshot).

The online lobby's rules panel (scene 8c2f0f09, root 135fd519: the course art and name, then LAPS / PRIVATE SLOTS / AI RACERS /
POWER-UPS / CATCH-UP) gets a 6th row under CATCH-UP: "DRIFT+  ON / OFF", darkened and never selectable, ON while every player
drives with a controller, OFF while a keyboard player is in (the race rule of src\steer.h; the C++ writes the value each lobby
frame, natives.cpp DrawDriftRow). No spare row exists in any build (re\lobby-drift-row\FINDINGS.md), so it is added:
  - 8 new objects cloned from the CATCH-UP row (its group d9001da0 with its 5 quads, its label f54a1de2 and value b1e14b39), each
    key moved one row down (the row spacing, measured from the POWER-UPS row to the CATCH-UP row), the two texts darkened
    (keyframe colour RGB ffffff -> 8c8c8c, alpha kept), the label's key STEAMMP_DRIFT_PLUS (text "DRIFT+", added to the text
    table), the value's key ON; the three top-level ones join the panel root's children before the red overlay ff1cc02e;
  - OBJS and KFRM: the new entries sorted in, every data offset (and every group's child-list pointer) moved past the entry table,
    the new data appended; the root's data (one child list longer) is appended too and its entry points there; the RELO lists
    are regenerated in their original order (checked: with nothing added they reproduce the original bytes); the chunk sizes
    re-laid (align64), the later chunks move with them;
  - the panel's backdrop 9425ff83 (and its red cursor twin 0c59643a) are repainted 44 px taller inside their own sprite rect
    (rows 443-488 were transparent): the bottom border moved down, the gap filled with the last interior row. Only the changed
    DXT3 blocks are re-encoded (steam_tiles.reencode), their alpha exact.
Every text in this panel is one TEXT element in font 1262d7ba, which has '+' (no font change: the popup attempt's mixed fonts
drew "DRIFT+" twice).

    import lobby_drift_row; payload, facts = lobby_drift_row.add_row(payload, name)
Needs numpy and Pillow (run without -I, as footer_keys.py).
"""
import struct
import sys
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parent / "codexui"))
import numpy as np  # noqa: E402
import steam_tiles as st  # noqa: E402
from build_ui import merge_text  # noqa: E402
from lobby_bars import align64, exact_alpha, relo_chunks  # noqa: E402
from netmenu.pkg import name_hash  # noqa: E402
from probe import text_info  # noqa: E402
from scene_container import packer_for, sections6  # noqa: E402

ROOT, OVERLAY = 0x135FD519, 0x0C59643A
RED_ELEMENT = 0xFF1CC02E
ROWS = (0xE2B20305, 0x6DCBB481, 0x181499BB, 0x7032AB2F, 0xD9001DA0)
LABELS = (0x44DAB7DB, 0x1E7D5398, 0xF42AE5ED, 0x7C1AD0FA, 0xF54A1DE2)
VALUES = (0xBFFC912A, 0x8E6475C9, 0x5D972022, 0xD8B238BD, 0xB1E14B39)
FONT = 0x1262D7BA
KEY = name_hash("STEAMMP_DRIFT_PLUS")
assert KEY == 0x852717F3
TEXT_ON = 0xFFC1DF26
GROUP = name_hash("STEAMMP_LOBBY_DRIFT_ROW")
LABEL = name_hash("STEAMMP_LOBBY_DRIFT_ROW_LABEL")
VALUE = name_hash("STEAMMP_LOBBY_DRIFT_ROW_VALUE")
assert VALUE == 0xC96A369E  # (natives.cpp kDriftValue)
QUADS = tuple(name_hash("STEAMMP_LOBBY_DRIFT_ROW_Q%d" % i) for i in range(5))
DIM = 0x8C8C8C
BACKDROPS = (0x9425FF83, 0x0C59643A)  # the box and its red twin, page 0
# Sprites are looked up by hash across every loaded scene, and other screens carry the stock box, so the taller box gets its own
# sprite records (same page and rect, new names) and the panel's two backdrop elements use them.
BACKDROP_ELEMENTS = {0x53351352: 0x9425FF83, 0xFF1CC02E: 0x0C59643A}  # TXTR element: its stock sprite
NEW_SPRITES = {0x9425FF83: name_hash("STEAMMP_LOBBY_PANEL_BOX"), 0x0C59643A: name_hash("STEAMMP_LOBBY_PANEL_BOX_RED")}
ROW_DIM = 0.55  # the DRIFT+ row's own box (its fill and border quads), darkened too (the user, 2026-10-27)
GROW = 44  # px: one row (0.0215 x 2048)


def _chunks(sec: bytes) -> list:
    """[(offset, tag, size)] walked by size."""
    out, p = [], 0
    while p < len(sec):
        tag = bytes(sec[p:p + 4])
        size = struct.unpack_from("<I", sec, p + 4)[0]
        assert size >= 16 and p + size <= len(sec)
        out.append((p, tag, size))
        p += size
    return out


def _relo(sec4: bytes, tag: bytes) -> tuple:
    """(RELO chunk offset, size, [(kind, field)] without the (0, 0) end) of one chunk's RELO."""
    q, entries = relo_chunks(sec4)[tag]
    size, used = struct.unpack_from("<II", sec4, q + 4)
    recs = [(k, f) for _, k, f in entries]
    assert recs[-1] == (0, 0)
    return q, size, recs[:-1]


def _relo_bytes(tag: bytes, recs: list) -> bytes:
    """A RELO chunk for `tag` with these records (+ the end record), laid out as the game's."""
    used = 4 + 8 * (len(recs) + 1)
    body = struct.pack("<4sIII4s", b"RELO", align64(16 + used), used, 0x44332211, tag)
    body += b"".join(struct.pack("<2I", k, f) for k, f in recs) + struct.pack("<2I", 0, 0)
    return body + bytes(align64(16 + used) - len(body))


def _chunk_bytes(tag: bytes, payload: bytes) -> bytes:
    """A chunk with this payload (everything after the 16-byte header), padded to align64."""
    used = len(payload)
    size = align64(16 + used)
    return struct.pack("<4sIII", tag, size, used, 0x44332211) + payload + bytes(size - 16 - used)


def _kfrm(c: bytes) -> tuple:
    """(entries [(hash, off, cnt, w, h)], data start) of a KFRM chunk; offsets are from +0x10."""
    n = struct.unpack_from("<I", c, 0x10)[0]
    ents = [struct.unpack_from("<IIIff", c, 0x14 + 0x14 * i) for i in range(n)]
    return ents, 0x14 + 0x14 * n


def _key_stride(cnt: int) -> int:
    """Bytes per key (GORD keys carry 4 vertex colours)."""
    return 0x38 if (cnt >> 16) & 2 else 0x28


def _keys(c: bytes, ent: tuple) -> bytearray:
    """One object's key block."""
    h, off, cnt, w, hh = ent
    return bytearray(c[0x10 + off:0x10 + off + (cnt & 0xFFFF) * _key_stride(cnt)])


def _objs(c: bytes) -> list:
    """[(hash, type, off, z)] of an OBJS chunk (offsets from +0x10)."""
    n = struct.unpack_from("<I", c, 0x18)[0]
    return [struct.unpack_from("<I4sII", c, 0x1C + 16 * i) for i in range(n)]


SELF_AT = {b"GROP": 0, b"GORD": 0, b"TEXT": 8}  # where an object's data names the object itself (a clone must carry its own name)


def _clone(data: bytes, typ: bytes, old: int, new: int) -> bytearray:
    """An object's data renamed to `new`."""
    d = bytearray(data)
    assert struct.unpack_from("<I", d, SELF_AT[typ])[0] == old, f"{old:08x}: its data's own name"
    struct.pack_into("<I", d, SELF_AT[typ], new)
    return d


def _obj_data(c: bytes, ent: tuple) -> bytes:
    """An object's data (TEXT 68 bytes, GORD 40, GROP 0x24 + 4 per child)."""
    h, typ, off, z = ent
    n = {b"TEXT": 68, b"GORD": 40}.get(typ)
    if typ == b"GROP":
        n = 0x24 + 4 * struct.unpack_from("<I", c, 0x10 + off + 0x1C)[0]
    assert n, f"{h:08x}: type {typ}"
    return bytes(c[0x10 + off:0x10 + off + n])


def rebuild_kfrm(c: bytes, relo: list, added: dict) -> tuple:
    """(KFRM chunk bytes, RELO records) with `added` {hash: (cnt, w, h, key block)} sorted in."""
    ents, start = _kfrm(c)
    used = struct.unpack_from("<I", c, 8)[0]
    assert relo == [(1, 8 + 0x14 * i) for i in range(len(ents))], "KFRM RELO: the entries' offsets only"
    assert min(e[1] for e in ents) + 0x10 >= start, "KFRM data inside the entry table"
    shift = 0x14 * len(added)
    data = bytearray(c[start:0x10 + used])
    rows = [(h, off + shift, cnt, w, hh) for h, off, cnt, w, hh in ents]
    for h, (cnt, w, hh, block) in added.items():
        assert h not in {e[0] for e in ents}
        rows.append((h, start + shift - 0x10 + len(data), cnt, w, hh))
        data += block
    rows.sort()
    payload = struct.pack("<I", len(rows)) + b"".join(struct.pack("<IIIff", *r) for r in rows) + bytes(data)
    return _chunk_bytes(b"KFRM", payload), [(1, 8 + 0x14 * i) for i in range(len(rows))]


def rebuild_objs(c: bytes, relo: list, added: dict, replace: dict) -> tuple:
    """(OBJS chunk bytes, RELO records) with `added` {hash: (type, z, data)} sorted in and `replace` {hash: data} (a group whose
    child list grew) appended as new data; group child-list pointers kept pointing at their lists."""
    ents = _objs(c)
    used = struct.unpack_from("<I", c, 8)[0]
    start = 0x1C + 16 * len(ents)
    n0 = len(ents)
    groups = [(h, off) for h, typ, off, z in ents if typ == b"GROP"]
    want = [(1, off + 0x20) for h, off in sorted(groups, key=lambda g: relo.index((1, g[1] + 0x20)))] + \
           [(1, 0x14 + 16 * i) for i in range(n0)]
    assert relo == want, "OBJS RELO: the groups' child lists, then the entries' offsets"
    for h, off in groups:
        assert struct.unpack_from("<I", c, 0x10 + off + 0x20)[0] == off + 0x24, f"{h:08x}: child-list pointer"
    shift = 16 * len(added)
    data = bytearray(c[start:0x10 + used])
    base = start + shift - 0x10  # the data's new offset from +0x10
    table = {h: [typ, off + shift, z] for h, typ, off, z in ents}
    for h, off in groups:  # the child-list pointers, moved with their data
        q = 0x10 + off + 0x20 - start
        struct.pack_into("<I", data, q, struct.unpack_from("<I", data, q)[0] + shift)
    group_fields = [off + shift + 0x20 for h, off in sorted(groups, key=lambda g: relo.index((1, g[1] + 0x20)))]
    for h, d in list(replace.items()) + [(h, v[2]) for h, v in added.items()]:
        at = base + len(data)
        d = bytearray(d)
        if (table.get(h) or added[h])[0] == b"GROP":
            struct.pack_into("<I", d, 0x20, at + 0x24)
            if h in replace:
                group_fields[[g for g, _ in sorted(groups, key=lambda g: relo.index((1, g[1] + 0x20)))].index(h)] = at + 0x20
            else:
                group_fields.append(at + 0x20)
        if h in added:
            assert h not in table
            table[h] = [added[h][0], at, added[h][1]]
        else:
            table[h][1] = at
        data += d
    rows = sorted((h, typ, off, z) for h, (typ, off, z) in table.items())
    payload = bytes(8) + struct.pack("<I", len(rows)) + b"".join(struct.pack("<I4sII", *r) for r in rows) + bytes(data)
    recs = [(1, f) for f in group_fields] + [(1, 0x14 + 16 * i) for i in range(len(rows))]
    return _chunk_bytes(b"OBJS", payload), recs


def _move_row(block: bytearray, stride: int, deltas: list, dim: bool) -> bytearray:
    """A copy of a key block one row lower (each key's y + its delta), its colours darkened if `dim`."""
    out = bytearray(block)
    for j in range(len(block) // stride):
        q = stride * j
        y = struct.unpack_from("<f", out, q + 0x10)[0]
        struct.pack_into("<f", out, q + 0x10, y + deltas[j])
        if dim:
            col = struct.unpack_from("<I", out, q + 8)[0]
            if col & 0xFFFFFF == 0xFFFFFF:
                struct.pack_into("<I", out, q + 8, (col & 0xFF000000) | DIM)
    return out


def _deltas(c: bytes, ents: dict, above: int, row: int) -> list:
    """Each key's y step from the object of the row above to this row's (the row spacing per key)."""
    a, b = _keys(c, ents[above]), _keys(c, ents[row])
    sa, sb = _key_stride(ents[above][2]), _key_stride(ents[row][2])
    assert sa == sb and len(a) == len(b), "the two rows' keys differ"
    out = []
    for j in range(len(b) // sb):
        ya, yb = (struct.unpack_from("<f", k, sb * j + 0x10)[0] for k in (a, b))
        assert struct.unpack_from("<2I", a, sa * j) == struct.unpack_from("<2I", b, sb * j), "the two rows' key states"
        out.append(yb - ya)
    return out


def _dim(col: int, f: float) -> int:
    """A colour's three low bytes scaled by f, alpha (the top byte) kept."""
    return (col & 0xFF000000) | sum(round(((col >> s) & 0xFF) * f) << s for s in (0, 8, 16))


def _dim_quad(block: bytes) -> bytes:
    """A quad's key block (0x38-byte keys: colour +8, 4 vertex colours +0x28) darkened."""
    out = bytearray(block)
    for q in range(0, len(out), 0x38):
        for at in (8, 0x28, 0x2C, 0x30, 0x34):
            struct.pack_into("<I", out, q + at, _dim(struct.unpack_from("<I", out, q + at)[0], ROW_DIM))
    return bytes(out)


def _add_sprites(sec3: bytearray, sec4: bytearray, name: str) -> None:
    """NEW_SPRITES' records (copies of the stock ones, renamed) sorted into the PTEX chunk, its pointers moved (as lobby_bars)."""
    size, used = struct.unpack_from("<II", sec3, 4)
    count = struct.unpack_from("<I", sec3, 0x10)[0]
    recs = [bytes(sec3[0x1C + 40 * i:0x1C + 40 * (i + 1)]) for i in range(count)]
    hashes = [struct.unpack_from("<I", r, 0)[0] for r in recs]
    assert hashes == sorted(hashes) and not set(NEW_SPRITES.values()) & set(hashes), f"{name}: PTEX records"
    for old, new in sorted(NEW_SPRITES.items(), key=lambda kv: kv[1]):
        record = struct.pack("<I", new) + recs[hashes.index(old)][4:]
        hashes = [struct.unpack_from("<I", sec3, 0x1C + 40 * i)[0] for i in range(count)]
        index = next((i for i, h in enumerate(hashes) if h > new), count)
        at = 0x1C + 40 * index
        rq, entries = relo_chunks(bytes(sec4))[b"PTEX"]
        for e_at, kind, field in entries:
            if not kind:
                continue
            value = struct.unpack_from("<I", sec3, 0x10 + field)[0]
            if 0x10 + value >= at:
                struct.pack_into("<I", sec3, 0x10 + field, value + 40)
            if 0x10 + field >= at:
                struct.pack_into("<I", sec4, e_at + 4, field + 40)
        sec3[at:at] = record
        new_used = used + 40
        new_size = align64(0x10 + new_used)
        pad_at = 0x10 + new_used
        del sec3[pad_at:size + 40]
        sec3[pad_at:pad_at] = bytes(new_size - pad_at)
        struct.pack_into("<3I", sec3, 4, new_size, new_used, 0x44332211)
        count += 1
        struct.pack_into("<I", sec3, 0x10, count)
        size, used = new_size, new_used
    hashes = [struct.unpack_from("<I", sec3, 0x1C + 40 * i)[0] for i in range(count)]
    assert hashes == sorted(hashes) and all(h in hashes for h in NEW_SPRITES.values()), f"{name}: the new sprites"


def _repaint(sec3: bytearray, name: str) -> dict:
    """The backdrop and its red twin 44 px taller inside their rects (page 0)."""
    count = struct.unpack_from("<I", sec3, 0x10)[0]
    recs = {struct.unpack_from("<I", sec3, 0x1C + 40 * i)[0]: struct.unpack_from("<I8fI", sec3, 0x1C + 40 * i) for i in range(count)}
    pl = st.pages(bytes(sec3))
    rects = []
    for h in BACKDROPS:
        r = recs[h]
        page = pl[r[9]]
        rects.append((r[9], round(r[1] * page[1]), round(r[2] * page[2]), round(r[5] * page[1]), round(r[6] * page[2])))
    assert len({r[0] for r in rects}) == 1, f"{name}: the two backdrops on different pages"
    page = pl[rects[0][0]]
    old = st.decode(bytes(sec3), page)
    new = old.copy()
    heights = []
    for _, x0, y0, x1, y1 in rects:
        box = old[y0:y1, x0:x1]
        alpha = box[:, :, 3]
        last = max(y for y in range(box.shape[0]) if alpha[y].any())  # the bottom border's last row
        assert not alpha[last + 1:].any() and y1 - y0 - 1 - last >= GROW, f"{name}: no {GROW} free rows under the box ({last})"
        # The interior (its checkered pattern) stretched evenly between the top border and the moved bottom border.
        heights.append(last + 1)
        border, top = last - 6, 3  # the bottom border with its anti-aliasing; the first interior row under the top border
        nb = new[y0:y1, x0:x1]
        nb[border + GROW:last + 1 + GROW] = box[border:last + 1]
        src = box[top:border].astype(float)
        for y in range(top, border + GROW):
            f = (y - top) * (border - 1 - top) / (border + GROW - 1 - top)
            i = int(f)
            j = min(i + 1, border - 1 - top)
            nb[y] = np.round(src[i] * (1 - (f - i)) + src[j] * (f - i)).astype(np.uint8)
    blocks = st.reencode(sec3, page, old, new)
    exact_alpha(sec3, page, new, blocks)
    got = st.decode(bytes(sec3), page).astype(int)
    mask = new[:, :, 3] > 0
    err = np.abs(got - new.astype(int))[:, :, :3][mask].mean()
    assert err < 4, f"{name}: re-encode error {err}"
    assert len(set(heights)) == 1, f"{name}: the two boxes' heights differ"
    return {"blocks": len(blocks), "reencode_error": round(float(err), 3), "scale": heights[0] / (heights[0] + GROW)}


def add_row(entry: bytes, name: str) -> tuple:
    """A lobby scene container with the DRIFT+ row added: (payload bytes, facts)."""
    secs = sections6(entry)
    a3, d3, s3 = secs[3]
    a4, d4, s4 = secs[4]
    pack = packer_for(entry)
    sec3 = bytearray(zlib.decompress(entry[a3:a3 + s3]))
    sec4 = bytearray(zlib.decompress(entry[a4:a4 + s4]))
    facts = _repaint(sec3, name)
    pages_before = [p[1:] for p in st.pages(bytes(sec3))]
    _add_sprites(sec3, sec4, name)
    assert [p[1:] for p in st.pages(bytes(sec3))] == pages_before, f"{name}: the pages after the new sprite records"
    chunks = _chunks(sec3)
    tags = [t for _, t, _ in chunks]
    get = {t: bytes(sec3[p:p + n]) for p, t, n in chunks}
    relos = {t: _relo(bytes(sec4), t) for t in (b"KFRM", b"OBJS", b"TEXT")}
    # Self-check: rebuilt with nothing added, KFRM and OBJS are the original bytes.
    k0, kr0 = rebuild_kfrm(get[b"KFRM"], relos[b"KFRM"][2], {})
    o0, or0 = rebuild_objs(get[b"OBJS"], relos[b"OBJS"][2], {}, {})
    assert k0 == get[b"KFRM"] and kr0 == relos[b"KFRM"][2], f"{name}: KFRM does not rebuild as it is"
    assert o0 == get[b"OBJS"] and or0 == relos[b"OBJS"][2], f"{name}: OBJS does not rebuild as it is"
    for t in (b"KFRM", b"OBJS"):
        q, size, recs = relos[t]
        assert _relo_bytes(t, recs) == bytes(sec4[q:q + size]), f"{name}: {t} RELO does not rebuild as it is"
    k, o = get[b"KFRM"], bytearray(get[b"OBJS"])
    kents = {e[0]: e for e in _kfrm(k)[0]}
    oents = {e[0]: e for e in _objs(o)}
    for element, sprite in BACKDROP_ELEMENTS.items():  # the backdrops show the taller box's own sprites
        at = 0x10 + oents[element][2]
        assert oents[element][1] == b"TXTR" and struct.unpack_from("<I", o, at)[0] == sprite, f"{name}: {element:08x}'s sprite"
        struct.pack_into("<I", o, at, NEW_SPRITES[sprite])
    o = bytes(o)
    for h in (GROUP, LABEL, VALUE) + QUADS:
        assert h not in kents and h not in oents, f"{name}: {h:08x} taken"
    for h in LABELS + VALUES:
        assert struct.unpack_from("<I", _obj_data(o, oents[h]), 0)[0] == FONT, f"{name}: {h:08x}'s font"
    root = bytearray(_obj_data(o, oents[ROOT]))
    kids = list(struct.unpack_from("<%dI" % struct.unpack_from("<I", root, 0x1C)[0], root, 0x24))
    assert ROWS[4] in kids and LABELS[4] in kids and VALUES[4] in kids and RED_ELEMENT in kids, f"{name}: the panel's children"
    template = _obj_data(o, oents[ROWS[4]])
    quads = list(struct.unpack_from("<5I", template, 0x24))
    assert struct.unpack_from("<I", template, 0x1C)[0] == 5, f"{name}: the CATCH-UP row's quads"
    above_quads = list(struct.unpack_from("<5I", _obj_data(o, oents[ROWS[3]]), 0x24))
    # New objects (data and keys).
    added_o, added_k = {}, {}
    g = _clone(template, b"GROP", ROWS[4], GROUP)
    struct.pack_into("<5I", g, 0x24, *QUADS)
    added_o[GROUP] = (b"GROP", oents[ROWS[4]][3], bytes(g))
    e = kents[ROWS[4]]
    added_k[GROUP] = (e[2], e[3], e[4], _move_row(_keys(k, e), _key_stride(e[2]), _deltas(k, kents, ROWS[3], ROWS[4]), False))
    for new, src, above in zip(QUADS, quads, above_quads):
        assert _keys(k, kents[src]) == _keys(k, kents[above]), f"{name}: the rows' quads differ ({src:08x})"
        added_o[new] = (b"GORD", oents[src][3], bytes(_clone(_obj_data(o, oents[src]), b"GORD", src, new)))
        e = kents[src]
        assert _key_stride(e[2]) == 0x38, f"{name}: {src:08x}'s keys"
        added_k[new] = (e[2], e[3], e[4], _dim_quad(bytes(_keys(k, e))))
    for new, src, above, key in ((LABEL, LABELS[4], LABELS[3], KEY), (VALUE, VALUES[4], VALUES[3], TEXT_ON)):
        d = _clone(_obj_data(o, oents[src]), b"TEXT", src, new)
        struct.pack_into("<I", d, 4, key)
        added_o[new] = (b"TEXT", oents[src][3], bytes(d))
        e = kents[src]
        added_k[new] = (e[2], e[3], e[4], _move_row(_keys(k, e), _key_stride(e[2]), _deltas(k, kents, above, src), True))
    i = kids.index(RED_ELEMENT)
    kids[i:i] = [GROUP, LABEL, VALUE]
    root = root[:0x24] + struct.pack("<%dI" % len(kids), *kids)
    struct.pack_into("<I", root, 0x1C, len(kids))
    # The panel is only drawn inside its old box's window on screen (seen in game: dp5-dp6 cut the new row off at the old bottom,
    # dp7 moved up cut the course art off at the top), so the whole panel is scaled down evenly to its old height (the root's
    # keys' scale, about its top-left corner): nothing cropped or squashed (the user left the choice to us, 2026-10-27).
    k = bytearray(k)
    e = kents[ROOT]
    st_ = _key_stride(e[2])
    for j in range(e[2] & 0xFFFF):
        q = 0x10 + e[1] + st_ * j
        sx, sy = struct.unpack_from("<2f", k, q + 0x14)
        struct.pack_into("<2f", k, q + 0x14, sx * facts["scale"], sy * facts["scale"])
    k = bytes(k)
    k2, kr = rebuild_kfrm(k, relos[b"KFRM"][2], added_k)
    o2, orr = rebuild_objs(o, relos[b"OBJS"][2], added_o, {ROOT: bytes(root)})
    # The text.
    tq, tsize, _ = relos[b"TEXT"]
    t2, tr2, _ = merge_text(get[b"TEXT"], bytes(sec4[tq:tq + tsize]), {KEY: b"DRIFT+"})
    # Section 3 and 4 laid out again in their order.
    new3, new4 = bytearray(), bytearray()
    q = 0
    rel4 = []
    while q < len(sec4):
        size = struct.unpack_from("<I", sec4, q + 4)[0]
        rel4.append((bytes(sec4[q + 16:q + 20]), bytes(sec4[q:q + size])))
        q += size
    assert [t for t, _ in rel4] == tags, f"{name}: RELO chunks don't follow section 3's"
    for (p, t, n), (rt, rb) in zip(chunks, rel4):
        if t == b"KFRM":
            new3 += k2
            new4 += _relo_bytes(t, kr)
        elif t == b"OBJS":
            new3 += o2
            new4 += _relo_bytes(t, orr)
        elif t == b"TEXT":
            new3 += t2
            new4 += tr2
        else:
            new3 += sec3[p:p + n]
            new4 += rb
    check(bytes(new3), bytes(new4), name)
    payload = pack(zlib.compress(bytes(new3), 9), len(new3), zlib.compress(bytes(new4), 9), len(new4))
    facts.update({"objects_added": 8, "kfrm": len(k2) - len(k), "objs": len(o2) - len(o)})
    return payload, facts


def check(sec3: bytes, sec4: bytes, name: str) -> None:
    """The built scene reads back: sorted entries, every RELO pointer inside its chunk, the row's objects, keys and text."""
    chunks = {t: sec3[p:p + n] for p, t, n in _chunks(sec3)}
    k, o = chunks[b"KFRM"], chunks[b"OBJS"]
    kents, oents = _kfrm(k)[0], _objs(o)
    assert [e[0] for e in kents] == sorted(e[0] for e in kents) == [e[0] for e in oents], f"{name}: entries"
    for t in (b"KFRM", b"OBJS"):
        c = chunks[t]
        used = struct.unpack_from("<I", c, 8)[0]
        for kind, f in _relo(sec4, t)[2]:
            v = struct.unpack_from("<I", c, 0x10 + f)[0]
            assert 0x10 + f < 0x10 + used and 0x10 + v <= 0x10 + used, f"{name}: {t} pointer outside"
    od = {e[0]: e for e in oents}
    root = _obj_data(o, od[ROOT])
    kids = struct.unpack_from("<%dI" % struct.unpack_from("<I", root, 0x1C)[0], root, 0x24)
    assert GROUP in kids and LABEL in kids and VALUE in kids
    assert struct.unpack_from("<I", _obj_data(o, od[LABEL]), 4)[0] == KEY
    for h, (typ, off, z) in ((e[0], e[1:]) for e in oents):
        assert struct.unpack_from("<I", o, 0x10 + off + SELF_AT.get(typ, 0))[0] == h or typ not in SELF_AT, f"{name}: {h:08x} named otherwise"
    assert dict(text_info(chunks[b"TEXT"])).get(KEY) == b"DRIFT+"
