#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""sha256sum-style listing of a tree, so it can be diffed against the
receiver's `find . -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum`.

Output format is byte-identical to coreutils sha256sum:
    <hash><two spaces>./<relative path>
sorted by the raw bytes of the path (same order as `LC_ALL=C sort -z`).

Usage: python tree_hash.py <root> [suffix]
Prints: <file count> <sha256 of the whole listing>
"""
import hashlib
import os
import sys


def main():
    root = sys.argv[1]
    suffix = sys.argv[2] if len(sys.argv) > 2 else None

    items = []
    for dirpath, dirnames, filenames in os.walk(root):
        for nm in filenames:
            full = os.path.join(dirpath, nm)
            rel = "./" + os.path.relpath(full, root).replace(os.sep, "/")
            items.append(rel.encode("utf-8"))
    items.sort()

    h = hashlib.sha256()
    total = 0
    out = []
    for relb in items:
        rel = relb.decode("utf-8")
        full = os.path.join(root, rel[2:].replace("/", os.sep))
        if suffix and not rel.lower().endswith("." + suffix.lower()):
            continue
        fh = hashlib.sha256()
        with open(full, "rb") as f:
            while True:
                b = f.read(1 << 20)
                if not b:
                    break
                fh.update(b)
        line = ("%s  %s\n" % (fh.hexdigest(), rel)).encode("utf-8")
        h.update(line)
        total += 1
        if len(out) < 50:
            out.append(line.decode("utf-8"))
    print("%d %s" % (total, h.hexdigest()))
    for l in out[:10]:
        sys.stdout.write("  " + l)


if __name__ == "__main__":
    main()
