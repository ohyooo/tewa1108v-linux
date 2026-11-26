#!/usr/bin/env python3
"""Host-only reproducible packaging: no device operations or auth changes."""
from pathlib import Path
import argparse, gzip, hashlib, json, stat, struct, zlib
from prepare_stage3 import parse_newc, newc_record, check_arm_elf
from fip_patches import patch_fip_initrd
from tewa_image import parse
from dtb_probe import read_blob

def sha(b): return hashlib.sha256(b).hexdigest()
def meta(b): return {'bytes': len(b), 'sha256': sha(b), 'crc32': f'{zlib.crc32(b):08x}'}
ORDER=('hsan-mdio','hsan-lsw-csr','hsan-lsw-platform','hsan-lsw-dp','hsan-lsw-pfe','hsan-gemac-lan1','hsan-net-v300')
# Exact pin file supplied separately; generated from the current handoff.
BASE_ROOTFS='8957c7a442d231dbb6c06984621b4e4612d4ef11e41d751262d985be48329199'
BASE_FIP='b61916ee64682895497e43bf2317746d6345c09ce60b9cf0db9d03b38132e3ab'
IMAGE_SHA='d55149444eb63e86f1ff94b8a1cc2a90aece3a3d47d269958b13fcf8dcd67eb2'
SRC=Path(__file__).resolve().parent

def build(rootfs,carrier,image,modules,out):
    rootfs,carrier,image,modules,out=map(Path,(rootfs,carrier,image,modules,out))
    root=rootfs.read_bytes(); orig=carrier.read_bytes(); img=image.read_bytes()
    assert sha(root)==BASE_ROOTFS and sha(orig)==BASE_FIP and sha(img)==IMAGE_SHA
    base=parse(orig); assert base.payload[456:456+len(img)]==img
    _,_,nodes=read_blob(base.dtb,0)
    assert not any(k.startswith('hsan,experimental-') for v in nodes.values() for k in v)
    checks={'/':('compatible',),'/pie@12080000':('compatible','status','reg','hwver','tx_ring','rx_ring','gro_ring','interrupts'),'/gemac@14400000':('compatible','status','reg','port','phy-handle','phy-mode'),'/mdio0':('compatible','status','reg','hwver'),'/mdio0/ethernet-phy@3':('compatible','reg','phandle'),'/lsw_dp':('status','reg'),'/lsw_pfe':('status','reg')}
    dt_check=''.join(f'{sha(nodes[node][prop])}  /sys/firmware/devicetree/base{node.rstrip("/")}/{prop}\n' for node,props in checks.items() for prop in props)
    raw=gzip.decompress(root); records=parse_newc(raw); names={x[2] for x in records}
    assert len(names)==len(records)
    assert {'init','usr/bin/bash','usr/bin/dash','usr/bin/apt','usr/bin/dpkg','usr/bin/sha256sum','usr/bin/find','usr/bin/grep','usr/bin/dmesg','usr/bin/readlink','usr/bin/uname','usr/lib/os-release','usr/lib/ld-linux.so.3','dev/console','proc','sys','run','tmp'} <= names
    assert not any(n=='tewa' or n.startswith('tewa/') for n in names)
    pin_data=json.loads((SRC/'module-pins.json').read_text())
    blobs={n:(modules/(n+'.ko')).read_bytes() for n in ORDER}
    for n,b in blobs.items():
        assert meta(b)==pin_data[n]
        assert b[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',b,18)[0]==183 and b'vermagic=7.3.5 SMP preempt aarch64\0' in b
    pid1=check_arm_elf(SRC/'pid1.elf'); drv=check_arm_elf(SRC/'driver-test.elf'); gate=check_arm_elf(SRC/'gate-test.elf')
    extras={'tewa':(b'',stat.S_IFDIR|0o755),'tewa/modules':(b'',stat.S_IFDIR|0o755),'tewa/diagnose.sh':((SRC/'diagnose.sh').read_bytes(),stat.S_IFREG|0o755),'tewa/driver-test':(drv,stat.S_IFREG|0o755),'tewa/gate-test':(gate,stat.S_IFREG|0o755),'tewa/module-sha256.txt':(''.join(f'{sha(b)}  /tewa/modules/{n}.ko\n' for n,b in blobs.items()).encode(),stat.S_IFREG|0o444),'tewa/live-dt-sha256.txt':(dt_check.encode(),stat.S_IFREG|0o444)}
    extras.update({f'tewa/modules/{n}.ko':(b,stat.S_IFREG|0o444) for n,b in blobs.items()})
    chunks=[]; ino=max(r[3][0] for r in records)+1
    for a,z,n,f,d in records:
        if n=='init': chunks.append(newc_record(n,pid1,f[0],stat.S_IFREG|0o755))
        elif n=='TRAILER!!!':
            for name,(b,mode) in extras.items():
                chunks.append(newc_record(name,b,ino,mode)); ino+=1
            chunks.append(newc_record(n,b'',ino,0))
        else: chunks.append(raw[a:z])
    cpio=b''.join(chunks); initrd=gzip.compress(cpio,compresslevel=6,mtime=0)
    assert len(initrd)<0x03800000
    reread={n:(f,cpio[d:d+f[6]]) for a,z,n,f,d in parse_newc(gzip.decompress(initrd))}
    assert len(reread)==len(names)+len(extras)
    assert reread['init'][1]==pid1
    for n,(b,mode) in extras.items(): assert reread[n][1]==b and reread[n][0][1]==mode
    fip=patch_fip_initrd(orig,len(initrd)); after=parse(fip);_,_,an=read_blob(after.dtb,0)
    assert after.bl31==base.bl31 and after.payload==base.payload and after.header==base.header and after.overlays==base.overlays
    assert an['/chosen']['linux,initrd-start']==struct.pack('>I',0x88000000) and an['/chosen']['linux,initrd-end']==struct.pack('>I',0x88000000+len(initrd))
    assert all(an[n]==v for n,v in nodes.items() if n!='/chosen')
    out.mkdir(parents=True,exist_ok=True)
    cpio_name='stage160-debian13-armel-lan1.cpio.gz'; fip_name='stage160-debian13-armel-lan1.ramimage'
    (out/cpio_name).write_bytes(initrd);(out/fip_name).write_bytes(fip)
    pair={'stage':160,'purpose':'Debian ARMEL interactive shell and normal diagnostic module probe, no production trusted-receiver claim','initrd':{'name':cpio_name,'address':'0x88000000',**meta(initrd)},'carrier':{'name':fip_name,'address':'0x86000000',**meta(fip)},'base_rootfs':meta(root),'base_carrier':meta(orig),'image':meta(img),'pid1':meta(pid1),'driver_test':meta(drv),'gate_test':meta(gate),'modules':pin_data,'stored_dtb_sha256':sha(after.dtb),'initrd_end':hex(0x88000000+len(initrd)),'boot_components':{'bl31':sha(after.bl31),'stub':sha(after.payload[:456]),'overlay':sha(after.overlays)},'live_dt_profile':'critical property byte hashes; full effective DT is separately reported; no experimental gates','experimental_dma_enable':False,'board_test':'NOT_RUN'}
    (out/'PAIR.json').write_text(json.dumps(pair,indent=2)+'\n')
    print(json.dumps({k:pair[k] for k in ('initrd','carrier','pid1','initrd_end')},indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--rootfs',required=True);p.add_argument('--carrier',required=True);p.add_argument('--image',required=True);p.add_argument('--modules',required=True);p.add_argument('--out',required=True);a=p.parse_args();build(a.rootfs,a.carrier,a.image,a.modules,a.out)
