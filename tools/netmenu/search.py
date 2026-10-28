"""Offline native Search/Create successor to frozen Settings280f.

Only this directory is writable output. Dialog body adaptation is separately
owned/reviewed under dialogs; all search rows remain empty and hidden.
"""
# Vendored from Codex's work for this user (stage 4 of 4):
# R0927\native-menu-20261001\next-menu-flows\search-preview\build_search_preview.py, sha256 a788ca31...
# Changes: imports point at our tools; build_module/__main__ (which read Codex's staging folders)
# removed; tools/netmenu/__init__.py build() does the same final assembly.
import struct

from netmenu import settings

base,racer,Code=settings.base,settings.racer,settings.Code
FLOW,ACTION,AUTO_CREATE,CREATING,CREATE_RESULT=range(0x46,0x4b)
GLOBAL_COUNT=0x4b
NETWORK,DIALOGS=0x8398d0bc,0x694fe697
CHOICE=base.name_hash('codex_offline_search_choice')
SEARCH_SCENE=0xa363a411
SEARCH_ROWS=(0x30ef8abe,0x86597424,0x0162cf47,0x8cb75065,0xb2ddd2a5,0x81316b31,0x555f08ba,0xec479cc2)
BODY='Online search is unavailable in this offline menu preview. Create opens a local lobby preview. No match will be created.'
assert BODY.isascii() and len(BODY)<240


def call_args(c,target,args):
    c.raw(b'\x17')
    for kind,value in args:
        c.get(value) if kind=='global' else c.num(value)
    at=len(c.data);c.raw(b'\x34'+bytes([len(args)])+bytes(4)+struct.pack('<HH',1,0x7ffe)+b'\x18')
    c.calls.append((at,target))


def forest(c,slot,remove):
    call_args(c,0x1118,[('global',slot),('number',0),('number',int(remove))])


def request_notice(c):
    c.assign(ACTION,-1)
    c.ident(DIALOGS).ident(0xdfea0f93).ident(0x28bc494f).ident(NETWORK).ident(CHOICE).string(BODY).array(4).native(0x160c,3)


def return_mainmenu(c):
    racer.sound(c,'FE_BACK')
    c.ident(0xcc7b6d70).ident(0xce3d133a).raw(b'\x31').array(1).native(0x1607,3)
    c.ident(0xcc7b6d70).ident(0x4fce5e20).ident(0xee8a2362).native(0x160c,3)
    c.assign(3,0).assign(0x20,1)


def search_show(c,tag):
    c.assign(FLOW,0).assign(AUTO_CREATE,0).assign(CREATING,0).assign(CREATE_RESULT,0)
    c.assign(racer.MODE,0).assign(settings.SETTINGS_MODE,0).ident(0xee8a2362).put(2)
    # Retained PC1715..179F UI-only path; omit Searching labels/spinner entirely.
    forest(c,0x0a,False)
    c.ident(SEARCH_SCENE).get(9).raw(b'\x3c').native(0x0205,2)
    c.ident(0x5cf9607b).ident(0xa56426e2).ident(0x3ecdb4b9).num(0).native(0x020e,4)
    for row in SEARCH_ROWS:c.ident(row).native(0x0218,1)
    # Original cleanup1417 uses this exact hidden loading state.
    c.ident(0xfff27543).ident(0xa56426e2).native(0x0215,2)
    c.ident(0xcc7b6d70).ident(0x55379672).num(2).ident(0xd9529529).num(1).ident(0x3a648438).array(4).array(1).native(0x1607,3)
    c.ident(0xcc7b6d70).ident(0xce3d133a).ident(0x868b724d).array(1).native(0x1607,3)
    # Claim no shared dialog while another owner is active. The main frame
    # queues this request only after both existing dialog and Search animation
    # gates permit it; -1 subsequently means a delivered/queued response wait.
    c.assign(ACTION,-2)


def remove_lobby(c):
    forest(c,0x0b,True)
    for scene in (0x8c2f0f09,0x3ee6042a,0x423a1091):c.ident(scene).native(0x0206,1)


def insert_fragment(code,at,fragment,retain_entry=None):
    """Splice generated symbolic instructions without losing outer fixups."""
    delta=len(fragment.data);pos=lambda p:p+delta if p>=at else p
    assert not (set(code.labels)&set(fragment.labels))
    code.data[at:at]=fragment.data
    code.labels={name:pos(p) for name,p in code.labels.items()}
    if retain_entry is not None:code.labels[retain_entry]=at
    code.labels.update({name:at+p for name,p in fragment.labels.items()})
    code.local_fixups=[(pos(p),name) for p,name in code.local_fixups]+[(at+p,name) for p,name in fragment.local_fixups]
    code.calls=[(pos(p),target) for p,target in code.calls]+[(at+p,target) for p,target in fragment.calls]


def make_chunks(original):
    captured={}
    old_make,old_close=racer.make_chunks,racer.close_popup
    def close_with_reason(c,commit):
        tag='creation_close_'+str(commit)
        c.get(CREATING).branch(tag).assign(CREATE_RESULT,1 if commit else 2).label(tag)
        old_close(c,commit)
    def make_with_entry(original):
        chunks,detail=old_make(original)
        c=chunks[0x1672]
        # A real Create action uses the same available-racer opening logic.
        # It bypasses the lobby's Back/selection guard while pending; no device
        # event or key is synthesized, and normal Change Racer stays unchanged.
        at=c.labels['lobby_ready']
        fragment=Code().get(AUTO_CREATE).branch('creation_normal_guard').branch('lobby_accept',0x15).label('creation_normal_guard')
        insert_fragment(c,at,fragment,'lobby_ready')
        detail['base_frame_offset_in_wrapper']+=len(fragment.data)
        at=c.labels['lobby_accept']
        op=next(o for o in base.instructions(c.data,at,at+8,'<'))
        assert op['opcode']==0x32 and struct.unpack_from('<H',c.data,at+2)[0]==0x419
        fragment=Code().get(AUTO_CREATE).raw(b'\x50').get(AUTO_CREATE).branch('creation_attempt_marked')
        fragment.assign(AUTO_CREATE,2).label('creation_attempt_marked')
        insert_fragment(c,op['end'],fragment)
        detail['base_frame_offset_in_wrapper']+=len(fragment.data)
        captured['racer_frame']=c
        return chunks,detail
    try:
        racer.make_chunks,racer.close_popup=make_with_entry,close_with_reason
        chunks,detail=settings.make_chunks(original)
    finally:
        racer.make_chunks,racer.close_popup=old_make,old_close
    lobby_show=chunks[0xf95]
    for slot,value in ((FLOW,0),(ACTION,-1),(AUTO_CREATE,0),(CREATING,0),(CREATE_RESULT,0)):
        chunks[0xf7f].assign(slot,value)
    initial=Code();search_show(initial,'initial')
    chunks[0xf95]=initial
    inherited=chunks[0x1672]
    c=Code().get(FLOW).branch('search_active')
    c.get(AUTO_CREATE).branch('regular_lobby_frame')
    settings.append_code(c,captured['racer_frame'])
    c.get(AUTO_CREATE).num(2).raw(b'\x4a').branch('search_done')
    c.assign(AUTO_CREATE,0)
    c.get(racer.MODE).num(1).raw(b'\x4a').branch('creation_failed')
    c.branch('search_done',0x15).label('creation_failed')
    c.assign(CREATE_RESULT,2).branch('creation_result',0x15)
    c.label('regular_lobby_frame');settings.append_code(c,inherited)
    c.label('creation_result').get(CREATING).branch('search_done')
    c.get(racer.MODE).num(0).raw(b'\x4a').branch('search_done')
    c.get(CREATE_RESULT).num(1).raw(b'\x4a').branch('creation_cancel_check')
    c.assign(CREATING,0).assign(CREATE_RESULT,0).branch('search_done',0x15)
    c.label('creation_cancel_check').get(CREATE_RESULT).num(2).raw(b'\x4a').branch('search_done')
    remove_lobby(c);search_show(c,'creation_cancel')
    c.branch('search_done',0x15)
    c.label('search_active').ident(DIALOGS).ident(0x53335ccb).native(0x1607,2,drop=False).branch('dialog_idle')
    c.branch('search_done',0x15)
    c.label('dialog_idle').ident(0x5cf9607b).native(0x0213,1,drop=False).branch('search_done')
    c.get(ACTION).num(-2).raw(b'\x4a').branch('notice_already_requested')
    request_notice(c);c.branch('search_done',0x15)
    c.label('notice_already_requested')
    c.get(ACTION).num(0).raw(b'\x46').branch('have_action')
    c.branch('search_done',0x15)
    c.label('have_action').get(ACTION).num(0).raw(b'\x4a').branch('create_action')
    request_notice(c);c.branch('search_done',0x15)
    c.label('create_action').get(ACTION).num(1).raw(b'\x4a').branch('cancel_action')
    c.assign(ACTION,-1).assign(FLOW,1).assign(CREATING,1).assign(CREATE_RESULT,0).assign(AUTO_CREATE,1)
    forest(c,0x0a,True);c.ident(SEARCH_SCENE).native(0x0206,1)
    # Original creation defaults distinguish Friends privacy from Anyone.
    c.assign(base.LAPS,3).assign(base.AI,1).assign(base.ITEMS,1).assign(base.CATCHUP,1).assign(settings.COURSE,0)
    c.get(0x23).branch('anyone_defaults').assign(base.PRIVATE_SLOTS,7).branch('privacy_done',0x15)
    c.label('anyone_defaults').assign(base.PRIVATE_SLOTS,0).label('privacy_done')
    c.assign(0x34,-1)
    settings.append_code(c,lobby_show)
    c.branch('search_done',0x15)
    c.label('cancel_action');return_mainmenu(c)
    c.label('search_done')
    chunks[0x1672]=c
    # Append a private callback after the original code, protected by a jump.
    # Original dialog replacement/close callback(-1) is treated as Cancel.
    end=base.u32(original,12)+base.u32(original,16)
    callback=Code().branch('callback_after',0x15).label('callback_entry')
    callback.get(FLOW).num(0).raw(b'\x4a').branch('callback_return')
    callback.raw(b'\x0b\x01').num(-1).raw(b'\x4a').branch('callback_normal')
    callback.assign(ACTION,2).branch('callback_return',0x15)
    callback.label('callback_normal').raw(b'\x0b\x01').num(0).raw(b'\x46').branch('callback_upper')
    callback.branch('callback_return',0x15)
    callback.label('callback_upper').raw(b'\x0b\x01').num(2).raw(b'\x48').branch('callback_store')
    callback.branch('callback_return',0x15)
    callback.label('callback_store').raw(b'\x0b\x01').put(ACTION)
    callback.label('callback_return').raw(b'\x17\x1b').label('callback_after')
    chunks[end]=callback
    return chunks,dict(callback_original_anchor=end,callback_relative_entry=callback.labels['callback_entry'],
                       inherited_settings_details=detail)


def add_export(data,symbol,entry):
    at=base.u32(data,20);start=base.u32(data,12);count=base.u32(data,at)
    entries=dict(struct.iter_unpack('<II',data[at+4:at+4+8*count]));assert symbol not in entries
    entries[symbol]=entry-start;old_end=at+4+8*count
    table=struct.pack('<I',len(entries))+b''.join(struct.pack('<II',h,p) for h,p in sorted(entries.items()))
    out=bytearray(data[:at]+table+data[old_end:base.u32(data,8)])
    struct.pack_into('<I',out,8,base.u32(data,8)+8)
    for field in (24,28,44,52):
        value=base.u32(data,field)
        if value:assert value>=old_end;struct.pack_into('<I',out,field,value+8)
    for i in range(base.u32(data,48)):
        p=base.u32(data,44)+4*i;value=base.u32(data,p);assert value>=old_end
        struct.pack_into('<I',out,p+8,value+8)
    out+=bytes((-len(out))%4);base.validate(out)
    return bytes(out)


