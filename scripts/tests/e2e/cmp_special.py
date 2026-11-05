#!/usr/bin/env python3
# cmp_special.py - compare the local manifest with the remote sha256sum listing.
# usage: cmp_special.py <manifest.json> <remote-sha256sum.txt>
import json
import sys


def parse_remote(path):
    out = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            if "  " not in line:
                continue
            h, p = line.split("  ", 1)
            p = p.strip()
            if p.startswith("./"):
                p = p[2:]
            p = p.replace("\\", "/")
            out[p] = h
    return out


def main():
    with open(sys.argv[1], "r", encoding="utf-8") as f:
        local = json.load(f)
    remote = parse_remote(sys.argv[2])
    lok = {m["rel"]: m["sha256"] for m in local}

    missing, mismatch, ok = [], [], 0
    for rel, h in sorted(lok.items()):
        if rel not in remote:
            missing.append(rel)
        elif remote[rel] != h:
            mismatch.append((rel, h, remote[rel]))
        else:
            ok += 1
    extra = sorted(set(remote) - set(lok))

    print("local files  = %d" % len(lok))
    print("remote files = %d" % len(remote))
    print("identical    = %d" % ok)
    print("MISSING      = %d" % len(missing))
    for r in missing:
        print("   - %s" % r)
    print("MISMATCH     = %d" % len(mismatch))
    for r, a, b in mismatch:
        print("   ! %s  local=%s remote=%s" % (r, a[:12], b[:12]))
    print("EXTRA on remote = %d" % len(extra))
    for r in extra:
        print("   + %s" % r)
    return 0 if (not missing and not mismatch and not extra) else 1


if __name__ == "__main__":
    sys.exit(main())
