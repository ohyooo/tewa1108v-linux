"""Validate physical LAN UDP payloads with the freestanding ARM helper."""
from pathlib import Path
import base64
import json
import secrets
import serial
import socket
import threading
import time

tag = secrets.token_hex(6)
raw = bytearray()
result = {'pass': False, 'roundtrips': 0, 'sizes': [32, 1472, 4096, 8192]}
stop = threading.Event()
server = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
server.bind(('192.168.0.10', 18082))
server.settimeout(2)


def echo():
    try:
        while result['roundtrips'] < 1000 and not stop.is_set():
            try:
                data, peer = server.recvfrom(8193)
            except socket.timeout:
                continue
            seq = result['roundtrips']
            size = result['sizes'][seq & 3]
            expected = bytearray((i * 17 + seq * 31) & 255 for i in range(size))
            expected[0:2] = seq.to_bytes(2, 'big')
            if peer[0] != '192.168.0.1' or data != expected:
                raise RuntimeError('UDP_PEER_OR_PAYLOAD_MISMATCH')
            if server.sendto(data, peer) != size:
                raise RuntimeError('UDP_ECHO_SHORT_SEND')
            result['roundtrips'] += 1
    except (OSError, RuntimeError) as error:
        result['error'] = str(error)


worker = threading.Thread(target=echo)
worker.start()
try:
    with serial.Serial('COM11', 115200, timeout=.05, write_timeout=5,
                       xonxoff=False, rtscts=False, dsrdtr=False) as port:
        def command(text, seconds):
            marker = ('UDP_DONE_' + secrets.token_hex(6)).encode()
            start = len(raw)
            # Split marker in the command so its echo cannot satisfy the wait
            prefix, nonce = marker.decode().rsplit('_', 1)
            port.write((text + "; printf '\\n" + prefix + "_%s\\n' '" + nonce + "'\r").encode())
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                raw.extend(port.read(65536))
                if marker in raw[start:]:
                    return bytes(raw[start:])
            raise RuntimeError('UDP_UART_TIMEOUT')

        command("printf 'UDP_READY\\n'", 5)
        binary = Path('work/lan1-udp-test.elf').read_bytes()
        encoded = base64.b64encode(binary).decode()
        lines = '\n'.join(encoded[i:i + 64] for i in range(0, len(encoded), 64))
        port.write(("base64 -d > /tmp/lan1-udp-test <<'UDP_BINARY'\n" + lines + '\nUDP_BINARY\n').encode())
        response = command('chmod 700 /tmp/lan1-udp-test && timeout 30 /tmp/lan1-udp-test; '
                           'udp_rc=$?; printf "UDP_HELPER_RC=%s\\n" "$udp_rc"; '
                           'rm -f /tmp/lan1-udp-test', 40)
        worker.join(3)
        result['pass'] = (result['roundtrips'] == 1000 and 'error' not in result
                          and b'UDP_1000_ROUNDTRIPS_PASS' in response
                          and b'UDP_HELPER_RC=0' in response)
finally:
    stop.set()
    worker.join(3)
    server.close()
    Path('work').mkdir(exist_ok=True)
    Path('work/udp-roundtrip-' + tag + '.raw').write_bytes(raw)
    Path('work/udp-roundtrip-latest.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
print(json.dumps(result, indent=2))
raise SystemExit(0 if result['pass'] else 1)
