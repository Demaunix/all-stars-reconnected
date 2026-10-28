"""Unselected successor to Preview2: native local-draft Change Racer popup.

Stages only beside this file. Requires a byte-for-byte rebuild of the pinned
Preview2 network module; never starts from Preview1 or changes its files.
"""
# Vendored from Codex's work for this user (stage 2 of 4):
# R0927\native-menu-20261001\next-menu-flows\racer-preview\build_racer_preview.py, sha256 f69006d8...
# Changes: imports point at our tools; build_module/main (which read Codex's staging folders) removed.
# ROWS and RACER_ORDER are replaced by tools/netmenu/__init__.py configure().
import struct

from netmenu import preview2 as base

Code = base.Code
ROWS = base.ROWS
# Original charselscreen global31 order, translated to Preview2's draft indices.
RACER_ORDER = [0,6,8,7,10,9,11,17,19,20,18,16,15,12,13,14,5,4,2,3,1]

# New slots, beyond the original allocation, avoid all original and Preview2
# globals. The owning module keeps the availability mask for the popup lifetime.
MODE, CANDIDATE, AVAILABLE, COUNT, SCAN, POP_DIRTY = range(0x36, 0x3c)
GLOBAL_COUNT = 0x3c
POPUP, PORTRAIT, NAME, CONFIRM_STATUS = 0xa653f569, 0x20a43a61, 0x6a53b173, 0x2f912004
NAME_SHADOW = 0xfc4d712a
NAT_RACER_ON = 0x7a28  # (lobby racer index) -> 1 pickable / 0 its mod is off in this lobby (src/natives.h kNatRacerOn)
ONLINE = False  # build.build(online=True) sets it for stage 5
CHARSCREEN_SHA = '823579fda776f4bef75a7a169a31feee727e36ca580276689072ec0d5d1a4ce6'


def sound(c, key):
    c.string(key).string('NULL').num(1).native(0x1e00, 3)


def lobby_hints(c):
    # Context-sensitive legend, then stock LOBBY title. Both go through
    # navigator-owned buffers; no foreign text object is bound.
    tag='hints_'+str(len(c.data))
    c.get(base.SELECT).num(0).raw(b'\x4a').branch(tag+'_rules')
    c.ident(0xcc7b6d70).ident(0x55379672).num(6).ident(0xf1141a0a)
    c.num(0).ident(0xfb59b2c4).num(1).ident(0x3a648438).array(6).array(1).native(0x1607,3)
    c.branch(tag+'_title',0x15).label(tag+'_rules')
    c.ident(0xcc7b6d70).ident(0x55379672).num(6).ident(0xf1141a0a)
    c.num(7).ident(0xe52c719e).num(1).ident(0x3a648438).array(6).array(1).native(0x1607, 3)
    c.label(tag+'_title')
    c.ident(0xcc7b6d70).ident(0xce3d133a).ident(0x8d9e062e).array(1).native(0x1607, 3)


def close_popup(c, commit):
    if commit:
        c.get(CANDIDATE).put(base.CHARACTER)
    sound(c, 'FE_SELECT' if commit else 'FE_BACK')
    c.ident(POPUP).ident(0xc1dd7351).ident(0x36a7542b).num(0).native(0x020e, 4)
    # Mode2 owns closing until the original animation-ready predicate passes.
    c.assign(MODE, 2).assign(base.DIRTY, 1)
    c.branch('all_done', 0x15)


def cycle(c, step, tag):
    # COUNT>0 is an opening invariant, so at most 21 candidates are visited.
    c.label(tag + '_loop')
    for position,index in enumerate(RACER_ORDER):
        c.get(CANDIDATE).num(index).raw(b'\x4a').branch(tag+'_after_'+str(index))
        c.assign(CANDIDATE,RACER_ORDER[(position+step)%len(RACER_ORDER)]).branch(tag+'_test',0x15)
        c.label(tag+'_after_'+str(index))
    c.label(tag + '_test').get(AVAILABLE).get(CANDIDATE).raw(b'\x23').branch(tag + '_loop')
    c.assign(POP_DIRTY, 1)


def draw_popup(c):
    c.get(POP_DIRTY).branch('all_done')
    for index, (_, identity) in enumerate(ROWS):
        end = 'popup_draw_' + str(index)
        c.get(CANDIDATE).num(index).raw(b'\x4a').branch(end)
        c.ident(PORTRAIT).get(0x13).ident(base.name_hash(identity)).raw(b'\x23').num(1).raw(b'\x23\x3c').native(0x0228, 2)
        c.ident(NAME).get(0x13).ident(base.name_hash(identity)).raw(b'\x23').num(2).raw(b'\x23\x3c').native(0x021f, 2)
        c.ident(NAME_SHADOW).get(0x13).ident(base.name_hash(identity)).raw(b'\x23').num(2).raw(b'\x23\x3c').native(0x021f, 2)
        c.label(end)
    c.ident(CONFIRM_STATUS).ident(0xfbc94a42).native(0x0215, 2)
    c.assign(POP_DIRTY, 0).branch('all_done', 0x15)


def make_chunks(original):
    chunks = base.make_chunks(original)
    chunks[0xf7f].assign(MODE, 0).assign(CANDIDATE, 0).assign(COUNT, 0).assign(SCAN, 0).assign(POP_DIRTY, 0)
    for _ in ROWS:
        chunks[0xf7f].num(0)
    chunks[0xf7f].array(len(ROWS)).put(AVAILABLE)
    # Show/re-entry cannot inherit a stale popup state.
    chunks[0xf95].ident(POPUP).ident(0x36a7542b).native(0x0215,2)
    chunks[0xf95].assign(MODE, 0).assign(POP_DIRTY, 0)
    lobby_hints(chunks[0xf95])

    old_frame = chunks[0x1672]
    # Use the original modal's directional press events for inline rules too;
    # Preview2's held-level0427/0428 advanced once per rendered frame.
    input_edits=[]
    for o in base.instructions(old_frame.finish(),0,len(old_frame.data),'<'):
        if o['opcode']==0x32:
            nid=struct.unpack_from('<H',old_frame.data,o['offset']+2)[0]
            if nid in (0x0427,0x0428):
                struct.pack_into('<H',old_frame.data,o['offset']+2,nid-4)
                input_edits.append((o['offset'],nid,nid-4))
    assert len(input_edits)==2
    c = Code().get(MODE).num(2).raw(b'\x4a').branch('not_closing')
    c.ident(POPUP).native(0x0213,1,drop=False).branch('all_done')
    c.assign(MODE,0).assign(base.DELAY,2).assign(base.DIRTY,1)
    lobby_hints(c)
    # Restore the committed row only after closing, without polling lobby keys.
    base.draw(c)
    c.branch('all_done',0x15)
    c.label('not_closing').get(MODE).branch('lobby')
    c.ident(POPUP).native(0x0213,1,drop=False).branch('all_done')
    c.get(base.DELAY).num(0).raw(b'\x48').branch('modal_inputs')
    c.get(base.DELAY).num(1).raw(b'\x43').put(base.DELAY).branch('modal_render', 0x15)
    c.label('modal_inputs').native(0x041a, 0, drop=False).branch('modal_accept')
    close_popup(c, False)
    c.label('modal_accept').native(0x0419, 0, drop=False).branch('modal_left')
    close_popup(c, True)
    c.label('modal_left').native(0x0423, 0, drop=False).branch('modal_right')
    cycle(c, -1, 'previous_available')
    sound(c, 'FE_MOVE')
    c.branch('modal_render', 0x15)
    c.label('modal_right').native(0x0424, 0, drop=False).branch('modal_render')
    cycle(c, 1, 'next_available')
    sound(c, 'FE_MOVE')
    c.label('modal_render')
    draw_popup(c)

    # The Preview2 frame stays identical except the two input IDs above. Intercept only
    # Confirm on its racer field after debounce and without a simultaneous Back.
    c.label('lobby').get(base.DELAY).num(0).raw(b'\x48').branch('lobby_ready')
    c.branch('run_base', 0x15)
    c.label('lobby_ready').get(base.SELECT).num(0).raw(b'\x4a').branch('run_base')
    c.native(0x041a, 0, drop=False).branch('lobby_accept')
    c.branch('run_base', 0x15)
    c.label('lobby_accept').native(0x0419, 0, drop=False).branch('run_base')

    # The retained Initialize obtained this list through charselscreen's
    # read-only DC8A3C6D export. Missing, empty, or unknown-only lists fail closed.
    c.get(0x14).raw(b'\x31\x4a').branch('list_not_null')
    c.branch('unavailable', 0x15)
    # Array equality supports None in the real type-8 class, but not integer
    # zero. The pinned getter contract is array/None, so do not compare to 0.
    c.label('list_not_null').get(0x14).raw(b'\x1f').num(0).raw(b'\x48').branch('unavailable')
    for _ in ROWS:
        c.num(0)
    c.array(len(ROWS)).put(AVAILABLE).assign(COUNT, 0).assign(SCAN, 0)
    c.label('scan_list').get(SCAN).get(0x14).raw(b'\x1f\x46').branch('scan_done')
    for index, (_, identity) in enumerate(ROWS):
        done = 'identity_' + str(index)
        c.get(0x14).get(SCAN).raw(b'\x23').ident(base.name_hash(identity)).raw(b'\x4a').branch(done)
        # 1.1 ENABLE MODIFICATIONS: a racer whose mod is off in this lobby is skipped (the console's picker skipped DLC
        # racers with the DLC off); native 0x7a28 = src/natives.h kNatRacerOn, same index as the C++ racer list.
        if ONLINE:  # (only stage 5: the offline chain stays Preview5's bytes)
            c.num(index).native(NAT_RACER_ON, 1, drop=False).branch(done)
        # Repeated entries are harmless: only newly seen mapped identities count.
        c.get(AVAILABLE).num(index).raw(b'\x23').branch(done + '_mark')
        c.branch(done, 0x15)
        c.label(done + '_mark').num(1).num(index).raw(b'\x28\x01' + struct.pack('<H', AVAILABLE))
        c.get(COUNT).num(1).raw(b'\x42').put(COUNT).label(done)
    c.get(SCAN).num(1).raw(b'\x42').put(SCAN).branch('scan_list', 0x15)
    c.label('scan_done').get(COUNT).num(0).raw(b'\x48').branch('unavailable')
    c.get(base.CHARACTER).put(CANDIDATE)
    c.get(AVAILABLE).get(CANDIDATE).raw(b'\x23').branch('find_first')
    c.branch('open_popup', 0x15)
    c.label('find_first').assign(CANDIDATE, RACER_ORDER[-1])
    cycle(c, 1, 'first_available')
    c.label('open_popup').assign(MODE, 1).assign(POP_DIRTY, 1).assign(base.DELAY, 2)
    sound(c, 'FE_SELECT')
    c.ident(POPUP).ident(0xb2244a6d).ident(0x02f0b24c).num(0).native(0x020e, 4)
    c.call(0x203e)  # retained native popup legend and title, UI-only
    c.branch('modal_render', 0x15)
    c.label('unavailable')
    sound(c, 'FE_NEGATIVE')
    c.assign(base.DELAY, 2).branch('all_done', 0x15)
    c.label('run_base')
    c.get(base.SELECT).put(SCAN)
    baseline_at = len(c.data)
    c.raw(old_frame.finish())
    c.calls.extend((baseline_at + at, target) for at, target in old_frame.calls)
    c.get(3).branch('all_done')
    c.get(base.SELECT).get(SCAN).raw(b'\x4b').branch('all_done')
    lobby_hints(c)
    c.label('all_done')
    chunks[0x1672] = c
    return chunks, {'base_frame_offset_in_wrapper': baseline_at, 'base_frame_length': len(old_frame.data),
                    'inline_input_edits':input_edits,'original_racer_order':RACER_ORDER,
                    'animation_gate':'0213 gates modal inputs and closing completion'}


