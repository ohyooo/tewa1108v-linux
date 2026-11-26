"""One bounded TCP round trip over the physical LAN, controlled through UART."""
from pathlib import Path
import argparse
import json
import secrets
import serial
import socket
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=0)
parser.add_argument('--port', type=int, default=18080)
parser.add_argument('--interface-cycles', type=int, default=0)
args = parser.parse_args()
if not 0 <= args.seconds <= 300 or not 1024 <= args.port <= 65535:
    parser.error('duration or port outside test bounds')
if not 0 <= args.interface_cycles <= 3 or (args.interface_cycles and
                                         args.seconds < args.interface_cycles * 5 + 5):
    parser.error('interface cycles need 5 seconds per cycle plus 5 seconds')
Path('work').mkdir(exist_ok=True)
size = 1024 * 1024
tag = secrets.token_hex(8)
payload = secrets.token_bytes(size)
raw = bytearray()
result = {"bytes_per_round": size, "minimum_seconds": args.seconds, "port": args.port,
          "pass": False, "rounds": 0, "interface_cycles": args.interface_cycles}
listener = socket.socket()
listener.bind(("192.168.0.10", args.port))
listener.listen(1)
listener.settimeout(35)


def server():
    try:
        with listener:
            conn, peer = listener.accept()
            with conn:
                conn.settimeout(30)
                if peer[0] != "192.168.0.1":
                    raise RuntimeError("UNEXPECTED_TCP_PEER")
                started = time.monotonic()
                while True:
                    round_payload = result['rounds'].to_bytes(8, 'big') + payload[8:]
                    conn.sendall((str(size) + '\n').encode() + round_payload)
                    received = bytearray()
                    while len(received) < size:
                        chunk = conn.recv(min(65536, size - len(received)))
                        if not chunk:
                            break
                        received.extend(chunk)
                    if bytes(received) != round_payload:
                        raise RuntimeError('TCP_PAYLOAD_MISMATCH')
                    result['rounds'] += 1
                    if time.monotonic() - started >= args.seconds:
                        break
                conn.sendall(b'0\n')
                result["elapsed_seconds"] = time.monotonic() - started
                result["returned_bytes"] = len(received)
                result['bytes_each_direction'] = size * result['rounds']
                result["pass"] = bytes(received) == round_payload
    except (OSError, RuntimeError) as error:
        result["error"] = str(error)


worker = threading.Thread(target=server, daemon=True)
worker.start()
try:
    with serial.Serial("COM11", 115200, timeout=0.05, write_timeout=10,
                       xonxoff=False, rtscts=False, dsrdtr=False) as port:
        def command(text, seconds):
            nonce = secrets.token_hex(8)
            marker = ("TCP_DONE_" + nonce).encode()
            start = len(raw)
            port.write((text + "; printf '\\nTCP_DONE_%s\\n' '" + nonce + "'\r").encode())
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                raw.extend(port.read(65536))
                if marker in raw[start:]:
                    return bytes(raw[start:])
            raise RuntimeError("TCP_SERIAL_COMMAND_TIMEOUT")

        command("printf 'TCP_READY_%s\\n' '" + tag + "'", 5)
        cycle_setup = cycle_finish = ''
        if args.interface_cycles:
            ready = command('test -e /sys/firmware/devicetree/base/pie@12080000/'
                            'hsan,experimental-retain-dma-on-down && '
                            'test -x /tmp/lan1-interface-test; '
                            'printf "TCP_CYCLE_READY=%s\\n" "$?"', 5)
            if b'TCP_CYCLE_READY=0' not in ready:
                raise RuntimeError('TCP_INTERFACE_CYCLE_PROFILE_OR_HELPER_MISSING')
            cycle_file = '/tmp/tewa-cycle-' + tag + '.log'
            indices = ' '.join(str(i) for i in range(args.interface_cycles))
            cycle_setup = (f'(for cycle in {indices}; do sleep 1; '
                           'timeout 10 /tmp/lan1-interface-test || exit 1; '
                           f'done) > {cycle_file} 2>&1 & cycle_pid=$!; ')
            cycle_finish = (f'wait "$cycle_pid"; cycle_rc=$?; cat {cycle_file}; '
                            'printf "TCP_CYCLE_RC=%s\\n" "$cycle_rc"; '
                            f'rm -f {cycle_file}; ')
        filename = "/tmp/tewa-tcp-" + tag + ".bin"
        script = (f"exec 3<>/dev/tcp/192.168.0.10/{args.port} || exit 1; "
                  "while read -r count <&3; do [ \"$count\" = 0 ] && break; "
                  f"head -c \"$count\" <&3 > {filename} || exit 2; "
                  f"cat {filename} >&3 || exit 3; "
                  "done; exec 3>&-; exec 3<&-")
        response = command(cycle_setup + f"timeout {args.seconds + 40} bash -c '" + script + "'; tcp_rc=$?; "
                           + cycle_finish +
                           "printf 'TCP_SHELL_RC=%s\\n' \"$tcp_rc\"; rm -f " + filename, args.seconds + 50)
        result["board_shell_rc_zero"] = b"TCP_SHELL_RC=0" in response
        result['interface_cycles_completed'] = response.count(b'INTERFACE_SOFT_RESUME_PASS')
        result['interface_cycles_pass'] = (not args.interface_cycles or
                                          (b'TCP_CYCLE_RC=0' in response and
                                           result['interface_cycles_completed'] == args.interface_cycles))
    worker.join(5)
    result["pass"] = (result["pass"] and result["board_shell_rc_zero"]
                      and result['interface_cycles_pass'] and not worker.is_alive())
finally:
    listener.close()
    Path("work/tcp-roundtrip-" + tag + ".raw").write_bytes(raw)
    Path("work/tcp-roundtrip-latest.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["pass"] else 1)
