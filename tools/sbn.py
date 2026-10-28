"""List or extract modules of a smospkg2 script package (e.g. Scripts/FrontEndScripts.sbn).

usage: python -I tools/sbn.py list <pkg.sbn>
       python -I tools/sbn.py extract <pkg.sbn> <module name> <out file>
Format (notes/SCRIPT-LOADER.md): "smospkg2", u32 32 at +8, count at +12, 32-byte entries from +40;
entry +16 name hash, +20 name offset, +24 back-pointer, +28 module data offset (all self-relative
to their own field). A module ("smosbinL") has its total size at +8.
"""
import hashlib, struct, sys
from pathlib import Path


def modules(data):
    if data[:8] != b"smospkg2" or struct.unpack_from("<I", data, 8)[0] != 32:
        raise SystemExit("not a smospkg2 package")
    count = struct.unpack_from("<I", data, 12)[0]
    out = []
    for i in range(count):
        e = 40 + 32 * i
        h, name_rel = struct.unpack_from("<II", data, e + 16)
        data_rel = struct.unpack_from("<I", data, e + 28)[0]
        name_at = (e + 20 + name_rel) & 0xFFFFFFFF
        name = data[name_at:data.index(b"\0", name_at)].decode("latin1")
        mod_at = (e + 28 + data_rel) & 0xFFFFFFFF
        size = struct.unpack_from("<I", data, mod_at + 8)[0]
        out.append((name, h, mod_at, size))
    return out


def main():
    cmd, pkg = sys.argv[1], Path(sys.argv[2])
    data = pkg.read_bytes()
    mods = modules(data)
    if cmd == "list":
        for name, h, at, size in mods:
            sha = hashlib.sha256(data[at:at + size]).hexdigest()[:12]
            print(f"{name:32} {h:08x} at {at:#08x} size {size:6} sha {sha}")
    elif cmd == "extract":
        for name, h, at, size in mods:
            if name == sys.argv[3]:
                Path(sys.argv[4]).write_bytes(data[at:at + size])
                print(f"wrote {size} bytes")
                return
        raise SystemExit("no such module")


if __name__ == "__main__":
    main()
