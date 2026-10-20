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

    bctr_tables = {}
    for t in tables:
        # Find the bctr this table belongs to (first bctr at/after its base).
        a = t["base"]
        while word(a) != 0x4E800420:
            a += 4
        bctr_tables[a] = t

    def run_end(start):
        """End of straight-line code from `start`: the first unconditional exit, except that
        a jump table's bctr continues to the end of its furthest case."""
        a, end = start, start
        furthest = start
        while True:
            w = word(a)
            a += 4
            if a - 4 in bctr_tables:
                furthest = max(furthest, max(bctr_tables[a - 4]["labels"]))
                continue
            if w in (0x4E800020, 0x4E800420) or (w >> 26 == 18 and not w & 1):
                if a > furthest:
                    return a

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

    # Small switches compiled as `mtctr rX; bdz case1; bdz case2; ...`. XenonRecomp turns a
    # conditional branch out of a function into a tail call, except the CTR-decrementing
    # forms (bdz/bdnz/bdzf/...), which it always emits as a local goto. Any such branch whose
    # target lies past the function XenonRecomp found means the function was cut short.
    start_set = set(starts)
    entries = {}  # shared tails entered from another function: start -> end
    for idx, fn_start in enumerate(starts):
        fn_end = starts[idx + 1] if idx + 1 < len(starts) else fn_start
        far = 0
        for a in range(fn_start, fn_end, 4):
            w = word(a)
            if w >> 26 != 16 or w & 1:  # bc without link
                continue
            bo = (w >> 21) & 31
            if bo & 0b00100:  # BO bit 2 set: CTR is not decremented
                continue
            target = (a + ((w & 0xFFFC) - (0x10000 if w & 0x8000 else 0))) & 0xFFFFFFFF
            if target >= fn_end:
                far = max(far, target)
            elif target < fn_start and target not in start_set:
                # Backward into another function's tail (the compiler shares identical case
                # code). The recompiler makes this a call, so the tail needs its own entry.
                entries[target] = run_end(target)
        if not far:
            continue
        a = far
        while True:
            w = word(a)
            a += 4
            if w in (0x4E800020, 0x4E800420) or (w >> 26 == 18 and not w & 1):
                break
        spans.append((fn_start, a))

    # Switch cases below the function holding the switch: shared tails again.
    for t in tables:
        j = bisect.bisect_right(starts, t["base"]) - 1
        if j < 0:
            continue
        for label in t["labels"]:
            if label < starts[j] and label not in start_set and label not in entries:
                entries[label] = run_end(label)

    # Anything the previous pass could not resolve: XenonRecomp writes `// ERROR <addr>` where
    # a branch leaves a function for an address that is not a function start (a shared tail
    # reached by an ordinary conditional branch). Give each one an entry point.
    import glob
    import os.path
    for path in glob.glob(os.path.join(os.path.dirname(mapping_cpp), "ppc_recomp.*.cpp")):
        for m in re.finditer(r"// ERROR:? (?:0x)?([0-9A-F]+)", open(path).read()):
            target = int(m.group(1), 16)
            if target not in start_set and target not in entries:
                entries[target] = run_end(target)

    # Keep the bounds from earlier passes: once a function is extended the problem is no longer
    # visible in the new output, and dropping the entry would cut the function again.
    import os
    if os.path.exists(out_path):
        for f in tomllib.load(open(out_path, "rb")).get("functions", []):
            if f.get("entry"):
                entries.setdefault(f["address"], run_end(f["address"]))
            else:
                spans.append((f["address"], f["address"] + f["size"]))

    # Several switches in one function give overlapping spans; XenonRecomp needs one entry.
    spans.sort()
    merged = []
    for lo, hi in spans:
        if merged and lo < merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], hi))
        else:
            merged.append((lo, hi))
    out = dict(merged)

    # Deliberately overlapping: the tail stays in its own function and is emitted again as an
    # entry point of its own.
    entry_starts = set()
    for start, end in entries.items():
        if start not in out:
            out[start] = end
            entry_starts.add(start)

    with open(out_path, "w") as f:
        f.write("functions = [\n")
        for s, e in sorted(out.items()):
            marker = ", entry = true" if s in entry_starts else ""
            f.write(f"    {{ address = 0x{s:X}, size = 0x{e - s:X}{marker} }},\n")
        f.write("]\n")
    print(f"{len(out)} functions given explicit bounds", file=sys.stderr)


if __name__ == "__main__":
    if len(sys.argv) != 6:
        sys.exit("usage: leaf_function_bounds.py IMAGE SWITCH_TOML ppc_func_mapping.cpp PDATA_LO:HI OUT")
    main(*sys.argv[1:])
