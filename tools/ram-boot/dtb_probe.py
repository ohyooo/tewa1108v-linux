#!/usr/bin/env python3
"""Read-only parser for the FDTs appended after the stock uImage BL33 payload."""
import argparse
import hashlib
import pathlib
import struct

FDT_MAGIC = 0xd00dfeed
FDT_BEGIN_NODE, FDT_END_NODE, FDT_PROP, FDT_NOP, FDT_END = 1, 2, 3, 4, 9


def align4(x):
    return (x + 3) & ~3


def read_blob(b, at):
    if at < 0 or at + 40 > len(b):
        raise ValueError('FDT header beyond input')
    fields = struct.unpack_from('>10I', b, at)
    if fields[0] != FDT_MAGIC:
        raise ValueError(f'not a FDT at {at:#x}')
    _, total, off_struct, off_strings, off_mem, ver, last_compat, bootcpu, size_strings, size_struct = fields
    if total < 40 or at + total > len(b):
        raise ValueError('FDT exceeds file')
    if ver < 16 or off_struct + size_struct > total or off_strings + size_strings > total or off_mem >= total:
        raise ValueError('FDT fields inconsistent')
    blob = b[at:at + total]
    strings = blob[off_strings:off_strings+size_strings]
    pos = off_struct
    end = pos + size_struct
    stack = []
    nodes = {}
    seen_end = False
    while pos + 4 <= end:
        tok, = struct.unpack_from('>I', blob, pos)
        pos += 4
        if tok == FDT_BEGIN_NODE:
            z = blob.find(b'\x00', pos, end)
            if z < 0:
                raise ValueError('unterminated node')
            name = blob[pos:z].decode('ascii', 'replace')
            pos = align4(z+1)
            path = ('/' if not stack else '/' + '/'.join(stack[1:] + [name]))
            stack.append(name)
            nodes[path] = {}
        elif tok == FDT_END_NODE:
            if not stack:
                raise ValueError('FDT_END_NODE without start')
            stack.pop()
        elif tok == FDT_PROP:
            if pos + 8 > end or not stack:
                raise ValueError('invalid property')
            length, str_off = struct.unpack_from('>II', blob, pos)
            pos += 8
            if str_off >= len(strings) or pos + length > end:
                raise ValueError('invalid property bounds')
            z = strings.find(b'\x00', str_off)
            if z < 0:
                raise ValueError('property name unterminated')
            key = strings[str_off:z].decode('ascii','replace')
            path = '/' if len(stack) == 1 else '/' + '/'.join(stack[1:])
            nodes[path][key] = blob[pos:pos+length]
            pos = align4(pos+length)
        elif tok == FDT_NOP:
            continue
        elif tok == FDT_END:
            if stack:
                raise ValueError('unclosed nodes')
            seen_end = True
            break
        else:
            raise ValueError(f'bad FDT token={tok:#x} pos={pos-4:#x}')
    if not seen_end:
        raise ValueError('FDT_END not found')
    return blob, fields, nodes


def render_value(raw):
    if len(raw) == 0:
        return 'true'
    if len(raw) <= 128 and any(32 <= v <= 126 for v in raw) and all((v == 0 or 32 <= v <= 126) for v in raw) and raw[-1] == 0:
        return repr(raw.rstrip(b'\x00').replace(b'\x00', b'|').decode('ascii'))
    if len(raw) <= 32 and len(raw) % 4 == 0:
        return ' '.join(f'0x{x:x}' for x in struct.unpack('>' + 'I' * (len(raw)//4), raw))
    return f'hex({raw[:24].hex()}{"..." if len(raw) > 24 else ""})'


def inspect(name):
    path = pathlib.Path(name)
    b = path.read_bytes()
    uh = 0xc2c0
    size, = struct.unpack_from('>I', b, uh+0xc)
    after = uh + 64 + size
    bl33_size, = struct.unpack_from('<Q', b, 0x30 + 16 + 40 + 24)
    end = uh + bl33_size
    print(f'{path.name}: uImage size=0x{size:x}, appended FDT start=0x{after:x}, BL33 end=0x{end:x}')
    if b[after:after+4] != bytes.fromhex('d00dfeed'):
        raise ValueError('FDT does not immediately follow the data')
    p = after
    count = 0
    while p < end:
        ix = b.find(bytes.fromhex('d00dfeed'), p, end)
        if ix < 0:
            break
        try:
            blob, header, nodes = read_blob(b, ix)
        except ValueError as e:
            print(f'  at 0x{ix:x}: invalid magic candidate: {e}')
            p = ix + 4
            continue
        label = f'FDT{count}'
        print(f'  {label}: file=0x{ix:x}, length={len(blob)} (0x{len(blob):x}), sha256={hashlib.sha256(blob).hexdigest()}, nodes={len(nodes)}')
        if count == 0:
            extracted = pathlib.Path('/mnt/data/mtd05_kernela_base.dtb').read_bytes()
            print('    identical to extracted A base:',blob == extracted)
            for node_path, props in nodes.items():
                focus = (node_path in ['/', '/chosen', '/cpus', '/memory', '/psci', '/firmware', '/reserved-memory'] or
                         node_path.startswith('/cpus/cpu@') or
                         node_path.startswith('/memory@') or
                         node_path.startswith('/reserved-memory/') or
                         ('psci' in node_path.lower()) or
                         ('gic' in node_path.lower()) or
                         'interrupt-controller' in node_path.lower())
                if not focus:
                    continue
                print(f'    {node_path}:')
                for k in ('compatible','device_type','reg','status','enable-method','method','cpu-release-addr','bootargs','#address-cells','#size-cells','no-map','reusable','linux,usable-memory'):
                    if k in props:
                        print(f'      {k}: {render_value(props[k])}')
        p = ix + len(blob)
        count += 1
    print('  valid FDT count:',count,'trailing bytes after last:',end-p)
    return count


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('kernels', nargs='+')
    args = ap.parse_args()
    for kernel in args.kernels:
        inspect(kernel)
