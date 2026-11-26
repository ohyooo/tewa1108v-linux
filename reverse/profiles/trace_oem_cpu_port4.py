"""Trace original CPU rate logical-port argument and port4 OPP map; no board access."""
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


def trace(path, rate):
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
        entry = symbol_address(symtab.get_symbol_by_name('hi_capa_set_cpu_rate')[0])

    imported_addresses = {address: name for name, address in imports.items()}
    sentinel = 0x4ff000
    observed = []

    def hook(machine, address, size, context):
        if address == sentinel:
            machine.emu_stop()
            return
        name = imported_addresses.get(address)
        if name is None:
            return
        if name != 'hi_queue_set_egr_shaping':
            raise RuntimeError('Unexpected import ' + name)
        observed.append([machine.reg_read(UC_ARM_REG_R0), machine.reg_read(UC_ARM_REG_R1)])
        machine.reg_write(UC_ARM_REG_R0, 0)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    for register, value in [(UC_ARM_REG_SP, 0x8ff000), (UC_ARM_REG_LR, sentinel), (UC_ARM_REG_R0, rate)]:
        uc.reg_write(register, value)
    uc.emu_start(entry, sentinel + 4, count=10000)
    assert uc.reg_read(UC_ARM_REG_PC) == sentinel and uc.reg_read(UC_ARM_REG_R0) == 0
    assert observed == [[4, rate]]
    return {'rate_argument_bits': rate, 'shaping_call': observed[0]}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--opp-trace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    original = json.loads(args.opp_trace.read_text(encoding='utf-8'))
    rows = [[a, b] for a, b in original['register_writes'] if 0x06042880 <= a < 0x060428a0]
    assert rows == [[0x06042880 + priority * 4, 0x100 | (33 + priority)] for priority in range(8)]
    result = {'scope': 'OFFLINE_ORIGINAL_A32_AND_INIT_TRACE_NOT_BOARD_ACCEPTANCE',
              'module_sha256': hashlib.sha256(args.module.read_bytes()).hexdigest(),
              'opp_trace_sha256': hashlib.sha256(args.opp_trace.read_bytes()).hexdigest(),
              'source_function': 'hi_capa_set_cpu_rate @0x5814',
              'models': 'queue shaping API records arguments and returns success; no rate programming or MMIO',
              'cases': [trace(args.module, rate) for rate in [0, 1, 1000, 100000, 0xffffffff]],
              'port4_opp_writes': rows,
              'qualification': 'CPU rate API explicitly selects logical port4; exact role in initial LAN1 receive still requires board proof; no new shaping or action policy inferred'}
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_CPU_RATE_PORT4_AND_OPP_PASS', len(result['cases']), len(rows))
