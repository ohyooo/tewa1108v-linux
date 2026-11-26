#!/usr/bin/env python3
"""Repack the pinned RAM pair with read-only watchdog diagnostics only."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path

from enable_reboot import meta
from fip_patches import patch_fip_initrd
from prepare_stage3 import newc_record, parse_newc
from tewa_image import parse

BASE_INITRD = "e0c6a88908fd482e2fc303585e5b3e51fdc3b7030165002749f05695baaf86b1"
BASE_CARRIER = "b575a03f8ad6d3ccb3eb90925e4fde645fd66d946e2a818cf967a053b7e3cbb5"
SCRIPT = "tewa/diagnose.sh"


def build(payload, diagnostic, output):
    pair = json.loads((payload / "PAIR.json").read_text(encoding="utf-8"))
    inputs = {}
    for key, expected in (("initrd", BASE_INITRD), ("carrier", BASE_CARRIER)):
        data = (payload / pair[key]["name"]).read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            raise ValueError("Pinned input mismatch: " + key)
        if any(pair[key][field] != value for field, value in meta(data).items()):
            raise ValueError("Input manifest mismatch: " + key)
        inputs[key] = data

    raw = gzip.decompress(inputs["initrd"])
    records = parse_newc(raw)
    if len({row[2] for row in records}) != len(records):
        raise ValueError("Duplicate CPIO name")
    if sum(row[2] == SCRIPT for row in records) != 1:
        raise ValueError("Expected one diagnostic script")
    replacement = diagnostic.read_bytes()
    chunks = []
    for start, end, name, fields, _ in records:
        if name == SCRIPT:
            chunks.append(newc_record(name, replacement, fields[0], fields[1]))
        else:
            chunks.append(raw[start:end])
    updated_raw = b"".join(chunks)
    updated_records = parse_newc(updated_raw)
    # Every unchanged entry must retain the complete original header and bytes,
    # including PID1, all seven modules, Bash and reboot. No payload execution.
    if len(updated_records) != len(records):
        raise ValueError("CPIO entry count changed")
    for before, after in zip(records, updated_records):
        if before[2] != after[2]:
            raise ValueError("CPIO ordering changed")
        if before[2] != SCRIPT:
            if raw[before[0]:before[1]] != updated_raw[after[0]:after[1]]:
                raise ValueError("Unrelated CPIO entry changed: " + before[2])
        elif updated_raw[after[4]:after[4] + after[3][6]] != replacement:
            raise ValueError("Diagnostic script serialization mismatch")
    initrd = gzip.compress(updated_raw, compresslevel=6, mtime=0)
    carrier = patch_fip_initrd(inputs["carrier"], len(initrd))
    original, updated = parse(inputs["carrier"]), parse(carrier)
    for field in ("bl31", "header", "payload", "overlays"):
        if getattr(original, field) != getattr(updated, field):
            raise ValueError("Boot component changed: " + field)
    for key, data in (("initrd", initrd), ("carrier", carrier)):
        pair[key].update(meta(data))
    pair["stored_dtb_sha256"] = hashlib.sha256(updated.dtb).hexdigest()
    pair["initrd_end"] = hex(0x88000000 + len(initrd))
    pair["purpose"] = "Watchdog class registration and dw_wdt binding diagnostics; original LAN1 gates"
    pair["board_test"] = "NOT_RUN_DIAGNOSTIC_REVISION"
    pair["watchdog"]["status"] = "REGISTRATION_UNKNOWN_OLD_IDENTITY_ONLY_PROBE_INCONCLUSIVE"
    pair["watchdog"]["adapted_fdt_sha256"] = pair["stored_dtb_sha256"]
    pair["diagnostic_revision"] = {
        "script": meta(replacement),
        "base_initrd_sha256": BASE_INITRD,
        "base_carrier_sha256": BASE_CARRIER,
        "changed_cpio_entries": [SCRIPT],
        "fdt_change": "/chosen/linux,initrd-end only",
        "watchdog_sysfs_attributes": "CONFIG_WATCHDOG_SYSFS not enabled in pinned config",
        "registration_evidence": "watchdog0 class directory and dev attribute",
        "driver_evidence": "watchdog0/device/driver resolves to dw_wdt",
    }
    output.mkdir(parents=True, exist_ok=True)
    (output / pair["initrd"]["name"]).write_bytes(initrd)
    (output / pair["carrier"]["name"]).write_bytes(carrier)
    (output / "PAIR.json").write_text(json.dumps(pair, indent=2) + "\n", encoding="utf-8")
    return {key: pair[key] for key in ("initrd", "carrier", "diagnostic_revision")}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--payload", type=Path, required=True)
    parser.add_argument("--diagnostic", type=Path, default=Path(__file__).with_name("diagnose.sh"))
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    print(json.dumps(build(args.payload, args.diagnostic, args.output), indent=2))
