"""Isolated DORA server and existing DNS service; UART must be free"""
import base64
import argparse
import json
import secrets
import serial
import socket
import struct
import threading
import time
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--interface-index',type=int,required=True)
parser.add_argument('--port',default='COM11')
args=parser.parse_args()
if args.interface_index<=0:parser.error('interface-index must be positive')
nonce=secrets.token_bytes(6)
result={'dhcp_discover':False,'dhcp_request':False,'board_dora':False,'board_dns':False}
raw=bytearray()
stop=threading.Event()
server=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
server.setsockopt(socket.SOL_SOCKET,socket.SO_BROADCAST,1)
server.setsockopt(socket.IPPROTO_IP,31,struct.pack('!I',args.interface_index))  # Windows IP_UNICAST_IF
server.bind(('0.0.0.0',67))
server.settimeout(.3)

def options(data):
    at=240
    found={}
    while at<len(data):
        code=data[at];at+=1
        if code==255:return found
        if code==0:continue
        if at==len(data):raise ValueError('DHCP_OPTION_LENGTH_MISSING')
        size=data[at];at+=1
        if size>len(data)-at or code in found:raise ValueError('DHCP_OPTION_INVALID')
        found[code]=data[at:at+size];at+=size
    raise ValueError('DHCP_END_MISSING')

def reply_to(packet,message):
    data=bytearray(240)
    data[:4]=bytes((2,1,6,0))
    data[4:8]=packet[4:8]
    data[10:12]=packet[10:12]
    data[16:20]=socket.inet_aton('192.168.0.1')
    data[20:24]=socket.inet_aton('192.168.0.10')
    data[28:44]=packet[28:44]
    data[236:240]=bytes((99,130,83,99))
    data+=bytes((53,1,message,54,4))+socket.inet_aton('192.168.0.10')
    data+=bytes((1,4))+socket.inet_aton('255.255.255.0')
    data+=bytes((6,4))+socket.inet_aton('192.168.0.10')
    data+=bytes((51,4))+struct.pack('!I',120)+bytes((255,))
    return bytes(data)+bytes(max(0,300-len(data)))

def serve():
    try:
        phase=1
        while not stop.is_set() and phase<4:
            try:data,peer=server.recvfrom(1500)
            except socket.timeout:continue
            if len(data)<240 or data[4:8]!=nonce[:4]:continue
            if data[:3]!=bytes((1,1,6)) or peer!=('0.0.0.0',68):
                raise ValueError('DHCP_CLIENT_OR_SOURCE_INVALID')
            if data[10:12]!=b'\x80\x00' or any(data[12:28]) or data[236:240]!=b'\x63\x82\x53\x63':
                raise ValueError('DHCP_HEADER_INVALID')
            opt=options(data)
            if opt.get(53)!=bytes((phase,)):raise ValueError('DHCP_SEQUENCE_INVALID')
            if phase==3 and (opt.get(50)!=socket.inet_aton('192.168.0.1') or
                             opt.get(54)!=socket.inet_aton('192.168.0.10')):
                raise ValueError('DHCP_REQUEST_LEASE_INVALID')
            result['dhcp_discover' if phase==1 else 'dhcp_request']=True
            response=reply_to(data,2 if phase==1 else 5)
            server.sendto(response,('255.255.255.255',68))
            phase+=2
    except (OSError,ValueError) as error:
        result['server_error']=str(error)

worker=threading.Thread(target=serve)
worker.start()
try:
    with serial.Serial(args.port,115200,timeout=.05,write_timeout=10,
                       xonxoff=False,rtscts=False,dsrdtr=False) as port:
        def command(text,seconds=15):
            tag=secrets.token_hex(6)
            marker=('DORA_DONE_'+tag).encode()
            start=len(raw)
            port.write((text+"; printf '\\nDORA_DONE_%s\\n' '"+tag+"'\r").encode())
            deadline=time.monotonic()+seconds
            while time.monotonic()<deadline:
                raw.extend(port.read(65536))
                if marker in raw[start:]:return bytes(raw[start:])
            raise RuntimeError('DORA_UART_TIMEOUT')
        command("printf 'DORA_READY\\n'",5)
        blob=base64.b64encode(Path('work/lan1-dhcp-dns-test.elf').read_bytes()).decode()
        encoded='\n'.join(blob[i:i+64] for i in range(0,len(blob),64))
        port.write(("base64 -d > /tmp/lan1-dhcp-dns-test <<'DORA_BINARY'\n"+encoded+'\nDORA_BINARY\n').encode())
        token=base64.b64encode(nonce).decode()
        response=command('chmod 700 /tmp/lan1-dhcp-dns-test; printf %s '+token+
                         ' | base64 -d | timeout 30 /tmp/lan1-dhcp-dns-test; echo DORA_HELPER_RC=$?',40)
        result['board_dora']=b'DHCP_DORA_PASS' in response
        result['board_dns']=b'DNS_A_RESPONSE_PASS' in response
        result['board_rc_zero']=b'DORA_HELPER_RC=0' in response
finally:
    stop.set();worker.join(2);server.close()
    Path('work/lan-dhcp-dns-console.raw').write_bytes(raw)
    result['pass']=all(result.get(key,False) for key in
                       ('dhcp_discover','dhcp_request','board_dora','board_dns','board_rc_zero')) and 'server_error' not in result
    Path('work/lan-dhcp-dns-result.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps(result,indent=2))
raise SystemExit(0 if result['pass'] else 1)
