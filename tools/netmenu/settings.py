"""Unselected native Settings successor to the exact final racer preview.

Frozen files are imported/read only. Stage output stays in this directory.
Original lobby/racer lifecycle is retained; rules commit only to local globals.
"""
# Vendored from Codex's work for this user (stage 3 of 4):
# R0927\native-menu-20261001\next-menu-flows\settings-preview\build_settings_preview.py, sha256 7b6c6c19...
# Changes: imports point at our tools; build_module/main (which read Codex's staging folders) removed.
# ROWS is replaced by tools/netmenu/__init__.py configure().
import struct

from netmenu import racer

base, Code, ROWS = racer.base, racer.Code, racer.ROWS
MODE, CANDIDATE, AVAILABLE, COUNT, SCAN, POP_DIRTY = racer.MODE, racer.CANDIDATE, racer.AVAILABLE, racer.COUNT, racer.SCAN, racer.POP_DIRTY
POPUP, PORTRAIT, NAME, NAME_SHADOW = racer.POPUP, racer.PORTRAIT, racer.NAME, racer.NAME_SHADOW
SETTINGS_MODE, DRAFT, SETTINGS_ROW, PREVIOUS_ROW, SETTINGS_DIRTY, COURSE, COURSE_AVAILABLE, COURSE_COUNT, COURSE_SCAN, COURSE_OLD = range(0x3C,0x46)
GLOBAL_COUNT = 0x46
SETTINGS = 0xEB17FB50
BARS = tuple(base.name_hash('NET_LOBBY_POPUPS\\BAR_0'+str(i)) for i in range(1,7))
assert BARS == (0x98e7e204,0x5692718f,0x0642e109,0xd5f74229,0x46e0f70a,0x783ac3fe)
TEXT_PAIRS = ((0x6634bccc,0x45326dd1),(0x64cacf8b,0xcf376f5f),
              (0xcf982c0f,0x8358de5d),(0x6a123176,0xe5e5b6cb),
              (0x852e17ce,0x0487dad7),(0xe5a32b27,0x6cdbed68))
RULE_SLOTS = (base.LAPS,base.PRIVATE_SLOTS,base.AI,base.ITEMS,base.CATCHUP)
BOUNDS = ((3,9),(0,7),(0,3),(0,1),(0,1))
COURSE_ART, COURSE_NAME = 0x7ac9264c,0x9cfb0450
STOCK_COURSES = 24
COURSE_TOTAL = STOCK_COURSES  # the online build sets ONLINE_COURSES while it runs (netmenu/build.py)
# Egg Hangar, the console's 25th lobby course (title update 2 fe_network_game: the course map's entry [course, art, name] and
# the order array's last item; the art and name come with our lobby scenes, tools/egg_lobby.py). Offered only where the
# Egg Hangar mod is installed: the C++ answers its availability query (2003 command 73) "no" otherwise.
ONLINE_COURSES = STOCK_COURSES + 1
EGG_COURSE, EGG_ART, EGG_NAME = 0x2c0dae1f,0xb2a1f58a,0xc6b8b684
MAP_CLOSE, ORDER_CLOSE = 0x89f,0x921  # in the stock stub: the map's `22 09 00 <48>` and the order's `22 08 00 <24>`


def egg_insertions(original):
    """{offset in the stock stub: bytes inserted before it}: Egg's map entry and order item."""
    assert original[MAP_CLOSE:MAP_CLOSE+10]==bytes.fromhex('22090030000000271100')
    assert original[ORDER_CLOSE:ORDER_CLOSE+10]==bytes.fromhex('22080018000000271200')
    entry=b''.join(b'\x02'+struct.pack('<I',v) for v in (EGG_COURSE,EGG_ART,EGG_NAME))+bytes.fromhex('22080002000000')
    return {MAP_CLOSE:entry,ORDER_CLOSE:b'\x02'+struct.pack('<I',EGG_COURSE)}


def draft(c,index):
    return c.get(DRAFT).num(index).raw(b'\x23')


def write_draft(c,index):
    return c.num(index).raw(b'\x28\x01'+struct.pack('<H',DRAFT))


def refresh_courses(c,tag):
    # Exact native2003 command73 availability query independently executed in
    # settings-abi/verify_settings_contracts.py. It only reads profile flags.
    for _ in range(COURSE_TOTAL):c.num(0)
    c.array(COURSE_TOTAL).put(COURSE_AVAILABLE).assign(COURSE_COUNT,0)
    for index in range(COURSE_TOTAL):
        c.num(73).get(0x12).num(index).raw(b'\x23\x3c\x39').native(0x2003,2,drop=False).branch(tag+'_no_'+str(index))
        c.num(1).num(index).raw(b'\x28\x01'+struct.pack('<H',COURSE_AVAILABLE))
        c.get(COURSE_COUNT).num(1).raw(b'\x42').put(COURSE_COUNT)
        c.label(tag+'_no_'+str(index))


def ensure_course(c,tag,in_draft):
    c.get(COURSE_COUNT).num(0).raw(b'\x48').branch(tag+'_done')
    c.get(COURSE_AVAILABLE)
    draft(c,0) if in_draft else c.get(COURSE)
    c.raw(b'\x23').branch(tag+'_find')
    c.branch(tag+'_done',0x15).label(tag+'_find')
    for index in range(COURSE_TOTAL):
        c.get(COURSE_AVAILABLE).num(index).raw(b'\x23').branch(tag+'_skip_'+str(index))
        c.num(index)
        write_draft(c,0) if in_draft else c.put(COURSE)
        c.branch(tag+'_done',0x15).label(tag+'_skip_'+str(index))
    c.label(tag+'_done')


def course_tuple(c,ordinal_slot=None,draft_value=False):
    c.get(0x11).get(0x12)
    draft(c,0) if draft_value else c.get(COURSE if ordinal_slot is None else ordinal_slot)
    return c.raw(b'\x23\x23')


def lobby_hints(c):
    # Proven original host Settings pair is [2,08D2A070]. Enter intentionally
    # retains the reviewed Change Racer route. No Start/Invite action appears.
    c.ident(0xcc7b6d70).ident(0x55379672)
    c.num(0).ident(0xfb59b2c4).num(2).ident(0x08d2a070).num(1).ident(0x3a648438)
    c.array(6).array(1).native(0x1607,3)
    c.ident(0xcc7b6d70).ident(0xce3d133a).ident(0x8d9e062e).array(1).native(0x1607,3)


def draw_lobby(c):
    # Native localized labels cover every original AI choice; these nodes are
    # lobby-owned. Only the explicit offline row uses our persistent buffer.
    for index,(key,slot) in enumerate(zip(base.RULE_KEYS,RULE_SLOTS)):
        c.ident(key).get(0x16).num(index).raw(b'\x23').get(slot).raw(b'\x23\x3c').native(0x021f,2)
    c.bind(0x7d63b234,6)
    for index,(label,identity) in enumerate(ROWS):
        end='local_row_'+str(index)
        c.get(base.CHARACTER).num(index).raw(b'\x4a').branch(end)
        c.fill(6,label+base.PREVIEW_SUFFIX)
        c.ident(0xa2eab366).get(0x13).ident(base.name_hash(identity)).raw(b'\x23').num(0).raw(b'\x23\x3c').native(0x0228,2)
        c.label(end)
    c.get(COURSE_COUNT).num(0).raw(b'\x48').branch('lobby_course_unavailable')
    c.ident(COURSE_ART).native(0x0217,1)
    c.ident(COURSE_NAME).native(0x0217,1)
    c.ident(COURSE_ART);course_tuple(c).num(0).raw(b'\x23\x3c').native(0x0228,2)
    c.ident(COURSE_NAME);course_tuple(c).num(1).raw(b'\x23\x3c').native(0x021f,2)
    c.branch('lobby_course_done',0x15).label('lobby_course_unavailable')
    c.ident(COURSE_ART).native(0x0218,1)
    c.ident(COURSE_NAME).native(0x0218,1)
    c.label('lobby_course_done')
    c.assign(base.DIRTY,0)


def lobby_frame():
    c=Code().get(base.DELAY).num(0).raw(b'\x48').branch('plain_inputs')
    c.get(base.DELAY).num(1).raw(b'\x43').put(base.DELAY).branch('plain_render',0x15)
    c.label('plain_inputs').native(0x041a,0,drop=False).branch('plain_render')
    racer.sound(c,'FE_BACK')
    c.ident(0xcc7b6d70).ident(0xce3d133a).raw(b'\x31').array(1).native(0x1607,3)
    c.ident(0xcc7b6d70).ident(0x4fce5e20).ident(0xee8a2362).native(0x160c,3)
    c.assign(3,0).assign(0x20,1).branch('plain_done',0x15)
    c.label('plain_render').get(base.DIRTY).branch('plain_done')
    draw_lobby(c)
    c.label('plain_done')
    return c


def replace_span(code,start,length,replacement):
    """Replace only the frozen builder's identified embedded inline frame.

    Remaining labels, short branches and local CALL fixups retain their exact
    target identities. No original bytes are searched by an ambiguous pattern.
    """
    end=start+length;delta=len(replacement.data)-length
    assert not any(start<=at<end for at,_ in code.local_fixups)
    assert not any(start<=at<end for at,_ in code.calls)
    assert not any(start<at<end for at in code.labels.values())
    pos=lambda at:at+delta if at>=end else at
    code.data[start:end]=replacement.finish()
    code.labels={name:pos(at) for name,at in code.labels.items()}
    code.local_fixups=[(pos(at),name) for at,name in code.local_fixups]
    code.calls=[(pos(at),target) for at,target in code.calls]
    # Replacement is already internally relocated; only its retained calls
    # need relocation at final module build time.
    code.calls.extend((start+at,target) for at,target in replacement.calls)


def append_code(dest,source):
    at=len(dest.data)
    dest.raw(source.finish())
    dest.calls.extend((at+p,target) for p,target in source.calls)


def render_settings(c):
    c.get(SETTINGS_DIRTY).branch('settings_done')
    # Original Mac3756 skips child BAR animations while parent opens, but
    # still writes labels at3B19. Keep the pending highlight dirty until ready.
    c.ident(SETTINGS).native(0x0213,1,drop=False).branch('settings_labels')
    c.get(PREVIOUS_ROW).get(SETTINGS_ROW).raw(b'\x4b').branch('settings_labels')
    for i,key in enumerate(BARS):
        c.get(PREVIOUS_ROW).num(i).raw(b'\x4a').branch('bar_old_'+str(i))
        c.ident(key).ident(0xd279b4c1).ident(0x0d09208a).num(0).native(0x020e,4)
        c.label('bar_old_'+str(i))
        c.get(SETTINGS_ROW).num(i).raw(b'\x4a').branch('bar_new_'+str(i))
        c.ident(key).ident(0x0d09208a).ident(0xd279b4c1).num(0).native(0x020e,4)
        c.label('bar_new_'+str(i))
    c.get(SETTINGS_ROW).put(PREVIOUS_ROW)
    c.label('settings_labels')
    c.get(COURSE_COUNT).num(0).raw(b'\x48').branch('settings_course_unavailable')
    for key in TEXT_PAIRS[0]:
        c.ident(key);course_tuple(c,draft_value=True).num(1).raw(b'\x23\x3c').native(0x021f,2)
    c.branch('settings_course_done',0x15).label('settings_course_unavailable')
    for key in TEXT_PAIRS[0]:c.bind(key,5)
    c.label('settings_course_done')
    for index,pair in enumerate(TEXT_PAIRS[1:]):
        for key in pair:
            c.ident(key).get(0x16).num(index).raw(b'\x23')
            draft(c,index+1).raw(b'\x23\x3c').native(0x021f,2)
    c.get(PREVIOUS_ROW).get(SETTINGS_ROW).raw(b'\x4a').branch('settings_done')
    c.assign(SETTINGS_DIRTY,0)


def close_settings(c,commit):
    if commit:
        draft(c,0).put(COURSE)
        for i,slot in enumerate(RULE_SLOTS):draft(c,i+1).put(slot)
    racer.sound(c,'FE_SELECT' if commit else 'FE_BACK')
    c.ident(SETTINGS).ident(0xc1dd7351).ident(0x36a7542b).num(0).native(0x020e,4)
    c.assign(SETTINGS_MODE,2).assign(base.DIRTY,1).branch('settings_done',0x15)


def change_setting(c,step,tag):
    c.get(SETTINGS_ROW).num(0).raw(b'\x4a').branch(tag+'_rules')
    c.get(COURSE_COUNT).num(0).raw(b'\x48').branch('settings_render')
    draft(c,0).put(COURSE_OLD)
    c.assign(COURSE_SCAN,0).label(tag+'_course_loop')
    draft(c,0).num(1).raw(b'\x43' if step<0 else b'\x42');write_draft(c,0)
    draft(c,0).num(0 if step<0 else COURSE_TOTAL-1).raw(b'\x46' if step<0 else b'\x48').branch(tag+'_course_test')
    c.num(COURSE_TOTAL-1 if step<0 else 0);write_draft(c,0)
    c.label(tag+'_course_test').get(COURSE_AVAILABLE);draft(c,0).raw(b'\x23').branch(tag+'_course_retry')
    c.assign(SETTINGS_DIRTY,1);racer.sound(c,'FE_MOVE');c.branch('settings_render',0x15)
    c.label(tag+'_course_retry').get(COURSE_SCAN).num(1).raw(b'\x42').put(COURSE_SCAN)
    c.get(COURSE_SCAN).num(COURSE_TOTAL).raw(b'\x46').branch(tag+'_course_abort')
    c.branch(tag+'_course_loop',0x15).label(tag+'_course_abort')
    c.get(COURSE_OLD);write_draft(c,0);c.branch('settings_render',0x15)
    c.label(tag+'_rules')
    for row,(low,high) in enumerate(BOUNDS,1):
        done=tag+'_'+str(row)
        c.get(SETTINGS_ROW).num(row).raw(b'\x4a').branch(done)
        draft(c,row).num(low if step<0 else high).raw(b'\x48' if step<0 else b'\x46').branch('settings_render')
        draft(c,row).num(1).raw(b'\x43' if step<0 else b'\x42');write_draft(c,row)
        c.assign(SETTINGS_DIRTY,1);racer.sound(c,'FE_MOVE')
        c.branch('settings_render',0x15).label(done)
    c.branch('settings_render',0x15)


def settings_wrapper(inner):
    c=Code().get(SETTINGS_MODE).num(2).raw(b'\x4a').branch('settings_not_closing')
    c.ident(SETTINGS).native(0x0213,1,drop=False).branch('settings_done')
    c.assign(SETTINGS_MODE,0).assign(base.DELAY,2).assign(base.DIRTY,1)
    lobby_hints(c);draw_lobby(c)
    c.branch('settings_done',0x15)
    c.label('settings_not_closing').get(SETTINGS_MODE).branch('settings_lobby')
    c.ident(SETTINGS).native(0x0213,1,drop=False).branch('settings_render')
    c.get(base.DELAY).num(0).raw(b'\x48').branch('settings_inputs')
    c.get(base.DELAY).num(1).raw(b'\x43').put(base.DELAY).branch('settings_render',0x15)
    c.label('settings_inputs').native(0x041a,0,drop=False).branch('settings_accept')
    close_settings(c,False)
    c.label('settings_accept').native(0x0419,0,drop=False).branch('settings_up')
    close_settings(c,True)
    c.label('settings_up').native(0x0421,0,drop=False).branch('settings_down')
    c.get(SETTINGS_ROW).num(0).raw(b'\x48').branch('settings_render')
    c.get(SETTINGS_ROW).num(1).raw(b'\x43').put(SETTINGS_ROW)
    c.assign(SETTINGS_DIRTY,1);racer.sound(c,'FE_MOVE');c.branch('settings_render',0x15)
    c.label('settings_down').native(0x0422,0,drop=False).branch('settings_left')
    c.get(SETTINGS_ROW).num(5).raw(b'\x46').branch('settings_render')
    c.get(SETTINGS_ROW).num(1).raw(b'\x42').put(SETTINGS_ROW)
    c.assign(SETTINGS_DIRTY,1);racer.sound(c,'FE_MOVE');c.branch('settings_render',0x15)
    c.label('settings_left').native(0x0423,0,drop=False).branch('settings_right')
    change_setting(c,-1,'decrease')
    c.label('settings_right').native(0x0424,0,drop=False).branch('settings_render')
    change_setting(c,1,'increase')
    c.label('settings_render');render_settings(c)
    c.branch('settings_done',0x15)
    c.label('settings_lobby').get(MODE).branch('settings_racer_idle')
    c.branch('settings_run_racer',0x15)
    c.label('settings_racer_idle').get(base.DELAY).num(0).raw(b'\x48').branch('settings_lobby_ready')
    c.branch('settings_run_racer',0x15)
    c.label('settings_lobby_ready').native(0x041a,0,drop=False).branch('settings_entry')
    c.branch('settings_run_racer',0x15)
    c.label('settings_entry').native(0x041d,0,drop=False).branch('settings_run_racer')
    c.get(COURSE)
    for slot in RULE_SLOTS:c.get(slot)
    c.array(6).put(DRAFT).assign(SETTINGS_MODE,1).assign(SETTINGS_ROW,0).assign(PREVIOUS_ROW,-1)
    c.assign(SETTINGS_DIRTY,1).assign(base.DELAY,2)
    refresh_courses(c,'entry_courses');ensure_course(c,'entry_course',True)
    c.get(COURSE_COUNT).num(0).raw(b'\x48').branch('entry_no_courses')
    c.branch('entry_course_visibility_done',0x15).label('entry_no_courses')
    c.ident(COURSE_ART).native(0x0218,1).ident(COURSE_NAME).native(0x0218,1)
    c.label('entry_course_visibility_done')
    for key in BARS:c.ident(key).ident(0x0d09208a).native(0x0215,2)
    racer.sound(c,'FE_SELECT')
    c.ident(SETTINGS).ident(0xb2244a6d).ident(0x02f0b24c).num(0).native(0x020e,4)
    c.call(0x1fdd)
    c.branch('settings_render',0x15)
    c.label('settings_run_racer');append_code(c,inner)
    c.label('settings_done')
    return c


def make_chunks(original):
    # Generation-only substitutions customize two explicit UI helpers. The
    # frozen builder's racer state machine is reused, and its source is never
    # written. Restore Python globals immediately, including on exceptions.
    old_draw,old_hints=base.draw,racer.lobby_hints
    try:
        base.draw,racer.lobby_hints=draw_lobby,lobby_hints
        chunks,detail=racer.make_chunks(original)
    finally:
        base.draw,racer.lobby_hints=old_draw,old_hints
    inner=chunks[0x1672]
    replace_span(inner,detail['base_frame_offset_in_wrapper'],detail['base_frame_length'],lobby_frame())
    chunks[0x1672]=settings_wrapper(inner)
    init=chunks[0xf7f]
    # Buffer5 was allocated by the inherited constructor but has no live
    # bindings. It now owns the explicit empty-availability course label.
    init.fill(5,'UNAVAILABLE')
    for slot,value in ((SETTINGS_MODE,0),(SETTINGS_ROW,0),(PREVIOUS_ROW,-1),(SETTINGS_DIRTY,0),(COURSE,0),(COURSE_COUNT,0),(COURSE_SCAN,0),(COURSE_OLD,0)):
        init.assign(slot,value)
    for value in (0,3,7,1,1,1):init.num(value)
    init.array(6).put(DRAFT)
    for _ in range(COURSE_TOTAL):init.num(0)
    init.array(COURSE_TOTAL).put(COURSE_AVAILABLE)
    show=chunks[0xf95]
    show.assign(base.SELECT,0).assign(SETTINGS_MODE,0).assign(SETTINGS_DIRTY,0)
    show.ident(SETTINGS).ident(0x36a7542b).native(0x0215,2)
    refresh_courses(show,'show_courses');ensure_course(show,'show_course',False)
    return chunks,detail


def retained_tables(original):
    course_map={}
    for at in range(0x68f,0x89f,22):
        assert all(original[at+x]==2 for x in (0,5,10))
        assert original[at+15:at+22]==bytes.fromhex('22080002000000')
        course_map[base.u32(original,at+1)]=[base.u32(original,at+6),base.u32(original,at+11)]
    order=[base.u32(original,at+1) for at in range(0x8a9,0x921,5)]
    assert len(course_map)==len(order)==STOCK_COURSES and len(set(order))==STOCK_COURSES
    assert set(order)==set(course_map)
    labels=[[base.u32(original,p+5*i+1) for i in range(n)] for p,n in ((0xcb5,10),(0xcee,9),(0xd22,4),(0xd3d,2),(0xd4e,2))]
    return course_map,order,labels


