"""Trace original GEMAC silent callback and direct register hook; no hardware access."""
import argparse
import hashlib
import json
import struct
from collections import Counter
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP


def trace(path, silent):
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
        entry = symbol_address(symtab.get_symbol_by_name('hi_mac_set_silent')[0])
        register_write = symbol_address(symtab.get_symbol_by_name('hi_reg_hw_write')[0])

    sentinel, base, obj = 0x4ff000, 0xa00000, 0x880000
    uc.mem_map(base, 0x10000)
    uc.mem_write(base + 0x48, struct.pack('<I', 0xa5a5a5a5))
    uc.mem_write(obj, struct.pack('<I', base))
    uc.mem_write(obj + 0x58, struct.pack('<I', register_write))
    writes = []

    def hook(machine, address, size, context):
        if address == sentinel:
            machine.emu_stop()

    def write_hook(machine, access, address, size, value, context):
        if base <= address < base + 0x10000:
            writes.append([address - base, size, value])

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.hook_add(UC_HOOK_MEM_WRITE, write_hook)
    for register, value in [(UC_ARM_REG_SP, 0x8ff000), (UC_ARM_REG_LR, sentinel),
                            (UC_ARM_REG_R0, obj), (UC_ARM_REG_R1, silent)]:
        uc.reg_write(register, value)
    uc.emu_start(entry, sentinel + 4, count=10000)
    assert uc.reg_read(UC_ARM_REG_PC) == sentinel
    assert writes == [[0x48, 4, silent & 1]]
    assert struct.unpack('<I', uc.mem_read(base + 0x48, 4))[0] == (silent & 1)
    return {'input': silent, 'writes': writes}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    with args.module.open('rb') as stream:
        elf = ELFFile(stream)
        symtab = elf.get_section_by_name('.symtab')
        data_index = next(i for i, section in enumerate(elf.iter_sections()) if section.name == '.data')
        relocation = next(r for section in elf.iter_sections()
                          if section['sh_type'] == 'SHT_REL' and section['sh_info'] == data_index
                          for r in section.iter_relocations() if r['r_offset'] == 0x44)
        symbol = symtab.get_symbol(relocation['r_info_sym'])
        value = symbol['st_value'] + struct.unpack_from('<I', elf.get_section(data_index).data(), 0x44)[0]
        assert elf.get_section(symbol['st_shndx']).name == '.text' and value == 0x54
        text = elf.get_section_by_name('.text').data()
        assert struct.unpack_from('<3I', text, 0x44ec) == (0xe1a00006, 0xe3a01000, 0xe12fff32)
    result = {'scope': 'OFFLINE_ORIGINAL_A32_CALLBACK_NOT_BOARD_ACCEPTANCE',
              'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
              'executed': ['hi_mac_set_silent @0x54', 'hi_reg_hw_write @0x399c'],
              'models': 'fake MAC object points to zeroed MMIO RAM; silent CSR initial value0xa5a5a5a5; no imported function stub',
              'link_up_call': 'hi_mac_link_up @0x44ec..0x44f4 calls ops+0x44 with r1=0 before pause callback',
              'link_down_call': 'hi_mac_link_down @0x4448..0x4450 calls same callback with r1=1',
              'cases': [trace(args.module, value) for value in [0, 1, 2, 3, 0xffffffff]],
              'qualification': 'Full 32-bit write, not RMW; changing this bit alone is not evidence of functioning RX or safe live renegotiation'}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_GEMAC_SILENT_CALLBACK_PASS', len(result['cases']))
