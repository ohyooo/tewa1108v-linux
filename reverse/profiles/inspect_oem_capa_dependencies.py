"""Extract original ELF callback provenance without hardware access."""
import argparse
import hashlib
import json
import struct
from pathlib import Path
from elftools.elf.elffile import ELFFile


def extract(directory):
    result = {'scope': 'STATIC_OEM_ELF_PROVENANCE_NOT_HARDWARE_ACCEPTANCE', 'modules': {}, 'pub_tables': []}
    for name in ['hi_lsw_pfe', 'hi_lsw_dp', 'hi_kcapa', 'hi_kcfe_pub', 'hi_kcfe_lrn_l2']:
        path = directory / (name + '.ko')
        result['modules'][name] = hashlib.sha256(path.read_bytes()).hexdigest()
    with (directory / 'hi_lsw_pfe.ko').open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        data = elf.get_section_by_name('.data').data()
        index = next(i for i, s in enumerate(elf.iter_sections()) if s.name == '.data')
        relocations = {r['r_offset']: r for section in elf.iter_sections()
                       if section['sh_type'] == 'SHT_REL' and section['sh_info'] == index
                       for r in section.iter_relocations()}

        def resolve(offset):
            rel = relocations[offset]
            assert rel['r_info_type'] == 2
            sym = symbols.get_symbol(rel['r_info_sym'])
            value = struct.unpack_from('<I', data, offset)[0] + sym['st_value']
            sec = elf.get_section(sym['st_shndx'])
            functions = [s.name for s in symbols.iter_symbols() if s['st_shndx'] == sym['st_shndx']
                         and s['st_value'] == value and s['st_info']['type'] == 'STT_FUNC']
            return sec, value, functions

        for offset in range(0xb0, 0xb0 + 9 * 56, 56):
            cfg = offset + 4
            table = struct.unpack_from('<I', data, offset)[0]
            section, value, _ = resolve(cfg)
            name = section.data()[value:].split(b'\0', 1)[0].decode('ascii')
            callbacks = {}
            for field, role in [(0x20, 'write'), (0x24, 'read'), (0x28, 'delete'), (0x30, 'dump')]:
                if cfg + field in relocations:
                    section, value, names = resolve(cfg + field)
                    callbacks[role] = {'section': section.name, 'offset': value, 'symbols': names}
            result['pub_tables'].append({'id': table, 'name': name,
                                         'entries': struct.unpack_from('<I', data, cfg + 8)[0],
                                         'entry_bytes': struct.unpack_from('<I', data, cfg + 16)[0],
                                         'callbacks': callbacks})
    expected = {0x101: 'nhp_write', 0x102: 'l3intf_write', 0x103: 'tag_write'}
    for table, function in expected.items():
        row = next(x for x in result['pub_tables'] if x['id'] == table)
        assert row['callbacks']['write']['symbols'] == [function]
    with (directory / 'hi_kcapa.ko').open('rb') as stream:
        elf = ELFFile(stream)
        symbols = elf.get_section_by_name('.symtab')
        index = next(i for i, s in enumerate(elf.iter_sections()) if s.name == '.rodata')
        relocation = next(r for section in elf.iter_sections()
                          if section['sh_type'] == 'SHT_REL' and section['sh_info'] == index
                          for r in section.iter_relocations() if r['r_offset'] == 0x38)
        symbol = symbols.get_symbol(relocation['r_info_sym'])
        value = symbol['st_value'] + struct.unpack_from('<I', elf.get_section(index).data(), 0x38)[0]
        assert elf.get_section(symbol['st_shndx']).name == '.text' and value == 0xe6c
    for name, offset, instructions in [('hi_kcfe_pub', 0x19e0, [0xe5933038]),
                                       ('hi_kcfe_lrn_l2', 0x290, [0xe3a0c007]),
                                       ('hi_kcfe_lrn_l2', 0x2b0, [0xe5cdc055])]:
        with (directory / (name + '.ko')).open('rb') as stream:
            data = ELFFile(stream).get_section_by_name('.text').data()
            assert list(struct.unpack_from('<' + 'I' * len(instructions), data, offset)) == instructions
    result['selector7_provenance'] = {
        'caller': 'hi_kcfe_lrn_l2.ko hi_cfe_lrn_l2_add: selector7 stored at flow+0x55, hi_adp_flow_add called at0x3e8',
        'adapter': 'hi_kcfe_pub.ko hi_adp_flow_add @0x19cc loads registered ops+0x38',
        'registration': 'hi_kcapa.ko init_module passes .rodata+0 to hi_cfe_adp_lsw_register; slot+0x38 resolves to hi_capa_flow_add @0xe6c',
        'action': 'hi_cfe_lrn_set_net_port copies six bytes of learned port data, then MAC/direction/QoS/tag/stat helpers populate the flow',
        'qualification': 'An L2 learning acceleration consumer is proven; a generic initial CPU receive/trap rule is NOT proven'}
    result['table100'] = {'owner': 'hi_lsw_dp.ko internal init @0x3fc, create @0x508',
                          'entry_bytes': 24, 'write': 'hi_tab_write_cnt', 'read': 'hi_tab_read_cnt',
                          'qualification': 'Statistics resource; allocation ID alone is not a forwarding target'}
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--modules', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = extract(args.modules)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_CAPA_CALLBACK_PROVENANCE_PASS', len(result['pub_tables']))
