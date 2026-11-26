"""Trace CAPA selector7 candidate construction with explicit preparation metadata; no board access."""
import argparse
import hashlib
import json
import struct
from collections import Counter
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP


def trace(path, metadata, raw_vid, selector, lookup_result, execute_preparation=False):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x100000, 0x400000)
    uc.mem_map(0x800000, 0x100000)
    sections, imports, calls, writes, tables = {}, {}, Counter(), [], []
    registers = {}
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        symtab = elf.get_section_by_name('.symtab')
        for index, section in enumerate(elf.iter_sections()):
            if section['sh_flags'] & 2:
                sections[index] = 0x100000 + index * 0x10000
                if section['sh_type'] != 'SHT_NOBITS':
                    uc.mem_write(sections[index], section.data())

        def symbol_address(symbol):
            index = symbol['st_shndx']
            if index == 'SHN_UNDEF':
                if symbol.name not in imports:
                    imports[symbol.name] = 0x400000 + len(imports) * 4
                return imports[symbol.name]
            if index == 'SHN_ABS':
                return symbol['st_value']
            return sections[index] + symbol['st_value']

        for section in elf.iter_sections():
            if section['sh_type'] != 'SHT_REL' or section['sh_info'] not in sections:
                continue
            for relocation in section.iter_relocations():
                kind = relocation['r_info_type']
                if kind in (0, 42):  # Unwind metadata is never executed
                    continue
                address = sections[section['sh_info']] + relocation['r_offset']
                old = struct.unpack('<I', uc.mem_read(address, 4))[0]
                target = symbol_address(symtab.get_symbol(relocation['r_info_sym']))
                if kind == 2:
                    value = target + old
                elif kind in (28, 29):
                    addend = (old & 0xffffff) << 2
                    if addend & 0x2000000:
                        addend -= 0x4000000
                    value = (old & 0xff000000) | (((target + addend - address) >> 2) & 0xffffff)
                elif kind in (43, 44):
                    addend = ((old >> 4) & 0xf000) | (old & 0xfff)
                    if kind == 44:
                        immediate = (target + (addend << 16)) >> 16
                    else:
                        immediate = target + addend
                    value = (old & ~0x000f0fff) | ((immediate & 0xf000) << 4) | (immediate & 0xfff)
                else:
                    raise ValueError(f'Unsupported relocation {kind}')
                uc.mem_write(address, struct.pack('<I', value & 0xffffffff))
        entry = symbol_address(symtab.get_symbol_by_name('hi_capa_flow_add')[0])

    imported_addresses = {address: name for name, address in imports.items()}
    sentinel, flow, action, output = 0x4ff000, 0x880000, 0x881000, 0x882000
    preparation = entry - 0xe6c + 0x4dc
    observed, allocations = [], []
    uc.mem_write(flow, bytes(128))
    uc.mem_write(flow, bytes([selector]))
    uc.mem_write(flow + 8, bytes.fromhex('021122334455'))
    uc.mem_write(flow + 14, struct.pack('<H', raw_vid))
    uc.mem_write(flow + 0x55, bytes([7]))
    uc.mem_write(action, bytes(64))
    uc.mem_write(action + 8, bytes([2, 5]))

    def hook(machine, address, size, context):
        if address == sentinel:
            machine.emu_stop()
            return
        a, b, c = (machine.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        if address == preparation and not execute_preparation:
            machine.mem_write(a, struct.pack('<4I', *metadata))
            result = 0
        else:
            name = imported_addresses.get(address)
            if name is None:
                return
            result = 0
            if name == 'memset':
                machine.mem_write(a, bytes([b & 255]) * c)
                result = a
            elif name == 'memset_s':
                count = machine.reg_read(UC_ARM_REG_R3)
                assert count <= b
                machine.mem_write(a, bytes([c & 255]) * count)
            elif name in ('hi_tab_mgr_alloc_direct', 'hi_tab_mgr_alloc_hash') and a != 0x11b:
                assert execute_preparation and a in [0x100, 0x101, 0x102, 0x103]
                allocations.append({'table': a, 'data_hex': bytes(machine.mem_read(b, c)).hex()})
                result = {0x100: metadata[2], 0x101: metadata[0], 0x102: metadata[1], 0x103: metadata[3]}[a]
            elif name in ('hi_tab_mgr_get_id', 'hi_tab_mgr_alloc_hash'):
                assert a == 0x11b and c == 32
                observed.append({'call': name, 'candidate_hex': bytes(machine.mem_read(b, c)).hex()})
                result = lookup_result if name == 'hi_tab_mgr_get_id' else 23
            else:
                raise RuntimeError('Unexpected imported call ' + name)
        machine.reg_write(UC_ARM_REG_R0, result & 0xffffffff)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    for register, value in [(UC_ARM_REG_SP, 0x8ff000), (UC_ARM_REG_LR, sentinel),
                            (UC_ARM_REG_R0, flow), (UC_ARM_REG_R1, action), (UC_ARM_REG_R2, output)]:
        uc.reg_write(register, value)
    uc.emu_start(entry, sentinel + 4, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == sentinel
    assert uc.reg_read(UC_ARM_REG_R0) == 0
    expected = bytearray(32)
    expected[:6] = bytes.fromhex('554433221102')
    struct.pack_into('<H', expected, 6, 0 if raw_vid == 0x1000 else raw_vid)
    struct.pack_into('<H', expected, 8, (1 << selector) & 0xffff)
    expected[10:16] = bytes([1, 1, 0x49 if raw_vid == 0x1000 else 5, 0x45, 0, 0x40])
    struct.pack_into('<I', expected, 20, 0x38 | (2 << 14) | (5 << 16))
    struct.pack_into('<H', expected, 26, (metadata[1] & 31) | ((metadata[0] & 511) << 5))
    struct.pack_into('<I', expected, 28, (metadata[2] & 511) | ((metadata[3] << 9) & 0x7fe00))
    assert observed and all(x['candidate_hex'] == expected.hex() for x in observed)
    assert len(observed) == (1 if lookup_result > 0 else 2)
    handle = struct.unpack('<I', uc.mem_read(output, 4))[0]
    assert handle == (0x40000000 | (lookup_result if lookup_result > 0 else 23))
    return {'metadata': metadata, 'raw_vid': raw_vid, 'selector': selector,
            'lookup_result': lookup_result, 'observed': observed, 'handle': handle,
            'preparation_executed': execute_preparation, 'preparation_allocations': allocations}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    cases = [trace(args.module, metadata, vid, selector, lookup)
             for metadata in [[0, 0, 0, 0], [0x123, 0x15, 0x17f, 0x1a5], [0xffffffff]*4]
             for vid in [0x1000, 37] for selector in [0, 10, 15] for lookup in [0, 31]]
    result = {'scope': 'OFFLINE_ORIGINAL_A32_CANDIDATE_CONSTRUCTION_NOT_HARDWARE_RULE',
              'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
              'models': ['preparation helper at 0x4dc explicitly replaced with four supplied metadata words and success return',
                         'table manager captures candidate and returns modeled handle; no hardware allocation or CSR write'],
              'preparation_executed_cases': [trace(args.module, [0x123, 0x15, 17, 0x1a5], vid, 10, lookup, True)
                                              for vid in [0x1000, 37] for lookup in [0, 31]],
              'source_function': 'hi_capa_flow_add @0xe6c selector7 branch @0x1114',
              'metadata_dependencies': {'word0': 'candidate halfword26 bits13:5', 'word1': 'halfword26 bits4:0',
                                        'word2': 'candidate word28 bits8:0', 'word3': 'word28 bits18:9'},
              'unresolved': 'Real helpers0x4dc/0x208/0xf0 are executed in four cases; table0x100/0x101/0x102/0x103 manager return IDs are still explicit models, not real allocations or hardware backing',
              'cases': cases}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_CAPA_SELECTOR7_CANDIDATE_PASS', len(cases))
