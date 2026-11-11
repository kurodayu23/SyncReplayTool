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

    # 只比对"按规则该传"的文件。
    #
    # special-probe 调用探针时传的是 --rules spe，所以无后缀（no-extension）
    # 和尾点（trailing.dot）这两类文件**本来就不该出现在远端**。
    # 原来无差别比对全量 manifest，于是每跑一次都报 2 个 MISSING，
    # 看起来像"产品静默漏传"，实际是判据和被测配置不一致（2026-11-06 实测）。
    #
    # 判定该传：rel 的扩展名（最后一个点之后、非空）为 spe，或 rel 里某个目录段之后
    # 还有名为 *.spe 的段 —— 这里简化成"路径最后一段以 .spe 结尾"，
    # 与 gen_special_files.py 造夹具时的命名一致。
    def should_send(rel):
        base = rel.rsplit("/", 1)[-1]
        if not base.startswith(".") and "." not in base:
            return False                       # 无后缀
        if base.endswith("."):
            return False                       # 尾点
        return base.lower().endswith(".spe")

    skipped = [rel for rel in lok if not should_send(rel)]
    lok_cmp = {r: h for r, h in lok.items() if should_send(r)}

    missing, mismatch, ok = [], [], 0
    for rel, h in sorted(lok_cmp.items()):
        if rel not in remote:
            missing.append(rel)
        elif remote[rel] != h:
            mismatch.append((rel, h, remote[rel]))
        else:
            ok += 1
    extra = sorted(set(remote) - set(lok_cmp))

    print("local files  = %d  (should send = %d, skipped by rules = %d)" % (len(lok), len(lok_cmp), len(skipped)))
    for r in skipped:
        print("   ~ skipped %s" % r)
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
