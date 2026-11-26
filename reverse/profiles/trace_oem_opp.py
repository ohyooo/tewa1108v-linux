"""Trace original A32 hi_pfe_opp_init with an explicit queue model; no board access."""
import argparse
import hashlib
import json
import struct
from collections import Counter
from pathlib import Path

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_HOOK_CODE, UC_MODE_ARM
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0
from unicorn.arm_const import UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_SP


def trace(path, queue_profile_path):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(0x100000, 0x400000)
    uc.mem_map(0x800000, 0x100000)
    sections, imports, calls, writes, tables = {}, {}, Counter(), [], []
    registers = {}
    queue_profile = json.loads(queue_profile_path.read_text(encoding="utf-8"))
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
        entry = symbol_address(symtab.get_symbol_by_name('hi_pfe_opp_init')[0])

    imported_addresses = {address: name for name, address in imports.items()}
    sentinel = 0x4ff000

    def hook(machine, address, size, context):
        if address == sentinel:
            machine.emu_stop()
            return
        name = imported_addresses.get(address)
        if name is None:
            return
        calls[name] += 1
        a, b, c = (machine.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
        result = 0
        if name == 'memset':
            machine.mem_write(a, bytes([b & 255]) * c)
            result = a
        elif name == 'memcpy':
            machine.mem_write(a, bytes(machine.mem_read(b, c)))
            result = a
        elif name in ('hi_queue_get_qid', 'hi_queue_get_q_num'):
            row = next((r for r in queue_profile['rows'] if r[1] == a and r[0] == 1), None)
            result = row[2 if name == 'hi_queue_get_qid' else 3] if row else 0x8fffffee
        elif name == 'hi_lsw_write':
            writes.append([a, b])
            registers[a] = b
        elif name == 'hi_lsw_read':
            result = registers.get(a, 0)
        elif name == 'hi_lsw_write_tab128':
            tables.append({'base': a, 'row': b, 'words': list(struct.unpack('<4I', machine.mem_read(c, 16)))})
        elif name in ('hi_dfx_print', 'printk', 'hi_tab_mgr_create'):
            pass
        else:
            raise RuntimeError(f'Unmodeled imported call {name} args={a:x},{b:x},{c:x}')
        machine.reg_write(UC_ARM_REG_R0, result)
        machine.reg_write(UC_ARM_REG_PC, machine.reg_read(UC_ARM_REG_LR))

    uc.hook_add(UC_HOOK_CODE, hook)
    uc.reg_write(UC_ARM_REG_SP, 0x8ff000)
    uc.reg_write(UC_ARM_REG_LR, sentinel)
    uc.emu_start(entry, sentinel + 4, count=1000000)
    if uc.reg_read(UC_ARM_REG_PC) != sentinel:
        raise RuntimeError('OEM init did not return normally')
    return {'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'scope': 'OFFLINE_EMULATED_OEM_INIT_NOT_BOARD_OBSERVATION',
            'models': {'initial_registers': 0, 'queue_profile_sha256': hashlib.sha256(queue_profile_path.read_bytes()).hexdigest()},
            'return_value': uc.reg_read(UC_ARM_REG_R0),
            'imported_calls': dict(calls), 'register_writes': writes,
            'table_writes': tables,
            'queue_model': 'OEM runtime-verified active rows from oem_queue_profile.json; missing port returns OEM sentinel; initial CSRs zero; no board acceptance'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--queue-profile', type=Path, required=True)
    args = parser.parse_args()
    result = trace(args.module, args.queue_profile)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'return_value': result['return_value'],
                      'register_writes': len(result['register_writes']),
                      'table_writes': len(result['table_writes'])}))
