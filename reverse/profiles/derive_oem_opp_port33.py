"""Extract exact port2/33 OPP writes from an original A32 init trace."""
import argparse
import hashlib
import json
from pathlib import Path


def derive(trace_path):
    trace = json.loads(trace_path.read_text(encoding='utf-8'))
    writes = trace['register_writes']
    assert trace['scope'] == 'OFFLINE_EMULATED_OEM_INIT_NOT_BOARD_OBSERVATION'
    port2 = [[a, b] for a, b in writes if 0x06042840 <= a < 0x06042860]
    assert port2 == [[0x06042840 + priority * 4, 0x100 | (161 + priority)] for priority in range(8)]
    port33 = [[a, b] for a, b in writes if 0x06058000 <= a < 0x06059000]
    expected = [[0x06058000 + priority * 4 + selector * 32, 0x100 | (169 + priority)]
                for priority in range(8) for selector in range(128)]
    assert port33 == expected
    return {'scope': 'DERIVED_FROM_ORIGINAL_A32_TRACE_NOT_BOARD_MAPPING',
            'trace_sha256': hashlib.sha256(trace_path.read_bytes()).hexdigest(),
            'port2': {'qid_base': 161, 'priorities': 8, 'writes': port2},
            'port33': {'qid_base': 169, 'priorities': 8, 'selectors': 128, 'writes': port33},
            'qualification': 'Exact boot-init writes are proven; these logical ports roles in LAN1 CPU receive are not yet established; this script does not write hardware'}


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--trace', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = derive(args.trace)
    args.output.write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    print('OEM_OPP_PORT2_PORT33_MAP_PASS', len(result['port2']['writes']), len(result['port33']['writes']))
