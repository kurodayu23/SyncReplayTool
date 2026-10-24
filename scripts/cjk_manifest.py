#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
生成「中文文案 -> 交付包二进制命中」的**清单表**（JSON）。

设计取舍
--------
1. **复用** scripts/verify_binary_strings.py 的字面量抽取器，不重写。
   那个文件已经解决了三个真坑：
     - 源码里的 \\xNN / \\uNNNN 逐字节转义（必须先按真实字节解码）；
     - 注释里的引号内容不算文案（不剥注释会假失败）；
     - #ifdef Q_OS_WIN 两边的文案只有一边会进二进制（必须按平台求值）。
   本文件只 import 它，不改它。

2. **额外记录命中的是哪种字节形态**（UTF-8 / UTF-16LE）。
   为什么必须记：
     - Qt5 的 QStringLiteral 展开成 u"..."（char16_t），落进二进制的是
       **UTF-16LE**；
     - QString::fromUtf8(u8"...") 落的是 **UTF-8**；
     - 只按 ASCII/UTF-8 搜，QStringLiteral 那一整片会**全部漏掉**，
       闸门会误报「源码写了但没编进去」。这一条踩过坑。

3. 清单表每行字段：
     text          文案本身
     src_file      文案出自哪个源文件（相对源码根）
     src_mtime     该源文件的 mtime
     hit           是否在二进制里命中
     hit_encoding  命中方式：utf-8 / utf-16le / none / allowlist
     utf8_hit      UTF-8 字节序列是否命中
     utf16le_hit   UTF-16LE 字节序列是否命中

用法
----
    python scripts/cjk_manifest.py --src <源码目录> --bin <二进制> \
                                   --platform win|linux [--out <manifest.json>]

退出码：0 = 全部命中；1 = 有缺失；2 = 参数/环境错误。
"""
import argparse
import datetime
import hashlib
import io
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    import verify_binary_strings as vbs
except ImportError as exc:  # pragma: no cover
    sys.stderr.write("找不到 verify_binary_strings.py（应在 scripts/ 同目录）：%s\n" % exc)
    sys.exit(2)


def selftest():
    """自检：证明 **UTF-16LE 分支真的能用**（不是摆设）。

    为什么要有这个自检：
        Qt5 的 QStringLiteral 展开成 u"..."（char16_t），落进二进制的是
        **UTF-16LE**；QString::fromUtf8(u8"...") 落的是 UTF-8。
        本工程约定统一用后者（build-win.ps1 的构建前门禁禁止
        QStringLiteral + 裸中文），所以真实包里的文案全是 UTF-8，
        UTF-16LE 分支平时一次都命中不了 —— 不自检就不知道它是不是坏的。

    自检做法：造一个**只含 UTF-16LE 字节**的假二进制，断言
    (a) UTF-8 搜不到；(b) UTF-16LE 搜得到；命中方式记为 utf-16le。
    """
    probes = [
        ("已到点，即将开始", "QStringLiteral 倒计时边界态"),
        ("同步进行中，结束后补跑", "QStringLiteral 倒计时边界态"),
        ("data 目录", "文件选择对话框"),
    ]
    blob = b""
    for text, _ in probes:
        blob += text.encode("utf-16-le") + b"\x00\x00"
    fails = []
    for text, why in probes:
        u8 = text.encode("utf-8")
        u16 = text.encode("utf-16-le")
        hit8 = u8 in blob
        hit16 = u16 in blob
        enc = "utf-8" if hit8 else ("utf-16le" if hit16 else "none")
        ok = (not hit8) and hit16
        print("  [%s] %-24s UTF-8命中=%s UTF-16LE命中=%s 命中方式=%s  (%s)"
              % ("OK" if ok else "FAIL", text, hit8, hit16, enc, why))
        if not ok:
            fails.append(text)
    if fails:
        print("自检失败：%d 条 UTF-16LE 文案没被搜出来" % len(fails))
        return 1
    print("自检通过：UTF-16LE 分支可用（%d 条）" % len(probes))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=False, help="源码目录")
    ap.add_argument("--bin", required=False, help="被检查的二进制")
    ap.add_argument("--platform", choices=sorted(vbs.PLATFORM_MACROS))
    ap.add_argument("--out", default="", help="清单表 JSON 输出路径（缺省打到 stdout）")
    ap.add_argument("--selftest", action="store_true", help="自检 UTF-16LE 搜索分支")
    args = ap.parse_args()

    if args.selftest:
        return selftest()

    if not args.src or not args.bin or not args.platform:
        ap.error("需要 --src / --bin / --platform（或用 --selftest）")

    src_root, binary = args.src, args.bin
    if not os.path.isdir(src_root):
        sys.stderr.write("源码目录不存在：%s\n" % src_root)
        return 2
    if not os.path.isfile(binary):
        sys.stderr.write("二进制不存在：%s\n" % binary)
        return 2

    blob = open(binary, "rb").read()
    lits = vbs.collect_literals(src_root, args.platform)
    allow, allow_path = vbs.load_allowlist(src_root)

    rows = []
    missing = 0
    for text, path in sorted(lits.items()):
        rel = os.path.relpath(path, src_root)
        row = {
            "text": text,
            "src_file": rel,
            "src_mtime": datetime.datetime.fromtimestamp(
                os.path.getmtime(path)).strftime("%Y-%m-%d %H:%M:%S"),
            "utf8_hit": False,
            "utf16le_hit": False,
            "hit": False,
            "hit_encoding": "none",
        }
        if text in allow:
            row["hit"] = True
            row["hit_encoding"] = "allowlist"
            rows.append(row)
            continue
        u8 = text.encode("utf-8")
        u16 = text.encode("utf-16-le")
        row["utf8_hit"] = u8 in blob
        row["utf16le_hit"] = u16 in blob
        if row["utf8_hit"]:
            row["hit"] = True
            row["hit_encoding"] = "utf-8"
        elif row["utf16le_hit"]:
            row["hit"] = True
            row["hit_encoding"] = "utf-16le"
        if not row["hit"]:
            missing += 1
        rows.append(row)

    manifest = {
        "binary": os.path.abspath(binary),
        "binary_bytes": len(blob),
        "binary_sha256": hashlib.sha256(blob).hexdigest().upper(),
        "binary_mtime": datetime.datetime.fromtimestamp(
            os.path.getmtime(binary)).strftime("%Y-%m-%d %H:%M:%S"),
        "src_root": os.path.abspath(src_root),
        "platform": args.platform,
        "allowlist": os.path.abspath(allow_path) if os.path.isfile(allow_path) else "",
        "total": len(rows),
        "hit": sum(1 for r in rows if r["hit"]),
        "missing": missing,
        "hit_by_encoding": {
            "utf-8": sum(1 for r in rows if r["hit_encoding"] == "utf-8"),
            "utf-16le": sum(1 for r in rows if r["hit_encoding"] == "utf-16le"),
            "allowlist": sum(1 for r in rows if r["hit_encoding"] == "allowlist"),
            "none": sum(1 for r in rows if r["hit_encoding"] == "none"),
        },
        "rows": rows,
    }

    text = json.dumps(manifest, ensure_ascii=False, indent=2)
    if args.out:
        outdir = os.path.dirname(os.path.abspath(args.out))
        if outdir and not os.path.isdir(outdir):
            os.makedirs(outdir)
        with io.open(args.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        sys.stdout.write("清单表已写：%s（%d 条，命中 %d，缺 %d）\n"
                         % (args.out, manifest["total"], manifest["hit"], manifest["missing"]))
    else:
        sys.stdout.write(text + "\n")

    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
