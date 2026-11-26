"""One bounded TCP round trip over the physical LAN, controlled through UART."""
from pathlib import Path
import argparse
import hashlib
import json
import secrets
import serial
import socket
import threading
import time

parser = argparse.ArgumentParser()
parser.add_argument('--seconds', type=int, default=0)
parser.add_argument('--port', type=int, default=18080)
args = parser.parse_args()
if not 0 <= args.seconds <= 300 or not 1024 <= args.port <= 65535:
    parser.error('duration or port outside test bounds')
Path('work').mkdir(exist_ok=True)
size = 1024 * 1024
tag = secrets.token_hex(8)
payload = secrets.token_bytes(size)
expected = hashlib.sha256(payload).hexdigest()
raw = bytearray()
result = {"bytes_per_round": size, "minimum_seconds": args.seconds, "port": args.port,
          "expected_sha256": expected, "pass": False, "rounds": 0}
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
                    conn.sendall((str(size) + '\n').encode() + payload)
                    received = bytearray()
                    while len(received) < size:
                        chunk = conn.recv(min(65536, size - len(received)))
                        if not chunk:
                            break
                        received.extend(chunk)
                    if bytes(received) != payload:
                        raise RuntimeError('TCP_PAYLOAD_MISMATCH')
                    result['rounds'] += 1
                    if time.monotonic() - started >= args.seconds:
                        break
                conn.sendall(b'0\n')
                result["elapsed_seconds"] = time.monotonic() - started
                result["returned_bytes"] = len(received)
                result["returned_sha256"] = hashlib.sha256(received).hexdigest()
                result['bytes_each_direction'] = size * result['rounds']
                result["pass"] = bytes(received) == payload
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
        filename = "/tmp/tewa-tcp-" + tag + ".bin"
        script = (f"exec 3<>/dev/tcp/192.168.0.10/{args.port} || exit 1; "
                  "while read -r count <&3; do [ \"$count\" = 0 ] && break; "
                  f"head -c \"$count\" <&3 > {filename} || exit 2; "
                  f"sha256sum {filename}; cat {filename} >&3; "
                  "done; exec 3>&-; exec 3<&-")
        response = command(f"timeout {args.seconds + 40} bash -c '" + script + "'; tcp_rc=$?; "
                           "printf 'TCP_SHELL_RC=%s\\n' \"$tcp_rc\"; rm -f " + filename, args.seconds + 50)
        result['board_checksum_count'] = response.count((expected + "  " + filename).encode())
        result["board_sha256_seen"] = result['board_checksum_count'] > 0
        result["board_shell_rc_zero"] = b"TCP_SHELL_RC=0" in response
    worker.join(5)
    result["pass"] = (result["pass"] and result["board_sha256_seen"]
                      and result['board_checksum_count'] == result['rounds']
                      and result["board_shell_rc_zero"] and not worker.is_alive())
finally:
    listener.close()
    Path("work/tcp-roundtrip-" + tag + ".raw").write_bytes(raw)
    Path("work/tcp-roundtrip-latest.json").write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
print(json.dumps(result, indent=2))
raise SystemExit(0 if result["pass"] else 1)
