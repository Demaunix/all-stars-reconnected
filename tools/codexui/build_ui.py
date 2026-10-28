"""Append original Metal UI resources while preserving every original PC entry.

Only creates files in separate-slot/ui. No live or released-package writes.
"""
from pathlib import Path
import sys,struct,json,zlib,hashlib,re
sys.path[:0]=['work/metal-sonic',str(Path(__file__).parent)]
from decode_ui_texture import sections,convert
from probe import chunks,text_info,sprites
R=Path('work/metal-sonic');O=Path(__file__).parent
SOURCE=Path('outputs/Metal-Sonic-Race-Test/backups/SumoToolResources-original.xpac')
sha=lambda d:hashlib.sha256(d).hexdigest()
LABEL={'EN':'7E231530','FR':'E05A903D','GE':'3A91BB97','IT':'7AC49646','SP':'B4B25A44'}
RESULTS={'EN':'FB9EB334','FR':'23D0E051','GE':'C2A993CB','IT':'4B9C2F2A','SP':'7A13C688'}
LANGS=('EN','US','JP','FR','GE','IT','SP')
NEEDED_SPRITES={0x275AB83D,0x81AB004B,0x54D95CA4,0x4DD55768}
NEEDED_TEXT={0xEDD0B6AB,0xEF22A41D,0xBF576F13}
COUNTERPART_SPRITES={0xE76CC970:0x275AB83D,0x130A9738:0xCFF23477,0x30D56ADC:0xCFF23477,
    0x40A3D831:0xE76B1254,0x880AC00C:0xE76B1254,0x59E10E82:0xFF5F8ABE}
NETWORK_TEXT={0xA12E891E:b'METAL SONIC',0xF1F0CC4C:b'METAL SONIC'}

def path_hash(s):
    v=0
    for c in s.upper().replace('/','\\')[::-1]:v=(ord(c)+131*v)&0xffffffff
    return v

def finish(d):
    d=bytearray(d);used=len(d)-16;d+=bytes((-len(d))%64)
    struct.pack_into('<2I',d,4,len(d),used)
    return bytes(d)

def ptex_info(c):
    sp=sprites(c);nt,ptr=struct.unpack_from('<2I',c,20);tex=[]
    for j in range(nt):
        p,n=struct.unpack_from('<2I',c,16+ptr+8*j);p+=16
        assert c[p:p+4]==b'DDS '
        tex.append(c[p:p+n])
    return sp,tex

def make_ptex(template,sp,tex):
    assert len({s[0] for s in sp})==len(sp)
    sp=sorted(sp,key=lambda s:s[0])
    body=bytearray(template[:28]);struct.pack_into('<3I',body,16,len(sp),len(tex),0)
    body+=b''.join(struct.pack('<I8fI',*s) for s in sp)
    entries=[]
    for d in tex:
        assert d[:4]==b'DDS '
        body+=bytes((-len(body))%4);entries.append((len(body)-16,len(d)));body+=d
    body+=bytes((-len(body))%4);table=len(body)-16
    body+=b''.join(struct.pack('<2I',*x) for x in entries)
    struct.pack_into('<I',body,24,table)
    return finish(body),table

def make_relo(template,tag,positions):
    rb=bytearray(template[:20]);rb[16:20]=tag
    rb+=b''.join(struct.pack('<2I',1,p) for p in positions)+bytes(8)
    return finish(rb)

def merge_ptex(c,relo,newsp,newtex):
    oldsp,oldtex=ptex_info(c);oldkeys={s[0] for s in oldsp}
    assert not oldkeys&{s[0] for s in newsp},'Unexpected native Metal key collision'
    newrows=[(*s[:9],s[9]+len(oldtex)) for s in newsp]
    result,table=make_ptex(c,oldsp+newrows,oldtex+newtex)
    nr=make_relo(relo,b'PTEX',[8]+[table+8*j for j in range(len(oldtex)+len(newtex))])
    checksp,checktex=ptex_info(result);byid={s[0]:s for s in checksp}
    assert all(byid[s[0]]==s for s in oldsp) and checktex[:len(oldtex)]==oldtex
    assert all(byid[s[0]]==s for s in newrows) and checktex[len(oldtex):]==newtex
    return result,nr,dict(old_sprites=len(oldsp),new_sprites=len(checksp),old_textures=len(oldtex),new_textures=len(checktex),
        added_sprite_keys=[f'{s[0]:08X}' for s in newsp],all_original_sprite_records_and_dds_preserved=True)

def merge_text(c,relo,added):
    before=text_info(c);assert not set(h for h,v in before)&set(added)
    combined=sorted(before+list(added.items()))
    assert len({h for h,v in combined})==len(combined)
    body=bytearray(c[:28]);struct.pack_into('<I',body,24,len(combined));body+=bytes(8*len(combined))
    for j,(h,value) in enumerate(combined):
        p=len(body)-16;body+=value+b'\0';struct.pack_into('<2I',body,28+8*j,h,p)
    result=finish(body);nr=make_relo(relo,b'TEXT',[16+8*j for j in range(len(combined))])
    values=dict(text_info(result));assert all(values[h]==v for h,v in before)
    assert all(values[h]==v for h,v in added.items())
    return result,nr,dict(old_texts=len(before),new_texts=len(combined),added_text_keys=[f'{h:08X}' for h in added],all_original_text_bytes_preserved=True)

def pack(ss):
    out=bytearray(72)
    for j,d in enumerate(ss):
        if not d:continue
        compressed=zlib.compress(d,9);out+=bytes((-len(out))%32);p=len(out);out+=compressed
        struct.pack_into('<3I',out,12*j,p,len(d),len(compressed))
    out+=bytes((-len(out))%32)
    assert sections(out,'<')==ss
    return bytes(out)

def console_art(member,section,offset):
    ss=sections((R/f'dlc-probe/members/{member}.bin').read_bytes(),'>')
    chunk=next(c for p,c in chunks(ss[section],'>') if p==offset)
    sp=sprites(chunk,'>');nt=struct.unpack_from('>I',chunk,20)[0]
    tex=[convert(ss[section],offset,ss[section+2],i)[0] for i in range(nt)]
    return ss,chunk,sp,tex

def check_relos(ss):
    for section in (0,3):
        cc,rr=chunks(ss[section]),chunks(ss[section+1]);assert len(cc)==len(rr)
        for (_,c),(_,r) in zip(cc,rr):
            assert c[:4]==r[16:20]
            used=struct.unpack_from('<I',r,8)[0];records=[struct.unpack_from('<2I',r,p) for p in range(20,16+used,8)]
            assert records[-1]==(0,0)
            for typ,p in records[:-1]:
                assert typ==1 and p+20<=len(c)
                target=struct.unpack_from('<I',c,p+16)[0]
                assert 0<=target+16<=len(c)
            if c[:4]==b'PTEX':
                nt,table=struct.unpack_from('<2I',c,20)
                assert records==[(1,8)]+[(1,table+8*j) for j in range(nt)]+[(0,0)]
            elif c[:4]==b'TEXT':
                n=struct.unpack_from('<I',c,24)[0]
                assert records==[(1,16+8*j) for j in range(n)]+[(0,0)]

def main():
    source=SOURCE.read_bytes()
    assert struct.unpack_from('<I',source,8)[0]==28672
    index={x['index']:x['name'] for x in json.loads((R/'pc-archive-index.json').read_text()) if x['archive']=='SumoToolResources.xpac'}
    n=struct.unpack_from('<I',source,12)[0]
    recs=[struct.unpack_from('<5I',source,24+20*i) for i in range(n)]
    assert all(recs[i][0]<recs[i+1][0] for i in range(n-1))
    assert path_hash(index[1413])==recs[1413][0]
    out=bytearray(source);updates=[];additions=[];foundmenus=set();foundhud=set();text_sources={}
    _,_,hudsp,hudtex=console_art('FB9EB334',3,0)
    assert {s[0] for s in hudsp}=={0x275AB83D,0xCFF23477,0xE76B1254,0xFF5F8ABE}
    assert hudtex==[(R/f'identity/metal-ui-{i}.dds').read_bytes() for i in range(3)]
    name_ss=sections((R/'dlc-probe/members/501C98BD.bin').read_bytes(),'>')
    assert dict(text_info(chunks(name_ss[3],'>')[0][1],'>'))[0xA12E891E]==NETWORK_TEXT[0xA12E891E]
    track_ss=sections((R/'identity3-stats-audit/tu2-members/354F9209.bin').read_bytes(),'>')
    assert dict(text_info(chunks(track_ss[3],'>')[0][1],'>'))[0xF1F0CC4C]==NETWORK_TEXT[0xF1F0CC4C]
    counterpart_indices={x['index'] for x in json.loads((O/'pc-counterpart-key-locations.json').read_text())}
    for i,r in enumerate(recs):
        name=index.get(i,'');menu=bool(re.search(r'/fe_character_select_[A-Z]{2}\.stz$',name));hud=bool(re.search(r'/Ingame/CommonHud_[A-Z]{2}\.stz$',name))
        if not(menu or hud or i in counterpart_indices):continue
        lang=name[-6:-4] if name.endswith('.stz') else None;native_lang=lang if lang in LABEL else 'EN';old=source[r[1]:r[1]+r[3]]
        ss=sections(old,'<');cc=[c for p,c in chunks(ss[3])];rr=[c for p,c in chunks(ss[4])]
        assert len(cc)==len(rr);changes=[];text={}
        if menu:
            foundmenus.add(lang);dss,dc,sp,tex=console_art(LABEL[native_lang],0,64)
            assert any(s[0]==0x01482F5A for c in cc if c[:4]==b'PTEX' for s in sprites(c))
            assert {s[0] for s in sp}==NEEDED_SPRITES
            text=dict(text_info(next(c for p,c in chunks(dss[3],'>') if c[:4]==b'TEXT'),'>'))
            assert set(text)==NEEDED_TEXT
            text_sources[lang]={'source_language':native_lang,'source_member':LABEL[native_lang],'fallback':native_lang!=lang,
                              'values':{f'{h:08X}':v.decode('utf-8') for h,v in text.items()}}
        elif hud:foundhud.add(lang);sp,tex=hudsp,hudtex
        for ci,c in enumerate(cc):
            if c[:4]==b'PTEX':
                if not(menu or hud):
                    oldkeys={s[0] for s in sprites(c)};needed={new for old,new in COUNTERPART_SPRITES.items() if old in oldkeys}
                    sp=[s for s in hudsp if s[0] in needed];tex=hudtex
                if sp:cc[ci],rr[ci],change=merge_ptex(c,rr[ci],sp,tex);changes.append({'type':'sprites',**change})
            elif c[:4]==b'TEXT':
                added=dict(text) if menu else {}
                if 0x0EBD3503 in dict(text_info(c)):added.update(NETWORK_TEXT)
                if added:cc[ci],rr[ci],change=merge_text(c,rr[ci],added);changes.append({'type':'text',**change})
        assert changes
        newss=list(ss);newss[3]=b''.join(cc);newss[4]=b''.join(rr)
        assert all(newss[j]==ss[j] for j in (0,1,2,5))
        # Unmodified scene/animation/font chunks and their relocations are preserved.
        for j,(oc,nc) in enumerate(zip(chunks(ss[3]),chunks(newss[3]))):
            if oc[1][:4] not in (b'PTEX',b'TEXT'):assert oc[1]==nc[1] and chunks(ss[4])[j][1]==chunks(newss[4])[j][1]
        # Validate rewritten variable tables. Other engine relocation types remain native.
        for ci,c in enumerate(cc):
            if c[:4] in (b'PTEX',b'TEXT'):
                check_relos([b'',b'',b'',c,rr[ci],b''])
        new=pack(newss);out+=bytes((-len(out))%2048);p=len(out);out+=new;out+=bytes((-len(out))%2048)
        recs[i]=(r[0],p,len(new),len(new),r[4])
        updates.append({'index':i,'hash':f'{r[0]:08X}','name':name,'language':lang,'source_sha256':sha(old),'new_sha256':sha(new),'changes':changes})
        if lang=='EN' and (menu or hud):(O/('menu-EN.stz' if menu else 'hud-EN.stz')).write_bytes(new)
    assert foundmenus==foundhud==set(LANGS)
    # Add the original Metal standalone result resources, with no dependency on Ulala IDs.
    template_record=struct.unpack_from('<5I',source,24+20*1413)
    template_ss=sections(source[template_record[1]:template_record[1]+template_record[3]],'<')
    template_pt=chunks(template_ss[3])[0][1];template_relo=chunks(template_ss[4])[0][1]
    template_scene_relo=chunks(template_ss[4])[1][1]
    for lang in LANGS:
        native_lang=lang if lang in RESULTS else 'EN';member=RESULTS[native_lang]
        xss,xpt,xsp,xtex=console_art(member,3,0)
        pt,table=make_ptex(template_pt,xsp,xtex)
        pr=make_relo(template_relo,b'PTEX',[8]+[table+8*j for j in range(len(xtex))])
        xs=next(c for p,c in chunks(xss[3],'>') if c[:4]==b'SCNE')
        scene=bytearray(xs)
        for p in range(4,44,4):scene[p:p+4]=xs[p:p+4][::-1]
        assert struct.unpack_from('<3I',scene,16)==(1,0x65A190A8,0)
        rss=[b'',b'',b'',pt+bytes(scene),pr+template_scene_relo,b''];check_relos(rss)
        payload=pack(rss);name=f'.\\Resource/SumoToolResources/Characters/Race_Results_MetalSonic_{lang}.stz';h=path_hash(name)
        assert h not in {r[0] for r in recs};out+=bytes((-len(out))%2048);p=len(out);out+=payload;out+=bytes((-len(out))%2048)
        recs.append((h,p,len(payload),len(payload),0))
        additions.append({'name':name,'hash':f'{h:08X}','source_member':member,'source_language':native_lang,'fallback':native_lang!=lang,'sha256':sha(payload)})
        (O/f'Race_Results_MetalSonic_{lang}.stz').write_bytes(payload)
    recs.sort(key=lambda r:r[0]);assert 24+len(recs)*20<=min(r[1] for r in recs)
    struct.pack_into('<I',out,12,len(recs))
    for i,r in enumerate(recs):struct.pack_into('<5I',out,24+20*i,*r)
    # Existing unmodified archives retain byte-identical stored payload and flags.
    byhash={r[0]:r for r in recs};changed={struct.unpack_from('<I',source,24+20*x['index'])[0] for x in updates}
    preserved=0
    for i in range(n):
        r=struct.unpack_from('<5I',source,24+20*i);nr=byhash[r[0]]
        if r[0] not in changed:assert r==nr and source[r[1]:r[1]+r[3]]==out[nr[1]:nr[1]+nr[3]];preserved+=1
    target=O/'SumoToolResources.xpac';target.write_bytes(out)
    report={'status':'staged-static-verification-passed-runtime-pending','source':str(SOURCE),'source_sha256':sha(source),
      'archive_sha256':sha(out),'archive_bytes':len(out),'old_member_count':n,'new_member_count':len(recs),
      'updated_existing_members':updates,'new_members':additions,'unchanged_existing_members':preserved,
      'text_sources':text_sources,'required_menu_sprite_keys':[f'{x:08X}' for x in sorted(NEEDED_SPRITES)],
      'required_menu_text_keys':[f'{x:08X}' for x in sorted(NEEDED_TEXT)],
      'hud_added_sprite_keys':[f'{s[0]:08X}' for s in hudsp],
      'counterpart_sprite_mapping':{f'{k:08X}':f'{v:08X}' for k,v in COUNTERPART_SPRITES.items()},
      'network_name_text':{'A12E891E':'METAL SONIC','F1F0CC4C':'METAL SONIC',
          'sources':['dlc-probe/members/501C98BD.bin section3 TEXT','identity3-stats-audit/tu2-members/354F9209.bin section3 TEXT']},
      'model_keys_handled_by_parent':['7B9D7E38','37FE3678','82DF1934'],
      'all_original_ulala_sprite_text_and_dds_values_preserved':True,'game_writes':False,
      'unresolved_required_ui_keys':[],
      'limitations':['US and JP use original English Metal labels/results as explicitly approved; no native Japanese localization supplied.',
        'Standalone result resource uses original Metal scene ID; runtime result-screen selection still needs testing.',
        'Native controller, audio event registration and actual menu/race testing belong to other staged tasks.']}
    (O/'verification.json').write_text(json.dumps(report,indent=2))
    print(json.dumps({k:v for k,v in report.items() if k not in ('updated_existing_members','new_members','text_sources')},indent=2))

if __name__=='__main__':main()
