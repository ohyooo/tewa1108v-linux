"""Read the complete IFC field-name table referenced by the original dump callback"""
import argparse
import json
import struct
from pathlib import Path

from elftools.elf.elffile import ELFFile


def inspect(path):
    with path.open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        dump = symbols.get_symbol_by_name('ifc_dump')[0]
        references = elf.get_section_by_name('.rel.text')
        # MOVW at ifc_dump+0x134 loads the key-name array anchor
        reference = next(r for r in references.iter_relocations()
                         if r['r_offset'] == dump['st_value'] + 0x134)
        anchor = symbols.get_symbol(reference['r_info_sym'])
        section = elf.get_section(anchor['st_shndx'])
        table = anchor['st_value'] + 16
        relocation_section = next(s for s in elf.iter_sections()
                                  if s['sh_type'] == 'SHT_REL' and s['sh_info'] == anchor['st_shndx'])
        relocated_symbols = elf.get_section(relocation_section['sh_link'])
        relocations = {r['r_offset']: r for r in relocation_section.iter_relocations()}
        result = []
        for field in range(128):
            offset = table + field * 4
            relocation = relocations[offset]
            if relocation['r_info_type'] != 2:
                raise ValueError('Expected ARM ABS32 string reference')
            symbol = relocated_symbols.get_symbol(relocation['r_info_sym'])
            addend = struct.unpack_from('<I', section.data(), offset)[0]
            data = elf.get_section(symbol['st_shndx']).data()
            start = symbol['st_value'] + addend
            name = data[start:data.index(b'\0', start)].decode('ascii')
            result.append({'field': field, 'name': name})
    return {'hardware_access': False, 'fields': result}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--module', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args.module)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('IFC_FIELDS_EXTRACTED', len(result['fields']))
