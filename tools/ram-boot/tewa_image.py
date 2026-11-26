#!/usr/bin/env python3
"""Offline TEWA ytcs/FIP/uImage inspector and RAM-only experimental packer.
Never opens device files, writes NAND, edits U-Boot, or overwrites source images.
"""
from __future__ import annotations
import argparse
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import zlib

BASE = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(BASE / 'reverse'))
from dtb_probe import read_blob
BL31_UUID = bytes.fromhex('47d4086d4cfe98469b952950cbbd5a00')
BL33_UUID = bytes.fromhex('d6d0eea7fcead54b97829934f234b6e4')
LIMIT = 0x800000
ENTRY = 0x80208000
IMAGE_BASE = 0x80400000
FDT_DEST = 0x82000000

class FormatError(ValueError):
    pass

def need(ok: bool, message: str) -> None:
    if not ok:
        raise FormatError(message)

def bounds(data: bytes, start: int, length: int) -> bytes:
    need(0 <= start <= len(data) and 0 <= length <= len(data)-start, 'out-of-bounds span')
    return data[start:start+length]

def u32(data: bytes, off: int, order: str = '<') -> int:
    return struct.unpack(order+'I', bounds(data, off, 4))[0]

def align(n: int, value: int = 16) -> int:
    return (n+value-1)//value*value

def digest(fip: bytes) -> bytes:
    return bytes(v ^ b'scty'[i % 4] for i,v in enumerate(hashlib.sha256(fip).digest()))

@dataclass(frozen=True)
class Entry:
    uuid: bytes
    offset: int
    size: int
    flags: int

@dataclass
class Parsed:
    raw: bytes
    fip: bytes
    entries: list[Entry]
    bl31: bytes
    bl33: bytes
    payload: bytes
    dtb: bytes
    overlays: bytes
    header: bytes
    overlay_count: int

def read_regular(path: Path, limit: int = 64*1024*1024) -> bytes:
    need(path.is_file() and not path.resolve().is_relative_to(Path('/dev')), 'regular input file required')
    need(path.stat().st_size <= limit, 'input exceeds size limit')
    return path.read_bytes()

def parse(raw: bytes) -> Parsed:
    need(bounds(raw,0,4)==b'ytcs', 'missing ytcs')
    hs,fs,ds=struct.unpack('<III', bounds(raw,4,12))
    need((hs,ds)==(16,32), 'unsupported ytcs header/digest size')
    fip=bounds(raw,hs+ds,fs)
    need(digest(fip)==bounds(raw,hs,ds), 'ytcs digest mismatch')
    need(u32(fip,0)==0xaa640001, 'FIP magic mismatch')
    entries=[]; cursor=16
    while True:
        rec=bounds(fip,cursor,40); uid=rec[:16]
        off,n,flags=struct.unpack('<QQQ',rec[16:])
        cursor+=40
        if not any(uid):
            need(n==0 and flags==0 and off<=len(fip), 'invalid FIP terminator')
            break
        need(len(entries)<16, 'too many FIP entries')
        bounds(fip,off,n)
        need(n>0, 'empty FIP entry')
        entries.append(Entry(uid,off,n,flags))
    need(len({e.uuid for e in entries})==len(entries),'duplicate FIP UUID')
    spans=sorted((e.offset,e.offset+e.size) for e in entries)
    last=cursor
    for start,end in spans:
        need(start>=last,'FIP overlaps ToC/another payload'); last=end
    by={e.uuid:bounds(fip,e.offset,e.size) for e in entries}
    need(BL31_UUID in by and BL33_UUID in by, 'missing BL31/BL33')
    b33=by[BL33_UUID]; header=bounds(b33,0,64)
    need(u32(header,0,'>')==0x27051956,'not a legacy uImage')
    h=bytearray(header); h[4:8]=bytes(4)
    need(zlib.crc32(h)==u32(header,4,'>'),'uImage header CRC mismatch')
    payload=bounds(b33,64,u32(header,12,'>'))
    need(zlib.crc32(payload)==u32(header,24,'>'),'uImage payload CRC mismatch')
    dtb,_,_=read_blob(b33,64+len(payload))
    overlay=b33[64+len(payload)+len(dtb):]
    need(u32(overlay,0,'>')==0xd7b7ab1e,'missing OEM overlay container')
    total,start,stride,count,header_size=struct.unpack('>5I',bounds(overlay,4,20))
    need(total==len(overlay) and (start,stride,header_size)==(32,32,32),'unsupported overlay layout')
    need(0<count<=64, 'bad overlay count')
    for i in range(count):
        n,at=struct.unpack('>II',bounds(overlay,start+i*stride,8))
        ov,_,_=read_blob(overlay,at)
        need(n==len(ov),'overlay FDT size mismatch')
    return Parsed(raw,fip,entries,by[BL31_UUID],b33,payload,dtb,overlay,header,count)

def image_contract(image: bytes) -> dict:
    need(len(image)>=64 and image[56:60]==b'ARM\x64','not an uncompressed ARM64 Image')
    text,mem,flags=struct.unpack_from('<QQQ',image,8)
    need(text in (0,0x80000),'only text_offset 0 and 0x80000 supported')
    need(mem>0 and not flags&1 and flags>>4==0,'unsupported Image size/endian/flags')
    target=IMAGE_BASE+text
    need((target-text)%0x200000==0,'Image placement violates 2 MiB base alignment')
    need(target+max(mem,len(image))<=FDT_DEST,'Image runtime memory overlaps DTB scratch')
    return dict(text_offset=hex(text),image_size=mem,flags=hex(flags),file_size=len(image),
                destination=hex(target),hardware_validated=False)

def patch_bl31(b: bytes) -> bytes:
    expected={0x4c34:0x52803a60,0x4c38:0xb9001260,0x4c5c:0xb9000460,
              0x4c60:0xd0000020,0x4c68:0xa8c27bfd,0x4c6c:0x17ffffde}
    for off,word in expected.items():
        need(u32(b,off)==word,f'BL31 instruction guard failed at {off:#x}')
    need(u32(b,0x4c64) in (0x91117800,0x91112c00),'unknown BL31 A/B variant')
    out=bytearray(b)
    for off,word in {0x4c60:0x528078a0,0x4c64:0xb9001060,0x4c6c:0xd65f03c0}.items():
        struct.pack_into('<I',out,off,word)
    return bytes(out)

def summary(p: Parsed) -> dict:
    h=p.header
    return {'sha256':hashlib.sha256(p.raw).hexdigest(),'ytcs_valid':True,'fip_size':len(p.fip),
            'bl31_size':len(p.bl31),'bl31_sha256':hashlib.sha256(p.bl31).hexdigest(),
            'bl33_size':len(p.bl33),'uimage_payload_bytes':len(p.payload),
            'load':hex(u32(h,16,'>')),'entry':hex(u32(h,20,'>')),
            'arch':h[29],'compression':h[31],'dtb_size':len(p.dtb),
            'overlay_size':len(p.overlays),'overlay_count':p.overlay_count,
            'fip_entries':[{'uuid':e.uuid.hex(),'offset':hex(e.offset),'size':e.size} for e in p.entries]}

def atomic_output(path: Path, data: bytes, inputs: tuple[Path,...]=()) -> None:
    resolved=path.resolve()
    need(not resolved.is_relative_to(Path('/dev')), 'device output prohibited')
    need(not path.name.endswith(('.nand.bin','.dd.bin')), 'backup filename prohibited as output')
    need(resolved not in {p.resolve() for p in inputs}, 'cannot overwrite an input')
    path.parent.mkdir(parents=True,exist_ok=True)
    fd,tmp=tempfile.mkstemp(prefix='.tewa-',dir=path.parent)
    try:
        with os.fdopen(fd,'wb') as out:
            out.write(data); out.flush(); os.fsync(out.fileno())
        os.replace(tmp,path)
    finally:
        if os.path.exists(tmp): os.unlink(tmp)

def build(original: Parsed, image: bytes, stub: bytes, kind: str) -> tuple[bytes,dict]:
    contract=image_contract(image)
    if kind=='linux':
        from bringup_overlays import sanitize
        cleaned,_=sanitize(original.dtb,original.overlays)
        need(cleaned==original.overlays,
             'Linux DTB and overlays are not a matching sanitized bringup pair')
    need(stub and len(stub)%8==0 and len(stub)<8192,'bad trampoline')
    need(original.header[29:32]==bytes((2,2,0)),'unexpected original image arch/type/compression')
    code=stub+image
    code+=bytes(align(len(code),8)-len(code))
    need(ENTRY+len(code)+0x20000<FDT_DEST,'OEM DTB merge workspace overlaps scratch')
    h=bytearray(original.header)
    struct.pack_into('>I',h,12,len(code))
    struct.pack_into('>I',h,20,ENTRY)
    struct.pack_into('>I',h,24,zlib.crc32(code))
    h[32:64]=('TEWA ARM64 '+kind.upper())[:31].encode().ljust(32,b'\0')
    h[4:8]=bytes(4); struct.pack_into('>I',h,4,zlib.crc32(h))
    b31=patch_bl31(original.bl31)
    b33=bytes(h)+code+original.dtb+original.overlays
    need([e.uuid for e in original.entries]==[BL31_UUID,BL33_UUID], 'unexpected FIP entry ordering')
    off31=align(16+3*40); off33=align(off31+len(b31)); end=align(off33+len(b33))
    f=bytearray(end); f[:16]=original.fip[:16]
    for idx,(old,off,data) in enumerate(zip(original.entries,(off31,off33),(b31,b33))):
        at=16+idx*40; f[at:at+16]=old.uuid
        struct.pack_into('<QQQ',f,at+16,off,len(data),old.flags)
        f[off:off+len(data)]=data
    struct.pack_into('<QQQ',f,16+2*40+16,end,0,0)
    result=b'ytcs'+struct.pack('<III',16,len(f),32)+digest(f)+bytes(f)
    need(len(result)<=LIMIT,'candidate exceeds existing 8 MiB kernel partition')
    check=parse(result)
    need(check.payload==code,'post-build payload verification failed')
    report=summary(check)|{'kind':kind,'arm64':contract,'ram_only':True,
        'flash_authorized':False,'on_device_tested':False,
        'nand_remaining_bytes':LIMIT-len(result),
        'warnings':['No validated RAM injection trigger','DTB scratch 0x82000000 unverified on device',
                    'Original DTB peripheral bindings are not a mainline board port',
                    'A correct checksum is not evidence of bootability']}
    return result,report

def main() -> int:
    ap=argparse.ArgumentParser(description=__doc__)
    sub=ap.add_subparsers(dest='cmd',required=True)
    i=sub.add_parser('inspect'); i.add_argument('image',type=Path)
    b=sub.add_parser('pack'); b.add_argument('--original',type=Path,required=True)
    b.add_argument('--image',type=Path,required=True); b.add_argument('--stub',type=Path,required=True)
    b.add_argument('--dtb',type=Path); b.add_argument('--overlays',type=Path); b.add_argument('--output',type=Path,required=True); b.add_argument('--kind',choices=['diagnostic','linux'],required=True)
    args=ap.parse_args()
    try:
        if args.cmd=='inspect':
            print(json.dumps(summary(parse(read_regular(args.image))),indent=2))
        else:
            raw=read_regular(args.original)
            original=parse(raw)
            if args.dtb:
                dtb=read_regular(args.dtb)
                check,_,_=read_blob(dtb,0)
                need(len(check)==len(dtb) and len(dtb)<=0x20000,'bad replacement DTB')
                original.dtb=dtb
            if args.overlays:
                from bringup_overlays import entries
                replacement=read_regular(args.overlays);entries(replacement)
                original.overlays=replacement
            out,report=build(original,read_regular(args.image),read_regular(args.stub),args.kind)
            atomic_output(args.output,out,tuple(p for p in (args.original,args.image,args.stub,args.dtb,args.overlays) if p))
            atomic_output(args.output.with_suffix('.json'),(json.dumps(report,indent=2)+'\n').encode())
            print(json.dumps(report,indent=2))
    except (FormatError,OSError,struct.error) as exc:
        print('ERROR: '+str(exc),file=sys.stderr); return 1
    return 0
if __name__=='__main__':
    raise SystemExit(main())
