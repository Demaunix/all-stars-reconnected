"""The game's own text tables with a few strings changed, shipped as archive overrides. Metal Sonic replaces the same table, and
the loader allowed one mod per archive member, so these ride with the footer key pictures (tools/footer_keys.py builds them for
the stock table and for Metal's): since ChaoGarage 1.2.0 both in the main mod's plan, each optional with a variant made on
Metal Sonic's version (tools/package.py).

Today: dialogs_EN.stz (file hash 9421F31B): "VIEW GAMER CARD" (BFB811CD) reads "VIEW STEAM PROFILE" (the lobby player popup's
button; the user, 2026-10-23). The .stz entry is the same 6-section container as the scenes; its TEXT chunk ({tag, size, size2,
0x44332211}, 8 more bytes, u32 count, (key, offset) pairs, NUL-terminated strings at offset + 16 from the chunk start; UTF-8, and
our strings are ASCII, so the cp1252 encoding below writes the same bytes) is
rebuilt with the new strings; everything else in the entry is copied as it is.

M8 (the user's MUTE / UNMUTE, re\\m8-yellow\\FINDINGS.md 3): the dialogs key NOTHING (72259212, no definition or script uses it on
PC) reads UNMUTE in each real language's table, for the lobby popups of a muted player (tools\\build_menus.py); German's MUTE
(51577208) read "EINSTELLEN" ("adjust"), now "TON AUS". The JP and US tables are placeholders: left alone. The strings are ASCII
(the tables are UTF-8).

    python tools/text_tables.py <project root>     (writes build/tiles/<KEY>.dat and prints the changed strings; a check)
"""
import struct
import sys
import zlib
from pathlib import Path

DIALOGS_EN = 0x9421F31B
NOTHING, MUTE = 0x72259212, 0x51577208
EDITS = {
    DIALOGS_EN: {0xBFB811CD: "VIEW STEAM PROFILE", NOTHING: "UNMUTE"},
    0x9AC5FB72: {NOTHING: "ACTIVER LE SON"},
    0xF08557E0: {NOTHING: "TON AN", MUTE: "TON AUS"},
    0x91EF54FD: {NOTHING: "ATTIVA AUDIO"},
    0x55CB9117: {NOTHING: "ACTIVAR SONIDO"},
}
NAMES = {DIALOGS_EN: "dialogs text table (English): VIEW GAMER CARD reads VIEW STEAM PROFILE, NOTHING reads UNMUTE",
         0x9AC5FB72: "dialogs text table (French): NOTHING reads ACTIVER LE SON (UNMUTE)",
         0xF08557E0: "dialogs text table (German): NOTHING reads TON AN (UNMUTE), MUTE reads TON AUS",
         0x91EF54FD: "dialogs text table (Italian): NOTHING reads ATTIVA AUDIO (UNMUTE)",
         0x55CB9117: "dialogs text table (Spanish): NOTHING reads ACTIVAR SONIDO (UNMUTE)"}


def read_text(dec: bytes) -> dict:
    """{key: bytes} of the TEXT chunk, in table order."""
    p = dec.find(b"TEXT")
    assert p >= 0, "no TEXT chunk"
    n = struct.unpack_from("<I", dec, p + 24)[0]
    out = []  # (a list: the table has repeated keys, e.g. junk entries)
    for k in range(n):
        key, off = struct.unpack_from("<II", dec, p + 28 + 8 * k)
        s = p + off + 16
        out.append((key, dec[s:dec.index(b"\0", s)]))
    return out


def patch_text(dec: bytes, edits: dict) -> bytes:
    """`dec` (the decompressed section holding the TEXT chunk) with the edited strings; the chunk's sizes updated."""
    # The real TEXT chunk (the entry's directory also lists chunks as "TEXT" + size + hash): {tag, capacity, used, 0x44332211,
    # 8 bytes, u32 count}; dialogs_EN: capacity 0x8000, used 0x7fc4, 641 strings, 60 bytes of room. The capacity stays, the strings
    # are laid out again inside it (3 more bytes for VIEW STEAM PROFILE), and `used` is updated.
    p = dec.find(b"TEXT")
    while p >= 0 and struct.unpack_from("<I", dec, p + 12)[0] != 0x44332211:
        p = dec.find(b"TEXT", p + 4)
    assert p >= 0, "no TEXT chunk"
    size, used = struct.unpack_from("<II", dec, p + 4)
    end = p + size  # (the capacity counts from the chunk start; `used` counts the payload after the 16-byte header)
    assert used + 16 <= size and end <= len(dec), "TEXT chunk past the data"
    n = struct.unpack_from("<I", dec, p + 24)[0]
    strings = read_text(dec[p:])
    assert len(strings) == n
    missing = [k for k in edits if k not in dict(strings)]
    assert not missing, "keys not in the table: " + ", ".join("%08X" % k for k in missing)
    table = bytearray()
    body = bytearray()
    base = -(-(28 + 8 * n) // 16) * 16  # the strings start 16-aligned after the pairs, as in the original (12 zero bytes there)
    for key, s in strings:
        data = edits[key].encode("cp1252") if key in edits else s
        table += struct.pack("<II", key, base + len(body) - 16)
        body += data + b"\0"
    chunk = bytearray(dec[p : p + 28]) + table + bytes(base - 28 - 8 * n) + body
    assert len(chunk) <= size, "the edited strings don't fit the chunk's capacity (%d over)" % (len(chunk) - size)
    struct.pack_into("<I", chunk, 8, len(chunk) - 16)  # used
    chunk += b"\0" * (end - p - len(chunk))
    return dec[:p] + bytes(chunk) + dec[end:]


def patch_entry(entry: bytes, edits: dict) -> bytes:
    """The archive member (its container) with the TEXT chunk's strings edited."""
    from scene_container import packer_for, sections6  # (the stock layout or Metal Sonic's; checked to reproduce the entry)
    pack = packer_for(entry)
    a, d, s = sections6(entry)[3]
    dec = zlib.decompress(entry[a : a + s])
    assert len(dec) == d and b"TEXT" in dec, "the text chunk isn't in section 3"
    new = patch_text(dec, edits)
    assert len(new) == len(dec)
    at = new.find(b"TEXT")
    while struct.unpack_from("<I", new, at + 12)[0] != 0x44332211:
        at = new.find(b"TEXT", at + 4)
    got, was = read_text(new[at:]), read_text(dec[at:])
    for key, text in edits.items():
        assert dict(got)[key] == text.encode("cp1252")
    assert [(k, v) for k, v in got if k not in edits] == [(k, v) for k, v in was if k not in edits], "another string changed"
    return pack(zlib.compress(new, 9), len(new))


def build_payloads(path: Path, rows: dict) -> dict:
    """{key: (original stored bytes, patched stored bytes)} for every edited table."""
    from steam_tiles import member
    out = {}
    for key, edits in EDITS.items():
        orig = member(path, rows, key)
        out[key] = (orig, patch_entry(orig, edits))
    return out


def main() -> None:
    root = Path(sys.argv[1]).resolve()
    sys.path.insert(0, str(root / "tools"))
    from scene_container import sections6
    from steam_tiles import ARCHIVE, GAME, read_rows
    path = GAME / ARCHIVE
    _, rows = read_rows(path)
    for key, (orig, patched) in build_payloads(path, rows).items():
        a, d, s = sections6(patched)[3]
        dec = zlib.decompress(patched[a : a + s])
        at = dec.find(b"TEXT")
        while struct.unpack_from("<I", dec, at + 12)[0] != 0x44332211:
            at = dec.find(b"TEXT", at + 4)
        texts = dict(read_text(dec[at:]))
        for k in EDITS[key]:
            print("%08X %08X %r" % (key, k, texts[k].decode("cp1252")))
        out = root / "build" / "tiles"
        out.mkdir(parents=True, exist_ok=True)
        (out / ("%08X.dat" % key)).write_bytes(patched)
        print("%08X: %d -> %d stored bytes" % (key, len(orig), len(patched)))


if __name__ == "__main__":
    main()
