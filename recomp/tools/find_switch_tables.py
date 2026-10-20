#!/usr/bin/env python3
"""Finds the jump tables in an Xbox 360 executable image and writes them in XenonRecomp's
switch-table TOML format.

XenonAnalyse matches a handful of fixed instruction sequences (the ones Sonic Unleashed's
compiler emits) and misses tables whose instructions are scheduled in a different order.
This finds every `bctr`, walks back to the bounds check that guards it
(`cmplwi rIdx, N` + `bgt default`), and then *executes* the instructions between the check
and the `bctr` once per case value. Whatever ends up in CTR is that case's target. That
covers absolute tables, byte/halfword offset tables and any instruction order, without
knowing the compiler's patterns.

Input is the decrypted, decompressed image (base 0x82000000 unless --base is given), as
written by recomp/tools/xex_dump.
"""
import argparse
import struct
import sys

MASK = 0xFFFFFFFF


class Unknown(Exception):
    pass


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def rotl32(x, n):
    n &= 31
    return ((x << n) | (x >> (32 - n))) & MASK if n else x


def mask32(mb, me):
    if mb <= me:
        m = 0
        for b in range(mb, me + 1):
            m |= 1 << (31 - b)
        return m
    return ~mask32(me + 1, mb - 1) & MASK


class Image:
    def __init__(self, data, base, text, functions=()):
        self.data, self.base = data, base
        self.text_lo, self.text_hi = text
        # Sorted function start addresses (from .pdata), for "same function" checks.
        self.functions = sorted(functions)

    def function_bounds(self, addr):
        import bisect
        i = bisect.bisect_right(self.functions, addr) - 1
        if i < 0:
            return None
        end = self.functions[i + 1] if i + 1 < len(self.functions) else self.text_hi
        return self.functions[i], end

    def word(self, addr):
        o = addr - self.base
        if o < 0 or o + 4 > len(self.data):
            raise Unknown("read outside image")
        return struct.unpack_from(">I", self.data, o)[0]

    def u8(self, addr):
        o = addr - self.base
        if o < 0 or o >= len(self.data):
            raise Unknown("read outside image")
        return self.data[o]

    def u16(self, addr):
        o = addr - self.base
        if o < 0 or o + 2 > len(self.data):
            raise Unknown("read outside image")
        return struct.unpack_from(">H", self.data, o)[0]

    def in_text(self, addr):
        return self.text_lo <= addr < self.text_hi


def is_bctr(w):
    return w == 0x4E800420


def decode_cmplwi(w):
    # cmpli crD, L=0, rA, UIMM
    if w >> 26 == 10 and not (w >> 21) & 1:
        return (w >> 23) & 7, (w >> 16) & 31, w & 0xFFFF
    return None


def decode_bc_gt(w, addr):
    """bgt crN, target (BO=12 branch if true, BI=crN*4+1). Returns (cr, target)."""
    if w >> 26 != 16:
        return None
    bo, bi = (w >> 21) & 31, (w >> 16) & 31
    if (bo & 0b11100) == 0b01100 and bi % 4 == 1 and not w & 3:
        return bi // 4, (addr + sext16(w & 0xFFFC)) & MASK
    return None


def execute(img, start, end, idx_reg, idx):
    """Runs the instructions in [start, end) with only idx_reg known. Returns CTR."""
    regs = {idx_reg: idx}
    ctr = None

    def g(r):
        if r not in regs:
            raise Unknown(f"r{r} unknown")
        return regs[r]

    addr = start
    while addr < end:
        w = img.word(addr)
        op = w >> 26
        rd, ra, rb = (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
        imm = w & 0xFFFF
        if op == 15:  # addis / lis
            regs[rd] = ((g(ra) if ra else 0) + (sext16(imm) << 16)) & MASK
        elif op == 14:  # addi / li
            regs[rd] = ((g(ra) if ra else 0) + sext16(imm)) & MASK
        elif op == 24:  # ori
            regs[ra] = g(rd) | imm
        elif op == 25:  # oris
            regs[ra] = g(rd) | (imm << 16)
        elif op == 28:  # andi.
            regs[ra] = g(rd) & imm
        elif op == 21:  # rlwinm
            sh, mb, me = (w >> 11) & 31, (w >> 6) & 31, (w >> 1) & 31
            regs[ra] = rotl32(g(rd), sh) & mask32(mb, me)
        elif op == 32:  # lwz
            regs[rd] = img.word(((g(ra) if ra else 0) + sext16(imm)) & MASK)
        elif op == 34:  # lbz
            regs[rd] = img.u8(((g(ra) if ra else 0) + sext16(imm)) & MASK)
        elif op == 40:  # lhz
            regs[rd] = img.u16(((g(ra) if ra else 0) + sext16(imm)) & MASK)
        elif op == 42:  # lha
            v = img.u16(((g(ra) if ra else 0) + sext16(imm)) & MASK)
            regs[rd] = (v - 0x10000 if v & 0x8000 else v) & MASK
        elif op == 31:
            xo = (w >> 1) & 0x3FF
            ea = lambda: ((g(ra) if ra else 0) + g(rb)) & MASK
            if xo == 23:  # lwzx
                regs[rd] = img.word(ea())
            elif xo == 87:  # lbzx
                regs[rd] = img.u8(ea())
            elif xo == 279:  # lhzx
                regs[rd] = img.u16(ea())
            elif xo == 343:  # lhax
                v = img.u16(ea())
                regs[rd] = (v - 0x10000 if v & 0x8000 else v) & MASK
            elif xo == 266:  # add
                regs[rd] = (g(ra) + g(rb)) & MASK
            elif xo == 40:  # subf
                regs[rd] = (g(rb) - g(ra)) & MASK
            elif xo == 24:  # slw
                s = g(rb) & 63
                regs[ra] = (g(rd) << s) & MASK if s < 32 else 0
            elif xo == 444:  # or / mr
                regs[ra] = g(rd) | g(rb)
            elif xo == 922:  # extsh
                v = g(rd) & 0xFFFF
                regs[ra] = (v - 0x10000 if v & 0x8000 else v) & MASK
            elif xo == 954:  # extsb
                v = g(rd) & 0xFF
                regs[ra] = (v - 0x100 if v & 0x80 else v) & MASK
            elif xo == 467 and ((w >> 11) & 0x3FF) == 0x120:  # mtctr
                ctr = g(rd)
            else:
                raise Unknown(f"x-form {xo} at {addr:X}")
        elif op in (36, 38, 44, 48, 50, 52, 54) or (op == 62 and w & 3 == 0):
            # stw/stb/sth, lfs/lfd, stfs/stfd, std: the scheduler interleaves these with the
            # switch preamble; non-update forms touch no GPR, so they cannot change CTR.
            pass
        elif op == 30:  # rldicl and friends: only clrldi rX,rY,32 (zero-extend) is common
            if (w >> 2) & 7 == 0 and ((w >> 11) & 31) == 0 and ((w >> 1) & 1) == 0:
                mb = ((w >> 6) & 31) | (((w >> 5) & 1) << 5)
                if mb >= 32:
                    regs[ra] = g(rd) & ((1 << (64 - mb)) - 1)
                else:
                    regs[ra] = g(rd)
            else:
                raise Unknown(f"rld* at {addr:X}")
        else:
            raise Unknown(f"opcode {op} at {addr:X}")
        addr += 4
    if ctr is None:
        raise Unknown("ctr not set")
    return ctr


def writes_reg(w, r):
    """Conservative: does this instruction (as run by execute) write GPR r?"""
    op = w >> 26
    rd, ra = (w >> 21) & 31, (w >> 16) & 31
    if op in (15, 14, 32, 34, 40, 42):
        return rd == r
    if op in (24, 25, 28, 21, 30):
        return ra == r
    if op == 31:
        xo = (w >> 1) & 0x3FF
        if xo in (23, 87, 279, 343, 266, 40):
            return rd == r
        if xo in (24, 444, 922, 954):
            return ra == r
    return False


def find_tables(img, max_back=24, max_cases=8192):
    tables, failures = [], []
    for addr in range(img.text_lo, img.text_hi, 4):
        if not is_bctr(img.word(addr)):
            continue

        # Walk back to the guarding cmplwi; remember the bgt after it.
        found = None
        bgt = None
        for k in range(1, max_back + 1):
            a = addr - 4 * k
            w = img.word(a)
            if is_bctr(w) or w == 0x4E800020 or (w >> 26 == 18):  # previous bctr/blr/b: left the block
                break
            b = decode_bc_gt(w, a)
            if b:
                bgt = (a, b)
            c = decode_cmplwi(w)
            if c and bgt and bgt[1][0] == c[0] and bgt[0] > a:
                found = (a, c, bgt)
                break
        if not found:
            # The compiler drops the bounds check when it can prove the range (a masked
            # index). Recognise lis/addi + rlwinm + lwzx + mtctr and read entries for as long
            # as they land inside this function.
            t = unguarded_table(img, addr, max_back)
            if t:
                tables.append(t)
            else:
                failures.append((addr, "no bounds check"))  # usually a virtual call, not a switch
            continue

        cmp_addr, (cr, idx_reg, n), (bgt_addr, (_, default)) = found
        if n + 1 > max_cases:
            failures.append((addr, f"{n + 1} cases"))
            continue

        start = bgt_addr + 4
        if any(writes_reg(img.word(a), idx_reg) for a in range(start, addr, 4)):
            failures.append((addr, f"r{idx_reg} overwritten before bctr"))
            continue

        try:
            labels = [execute(img, start, addr, idx_reg, i) for i in range(n + 1)]
        except Unknown as e:
            failures.append((addr, str(e)))
            continue

        if not all(img.in_text(l) for l in labels):
            failures.append((addr, "target outside .text"))
            continue

        tables.append({"base": start, "bctr": addr, "r": idx_reg, "default": default, "labels": labels})
    return tables, failures


def unguarded_table(img, bctr, max_back):
    bounds = img.function_bounds(bctr)
    if not bounds or not img.functions:
        return None
    fn_lo, fn_hi = bounds

    # Find mtctr <- lwzx rT, rBase, rOff, with rOff = rlwinm(rIdx, 2, ...).
    window = [bctr - 4 * k for k in range(max_back, 0, -1) if bctr - 4 * k >= fn_lo]
    mtctr_src = lwzx_at = idx_reg = off_reg = None
    for a in reversed(window):
        w = img.word(a)
        if mtctr_src is None and w >> 26 == 31 and (w >> 1) & 0x3FF == 467 and ((w >> 11) & 0x3FF) == 0x120:
            mtctr_src = (w >> 21) & 31
        elif mtctr_src is not None and lwzx_at is None and w >> 26 == 31 and (w >> 1) & 0x3FF == 23 and (w >> 21) & 31 == mtctr_src:
            lwzx_at, off_reg = a, (w >> 11) & 31
        elif lwzx_at is not None and w >> 26 == 21 and (w >> 16) & 31 == off_reg and ((w >> 11) & 31) == 2:
            idx_reg = (w >> 21) & 31
            break
    if idx_reg is None:
        return None

    # Start right after the last write to the index register, so it is live and unchanged.
    start = window[0]
    for a in window:
        if a < bctr and writes_reg(img.word(a), idx_reg):
            start = a + 4

    labels = []
    for i in range(1024):
        try:
            target = execute(img, start, bctr, idx_reg, i)
        except Unknown:
            break
        if not (fn_lo <= target < fn_hi) or target & 3:
            break
        labels.append(target)
    if len(labels) < 2:
        return None

    return {"base": start, "bctr": bctr, "r": idx_reg, "default": 0, "labels": labels}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("out")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0x82000000)
    ap.add_argument("--text", required=True, help="start:end of .text, virtual addresses (hex)")
    ap.add_argument("--pdata", help="start:end of .pdata (hex); enables unguarded tables")
    args = ap.parse_args()

    lo, hi = (int(x, 16) for x in args.text.split(":"))
    data = open(args.image, "rb").read()
    functions = []
    if args.pdata:
        # RUNTIME_FUNCTION entries: { begin address, packed length/flags }, big-endian.
        plo, phi = (int(x, 16) for x in args.pdata.split(":"))
        for o in range(plo - args.base, phi - args.base, 8):
            begin = struct.unpack_from(">I", data, o)[0]
            if begin:
                functions.append(begin)
    img = Image(data, args.base, (lo, hi), functions)
    tables, failures = find_tables(img)

    with open(args.out, "w") as f:
        f.write("# Generated by recomp/tools/find_switch_tables.py\n")
        for t in tables:
            f.write("\n[[switch]]\n")
            f.write(f"base = 0x{t['base']:X}\n")
            f.write(f"r = {t['r']}\n")
            f.write(f"default = 0x{t['default']:X}\n")
            f.write("labels = [\n")
            for l in t["labels"]:
                f.write(f"    0x{l:X},\n")
            f.write("]\n")

    reasons = {}
    for _, why in failures:
        key = why.split(" at ")[0]
        reasons[key] = reasons.get(key, 0) + 1
    print(f"switch tables: {len(tables)}", file=sys.stderr)
    print("bctr sites not resolved as switches:", file=sys.stderr)
    for k, v in sorted(reasons.items(), key=lambda kv: -kv[1]):
        print(f"  {v:5d}  {k}", file=sys.stderr)


if __name__ == "__main__":
    main()
