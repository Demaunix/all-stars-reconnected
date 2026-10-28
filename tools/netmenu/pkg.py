"""Script package helpers vendored from Codex's work for this user (logic unchanged, reindented):
read_package  metal-sonic\\separate-slot\\build_scripts.py (sha256 0c6685f6...)
name_hash     metal-sonic\\sumo_hash.py (sha256 76698087...)
align_metadata egg-hangar-port\\smos_alignment.py
"""
import struct

from smos_reloc import u32


def read_package(d):
    """Modules of a smospkg2 package. 'data' runs to the next module start, padding included."""
    assert d[:8]==b'smospkg2' and u32(d,8)==32
    result=[]
    for i in range(u32(d,12)):
        p=40+32*i
        np=p+20+struct.unpack_from('<i',d,p+20)[0]
        bp=p+28+struct.unpack_from('<i',d,p+28)[0]
        assert p+24+struct.unpack_from('<i',d,p+24)[0]==0
        name=d[np:d.index(0,np)].decode()
        result.append(dict(name=name,hash=u32(d,p+16),start=bp,entry=d[p:p+32]))
    starts=sorted({r['start'] for r in result})+[len(d)]
    for i,r in enumerate(result):
        end=next(p for p in starts if p>r['start'])
        r['data']=d[r['start']:end]
        assert r['data'][:8]==b'smosbinL'
    return result


def name_hash(s,seed=0x04c11db7):
    """Game string identifier: Jenkins lookup2, seed 0x04c11db7 (exe 0x4947e0)."""
    if isinstance(s,str):s=s.encode()
    a=b=0x9e3779b9;c=seed;mask=0xffffffff
    def mix(a,b,c):
        a=((a-b-c)&mask)^(c>>13);b=((b-c-a)&mask)^((a<<8)&mask);c=((c-a-b)&mask)^(b>>13)
        a=((a-b-c)&mask)^(c>>12);b=((b-c-a)&mask)^((a<<16)&mask);c=((c-a-b)&mask)^(b>>5)
        a=((a-b-c)&mask)^(c>>3);b=((b-c-a)&mask)^((a<<10)&mask);c=((c-a-b)&mask)^(b>>15)
        return a,b,c
    n=len(s);p=0
    while n-p>=12:
        a=(a+int.from_bytes(s[p:p+4],'little'))&mask;b=(b+int.from_bytes(s[p+4:p+8],'little'))&mask;c=(c+int.from_bytes(s[p+8:p+12],'little'))&mask;a,b,c=mix(a,b,c);p+=12
    c=(c+n)&mask;t=s[p:]
    a=(a+int.from_bytes(t[:4],'little'))&mask;b=(b+int.from_bytes(t[4:8],'little'))&mask;c=(c+(int.from_bytes(t[8:],'little')<<8))&mask
    return mix(a,b,c)[2]


def align_metadata(data):
    """Keep SMOS metadata 4-aligned after changing variable-length bytecode.

    Array/dictionary relocation uses the pointer low bit as its serialized flag,
    so an odd absolute address asserts in native 0048BCB0.
    """
    u=lambda p:struct.unpack_from('<I',data,p)[0]
    assert data[:8]==b'smosbinL'
    start=u(20)
    assert u(12)+u(16)<=start
    padding=(-start)%4
    if not padding:return data
    size=u(8)
    out=bytearray(data[:start]+bytes(padding)+data[start:size])
    struct.pack_into('<I',out,8,size+padding)
    for field in (20,24,28,44,52):
        old=u(field)
        assert old==0 or start<=old<size
        if old:struct.pack_into('<I',out,field,old+padding)
    for j in range(u(48)):
        position=u(44)+4*j
        target=u(position)
        assert start<=target<size
        struct.pack_into('<I',out,position+padding,target+padding)
    out+=bytes((-len(out))%4)
    assert out[u(12):u(12)+u(16)]==data[u(12):u(12)+u(16)]
    return bytes(out)


assert name_hash('none')==0x987b66c0
