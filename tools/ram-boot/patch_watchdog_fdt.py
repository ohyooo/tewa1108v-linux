#!/usr/bin/env python3
"""Offline, exact-node WDT DTB adaptation for a RAM-only diagnostic candidate.
No device I/O. Preserves all non-WDT nodes; assumes the OEM booted WDT's APB
clock remains hardware enabled, but makes no claim about its gate provider.
"""
import hashlib
import struct
from fip_patches import _find_fdt_node_prop
from dtb_probe import read_blob

WDT = '/watchdog@0x10128000'


def mutate_prop(src: bytes, key: str, replacement: bytes | None) -> bytes:
    src, _, nodes = read_blob(src, 0)
    _, found = _find_fdt_node_prop(src, WDT, key)
    if found is None:
        raise ValueError(f'WDT property missing: {key}')
    at, size = found
    old_start = at - 12
    old_end = at + ((size + 3) & ~3)
    if replacement is None:
        rep = b''
    else:
        nameoff = struct.unpack_from('>I', src, at-4)[0]
        rep = struct.pack('>III', 3, len(replacement), nameoff)
        rep += replacement + bytes((-len(replacement)) % 4)
    delta = len(rep) - (old_end - old_start)
    b = bytearray(src[:old_start] + rep + src[old_end:])
    for off, value in ((4, len(b)), (12, struct.unpack_from('>I', src, 12)[0]+delta), (36, struct.unpack_from('>I',src,36)[0]+delta)):
        struct.pack_into('>I', b, off, value)
    b, _, _ = read_blob(bytes(b), 0)
    return b


def patch(src: bytes) -> bytes:
    src, _, original = read_blob(src,0)
    node=original[WDT]
    expected={
       'compatible': b'snps,dw-wdt\0',
       'reg': struct.pack('>II', 0x10128000, 0x1000),
       'clocks': struct.pack('>II',0xa5,0xa6),
       'clock-names': b'tclk\0pclk\0',
       'interrupts': struct.pack('>III',0,0x30,4),
       'timeout-sec': struct.pack('>I',300),
    }
    for k,v in expected.items():
        if node.get(k)!=v: raise ValueError(f'board WDT property mismatch: {k}')
    # Built-in dw_wdt supports tclk-only mode and optional pretimeout IRQ.
    # The OEM pclk gate provider (hsan,clk-gate) is disabled in this source DTB
    # and not implemented by this Linux image; do NOT enable, reset or toggle it.
    # The early test relies on the OEM still-running clock gate (unknown until
    # real board readback). Drop unresolvable Linux pclk and optional pretimeout
    # IRQ only. The hardware WDT reset remains enabled, with kernel core pings.
    b=mutate_prop(src,'interrupts',None)
    b=mutate_prop(b,'clocks',struct.pack('>I',0xa5))
    b=mutate_prop(b,'clock-names',b'tclk\0')
    _,_,after=read_blob(b,0)
    if set(original)!=set(after):raise ValueError('DTB node set changed')
    for path, before_props in original.items():
        if path==WDT:
            x=dict(before_props);y=dict(after[path]);
            for k in ('interrupts','clocks','clock-names'):
                x.pop(k,None);y.pop(k,None)
            if x!=y:raise ValueError('unexpected WDT change')
        elif after[path]!=before_props:
            raise ValueError('unexpected DT change '+path)
    assert after[WDT]['clocks']==struct.pack('>I',0xa5)
    assert after[WDT]['clock-names']==b'tclk\0'
    assert 'interrupts' not in after[WDT]
    return b

if __name__=='__main__':
    import sys
    src=open(sys.argv[1],'rb').read();out=patch(src);open(sys.argv[2],'wb').write(out)
    print('PATCH_WDT_DT_PASS',len(src),len(out),hashlib.sha256(out).hexdigest())
