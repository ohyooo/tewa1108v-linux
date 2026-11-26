#!/usr/bin/env python3
"""Preserve the proven OEM FIP chain; patch a single PIE FDT boolean/ramdisk end."""
import hashlib
import struct
import zlib
from tewa_image import parse, digest, align, BL31_UUID, BL33_UUID
from dtb_probe import read_blob

STUB_SIZE=456
RAM_ADDR=0x88000000
RAM_MAX_END=0x8b800000
FIP_MAX=0x800000


def sha(x):
    return hashlib.sha256(x).hexdigest()


def require(ok,msg):
    if not ok:
        raise ValueError(msg)


def _find_fdt_node_prop(blob,node,prop=None):
    h=struct.unpack_from('>10I',blob)
    ss,st=h[2],h[9]
    strings=blob[h[3]:h[3]+h[8]]
    pos=ss;stack=[]; node_pos=None;prop_pos=None
    while pos<ss+st:
        token=struct.unpack_from('>I',blob,pos)[0];pos+=4
        if token==1:
            zero=blob.index(b'\0',pos,ss+st)
            stack.append(blob[pos:zero].decode('ascii'))
            pos=(zero+4)&~3
            path='/'+'/'.join(x for x in stack if x)
            if path==node:
                node_pos=pos
        elif token==2:
            stack.pop()
        elif token==3:
            size,off=struct.unpack_from('>II',blob,pos);pos+=8
            end=strings.find(b'\0',off)
            require(end>=0,'invalid FDT strings')
            name=strings[off:end].decode('ascii')
            path='/'+'/'.join(x for x in stack if x)
            if path==node and name==prop:
                prop_pos=(pos,size)
            pos=(pos+size+3)&~3
        elif token==4:
            pass
        elif token==9:
            break
        else:
            raise ValueError(f'FDT token {token} invalid')
    require(node_pos is not None,'FDT node missing '+node)
    return node_pos,prop_pos


def add_pie_optin(blob):
    blob,fields,before=read_blob(blob,0)
    target='/pie@12080000';prop='hsan,experimental-dma-enable'
    require(before[target].get('status')==b'okay\0','PIE node not enabled')
    require(prop not in before[target],'PIE DMA property already present')
    at,_=_find_fdt_node_prop(blob,target)
    h=struct.unpack_from('>10I',blob,0)
    offstr,sizestr,sizestruct=h[3],h[8],h[9]
    strings=blob[offstr:offstr+sizestr]
    name=prop.encode()+b'\0'
    idx=strings.find(name)
    if idx<0:
        idx=sizestr;extra=name
    else:
        extra=b''
    tok=struct.pack('>III',3,0,idx)
    out=bytearray(blob[:at]+tok+blob[at:offstr+sizestr]+extra+blob[offstr+sizestr:])
    for off,val in [(4,len(out)),(12,offstr+len(tok)),(32,sizestr+len(extra)),(36,sizestruct+len(tok))]:
        struct.pack_into('>I',out,off,val)
    b,_,after=read_blob(bytes(out),0)
    require(after[target][prop]==b'','PIE FDT bool missing after patch')
    for path in before:
        other=dict(after[path]);old=before[path]
        if path==target:
            other.pop(prop,None)
        require(other==old,'unexpected FDT change: '+path)
    require(set(before)==set(after),'unexpected FDT node change')
    return b


def edit_initrd_end(blob,compressed_size):
    require(0<compressed_size and RAM_ADDR+compressed_size<RAM_MAX_END,'initrd outside bound')
    blob,_,before=read_blob(blob,0)
    require(before['/chosen']['linux,initrd-start']==struct.pack('>I',RAM_ADDR),'wrong initrd start')
    at,found=_find_fdt_node_prop(blob,'/chosen','linux,initrd-end')
    require(found is not None and found[1]==4,'initrd-end property is missing or not u32')
    output=bytearray(blob)
    struct.pack_into('>I',output,found[0],RAM_ADDR+compressed_size)
    _,_,after=read_blob(bytes(output),0)
    for path in before:
        old=dict(before[path]);new=dict(after[path]);
        if path=='/chosen':
            old.pop('linux,initrd-end',None);new.pop('linux,initrd-end',None)
        require(old==new,'unexpected FDT property changed '+path)
    require(after['/chosen']['linux,initrd-end']==struct.pack('>I',RAM_ADDR+compressed_size),'initrd end mismatch')
    return bytes(output)


def build_from_stage2(base_bytes,new_image,expected_stage2_sha):
    require(sha(base_bytes)==expected_stage2_sha,'untrusted stage2 boot FIP')
    base=parse(base_bytes)
    require([e.uuid for e in base.entries]==[BL31_UUID,BL33_UUID],'FIP ToC unexpected')
    require(new_image[56:60]==b'ARM\x64','not AArch64 Image')
    require(len(new_image)<8*1024*1024,'Image oversized')
    payload=base.payload[:STUB_SIZE]+new_image
    payload+=bytes((-len(payload))%8)
    header=bytearray(base.header)
    struct.pack_into('>I',header,12,len(payload))
    struct.pack_into('>I',header,24,zlib.crc32(payload))
    struct.pack_into('>I',header,4,0)
    struct.pack_into('>I',header,4,zlib.crc32(header))
    fdt=add_pie_optin(base.dtb)
    fdt=edit_initrd_end(fdt,48881149)
    out=rebuild_fip(base,bytes(header),payload,fdt)
    return out


def rebuild_fip(base,header,payload,fdt):
    bl33=header+payload+fdt+base.overlays
    o31=align(16+3*40);o33=align(o31+len(base.bl31));end=align(o33+len(bl33))
    fip=bytearray(end);fip[:16]=base.fip[:16]
    for i,(entry,off,piece) in enumerate(zip(base.entries,(o31,o33),(base.bl31,bl33))):
        at=16+i*40
        fip[at:at+16]=entry.uuid
        struct.pack_into('<QQQ',fip,at+16,off,len(piece),entry.flags)
        fip[off:off+len(piece)]=piece
    struct.pack_into('<QQQ',fip,16+2*40+16,end,0,0)
    blob=b'ytcs'+struct.pack('<III',16,len(fip),32)+digest(bytes(fip))+bytes(fip)
    require(len(blob)<=FIP_MAX,'OEM 8MiB FIP limit exceeded')
    check=parse(blob)
    for prop in ('bl31','overlays'):
        require(getattr(check,prop)==getattr(base,prop),'boot component changed '+prop)
    require(check.payload[:STUB_SIZE]==base.payload[:STUB_SIZE],'boot trampoline changed')
    require(check.dtb==fdt,'FDT serialization mismatch')
    require(check.header==header and check.payload==payload,'kernel or header mismatch')
    return blob


def patch_fip_initrd(base_bytes,compressed_size):
    before=parse(base_bytes)
    newdtb=edit_initrd_end(before.dtb,compressed_size)
    out=rebuild_fip(before,before.header,before.payload,newdtb)
    return out
