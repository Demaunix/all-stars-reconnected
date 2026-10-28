"""Native Xbox-to-PC UI texture conversion. Keeps original texels and alpha."""
from pathlib import Path
import struct,zlib,json
def sections(d,e):
 h=struct.unpack_from(e+'18I',d);out=[]
 for i in range(6):
  p,n,c=h[i*3:i*3+3];b=zlib.decompress(d[p:p+c]) if c else b'';assert len(b)==n;out.append(b)
 return out
def address(x,y,pitch,log):
 q=((((y>>5)*(pitch>>5)+(x>>5))<<6)|(((y>>1)&7)<<3)|(x&7))<<log
 return ((y&1)<<4)|((((x>>3)&3)^(((y>>3)&1)<<1))<<6)|(((y>>4)&1)<<11)|(q&15)|(((q>>4)&1)<<5)|(((q>>5)&7)<<8)|((q>>8)<<12)
def convert(info,base,gpu,index):
 u=lambda p:struct.unpack_from('>I',info,p)[0]
 desc,off=struct.unpack_from('>2I',info,base+16+u(base+24)+8*index);desc+=base+16
 fetch=struct.unpack_from('>6I',info,desc+28);w=(fetch[2]&8191)+1;h=((fetch[2]>>13)&8191)+1;fmt=fetch[1]&63;endian=(fetch[1]>>6)&3
 assert fmt in (6,18,19,20) and (fetch[5]>>9)&3==1
 bs=1 if fmt==6 else 4;nx=(w+bs-1)//bs;ny=(h+bs-1)//bs;bpp=4 if fmt==6 else 8 if fmt==18 else 16;log=bpp.bit_length()-1
 pitch=max(32,(((((fetch[0]>>22)&511)*32+bs-1)//bs)+31)&~31);assert pitch>=nx
 result=bytearray(nx*ny*bpp);seen=set()
 for y in range(ny):
  for x in range(nx):
   p=off+((fetch[1]>>12)<<12)+(address(x,y,pitch,log) if fetch[0]>>31 else (y*pitch+x)*bpp)
   assert p not in seen and p+bpp<=len(gpu);seen.add(p);s=gpu[p:p+bpp]
   if endian==1:s=b''.join(s[i:i+2][::-1] for i in range(0,bpp,2))
   elif endian==2:s=b''.join(s[i:i+4][::-1] for i in range(0,bpp,4))
   else:assert endian==0
   result[(y*nx+x)*bpp:(y*nx+x+1)*bpp]=s
 header=bytearray(128);header[:4]=b'DDS ';struct.pack_into('<7I',header,4,124,0x100f if fmt==6 else 0x81007,h,w,w*4 if fmt==6 else len(result),0,1)
 if fmt==6:struct.pack_into('<8I',header,76,32,0x41,0,32,0xff0000,0xff00,0xff,0xff000000)
 else:struct.pack_into('<II4s',header,76,32,4,{18:b'DXT1',19:b'DXT3',20:b'DXT5'}[fmt])
 struct.pack_into('<I',header,108,0x1000)
 return bytes(header)+result,dict(width=w,height=h,format=fmt,endian=endian,source_offset=off,fetch=fetch)
if __name__=='__main__':
 O=Path('work/metal-sonic/identity');ss=sections(Path('work/metal-sonic/dlc-probe/members/FB9EB334.bin').read_bytes(),'>');out=[]
 for i in range(3):
  raw,meta=convert(ss[3],0,ss[5],i);(O/f'metal-ui-{i}.dds').write_bytes(raw);out.append(meta)
 (O/'metal-ui-conversion.json').write_text(json.dumps(out,indent=2));print(out)
