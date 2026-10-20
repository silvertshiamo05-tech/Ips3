#!/usr/bin/env python3
"""Explicit function boundaries for XenonRecomp, for switch tables in leaf functions.

Functions with a stack frame are listed in .pdata and XenonRecomp sizes them correctly.
Leaf functions are not, and XenonRecomp's boundary analysis treats the `bctr` of a jump
table as a tail call, ending the function before its cases. For every switch table whose
function has no .pdata entry this emits a `functions` entry spanning from the function
start XenonRecomp already found (from ppc_func_mapping.cpp of a previous run) to the first
unconditional exit (blr / b / bctr) at or after the furthest case label.
"""
import bisect
import re
import struct
import sys
import tomllib

BASE = 0x82000000


def main(image_path, switch_toml, mapping_cpp, pdata_range, out_path):
    data = open(image_path, "rb").read()
    word = lambda a: struct.unpack_from(">I", data, a - BASE)[0]

    plo, phi = (int(x, 16) for x in pdata_range.split(":"))
    pdata = []
    for o in range(plo - BASE, phi - BASE, 8):
        begin, flags = struct.unpack_from(">II", data, o)
        if begin:
            pdata.append((begin, begin + ((flags >> 8) & 0x3FFFFF) * 4))
    pdata.sort()
    pstarts = [b for b, _ in pdata]

    starts = sorted(int(m, 16) for m in re.findall(r"\{ 0x([0-9A-F]+), sub_", open(mapping_cpp).read()))
    tables = tomllib.load(open(switch_toml, "rb"))["switch"]

    spans = []
    for t in tables:
        i = bisect.bisect_right(pstarts, t["base"]) - 1
        in_pdata = i >= 0 and pdata[i][0] <= t["base"] < pdata[i][1]
        if in_pdata and all(pdata[i][0] <= l < pdata[i][1] for l in t["labels"]):
            continue  # sized correctly from .pdata

        j = bisect.bisect_right(starts, t["base"]) - 1
        if j < 0:
            continue
        lo = pdata[i][0] if in_pdata else starts[j]

        # Cases can be laid out before the switch; XenonRecomp then splits the function at
        # one of them. Pull the start back to the piece holding the lowest case.
        k = bisect.bisect_right(starts, min(t["labels"])) - 1
        if 0 <= k and starts[k] < lo:
            lo = starts[k]

        a = max(t["labels"])
        while True:
            w = word(a)
            a += 4
            if w in (0x4E800020, 0x4E800420) or (w >> 26 == 18 and not w & 1):  # blr, bctr, b
                break
        hi = max(a, pdata[i][1] if in_pdata else 0)
        spans.append((lo, hi))

    # Several switches in one function give overlapping spans; XenonRecomp needs one entry.
    spans.sort()
    merged = []
    for lo, hi in spans:
        if merged and lo < merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    out = dict(merged)

    with open(out_path, "w") as f:
        f.write("functions = [\n")
        for s, e in sorted(out.items()):
            f.write(f"    {{ address = 0x{s:X}, size = 0x{e - s:X} }},\n")
        f.write("]\n")
    print(f"{len(out)} leaf functions given explicit bounds", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 6:
        sys.exit("usage: leaf_function_bounds.py IMAGE SWITCH_TOML ppc_func_mapping.cpp PDATA_LO:HI OUT")
    main(*sys.argv[1:])
