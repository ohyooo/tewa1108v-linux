#!/usr/bin/env python3
"""Patch ONLY Debian ARMEL initramfs and /chosen initrd-end in the pinned Stage161 FIP.

Original Stage161 pair must match byte-exact hardware-observed hashes.
Static ARM EABI /usr/sbin/reboot is the only new executable; usr/bin/reboot
is a symlink. Kernel, modules, BL31, trampoline, overlays and watchdog FDT
properties are preserved unchanged.
"""
import argparse
import gzip
import hashlib
import json
import stat
import struct
import zlib
from pathlib import Path
from prepare_stage3 import check_arm_elf, parse_newc, newc_record
from tewa_image import parse
from fip_patches import patch_fip_initrd, STUB_SIZE
from dtb_probe import read_blob

BASE_ROOT = '747bd922e619fccd2a3b1c86fe10364c4575c5e9e753f112d98288454cf008fb'
BASE_FIP = '5109c0805934932647f6a1bfb7a5f06e7b216dcc22291fa44e1e10aa36a7dea7'
EXPECTED_IMAGE = '40094da02b687c9d074c7c21e81ab9246655a1c2e6cdf7970a85f779bb31b257'

def sha(blob): return hashlib.sha256(blob).hexdigest()
def meta(blob): return {'bytes':len(blob),'sha256':sha(blob),'crc32':f'{zlib.crc32(blob):08x}'}

def build(src_initrd, src_fip, reboot_binary, dst_initrd, dst_fip):
    old_root = Path(src_initrd).read_bytes()
    old_fip = Path(src_fip).read_bytes()
    binary = check_arm_elf(Path(reboot_binary))
    if sha(old_root)!=BASE_ROOT or sha(old_fip)!=BASE_FIP:
        raise ValueError('Stage161 base pair identity mismatch: refusing repack')
    raw = gzip.decompress(old_root)
    entries = parse_newc(raw)
    names = {e[2] for e in entries}
    assert {'usr/sbin','usr/bin','usr/bin/bash','init','tewa/diagnose.sh','TRAILER!!!'} <= names
    assert 'usr/sbin/reboot' not in names and 'usr/bin/reboot' not in names
    chunks=[]
    ino=max(e[3][0] for e in entries)+1
    for start, end, name, fields, _ in entries:
        if name=='TRAILER!!!':
            chunks.append(newc_record('usr/sbin/reboot',binary,ino,stat.S_IFREG|0o755));ino+=1
            chunks.append(newc_record('usr/bin/reboot',b'../sbin/reboot',ino,stat.S_IFLNK|0o777));ino+=1
            chunks.append(newc_record('TRAILER!!!',b'',ino,0))
        else:
            chunks.append(raw[start:end])
    result=gzip.compress(b''.join(chunks),compresslevel=6,mtime=0)
    decoded=gzip.decompress(result)
    reread={name:(fields,decoded[data:data+fields[6]]) for _,_,name,fields,data in parse_newc(decoded)}
    assert len(reread)==len(entries)+2
    assert reread['usr/sbin/reboot'][1]==binary
    assert reread['usr/bin/reboot'][1]==b'../sbin/reboot'
    assert stat.S_ISREG(reread['usr/sbin/reboot'][0][1])
    assert stat.S_ISLNK(reread['usr/bin/reboot'][0][1])
    assert len(result)<0x03800000
    updated_fip=patch_fip_initrd(old_fip,len(result))
    a,b=parse(old_fip),parse(updated_fip)
    assert b.bl31==a.bl31 and b.header==a.header and b.payload==a.payload and b.overlays==a.overlays
    assert sha(b.payload[STUB_SIZE:STUB_SIZE+7931912])==EXPECTED_IMAGE
    _,_,before=read_blob(a.dtb,0)
    _,_,after=read_blob(b.dtb,0)
    assert set(before)==set(after)
    for path in before:
        old, newer=dict(before[path]),dict(after[path])
        if path=='/chosen':
            assert newer.pop('linux,initrd-end')==struct.pack('>I',0x88000000+len(result))
            old.pop('linux,initrd-end')
        assert newer==old,('Unexpected DTB change',path)
    assert len(updated_fip)<=0x800000
    Path(dst_initrd).write_bytes(result)
    Path(dst_fip).write_bytes(updated_fip)
    return {'initrd':meta(result),'carrier':meta(updated_fip),'reboot':meta(binary), 'initrd_end':hex(0x88000000+len(result)), 'dtb':sha(b.dtb)}

if __name__=='__main__':
    p=argparse.ArgumentParser()
    p.add_argument('--old-initrd',required=True)
    p.add_argument('--old-carrier',required=True)
    p.add_argument('--reboot-elf',required=True)
    p.add_argument('--output-initrd',required=True)
    p.add_argument('--output-carrier',required=True)
    args=p.parse_args()
    print(json.dumps(build(args.old_initrd,args.old_carrier,args.reboot_elf,args.output_initrd,args.output_carrier),indent=2))
