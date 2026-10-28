from pathlib import Path
import sys,json,struct,zlib,hashlib
sys.path.insert(0,'work/metal-sonic')
from decode_ui_texture import sections
R=Path('work/metal-sonic'); O=Path(__file__).parent
KEYS={int(x,16) for x in '7B9D7E38 37FE3678 82DF1934 275AB83D EDD0B6AB EF22A41D BF576F13 81AB004B 54D95CA4 4DD55768 01482F5A'.split()}

def chunks(d,e='<'):
    p=0;result=[]
    while p<len(d):
        tag=d[p:p+4];n=struct.unpack_from(e+'I',d,p+4)[0]
        assert n>=16 and p+n<=len(d),(p,n,len(d))
        result.append((p,d[p:p+n]));p+=n
    return result

def text_info(c,e='<'):
    n=struct.unpack_from(e+'I',c,24)[0];result=[]
    for j in range(n):
        h,p=struct.unpack_from(e+'2I',c,28+8*j);p+=16
        result.append((h,c[p:c.index(0,p)]))
    return result

def sprites(c,e='<'):
    n,nt,ptr=struct.unpack_from(e+'3I',c,16)
    return [struct.unpack_from(e+'I8fI',c,28+40*j) for j in range(n)]

def main():
    found=[]
    for p in (R/'dlc-probe/members').glob('*.bin'):
        d=p.read_bytes()
        try:ss=sections(d,'>')
        except (AssertionError,zlib.error,struct.error):continue
        for si in (0,3):
            for off,c in chunks(ss[si],'>'):
                if c[:4]==b'PTEX':
                    sp=sprites(c,'>'); hits=[s for s in sp if s[0] in KEYS]
                    if hits or p.stem=='FB9EB334':found.append({'member':p.stem,'section':si,'chunk_offset':off,'tag':'PTEX','hits':hits,'all_sprite_hashes':[f'{s[0]:08X}' for s in sp]})
                elif c[:4]==b'TEXT':
                    tt=text_info(c,'>');hits=[(f'{h:08X}',v.decode('utf-8',errors='replace')) for h,v in tt if h in KEYS]
                    if hits:found.append({'member':p.stem,'section':si,'chunk_offset':off,'tag':'TEXT','hits':hits,'all_text':[(f'{h:08X}',v.decode('utf-8',errors='replace')) for h,v in tt]})
    (O/'dlc-required-key-locations.json').write_text(json.dumps(found,indent=2))
    print(json.dumps(found,indent=2))

if __name__=='__main__':main()
