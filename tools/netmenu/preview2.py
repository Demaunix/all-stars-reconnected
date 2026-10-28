"""Preview 2: correct native row/loading states; no service or race calls.

Only stages files next to this generator. The retained PC lobby creates and
destroys its own UI resources. This patch fills its stripped Show callback and
active frame branch with private draft settings, one explicitly local preview
row, and Back. It does not port any console/Mac network implementation.
"""
# Vendored from Codex's work for this user (stage 1 of 4):
# R0927\native-menu-20261001\network-preview2\build_network_preview.py, sha256 17c41ac6...
# Changes: imports point at our tools; patch_network/main (which read Codex's staging folders) removed.
# ROWS is the draft racer list; tools/netmenu/__init__.py configure() replaces it.
import struct

from smos_reloc import insert, instructions, validate, u32
from netmenu.pkg import align_metadata, name_hash

# Codex's 21 stock rows. Index = draft racer number (global 2F).
ROWS = [
    ('SONIC', 'soniccar'), ('TAILS', 'tails'), ('KNUCKLES', 'knuckles'),
    ('AMY', 'amy'), ('SHADOW', 'shadow'), ('DR. EGGMAN', 'eggman'),
    ('AI AI', 'aiai'), ('AMIGO', 'samba'), ('BILLY HATCHER', 'billyhatcher'),
    ('BEAT', 'beat'), ('ULALA', 'ulala'), ('B.D. JOE', 'bdjoe'),
    ('ALEX KIDD', 'alexkidd'), ('OPA-OPA', 'opaopa'), ('BIG', 'bigthecat'),
    ('BONANZA BROS.', 'bonanzabrothers'), ('CHUCHUS', 'chuchurocket'),
    ('ZOBIO & ZOBIKO', 'houseofthedead'), ('JACKY & AKIRA', 'jackybryant'),
    ('RYO', 'ryobike'), ('RYO FORKLIFT', 'ryoforklift'),
]
RULE_KEYS = [name_hash('NET_LOBBY\\' + n + '_TXT') for n in ('LAPS', 'RACERS', 'AI', 'ITEMS', 'CATCHUP')]
HEADING_KEYS = ()  # Preview 1's keys belong to the shared creating overlay.
ROW_GROUPS = (0xcec457a2, 0xc1afeaef, 0x77e51b23, 0xcfd654a0,
              0x8601b69f, 0x1055715d, 0xe47d740b, 0x56e40343)
PREVIEW_SUFFIX = ' - OFFLINE PREVIEW'

# These globals have no reads in the unmodified PC payload: the backend using
# them was stripped. Global 2A is an owned array of persistent text buffers.
SELECT, LAPS, BUFFERS = 0x26, 0x27, 0x2a
PRIVATE_SLOTS, AI, ITEMS, CATCHUP, CHARACTER = 0x2b, 0x2c, 0x2d, 0x2e, 0x2f
DIRTY, DELAY = 0x30, 0x31

class Code:
    def __init__(self):
        self.data = bytearray(); self.labels = {}; self.local_fixups = []; self.calls = []
    def raw(self, data): self.data.extend(data); return self
    def num(self, n): return self.raw(b'\x51' + struct.pack('<b', n))
    def ident(self, n): return self.raw(b'\x02' + struct.pack('<I', n))
    def string(self, s): return self.raw(b'\x04' + s.encode('utf-8') + b'\0')  # (UTF-8, as the text path; ASCII is unchanged)
    def get(self, n): return self.raw(b'\x57' + bytes([n]))
    def put(self, n): return self.raw(b'\x27' + struct.pack('<H', n))
    def array(self, n): return self.raw(b'\x22\x08\0' + struct.pack('<I', n))
    def native(self, n, argc, drop=True):
        self.raw(b'\x32' + struct.pack('<BHHH', argc, n, 1, 0x7ffe))
        if drop: self.raw(b'\x18')
        return self
    def call(self, old_target):
        self.raw(b'\x17'); at = len(self.data)
        self.raw(b'\x34\0' + bytes(4) + struct.pack('<HH', 1, 0x7ffe) + b'\x18')
        self.calls.append((at, old_target)); return self
    def call_chunk(self, anchor, name):
        """Call label `name` of the chunk inserted at original offset `anchor` (build.py resolves it)."""
        return self.call((anchor, name))
    def label(self, name): self.labels[name] = len(self.data); return self
    def branch(self, name, op=0x14):
        at = len(self.data); self.raw(bytes([op, 0, 0])); self.local_fixups.append((at, name)); return self
    def finish(self):
        for at, name in self.local_fixups:
            struct.pack_into('<h', self.data, at+1, self.labels[name]-(at+3))
        return bytes(self.data)
    def buffer(self, index): return self.get(BUFFERS).num(index).raw(b'\x23')
    def fill(self, index, value): return self.buffer(index).string(value).native(0x1613, 2)
    def bind(self, key, index): return self.ident(key).buffer(index).native(0x021e, 2)
    def assign(self, slot, value): return self.num(value).put(slot)

def draw(c):
    # Bind only the lobby's own rule and row text. The shared creating overlay
    # and persistent navigator title retain their original buffer ownership.
    for i, key in enumerate(RULE_KEYS): c.bind(key, i)
    c.bind(0x7d63b234, 6)
    for field, slot, values in [
        (1, LAPS, [str(n) for n in range(1, 10)]),
        (2, PRIVATE_SLOTS, [str(n) for n in range(8)]),
        # The preview offers a bounded subset of the original four AI choices.
        (3, AI, ['OFF', 'BEGINNER']), (4, ITEMS, ['OFF', 'ON']), (5, CATCHUP, ['OFF', 'ON']),
    ]:
        base = 1 if field == 1 else 0
        for number, value in enumerate(values, base):
            tag = 'rule_' + str(field) + '_' + str(number)
            c.get(slot).num(number).raw(b'\x4a').branch(tag+'_next')
            c.get(SELECT).num(field).raw(b'\x4a').branch(tag+'_plain')
            c.fill(field-1, '> '+value).branch(tag+'_next', 0x15).label(tag+'_plain')
            c.fill(field-1, value).label(tag+'_next')
    for index, (label, keyname) in enumerate(ROWS):
        tag = 'character_'+str(index)
        c.get(CHARACTER).num(index).raw(b'\x4a').branch(tag+'_next')
        c.get(SELECT).num(0).raw(b'\x4a').branch(tag+'_plain')
        c.fill(6, '> '+label+PREVIEW_SUFFIX).branch(tag+'_portrait', 0x15)
        c.label(tag+'_plain').fill(6, label+PREVIEW_SUFFIX).label(tag+'_portrait')
        # The stock map supplies the actual native portrait; no guessed art.
        c.ident(0xa2eab366).get(0x13).ident(name_hash(keyname)).raw(b'\x23').num(0).raw(b'\x23\x3c').native(0x0228, 2)
        c.label(tag+'_next')
    c.assign(DIRTY, 0)

def make_chunks(original):
    init = Code()
    for _ in range(7): init.raw(b'\x55\x60').native(0x1610, 1, drop=False)
    init.array(7).put(BUFFERS)
    init.fill(5, 'MENU PREVIEW - OFFLINE')
    for slot, value in [(SELECT, 0), (LAPS, 3), (PRIVATE_SLOTS, 7), (AI, 1), (ITEMS, 1),
                        (CATCHUP, 1), (CHARACTER, 0), (DIRTY, 1), (DELAY, 0)]: init.assign(slot, value)

    show = Code().assign(0x21, 1).assign(0x22, 0).ident(0xee8a2362).put(2)
    show.call(0x1a9d)
    # Mac original local UI transition 29B0..29CB reverses the loading state
    # initialized by the retained PC Show helper. These are only GUI arguments;
    # none of the surrounding backend-success code is copied or invoked.
    show.ident(0xfff27543).ident(0x3ecdb4b9).ident(0xa56426e2).num(0).num(0).native(0x020e, 5)
    # The row is a direct scene child, but showing it does not reset its GROP
    # keyframe. Original 29F4..2A1C resets each row to this verified native state
    # before hiding/populating. Preview 1 omitted that animation reset.
    for key in ROW_GROUPS: show.ident(key).ident(0x74e7e7bc).native(0x0215, 2)
    # Clear every participant slot, then display exactly one labelled local
    # draft. Hide its readiness/voice/rank adornments; no ready-state assertion.
    for key in ROW_GROUPS: show.ident(key).native(0x0218, 1)
    show.ident(ROW_GROUPS[0]).native(0x0217, 1)
    for key in (0x7be7f85f, 0x4e7d2ee9, 0x072d68f4): show.ident(key).native(0x0218, 1)
    # Exact subset of the retained Settings footer: vertical Select,
    # horizontal Change, Back. There is no misleading Accept Changes action.
    show.ident(0xcc7b6d70).ident(0x55379672).num(6).ident(0xf1141a0a).num(7).ident(0xe52c719e).num(1).ident(0x3a648438).array(6).array(1).native(0x1607, 3)
    show.assign(DIRTY, 1).assign(DELAY, 2)

    frame = Code()
    frame.get(DELAY).num(0).raw(b'\x48').branch('inputs')
    frame.get(DELAY).num(1).raw(b'\x43').put(DELAY).branch('render', 0x15)
    frame.label('inputs').native(0x041a, 0, drop=False).branch('navigation')
    frame.string('FE_BACK').string('NULL').num(1).native(0x1e00, 3)
    frame.ident(0xcc7b6d70).ident(0xce3d133a).raw(b'\x31').array(1).native(0x1607, 3)
    frame.ident(0xcc7b6d70).ident(0x4fce5e20).ident(0xee8a2362).native(0x160c, 3)
    frame.assign(3, 0).assign(0x20, 1).branch('done', 0x15)
    frame.label('navigation')
    for native, delta, tag in [(0x0421, -1, 'up'), (0x0422, 1, 'down')]:
        frame.native(native, 0, drop=False).branch(tag+'_done')
        frame.get(SELECT).num(abs(delta)).raw(b'\x43' if delta<0 else b'\x42').put(SELECT)
        frame.get(SELECT).num(0).raw(b'\x46').branch(tag+'_high')
        frame.assign(SELECT, 5)
        frame.label(tag+'_high').get(SELECT).num(5).raw(b'\x48').branch(tag+'_changed')
        frame.assign(SELECT, 0)
        frame.label(tag+'_changed').assign(DIRTY, 1).label(tag+'_done')
    for natives, delta, tag in [((0x0427,), -1, 'previous'), ((0x0428, 0x0419), 1, 'next')]:
        for i, native in enumerate(natives):
            frame.native(native, 0, drop=False)
            if i: frame.raw(b'\x50')
        frame.branch(tag+'_done')
        for field, slot, low, high in [(0, CHARACTER, 0, len(ROWS)-1), (1, LAPS, 1, 9),
                                      (2, PRIVATE_SLOTS, 0, 7), (3, AI, 0, 1), (4, ITEMS, 0, 1), (5, CATCHUP, 0, 1)]:
            sub=tag+'_'+str(field)
            frame.get(SELECT).num(field).raw(b'\x4a').branch(sub+'_done')
            frame.get(slot).num(abs(delta)).raw(b'\x43' if delta<0 else b'\x42').put(slot)
            frame.get(slot).num(low).raw(b'\x46').branch(sub+'_high')
            frame.assign(slot, high)
            frame.label(sub+'_high').get(slot).num(high).raw(b'\x48').branch(sub+'_done')
            frame.assign(slot, low).label(sub+'_done')
        frame.assign(DIRTY, 1).label(tag+'_done')
    frame.label('render').get(DIRTY).branch('done')
    draw(frame)
    frame.label('done')
    return {0xf7f:init, 0xf95:show, 0x1672:frame}

