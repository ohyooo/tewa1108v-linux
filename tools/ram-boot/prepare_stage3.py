#!/usr/bin/env python3
"""Windows-native, offline Debian armel initramfs network overlay and FIP patch.

Accepts the original target-verified Debian armel initramfs OR a regenerated
initramfs with a digest-pinned official Debian 13 OCI provenance chain.
Does not execute ARM code on the PC or modify cached rootfs inputs.
"""
import argparse
import gzip
import hashlib
import json
import stat
import struct
import zlib
from pathlib import Path
from fip_patches import patch_fip_initrd, require, sha
from tewa_image import parse
from dtb_probe import read_blob

RAM_START=0x88000000
BASE_RAM_SIZE=48881149
BASE_RAM_CRC='10a83699'
INIT_PATH='init'
DHCP_PATH='usr/sbin/tewa-dhcp'
IMAGE_NAME='tewa-LAN1-PIE-V300-DATAPATH-EXPERIMENT-RAMONLY.ramimage'
ROOT_OUTPUT='tewa-debian13-armel-LAN1-DHCP-RAMONLY.cpio.gz'
FIP_OUTPUT='tewa-LAN1-PIE-V300-DHCP-RAMONLY.ramimage'


def align(n):
    return (n+3)&~3


def parse_newc(raw):
    at=0
    entries=[]
    while at<len(raw):
        if raw[at:at+6] not in (b'070701',b'070702'):
            raise ValueError('invalid newc entry at byte '+str(at))
        h=raw[at:at+110]
        require(len(h)==110,'short newc header')
        fields=[int(h[6+i*8:14+i*8],16) for i in range(13)]
        count,size=fields[11],fields[6]
        require(0<count<=4096 and 0<=size<512*1024*1024,'invalid newc filesize/name length')
        name_start=at+110
        name_bytes=raw[name_start:name_start+count]
        require(len(name_bytes)==count and name_bytes[-1:]==b'\0','unterminated newc filename')
        name=name_bytes[:-1].decode('utf8')
        data_start=align(name_start+count)
        nextpos=align(data_start+size)
        require(nextpos<=len(raw),'truncated newc data')
        entries.append((at,nextpos,name,fields,data_start))
        at=nextpos
        if name=='TRAILER!!!':
            require(all(c==0 for c in raw[at:]),'newc has nonzero content after trailer')
            return entries
    raise ValueError('newc missing trailer')


def newc_record(name,content,ino,mode=stat.S_IFREG|0o755):
    n=name.encode()+b'\0'
    values=[ino,mode,0,0,1,0,len(content),0,0,0,0,len(n),0]
    h=b'070701'+b''.join(f'{value & 0xffffffff:08x}'.encode() for value in values)
    head=h+n+b'\0'*(-len(h+n)%4)
    return head+content+b'\0'*(-len(content)%4)


def check_arm_elf(path):
    b=path.read_bytes()
    require(len(b)>512 and b[:6]==b'\x7fELF\x01\x01','expected ELF32 little endian')
    require(struct.unpack_from('<H',b,18)[0]==40,'not ELF32 ARM')
    require(struct.unpack_from('<I',b,36)[0]&0x600==0x200,'expected ARM EABI soft-float')
    require(b'libc.so' not in b and b'/lib/ld-linux' not in b,'not static freestanding ELF')
    return b


def inject_cached_cpio(cached, init_elf, dhcp_elf):
    # Caller validates either the hardware-PASS CRC, or the official OCI
    # download SHA256 + independently created initramfs SHA256 audit chain.
    require(0<len(cached)<0x03800000,'base initramfs exceeds RAM size limit')
    require(cached.startswith(b'\x1f\x8b\x08'),'base rootfs is not gzip')
    raw=gzip.decompress(cached)
    records=parse_newc(raw)
    names={r[2] for r in records}
    for key in ('init','usr/bin/apt','usr/bin/dpkg','usr/bin/dash','var/lib/dpkg/status'):
        require(key in names,'not the Debian armel minbase: '+key)
    require(DHCP_PATH not in names,'network probe already exists in rootfs')
    require(sum(r[2]=='init' for r in records)==1,'ambiguous Debian PID1')
    pos=0;patched=[]
    for start,end,name,fields,data_start in records:
        require(start==pos,'newc record discontinuity')
        if name=='init':
            old=raw[data_start:data_start+fields[6]]
            require(old[:4]==b'\x7fELF' and old[4]==1,'existing init is not static ARM32 ELF')
            patched.append(newc_record('init',init_elf,fields[0],mode=stat.S_IFREG|0o755))
        elif name=='TRAILER!!!':
            patched.append(newc_record(DHCP_PATH,dhcp_elf,fields[0]+10000))
            patched.append(newc_record('TRAILER!!!',b'',fields[0]+10001,mode=0))
        else:
            patched.append(raw[start:end])
        pos=end
    cpio=b''.join(patched)
    require('usr/bin/dpkg' in names and 'usr/bin/apt' in names,'package tools not present')
    output=gzip.compress(cpio,compresslevel=6,mtime=0)
    require(len(output)<0x03800000,'rootfs exceeds 56MiB RAM ceiling')
    check=parse_newc(gzip.decompress(output))
    require(sum(r[2]==DHCP_PATH for r in check)==1,'DHCP ELF missing after repack')
    require(sum(r[2]=='init' for r in check)==1,'bad init overlay')
    return output,{'input_cpio_entries':len(records),'output_cpio_entries':len(check),
                   'newc_bytes':len(cpio),'compressed_bytes':len(output),
                   'dhcp_elf_sha256':sha(dhcp_elf),'init_elf_sha256':sha(init_elf)}


def verify_rootfs_provenance(rootfs, cached):
    """Verify the old target-PASS bytes or a newly fetched official OCI image.

    This prevents loosening the target-PASS CRC check to accept arbitrary gzip
    files after the Windows Downloads/cache folder is deleted.
    """
    crc=f'{zlib.crc32(cached)&0xffffffff:08x}'
    if len(cached)==BASE_RAM_SIZE and crc==BASE_RAM_CRC:
        provenance={'kind':'REAL_TARGET_VERIFIED_BASE','crc32':crc,
                    'size':len(cached)}
    else:
        from fetch_debian13_armel_native import MANIFEST
        root_report=rootfs.with_name('tewa-debian13-armel-rootfs-audit.json')
        official_report=rootfs.with_name('debian13-armel-official-oci-download-audit.json')
        official_tar=rootfs.with_name('debian-13-trixie-armel-official-oci-rootfs.tar.gz')
        require(root_report.is_file() and official_report.is_file() and official_tar.is_file(),
                'unrecognized base initramfs: neither hardware-PASS CRC nor pinned official OCI build audit')
        a=json.loads(root_report.read_text(encoding='utf8'))
        b=json.loads(official_report.read_text(encoding='utf8'))
        require(a.get('architecture')=='armel' and a.get('suite')=='trixie',
                'regenerated base initramfs has wrong Debian architecture/suite')
        require(a.get('dpkg_status_package_entries',0)>=35,'regenerated base too few configured packages')
        require(a.get('source')==official_tar.name and a.get('source_sha256')==sha(official_tar.read_bytes()),
                'base initramfs tar provenance is not cryptographically verified')
        require(a.get('initramfs_bytes')==len(cached) and a.get('initramfs_sha256')==sha(cached),
                'regenerated base initramfs SHA256 is not verified')
        require(b.get('manifest_digest')==MANIFEST and b.get('rootfs_sha256')=='sha256:'+a['source_sha256'],
                'official OCI manifest/layer pinned SHA256 mismatch')
        require(b.get('architecture')=='armel' and b.get('suite')=='trixie' and b.get('configured_packages',0)>=35,
                'official downloaded rootfs architecture/status not verified')
        provenance={'kind':'OFFICIAL_DIGEST_PINNED_REGENERATED_NOT_TARGET_BOOTED',
                    'oci_manifest':MANIFEST,'layer_sha256':a['source_sha256'],
                    'packages':a['dpkg_status_package_entries'],'crc32':crc,'size':len(cached)}
    # Any reported audit must be consistent with the actual input bytes.
    root_report=rootfs.with_name('tewa-debian13-armel-rootfs-audit.json')
    if root_report.is_file():
        a=json.loads(root_report.read_text(encoding='utf8'))
        require(a.get('architecture')=='armel' and a.get('suite')=='trixie',
                'cached rootfs audit is not Debian trixie armel')
        require(a.get('initramfs_sha256')==sha(cached),
                'cached rootfs SHA256 inconsistent with its own audit')
    return provenance


def prepare(rootfs,base_fip,init_path,dhcp_path,output,expected_fip_sha,cache_inputs=None,reuse_overlay_from=None):
    require(sha(base_fip.read_bytes())==expected_fip_sha,'stage3 base FIP pinned SHA mismatch')
    b=parse(base_fip.read_bytes())
    _,_,n=read_blob(b.dtb,0)
    require('hsan,experimental-dma-enable' in n['/pie@12080000'],
            'FIP missing explicit experimental DMA opt-in')
    require(n['/chosen']['linux,initrd-start']==struct.pack('>I',RAM_START),'initramfs address is incorrect')
    init_elf=check_arm_elf(init_path)
    dhcp_elf=check_arm_elf(dhcp_path)
    cached=rootfs.read_bytes()
    provenance=verify_rootfs_provenance(rootfs,cached)
    root, meta = None, None
    if reuse_overlay_from is not None:
        previous_report=reuse_overlay_from/'network-stage3-run-report.json'
        previous_root=reuse_overlay_from/ROOT_OUTPUT
        if previous_report.is_file() and previous_root.is_file():
            try:
                previous=json.loads(previous_report.read_text(encoding='utf8'))
                previous_inputs=previous.get('cache_inputs') or {}
                matches=(
                    previous.get('base_rootfs_sha256')==sha(cached) and
                    previous_inputs.get('init_elf_sha256')==sha(init_elf) and
                    previous_inputs.get('dhcp_elf_sha256')==sha(dhcp_elf) and
                    previous_root.stat().st_size==previous.get('rootfs_bytes') and
                    sha_file(previous_root)==previous.get('rootfs_sha256') and
                    previous_root.stat().st_size < 0x03800000 and
                    previous_root.open('rb').read(3)==b'\x1f\x8b\x08' and
                    isinstance(previous.get('patch_meta'), dict)
                )
                if matches:
                    root=previous_root.read_bytes()
                    meta=previous['patch_meta']
                    print('STAGE12_OVERLAY_CACHE_HIT: reusing SHA256-validated previous stage Debian overlay; no gzip recompression',flush=True)
            except (OSError,ValueError,KeyError,TypeError):
                pass
    if root is None:
        print('STAGE12_OVERLAY_CACHE_MISS: create Debian overlay once from verified base',flush=True)
        root,meta=inject_cached_cpio(cached,init_elf,dhcp_elf)
    fip=patch_fip_initrd(base_fip.read_bytes(),len(root))
    out=parse(fip)
    require(out.header==b.header and out.payload==b.payload and out.overlays==b.overlays and out.bl31==b.bl31,
            'trusted boot chain modified by stage3 runtime patch')
    _,_,nodes=read_blob(out.dtb,0)
    require(nodes['/chosen']['linux,initrd-end']==struct.pack('>I',RAM_START+len(root)),
            'initramfs endpoint invalid')
    output.mkdir(parents=True,exist_ok=True)
    (output/ROOT_OUTPUT).write_bytes(root)
    (output/FIP_OUTPUT).write_bytes(fip)
    report={'status':'NETWORK_DATAPATH_EXPERIMENT_OFFLINE_VERIFIED_HARDWARE_TX_RX_UNKNOWN',
            'flash_authorized':False,'script_has_no_direct_nand_ops':True,'oem_bootloader_may_write_nand_env_after_reset':True,'bootfip_may_change_boot_selection':True,
            'dhcp_is_client':True,'dhcp_network_10_0_0_0_24_not_yet_proven':True,
            'device_mac':'52:f1:e8:c8:c8:28',
            'base_rootfs_sha256':sha(cached),'base_rootfs_crc32':f'{zlib.crc32(cached)&0xffffffff:08x}',
            'base_rootfs_provenance':provenance,
            'base_fip_sha256':sha(base_fip.read_bytes()),
            'rootfs_name':ROOT_OUTPUT,'rootfs_bytes':len(root),'rootfs_crc32':f'{zlib.crc32(root)&0xffffffff:08x}',
            'rootfs_sha256':sha(root),
            'fip_name':FIP_OUTPUT,'fip_bytes':len(fip),'fip_crc32':f'{zlib.crc32(fip)&0xffffffff:08x}',
            'fip_sha256':sha(fip),'new_image_sha256':sha(out.payload[456:]),
            'bootchain':{'bl31_sha256':sha(out.bl31),'trampoline_sha256':sha(out.payload[:456]),
                         'dtbo_sha256':sha(out.overlays)},
            'initramfs_start':hex(RAM_START),'initramfs_end':hex(RAM_START+len(root)),
            'patch_meta':meta, 'cache_inputs':cache_inputs}
    (output/'network-stage3-run-report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print(json.dumps(report,indent=2))
    return report


def sha_file(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def cached_report_ok(output, inputs):
    report_path=output/'network-stage3-run-report.json'
    if not report_path.is_file():
        return False
    try:
        report=json.loads(report_path.read_text(encoding='utf8'))
        if report.get('cache_inputs') != inputs:
            return False
        for key, name in (('rootfs',ROOT_OUTPUT), ('fip',FIP_OUTPUT)):
            file=output/name
            if not file.is_file() or file.stat().st_size!=report[key+'_bytes']:
                return False
            if sha_file(file)!=report[key+'_sha256']:
                return False
        return True
    except (OSError, ValueError, KeyError, TypeError):
        return False


def main():
    p=argparse.ArgumentParser()
    for opt in ('rootfs','base-fip','init-elf','dhcp-elf','output'):
        p.add_argument('--'+opt,type=Path,required=True)
    p.add_argument('--expected-fip-sha256',required=True)
    p.add_argument('--force',action='store_true',help='rebuild even if SHA256-validated output is cached')
    p.add_argument('--reuse-overlay-from',type=Path,help='previous stage cache with matching Debian base/ELF hashes; never trusts mismatched output')
    a=p.parse_args()
    tools=Path(__file__).resolve().parent
    inputs={
        'rootfs_sha256':sha_file(a.rootfs),
        'base_fip_sha256':sha_file(a.base_fip),
        'init_elf_sha256':sha_file(a.init_elf),
        'dhcp_elf_sha256':sha_file(a.dhcp_elf),
        'builder_sha256':sha_file(Path(__file__)),
        'fip_patches_sha256':sha_file(tools/'fip_patches.py'),
        'tewa_image_sha256':sha_file(tools/'tewa_image.py'),
        'dtb_probe_sha256':sha_file(tools/'dtb_probe.py'),
    }
    require(inputs['base_fip_sha256']==a.expected_fip_sha256,
            'pinned Stage12 base FIP SHA mismatch')
    if not a.force and cached_report_ok(a.output, inputs):
        print('STAGE12_CACHE_HIT: verified Debian initramfs and RAM FIP unchanged; skip decompress/recompress/repack.',flush=True)
        print('CACHED_TFTP_DIR:',a.output.resolve(),flush=True)
        return
    print('STAGE12_CACHE_MISS: an input or output hash changed; rebuilding RAM artifacts once.',flush=True)
    prepare(a.rootfs,a.base_fip,a.init_elf,a.dhcp_elf,a.output,
            a.expected_fip_sha256,cache_inputs=inputs,
            reuse_overlay_from=a.reuse_overlay_from if not a.force else None)


if __name__=='__main__':
    main()
