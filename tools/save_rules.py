"""Print the race settings records stored in ssr_save.bin copies (read-only; notes/RULES.md).

usage: python tools/save_rules.py <ssr_save.bin> [...]
The file is a 16-byte header plus the profile manager; settings record i is at 0xebd0 + i*0xac and the
active index at 0xb1020.
"""
import struct
import sys

FIELDS = (('racers', 0x0, 'I'), ('laps', 0x4, 'I'), ('difficulty', 0x8, 'i'), ('items', 0xc, 'I'),
          ('hazards', 0x10, 'B'), ('catchup', 0x11, 'B'))


def describe(path: str) -> str:
    """One line per settings record, the active one marked."""
    data = open(path, 'rb').read()
    active = struct.unpack_from('<I', data, 0xb1020)[0]
    lines = [f'{path}  (active profile {active})']
    for i in range(4):
        base = 0xebd0 + i * 0xac
        values = ', '.join(f'{name} {struct.unpack_from("<" + fmt, data, base + off)[0]}' for name, off, fmt in FIELDS)
        lines.append(f'  {"*" if i == active else " "} record {i}: {values}')
    return '\n'.join(lines)


if __name__ == '__main__':
    for arg in sys.argv[1:]:
        print(describe(arg))
