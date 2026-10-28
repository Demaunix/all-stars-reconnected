"""The scenes' and text tables' container (SumoToolResources .stz members): six (offset, decoded, stored) sections, the data in
section 3 as a zlib stream. The stock entries put the data at 0x48 with sections at offsets 8 (mod 64) (steam_tiles.pack);
Metal Sonic's payloads put it at 0x60 with 32-byte alignment. packer_for() finds the rule that reproduces an entry byte for
byte and returns a function (stream, decoded size) -> container, so a patched entry keeps its own layout."""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import steam_tiles as st  # noqa: E402


def sections6(entry: bytes) -> list:
    """The container's six (offset, decoded, stored) sections, as they are."""
    t = struct.unpack_from("<18I", entry, 0)
    return [t[k:k + 3] for k in range(0, 18, 3)]


def packer_for(entry: bytes):
    """A function (stream, decoded[, stream4, decoded4]) -> container bytes in this entry's own layout (checked to reproduce the
    entry); section 4 is the entry's own unless a new one is given (the lobby bars' RELO change, tools/lobby_bars.py)."""
    secs = sections6(entry)
    o3, d3, s3 = secs[3]
    o4, d4, s4 = secs[4]

    def aligned(a: int):
        def f(stream: bytes, decoded: int, stream4: bytes = None, decoded4: int = None) -> bytes:
            sec4 = entry[o4:o4 + s4] if stream4 is None else stream4
            n4 = (o3 + len(stream) + a - 1) // a * a
            end = (n4 + len(sec4) + a - 1) // a * a
            out = bytearray(end)
            struct.pack_into("<18I", out, 0, *secs[0], *secs[1], *secs[2], o3, decoded, len(stream), n4,
                             d4 if stream4 is None else decoded4, len(sec4), *secs[5])
            out[o3:o3 + len(stream)] = stream
            out[n4:n4 + len(sec4)] = sec4
            return bytes(out)
        return f

    def stock(stream: bytes, decoded: int, stream4: bytes = None, decoded4: int = None) -> bytes:
        if stream4 is None:
            return st.pack(entry, stream, decoded)
        # st.pack's layout with another section 4: each section at 8 (mod 64), 24 zero bytes after the last.
        st.sections(entry)
        n4 = st.next_section(0x48 + len(stream))
        n5 = st.next_section(n4 + len(stream4))
        out = bytearray(n5 + 0x18)
        struct.pack_into("<18I", out, 0, 0x48, 0, 0, 0x48, 0, 0, 0x48, 0, 0, 0x48, decoded, len(stream), n4, decoded4, len(stream4), n5, 0, 0)
        out[0x48:0x48 + len(stream)] = stream
        out[n4:n4 + len(stream4)] = stream4
        return bytes(out)

    candidates = ([stock] if secs[0] == (0x48, 0, 0) else []) + [aligned(32), aligned(16), aligned(64), aligned(8)]
    for cand in candidates:
        try:
            if cand(entry[o3:o3 + s3], d3) == entry:
                return cand
        except AssertionError:
            continue
    raise AssertionError("no layout rule reproduces this scene container")
