#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Generate a C include table from Mesa/Freedreno a8xx_perfcntrs.xml.

Usage:
  python3 generate_a8xx_perf_table.py a8xx_perfcntrs.xml > a8xx_perf_table.inc
"""
from __future__ import annotations
import re
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# KGSL/adreno perfcounter group IDs. SP=0x0a and ALWAYSON=0x1b match the
# proven working probes. The contiguous early groups match the classic Adreno
# group ordering used by KGSL. Keep this table near the generator so it is easy
# to patch if a vendor kernel uses a different ID for newer/less-used groups.
GROUP_IDS = {
    "cp": 0x00,
    "rbbm": 0x01,
    "pc": 0x02,
    "vfd": 0x03,
    "hlsq": 0x04,
    "vpc": 0x05,
    "tse": 0x06,
    "ras": 0x07,
    "uche": 0x08,
    "tp": 0x09,
    "sp": 0x0A,
    "rb": 0x0B,
    "vsc": 0x0C,
    "ccu": 0x0D,
    "lrz": 0x0E,
    "cmp": 0x0F,
    "gbif": 0x11,
    "gbif_pwr": 0x12,
    "alwayson": 0x1B,
    "gmu_xoclk": 0x1C,
    "gmu_gmuclk": 0x1D,
    "gmu_perf": 0x1E,
    "ufc": 0x1F,
}

ENUM_RE = re.compile(r"^a8xx_(.+)_perfcounter_select$")

def cstr(s: str) -> str:
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'

def main() -> int:
    if len(sys.argv) != 2:
        print("usage: generate_a8xx_perf_table.py a8xx_perfcntrs.xml", file=sys.stderr)
        return 2

    root = ET.parse(sys.argv[1]).getroot()
    ns = {"n": root.tag.split('}')[0].strip('{')} if root.tag.startswith('{') else {}

    rows = []
    for enum in root.findall("n:enum" if ns else "enum", ns):
        enum_name = enum.attrib.get("name", "")
        m = ENUM_RE.match(enum_name)
        if not m:
            continue
        group_name = m.group(1)
        if group_name not in GROUP_IDS:
            print(f"warning: no group id for {group_name}; skipping", file=sys.stderr)
            continue
        gid = GROUP_IDS[group_name]
        for val in enum.findall("n:value" if ns else "value", ns):
            raw_name = val.attrib["name"]
            selector = int(val.attrib["value"], 0)
            short = re.sub(r"^A8XX_PERF_", "", raw_name)
            rows.append((group_name.upper(), gid, selector, raw_name, short))

    print("/*")
    print(" * Auto-generated from Mesa/Freedreno a8xx_perfcntrs.xml.")
    print(" * SPDX-License-Identifier: MIT")
    print(" * See THIRD_PARTY_NOTICES.md. Do not edit by hand.")
    print(" */")
    print("static const struct counter_desc k_counters[] = {")
    for group, gid, selector, raw_name, short in rows:
        print(f"  {{{cstr(group)}, 0x{gid:02x}, {selector}u, {cstr(raw_name)}, {cstr(short)}}},")
    print("};")
    print("static const size_t k_num_counters = sizeof(k_counters) / sizeof(k_counters[0]);")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())
