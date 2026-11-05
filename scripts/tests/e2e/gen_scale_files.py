#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate a large local tree for the scale test.

Layout: <root>/dNN/<name>.spe  -- nested dirs, mixed names (ascii / chinese /
spaces / long).  Every file gets deterministic content so the byte-level
reconciliation on the receiver is meaningful.

Usage: python gen_scale_files.py <root> <count>
Prints: <count> <dirs>
"""
import io
import os
import sys


def main():
    root = sys.argv[1]
    count = int(sys.argv[2])
    per_dir = 1000

    names = []
    # a few special names sprinkled in, then plain numbered ones
    specials = [
        "中文 文件.spe",
        "with space.spe",
        "dash-and_dot.v2.spe",
        "UPPER.SPE",
    ]

    made = 0
    d = 0
    while made < count:
        sub = os.path.join(root, "d%03d" % d)
        os.makedirs(sub, exist_ok=True)
        for i in range(per_dir):
            if made >= count:
                break
            if made < len(specials):
                nm = specials[made]
            else:
                nm = "f%06d.spe" % made
            body = ("spe-%d-" % made).encode("utf-8") + b"x" * (made % 97) + b"-END\n"
            with open(os.path.join(sub, nm), "wb") as f:
                f.write(body)
            made += 1
        d += 1
    print("%d %d" % (made, d))


if __name__ == "__main__":
    main()
