"""Build the network-lobby module (fe_network_game) from the stock PC stub with a given racer list.

The final assembly is Codex's search-preview build_module, minus its rebuild-and-compare of the
earlier stages' staged files. With Codex's 21 rows it must reproduce the Preview5 module exactly
(tools/build_menus.py checks that).
"""
import struct

from smos_reloc import insert, instructions, validate, u32
from netmenu import preview2, racer, settings, search, steam
from netmenu.pkg import align_metadata

STOCK_ROWS = list(preview2.ROWS)
STOCK_ORDER = list(racer.RACER_ORDER)
# Natives each stage was allowed to add (from the four builders' own allowlists).
ALLOWED_NATIVES = {0x020e, 0x0215, 0x0217, 0x0218, 0x021e, 0x0228, 0x0419, 0x041a, 0x0421, 0x0422,
                   0x1607, 0x160c, 0x1610, 0x1613, 0x1e00,      # Preview2 (minus 0427/0428, retired by the racer stage)
                   0x0213, 0x021f, 0x0423, 0x0424,              # racer popup
                   0x041d, 0x2003,                              # Settings
                   0x0205, 0x0206}                              # Search/Create
NEW_GLOBALS = (preview2.SELECT, preview2.LAPS, preview2.BUFFERS, preview2.PRIVATE_SLOTS, preview2.AI,
               preview2.ITEMS, preview2.CATCHUP, preview2.CHARACTER, preview2.DIRTY, preview2.DELAY)


def configure(rows, order):
    """rows: [(label, identity)], index = draft racer number. order: popup cycle order (indices)."""
    assert sorted(order) == list(range(len(rows))), order
    assert len({identity for _, identity in rows}) == len(rows)
    preview2.ROWS = racer.ROWS = settings.ROWS = list(rows)
    racer.RACER_ORDER = list(order)


def build(original, rows, order, online=False):
    """online=False: Codex's offline preview (the regression reference); True: stage 5, Steam."""
    configure(rows, order)
    suffix = preview2.PREVIEW_SUFFIX
    if online:
        preview2.PREVIEW_SUFFIX = ''  # the lobby row shows just the racer
        settings.COURSE_TOTAL = settings.ONLINE_COURSES  # Egg Hangar's course (settings.egg_insertions)
        racer.ONLINE = True  # the picker asks which racers this lobby's mods allow
    try:
        return _build(original, steam if online else search)
    finally:
        racer.ONLINE = False
        configure(STOCK_ROWS, STOCK_ORDER)
        preview2.PREVIEW_SUFFIX = suffix
        settings.COURSE_TOTAL = settings.STOCK_COURSES


def _build(original, stage):
    validate(original)
    assert u32(original, 0x28) == 0x36
    # Preview2's preconditions on the stock stub: the globals it claims are unread, the Show
    # helper ends `ZERO; RETURN`, and the active-frame branch it extends is where it expects.
    for slot in NEW_GLOBALS:
        assert not any(o['opcode'] == 0x57 and original[o['offset'] + 1] == slot for o in instructions(original))
    assert original[0xf94:0xf96] == b'\x17\x1b'
    assert original[0x166d:0x1672] == b'\x57\x03\x14\0\0'
    settings.retained_tables(original)

    chunks, detail = stage.make_chunks(original)
    raw = {at: c.finish() for at, c in chunks.items()}
    egg = settings.egg_insertions(original) if stage is steam else {}
    assert not set(egg) & set(raw)
    changed, relocation = insert(original, {**raw, **egg})
    out = bytearray(changed)
    loc = lambda p: p + sum(len(d) for at, d in {**raw, **egg}.items() if at <= p)
    if egg:  # the constructors' counts: 25 map pairs, 25 order items
        assert out[loc(settings.MAP_CLOSE):loc(settings.MAP_CLOSE) + 7] == bytes.fromhex('22090030000000')
        assert out[loc(settings.ORDER_CLOSE):loc(settings.ORDER_CLOSE) + 7] == bytes.fromhex('22080018000000')
        struct.pack_into('<I', out, loc(settings.MAP_CLOSE) + 3, 2 * settings.ONLINE_COURSES)
        struct.pack_into('<I', out, loc(settings.ORDER_CLOSE) + 3, settings.ONLINE_COURSES)
    starts = {at: at + sum(len(d) for pos, d in {**raw, **egg}.items() if pos < at) for at in raw}
    # A call target is an original offset, or (anchor, label): a label in the chunk inserted at anchor.
    dest = lambda t: starts[t[0]] + chunks[t[0]].labels[t[1]] if isinstance(t, tuple) else loc(t)
    for at, c in chunks.items():
        for p, target in c.calls:
            new = starts[at] + p
            struct.pack_into('<i', out, new + 2, dest(target) - (new + 10))
    struct.pack_into("<I", out, 0x28, stage.GLOBAL_COUNT)
    out = align_metadata(bytes(out))
    callback = starts[detail['callback_original_anchor']] + detail['callback_relative_entry']
    out = search.add_export(out, search.CHOICE, callback)
    validate(out)

    # Inactive screens must still skip the whole added frame body.
    assert next(o for o in instructions(out) if o['offset'] == loc(0x166f))['target'] == loc(0x1672)
    natives = {struct.unpack_from('<H', d, o['offset'] + 2)[0]
               for d in raw.values() for o in instructions(d, 0, len(d), '<') if o['opcode'] == 0x32}
    allowed = ALLOWED_NATIVES | (steam.NATIVES if stage is steam else set())
    assert natives <= allowed, sorted(hex(n) for n in natives - allowed)
    targets = {t for c in chunks.values() for _, t in c.calls}
    assert sorted(t for t in targets if not isinstance(t, tuple)) == [0x1118, 0x1a9d, 0x1fdd, 0x203e]
    assert {t[1] for t in targets if isinstance(t, tuple)} <= ({steam.SHOW_LABEL, steam.DRAW_LABEL, steam.FOOTER_LABEL, steam.REFUSAL_LABEL} if stage is steam else set())
    # The only settings native is the read-only course-availability query (command 73).
    for d in raw.values():
        ops = list(instructions(d, 0, len(d), '<'))
        for i, o in enumerate(ops):
            if o['opcode'] == 0x32 and struct.unpack_from('<H', d, o['offset'] + 2)[0] == 0x2003:
                assert d[ops[i - 6]['offset']:ops[i - 6]['end']] == b'\x51\x49'
    return out[:u32(out, 8)]
