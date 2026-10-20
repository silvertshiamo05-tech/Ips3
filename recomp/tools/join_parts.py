#!/usr/bin/env python3
# Ips3: puts files saved by the Disc Unpacker page back together.
#
#   join_parts.py <out folder> part01.zip part02.zip ...
#
# Split zips hold pieces named "<disc path>.<byte offset>" (10 digits); unsplit zips hold whole
# files. Every piece's CRC is checked, and each file must come out contiguous from offset 0.
import collections
import os
import re
import sys
import zipfile

PIECE = re.compile(r"^(.*)\.(\d{10})$")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__ or "usage: join_parts.py <out folder> <zip>...")

    out_dir = sys.argv[1]
    pieces = collections.defaultdict(list)  # path -> [(offset, zip path, member)]

    for archive in sys.argv[2:]:
        with zipfile.ZipFile(archive) as z:
            bad = z.testzip()
            if bad is not None:
                sys.exit(f"{archive}: {bad} is corrupt")
            for info in z.infolist():
                m = PIECE.match(info.filename)
                path, offset = (m.group(1), int(m.group(2))) if m else (info.filename, 0)
                pieces[path].append((offset, info.file_size, archive, info.filename))

    for path, parts in sorted(pieces.items()):
        parts.sort()
        target = os.path.join(out_dir, *path.split("/"))
        os.makedirs(os.path.dirname(target), exist_ok=True)
        expected = 0
        with open(target, "wb") as out:
            for offset, size, archive, member in parts:
                if offset != expected:
                    sys.exit(f"{path}: piece at {offset} but {expected} bytes so far (a part is missing)")
                with zipfile.ZipFile(archive) as z, z.open(member) as src:
                    while chunk := src.read(1 << 22):
                        out.write(chunk)
                expected += size
        print(f"{path}: {expected} bytes from {len(parts)} piece(s)")


if __name__ == "__main__":
    main()
