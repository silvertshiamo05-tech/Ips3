#!/usr/bin/env python3
# Ips3: builds an XDVDFS (Xbox 360 disc) image from a folder, for testing the runtime's disc
# reader without a real disc.
#
#   make_xiso.py <folder> <out.iso> [--partition 0xFD90000]
#
# --partition places the file system where a real disc of that format has its game partition
# (XGD2: 0xFD90000); the default 0 makes a compact "rebuilt" image. The output is sparse.
import argparse
import os
import struct

SECTOR = 0x800
MAGIC = b"MICROSOFT*XBOX*MEDIA"


class Node:
    def __init__(self, name, path, is_dir):
        self.name = name
        self.path = path
        self.is_dir = is_dir
        self.children = []
        self.sector = 0
        self.size = 0


def scan(path, name=""):
    node = Node(name, path, os.path.isdir(path))
    if node.is_dir:
        for entry in sorted(os.listdir(path), key=str.upper):
            node.children.append(scan(os.path.join(path, entry), entry))
    else:
        node.size = os.path.getsize(path)
    return node


def entry_size(child):
    return (14 + len(child.name.encode()) + 3) & ~3


def layout_table(children):
    """Serialises a directory as a balanced binary tree. Returns (bytes, fixups) where fixups
    lists (offset, child) for the sector/size fields to fill once children are placed."""
    order = []

    def build(items):
        if not items:
            return None
        mid = len(items) // 2
        return (items[mid], build(items[:mid]), build(items[mid + 1:]))

    tree = build(children)

    # Pre-order placement; an entry never straddles a sector.
    offsets = {}
    cursor = 0

    def place(node):
        nonlocal cursor
        if node is None:
            return
        child, left, right = node
        size = entry_size(child)
        if cursor // SECTOR != (cursor + size - 1) // SECTOR:
            cursor = (cursor // SECTOR + 1) * SECTOR
        offsets[id(child)] = cursor
        order.append(node)
        cursor += size
        place(left)
        place(right)

    place(tree)
    table = bytearray(b"\xff" * (((cursor + SECTOR - 1) // SECTOR) * SECTOR or SECTOR))
    fixups = []
    for child, left, right in order:
        offset = offsets[id(child)]
        left_offset = offsets[id(left[0])] // 4 if left else 0
        right_offset = offsets[id(right[0])] // 4 if right else 0
        name = child.name.encode()
        struct.pack_into("<HHIIBB", table, offset, left_offset, right_offset, 0, 0, 0x10 if child.is_dir else 0x80, len(name))
        table[offset + 14:offset + 14 + len(name)] = name
        pad = entry_size(child) - 14 - len(name)
        table[offset + 14 + len(name):offset + 14 + len(name) + pad] = b"\x00" * pad
        fixups.append((offset, child))
    return table if children else bytearray(), fixups


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("out")
    parser.add_argument("--partition", type=lambda v: int(v, 0), default=0)
    args = parser.parse_args()

    root = scan(args.folder)
    next_sector = 33
    tables = []  # (node, table, fixups)

    def allocate(node):
        nonlocal next_sector
        table, fixups = layout_table(node.children)
        node.size = len(table)
        node.sector = next_sector if table else 0
        next_sector += len(table) // SECTOR
        tables.append((node, table, fixups))
        for child in node.children:
            if child.is_dir:
                allocate(child)
            else:
                child.sector = next_sector
                next_sector += max(1, (child.size + SECTOR - 1) // SECTOR)

    allocate(root)

    with open(args.out, "wb") as out:
        base = args.partition
        descriptor = bytearray(SECTOR)
        descriptor[0:20] = MAGIC
        struct.pack_into("<II", descriptor, 20, root.sector, root.size)
        descriptor[0x7EC:0x7EC + 20] = MAGIC
        out.seek(base + 32 * SECTOR)
        out.write(descriptor)

        for node, table, fixups in tables:
            for offset, child in fixups:
                struct.pack_into("<II", table, offset + 4, child.sector, child.size)
            if table:
                out.seek(base + node.sector * SECTOR)
                out.write(table)
            for child in node.children:
                if not child.is_dir:
                    out.seek(base + child.sector * SECTOR)
                    with open(child.path, "rb") as source:
                        out.write(source.read())

        out.truncate(base + next_sector * SECTOR)

    print(f"{args.out}: {next_sector} sectors at 0x{args.partition:X}")


if __name__ == "__main__":
    main()
