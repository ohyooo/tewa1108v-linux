"""Trace original IPS diagnostic register reads with modeled counters; no board access."""
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


def trace(path):
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
        entry = symbol_address(symtab.get_symbol_by_name('ips_diag_get_cnt')[0])

    imported_addresses = {address: name for name, address in imports.items()}
    sentinel, output = 0x4ff000, 0x880000
    reads = []
    ports = [4, 3, 8, 9, 10, 11, 12, 0, 64, 13, 20]
    uc.mem_write(output, struct.pack('<I', 0xffffffff) + bytes(28))

    def hook(machine, address, size, context):
        if address == sentinel:
            machine.emu_stop()
            return
        name = imported_addresses.get(address)
        if name is None:
            return
        result = 0
        if name == 'hi_lsw_read':
            reg = machine.reg_read(UC_ARM_REG_R0)
            reads.append(reg)
            result = 1
        elif name != 'hi_dfx_print':
            raise RuntimeError('Unexpected import ' + name)
        machine.reg_write(UC_ARM_REG_R0, result)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    for register, value in [(UC_ARM_REG_SP, 0x8ff000), (UC_ARM_REG_LR, sentinel), (UC_ARM_REG_R0, output)]:
        uc.reg_write(register, value)
    uc.emu_start(entry, sentinel + 4, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == sentinel
    expected = []
    for row, port in enumerate(ports):
        expected.append(0x4400101c + row * 28)
        if port < 32:
            expected.extend(0x4400101c + row * 28 + field * 4 for field in range(1, 7))
    expected.append(0x44001344)
    assert reads == expected
    return {'scope': 'OFFLINE_ORIGINAL_A32_READ_TRACE_NOT_BOARD_DATA',
            'module_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'models': 'u32 port mask all ones; all register reads return1; output printing stubbed',
            'source': 'ips_diag_get_cnt @0x63b0', 'port_selectors': ports, 'register_reads': reads,
            'fields': ['raw0', 'sop_drop', 'eop_drop', 'exlong_drop', 'exshort_drop', 'mac_err_drop', 'burr_drop'],
            'driver_sampling': 'rows0..7,9,10 only; row8 invalid, since selector64 cannot select the six error registers',
            'qualification': 'raw0 meaning and clear-on-read behavior not yet qualified; no MMIO writes or DMA ownership changes'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = trace(args.module)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_IPS_READ_TRACE_PASS', len(result['register_reads']))
