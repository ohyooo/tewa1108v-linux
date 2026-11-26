"""Trace original A32 hi_eqm_read_tab buffer bounds with modeled CSR reads; no board access."""
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


def trace(path, table, row, requested_words):
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
        entry = symbol_address(symtab.get_symbol_by_name('hi_eqm_read_tab')[0])

    imported_addresses = {address: name for name, address in imports.items()}
    sentinel, output = 0x4ff000, 0x880000
    reads = []
    model = {0x42000048: 0}
    data_registers = [0x4200004c, 0x42000050, 0x42000054, 0x42000058, 0x4200006c]
    model.update({address: 0x11220000 + index for index, address in enumerate(data_registers)})

    def hook(machine, address, size, context):
        if address == sentinel:
            machine.emu_stop()
            return
        name = imported_addresses.get(address)
        if name is None:
            return
        a, b = (machine.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1))
        result = 0
        if name == 'hi_lsw_write':
            writes.append([a, b])
        elif name == 'hi_lsw_read':
            if a not in model:
                raise RuntimeError(f'Unmodeled CSR {a:x}')
            reads.append(a)
            result = model[a]
        else:
            raise RuntimeError('Unexpected imported call ' + name)
        machine.reg_write(UC_ARM_REG_R0, result)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.mem_write(output, bytes([0xa5]) * 32)
    for register, value in [(UC_ARM_REG_SP, 0x8ff000), (UC_ARM_REG_LR, sentinel),
                            (UC_ARM_REG_R0, table), (UC_ARM_REG_R1, row),
                            (UC_ARM_REG_R2, output), (UC_ARM_REG_R3, requested_words)]:
        uc.reg_write(register, value)
    uc.emu_start(entry, sentinel + 4, count=100000)
    assert uc.reg_read(UC_ARM_REG_PC) == sentinel
    expected_count = min(requested_words, 5)
    words = list(struct.unpack('<8I', uc.mem_read(output, 32)))
    assert words[:expected_count] == [model[a] for a in data_registers[:expected_count]]
    assert words[expected_count:] == [0xa5a5a5a5] * (8 - expected_count)
    assert writes == [[0x42000048, 0x80000000 | ((table & 127) << 20) | (row & 65535)]]
    assert reads == [0x42000048] + data_registers[:expected_count]
    assert uc.reg_read(UC_ARM_REG_R0) == 0
    return {'table': table, 'row': row, 'requested_words': requested_words,
            'actual_words': expected_count, 'writes': writes, 'reads': reads,
            'output_words': words, 'unwritten_tail_preserved': True}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = {'scope': 'OFFLINE_ORIGINAL_A32_EXECUTION_NOT_BOARD_ACCEPTANCE',
              'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
              'model': 'command completes immediately with no error; five CSR data words carry distinct sentinel values',
              'cases': [trace(args.module, table, row, count) for table in [80, 81]
                        for row in [81, 129, 160] for count in [0, 1, 3, 5, 8]],
              'conclusion': 'r3 counts u32 words and is clamped to five; request 8 writes only 20 bytes; current five-word CSR API suffices',
              'queue_count_consumer': {'function': 'hi_queue_port_cnt_get @0x4da8',
                                       'table81_activity': 'word0 + word1 + word2, modulo u32',
                                       'table81_extra': 'word3',
                                       'table80_low15': 'word2 & 0x7fff',
                                       'effect_on_hardware_counters': 'NOT_QUALIFIED_CLEAR_ON_READ_NOT_EXCLUDED'}}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_EQM_READ_BOUNDS_PASS', len(result['cases']))
