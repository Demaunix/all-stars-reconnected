"""Disassemble or dump bytes of the decrypted game exe at a virtual address.

usage: python tools/disasm.py <va hex> [count=24] [--bytes N] [--exe path]
Default exe: re/bin/SSASR-unpacked.exe (a decrypted copy of the Steam exe, kept outside this repository).
"""
import struct, sys
from pathlib import Path

import capstone


def sections(data):
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    optsz = struct.unpack_from("<H", data, pe + 20)[0]
    base = struct.unpack_from("<I", data, pe + 24 + 28)[0]
    out = []
    for i in range(nsec):
        o = pe + 24 + optsz + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode("latin1")
        vsz, va, rawsz, raw = struct.unpack_from("<IIII", data, o + 8)
        out.append((name, base + va, max(vsz, rawsz), raw, rawsz))
    return base, out


def read(data, secs, va, n):
    for name, sva, size, raw, rawsz in secs:
        if sva <= va < sva + size:
            off = va - sva
            chunk = data[raw + off: raw + min(off + n, rawsz)]
            return chunk + b"\0" * (n - len(chunk))
    raise SystemExit(f"0x{va:x} is not in any section")


def main():
    args = sys.argv[1:]
    exe = Path(__file__).resolve().parent.parent / "re/bin/SSASR-unpacked.exe"
    nbytes = None
    if "--exe" in args:
        i = args.index("--exe"); exe = Path(args[i + 1]); del args[i:i + 2]
    if "--bytes" in args:
        i = args.index("--bytes"); nbytes = int(args[i + 1], 0); del args[i:i + 2]
    va = int(args[0], 16)
    count = int(args[1]) if len(args) > 1 else 24
    data = exe.read_bytes()
    _, secs = sections(data)
    if nbytes:
        print(read(data, secs, va, nbytes).hex(" "))
        return
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    code = read(data, secs, va, count * 16)
    for k, ins in enumerate(md.disasm(code, va)):
        if k >= count:
            break
        print(f"{ins.address:08x}  {ins.bytes.hex(' '):<24} {ins.mnemonic} {ins.op_str}")


if __name__ == "__main__":
    main()
