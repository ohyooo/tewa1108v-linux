#!/usr/bin/env python3
"""RAM-only clock/reset/FAM diagnostics; PIE traffic gate is retained."""

import argparse
import gzip
import hashlib
import json
import struct
import zlib
from pathlib import Path

from dtb_probe import read_blob
from enable_reboot import meta
from fip_patches import STUB_SIZE, _find_fdt_node_prop, edit_initrd_end, rebuild_fip
from prepare_stage3 import newc_record, parse_newc
from tewa_image import parse

ORDER = (
    "hsan-lsw-crg", "hsan-mdio", "hsan-lsw-csr", "hsan-lsw-platform",
    "hsan-lsw-dp", "hsan-lsw-pfe", "hsan-gemac-lan1", "hsan-net-v300",
)
BASE_INITRD = "64bf1a3d0419799b592a8e40879790b5757e360ffa564816c52a380f2fb2e9d8"
BASE_FIP = "4367de379795f1605f8dd31f5e95325c30b419e54b499dc49b134d60803db5e4"


def prop(blob, path, key, value):
    blob, _, before = read_blob(blob, 0)
    start, found = _find_fdt_node_prop(blob, path, key)
    h = list(struct.unpack_from(">10I", blob))
    strings = blob[h[3]:h[3] + h[8]]
    name = key.encode("ascii") + b"\0"
    index = strings.find(name)
    extra = b"" if index >= 0 else name
    if index < 0:
        index = len(strings)
    if found:
        start = found[0] - 12
        end = found[0] + ((found[1] + 3) & ~3)
    else:
        end = start
    if value is None:
        if not found:
            raise ValueError("FDT deletion target absent: " + path + "/" + key)
        record = b""
        extra = b""
    else:
        record = struct.pack(">III", 3, len(value), index)
        record += value + bytes((-len(value)) % 4)
    delta = len(record) - (end - start)
    result = bytearray(blob[:start] + record + blob[end:h[3] + h[8]])
    result += extra + blob[h[3] + h[8]:]
    h[1] = len(result)
    h[3] += delta
    h[8] += len(extra)
    h[9] += delta
    struct.pack_into(">10I", result, 0, *h)
    result, _, after = read_blob(bytes(result), 0)
    expected = {n: dict(p) for n, p in before.items()}
    if value is None:
        del expected[path][key]
    else:
        expected[path][key] = value
    if after != expected:
        raise ValueError("Unexpected FDT mutation: " + path + "/" + key)
    return result


def build(base_payload, artifacts, helper, diagnostic, output, fam_shadow=False,
          boot_reset=False, arm_fam=False, eqm=False, queue_routing=False,
          pie_reset=False, pie_map=False, pfe_globals=False, pie_acp=False,
          pfe_opp=False, lan1_opp=False, lan1_pub=False, cpu_ifc=False,
          cpu_diagnostic=False, noncoherent_dma=False):
    if arm_fam and not (fam_shadow and boot_reset):
        raise ValueError("FAM hardware init requires CPU staging and cold boot reset")
    if eqm and not arm_fam:
        raise ValueError("EQM requires this boot's FAM initialization")
    if queue_routing and not eqm:
        raise ValueError("Queue routing requires ordinary EQM initialization")
    if pie_map and not (queue_routing and pie_reset):
        raise ValueError("PIE mapping requires verified queue routes and an empty PIE owner")
    if pfe_globals and not pie_map:
        raise ValueError("PFE globals require this boot's completed DP reset and PIE map")
    if pie_acp and not pie_reset:
        raise ValueError("PIE ACP requires a verified empty cold boot owner")
    if pfe_opp and not pfe_globals:
        raise ValueError("PFE OPP requires global initialization")
    if lan1_opp and not pfe_opp:
        raise ValueError("LAN1 OPP queue route requires the global OPP prelude")
    if lan1_pub and not pfe_globals:
        raise ValueError("LAN1 PUB requires global initialization")
    if cpu_ifc and not (lan1_pub and lan1_opp):
        raise ValueError("CPU IFC requires the LAN1 PUB and OPP hardware plans")
    if cpu_diagnostic and not (cpu_ifc and pie_acp):
        raise ValueError("CPU DMA diagnostic requires the complete IFC/OPP/PUB/ACP bootstrap")
    if noncoherent_dma and not cpu_diagnostic:
        raise ValueError("Noncoherent DMA requires CPU diagnostic mode")
    pair = json.loads((base_payload / "PAIR.json").read_text(encoding="utf-8"))
    root = (base_payload / pair["initrd"]["name"]).read_bytes()
    carrier = (base_payload / pair["carrier"]["name"]).read_bytes()
    if hashlib.sha256(root).hexdigest() != BASE_INITRD:
        raise ValueError("Base diagnostic initrd pin mismatch")
    if hashlib.sha256(carrier).hexdigest() != BASE_FIP:
        raise ValueError("Base diagnostic FIP pin mismatch")
    original = parse(carrier)
    image = (artifacts / "Image").read_bytes()
    if image[56:60] != b"ARM\x64":
        raise ValueError("Not an ARM64 Image")
    modules = {name: (artifacts / (name + ".ko")).read_bytes() for name in ORDER}
    for name, data in modules.items():
        if data[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", data, 18)[0] != 183:
            raise ValueError("Not an ARM64 module: " + name)
        if b"vermagic=7.3.5 SMP preempt aarch64\0" not in data:
            raise ValueError("Module vermagic mismatch: " + name)
    dtb = original.dtb
    clock = "/clk@14980000"
    edits = {
        clock: {
            "compatible": b"hsan,tewa-lsw-crg\0",
            "reg": struct.pack(">4I", 0x14980000, 0x100, 0x10108000, 0x1000),
            "reg-names": b"lsw\0peri\0",
            "hsan,reset-providers": struct.pack(">2I", 0xa2, 0xf2),
            "status": b"okay\0",
        },
        clock + "/lswclk_gate@002401": {"status": b"okay\0"},
        clock + "/lswclk_gate@002402": {"status": b"okay\0"},
        "/reset@0": {"status": b"okay\0"},
        "/reset@2": {"status": b"okay\0"},
        "/lsw_dp": {"status": b"okay\0", "hsan,experimental-enable-dp-clocks": b""},
        "/lsw_pfe": {"status": b"okay\0", "hsan,experimental-enable-pfe-clock": b""},
    }
    if fam_shadow:
        edits["/lsw_dp"].update({
            "hsan,experimental-prepare-fam-dma-shadow": b"",
            "hsan,observed-oem-fam-runtime-geometry": struct.pack(
                ">6I", 0x90000000, 0x2400000, 0x1000000, 8192, 512, 2048),
            "hsan,observed-fam-address-table": struct.pack(
                ">3I", 0x90400000, 2048, 16384),
        })
    if boot_reset:
        edits["/lsw_dp"]["hsan,experimental-reset-dp-at-boot"] = b""
    if arm_fam:
        if b"experimental_allow_lsw_writes=1\0" not in helper.read_bytes():
            raise ValueError("Loader does not pin the required CSR parameter")
        for key in (
            "hsan,experimental-program-global-registers",
            "hsan,experimental-program-dp-register-init",
            "hsan,experimental-arm-fam-oem-registers",
        ):
            edits["/lsw_dp"][key] = b""
    queue_profile = None
    if eqm:
        queue_profile = json.loads(
            Path(__file__).with_name("oem_queue_profile.json").read_text(encoding="utf-8")
        )
        rows = queue_profile["rows"]
        if len(rows) != 30 or any(len(row) != 11 for row in rows):
            raise ValueError("OEM queue record geometry mismatch")
        bitmap = int(queue_profile["queue_owner_port_bitmap_u64"], 16)
        active = sum(1 << row[1] for row in rows if row[0] == 1)
        if active != bitmap or queue_profile["queue_rows"] != 192:
            raise ValueError("OEM ordinary queue ownership mismatch")
        edits["/lsw_dp"].update({
            "port_bitmap": struct.pack(">Q", bitmap),
            "hsan,observed-oem-queue-table": struct.pack(">330I", *(v for row in rows for v in row)),
            "hsan,experimental-program-eqm-bootstrap": b"",
            "hsan,experimental-program-eqm-per-port": b"",
            "hsan,experimental-verify-eqm-readback": b"",
        })
    if queue_routing:
        edits["/lsw_dp"]["hsan,experimental-program-queue-routing"] = b""
    if pie_reset:
        edits["/pie@12080000"] = {"hsan,experimental-reset-pie-at-boot": b""}
    if cpu_diagnostic:
        edits["/lsw_dp"]["hsan,experimental-program-ips-queues"] = b""
        edits["/lsw_dp"]["hsan,experimental-enable-ips-before-fam"] = b""
        edits["/lsw_pfe"]["hsan,experimental-program-ofc-control"] = b""
        edits["/lsw_pfe"]["hsan,experimental-program-pa-prelude"] = b""
        edits["/lsw_pfe"]["hsan,experimental-program-cpu-opp"] = b""
        edits["/pie@12080000"].update({
            "hsan,experimental-dma-enable": b"",
            "hsan,experimental-cpu-lan1-diagnostic": b"",
            "hsan,experimental-program-pie-global-registers": b"",
        })
        edits["/gemac@14400000"] = {
            "hsan,experimental-program-oem-gemac-link": b"",
            "hsan,experimental-read-gemac-oem-counters": b"",
        }
        edits["/lsw_pfe"]["hsan,experimental-read-pfe-oem-diagnostics"] = b""
    if noncoherent_dma:
        edits["/pie@12080000"]["dma-coherent"] = None
        edits["/pie@12080000"]["hsan,experimental-noncoherent-dma"] = b""
    if pie_map:
        edits["/lsw_dp"].update({
            "hsan,observed-pie-gro-channels": struct.pack(">2I", 4, 4),
            "hsan,experimental-program-pie-v300-map": b"",
            "hsan,experimental-verify-pie-map-data": b"",
        })
    if pfe_globals:
        edits["/lsw_pfe"]["hsan,experimental-program-global-registers"] = b""
        edits["/lsw_pfe"]["hsan,observed-pie-gro-count"] = struct.pack(">I", 4)
    if pie_acp:
        edits["/pie@12080000"]["hsan,experimental-program-pie-acp-at-boot"] = b""
    if pfe_opp:
        edits["/lsw_pfe"]["hsan,experimental-program-opp-prelude"] = b""
    if lan1_opp:
        edits["/lsw_pfe"]["hsan,experimental-program-lan1-opp"] = b""
    if lan1_pub:
        edits["/lsw_pfe"].update({
            "hsan,experimental-program-lan1-pub": b"",
            "hsan,observed-lan1-vlan-id": struct.pack(">I", 0),
        })
    ifc_profile = None
    if cpu_ifc:
        ifc_profile = json.loads(
            Path(__file__).with_name("oem_ifc_profile.json").read_text(encoding="utf-8")
        )
        trace_bytes = Path(__file__).with_name("oem_ifc_init_trace.json").read_bytes()
        rows = ifc_profile["rows"]
        if (ifc_profile["oem_module_sha256"] !=
                "7591ef5077c540b40f60b4bd5b30620775ab025c4389e353f0f5a1e261e1804e" or
                hashlib.sha256(trace_bytes).hexdigest() != ifc_profile["trace_sha256"] or
                ifc_profile["rule_indices"] != list(range(1, 12)) or
                ifc_profile["hash_allocation_start"] != 0 or
                ifc_profile["hash_reserved_rows"] != 1 or
                len(rows) != 11 or any(len(row) != 15 for row in rows)):
            raise ValueError("OEM IFC source trace or geometry mismatch")
        words = [value for row in rows for value in row]
        crc = zlib.crc32(struct.pack("<165I", *words), 0xffffffff) ^ 0xffffffff
        if crc != int(ifc_profile["linux_crc32_le_seed0"], 16):
            raise ValueError("OEM IFC little-endian image CRC mismatch")
        edits["/lsw_pfe"].update({
            "hsan,experimental-program-ifc-defaults": b"",
            "hsan,experimental-ifc-cpu-path": b"",
            "hsan,observed-ifc-hash-allocation": struct.pack(">I", 0),
            "hsan,observed-ifc-default-words": struct.pack(">165I", *words),
            "hsan,observed-ifc-default-crc32": struct.pack(">I", crc),
        })
    for path, props in edits.items():
        for key, value in props.items():
            dtb = prop(dtb, path, key, value)
    _, _, nodes = read_blob(dtb, 0)
    # Hash every property of the changed nodes and the unchanged DMA owner.
    checks = sorted(set(edits) | {
        "/pie@12080000", "/gemac@14400000", "/mdio0",
        "/mdio0/ethernet-phy@3", "/watchdog@0x10128000",
    })
    live = "".join(
        f"{hashlib.sha256(value).hexdigest()}  /sys/firmware/devicetree/base{path}/{key}\n"
        for path in checks for key, value in sorted(nodes[path].items())
    ).encode()
    replacement = {f"tewa/modules/{n}.ko": b for n, b in modules.items()}
    replacement.update({
        "tewa/driver-test": helper.read_bytes(),
        "tewa/diagnose.sh": diagnostic.read_bytes(),
        "tewa/live-dt-sha256.txt": live,
        "tewa/module-sha256.txt": "".join(
            f"{hashlib.sha256(b).hexdigest()}  /tewa/modules/{n}.ko\n"
            for n, b in modules.items()
        ).encode(),
    })
    raw = gzip.decompress(root)
    records = parse_newc(raw)
    chunks = []
    inode = max(row[3][0] for row in records) + 1
    pending = dict(replacement)
    for start, end, name, fields, _ in records:
        if name in pending:
            chunks.append(newc_record(name, pending.pop(name), fields[0], fields[1]))
        elif name == "TRAILER!!!":
            for new_name, data in pending.items():
                chunks.append(newc_record(new_name, data, inode, 0o100444))
                inode += 1
            chunks.append(newc_record(name, b"", inode, 0))
        else:
            chunks.append(raw[start:end])
    cpio = b"".join(chunks)
    reread = {n: cpio[d:d + f[6]] for _, _, n, f, d in parse_newc(cpio)}
    if any(reread.get(n) != b for n, b in replacement.items()):
        raise ValueError("CPIO replacement verification failed")
    for start, end, name, fields, data_at in records:
        if name not in replacement and name != "TRAILER!!!":
            if reread[name] != raw[data_at:data_at + fields[6]]:
                raise ValueError("Unrelated CPIO content changed: " + name)
    initrd = gzip.compress(cpio, compresslevel=6, mtime=0)
    dtb = edit_initrd_end(dtb, len(initrd))
    payload = original.payload[:STUB_SIZE] + image
    payload += bytes((-len(payload)) % 8)
    header = bytearray(original.header)
    struct.pack_into(">I", header, 12, len(payload))
    struct.pack_into(">I", header, 24, zlib.crc32(payload))
    struct.pack_into(">I", header, 4, 0)
    struct.pack_into(">I", header, 4, zlib.crc32(header))
    fip = rebuild_fip(original, bytes(header), payload, dtb)
    output.mkdir(parents=True, exist_ok=True)
    for key, data in (("initrd", initrd), ("carrier", fip)):
        (output / pair[key]["name"]).write_bytes(data)
        pair[key].update(meta(data))
    pair["image"] = meta(image)
    pair["modules"] = {name: meta(data) for name, data in modules.items()}
    pair["driver_test"] = meta(helper.read_bytes())
    pair["purpose"] = "OEM CCF/reset providers and read-only DP/PFE probe; DMA gates retained"
    pair["fam_shadow_requested"] = fam_shadow
    pair["boot_reset_requested"] = boot_reset
    pair["fam_hardware_init_requested"] = arm_fam
    pair["eqm_ordinary_queue_init_requested"] = eqm
    pair["queue_routing_requested"] = queue_routing
    pair["queue_default_shaping_requested"] = queue_routing
    pair["pie_boot_reset_requested"] = pie_reset
    pair["pie_map_requested"] = pie_map
    pair["pfe_globals_requested"] = pfe_globals
    pair["pie_acp_requested"] = pie_acp
    pair["pfe_opp_requested"] = pfe_opp
    pair["lan1_opp_requested"] = lan1_opp
    pair["lan1_pub_requested"] = lan1_pub
    pair["cpu_ifc_requested"] = cpu_ifc
    pair["cpu_lan1_diagnostic_requested"] = cpu_diagnostic
    pair["pie_global_init_requested"] = cpu_diagnostic
    pair["cpu_opp_requested"] = cpu_diagnostic
    pair["noncoherent_dma_requested"] = noncoherent_dma
    if ifc_profile:
        pair["ifc_profile"] = ifc_profile
    if eqm:
        pair["queue_profile"] = queue_profile
    pair["module_parameters"] = {
        "hsan-lsw-csr": "experimental_allow_lsw_writes=1" if arm_fam else ""
    }
    if arm_fam:
        pair["purpose"] = "OEM boot reset and FAM init ACK; no PIE traffic, STOP/owner return not qualified"
    elif boot_reset:
        pair["purpose"] = "OEM boot reset and CPU-owned FAM staging; no DMA start"
    if eqm:
        pair["purpose"] = "FAM and ordinary EQM initialization/readback; auxiliary queues/PIE/PFE/GRO/STOP remain unqualified"
    if cpu_diagnostic:
        pair["purpose"] = "Single cold-boot CPU LAN1 open diagnostic with eight owned RX rings; aggregate delivery/traffic/STOP acceptance not claimed"
    pair["fam_shadow_provenance"] = (
        "OEM /proc memory/config and fam_status; hi_fam_init_buf .data+0x70; "
        "geometry provenance only; hardware ownership requires same-boot ACK evidence"
    )
    pair["base_diagnostic_revision"] = pair.pop("diagnostic_revision", None)
    original_contents = {
        name: raw[data_at:data_at + fields[6]]
        for _, _, name, fields, data_at in records
    }
    pair["diagnostic_revision"] = {
        "script": meta(diagnostic.read_bytes()),
        "base_initrd_sha256": BASE_INITRD,
        "base_carrier_sha256": BASE_FIP,
        "changed_cpio_entries": sorted(
            name for name, data in replacement.items()
            if original_contents.get(name) != data
        ),
        "fdt_modified_nodes": sorted(set(edits) | {"/chosen"}),
        "new_kernel": meta(image),
        "preserved_components": ["BL31", "trampoline", "OEM overlays"],
    }
    pair["board_test"] = "NOT_RUN"
    pair["stored_dtb_sha256"] = hashlib.sha256(dtb).hexdigest()
    pair["initrd_end"] = hex(0x88000000 + len(initrd))
    pair["live_dt_profile"] = "Exact hashes of all modified nodes and PIE/GEMAC/MDIO"
    (output / "PAIR.json").write_text(json.dumps(pair, indent=2) + "\n", encoding="utf-8")
    return pair


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    for key in ("base-payload", "artifacts", "helper", "diagnostic", "output"):
        parser.add_argument("--" + key, required=True, type=Path)
    parser.add_argument("--fam-shadow", action="store_true")
    parser.add_argument("--boot-reset", action="store_true")
    parser.add_argument("--arm-fam", action="store_true")
    parser.add_argument("--eqm", action="store_true")
    parser.add_argument("--queue-routing", action="store_true")
    parser.add_argument("--pie-reset", action="store_true")
    parser.add_argument("--pie-map", action="store_true")
    parser.add_argument("--pfe-globals", action="store_true")
    parser.add_argument("--pie-acp", action="store_true")
    parser.add_argument("--pfe-opp", action="store_true")
    parser.add_argument("--lan1-opp", action="store_true")
    parser.add_argument("--lan1-pub", action="store_true")
    parser.add_argument("--cpu-ifc", action="store_true")
    parser.add_argument("--cpu-diagnostic", action="store_true")
    parser.add_argument("--noncoherent-dma", action="store_true")
    args = parser.parse_args()
    result = build(args.base_payload, args.artifacts, args.helper, args.diagnostic,
                   args.output, args.fam_shadow, args.boot_reset, args.arm_fam,
                   args.eqm, args.queue_routing, args.pie_reset, args.pie_map,
                   args.pfe_globals, args.pie_acp, args.pfe_opp, args.lan1_opp,
                   args.lan1_pub, args.cpu_ifc, args.cpu_diagnostic,
                   args.noncoherent_dma)
    print(json.dumps({k: result[k] for k in ("initrd", "carrier")}, indent=2))
