#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
门禁：源码里的中文字面量不许出坏字。

为什么
----
Qt5 的 QStringLiteral 展开成 u"..."（char16_t 字面量）。
MSVC 编译 u"" "中文" 时，是先按**当前代码页**解码这段窄串、再转成 UTF-16；
`/utf-8` 在 VS2015 上救不了这个转换。实测产物（build-final 的 exe）里
"data 目录" 存成了 UTF-16 的 29 94 BC E1 8D 7D（U+9429 U+E1BC U+7D8D），
而不是"目录"（76 EE 55 5F）—— 编译期就错了，不是显示层问题。

本工程早就在 FileTableModel.cpp:13-16 踩过同一个坑并写下约定：
「fromUtf8 才是按字节序列解码」，因此全工程的中文都用
QString::fromUtf8(u8"…") 或 U("\\xE6\\x97\\xA5") 这类写法。
这 5 处是漏网的，本脚本就是防止再漏网的闸门。

第二个检查（本脚本新增）
------------------------
上面的门禁只拦「QStringLiteral + 裸中文」，**拦不住"转义里写错字节"**。

实测（2026-10-23）：源码里 5 处客户可见文案是这么写的
    "\\xE6\\x9C\\xAC\\xE8\\xB7\\x91"   // 本跑   ← 应为「本次」
    "\\xE7\\xBB\\x93\\xE6\\x9D\\x82"   // 结杂   ← 应为「结束」
    "\\xE5\\xB7\\xB2\\xE5\\x88\\xB0\\xE6\\x97\\xB6" // 已到时 ← 应为「已到点」
    "\\xE4\\xB8\\x80\\xE8\\xB7\\xB5"   // 一践   ← 应为「一键」
    "\\xE6\\x8B\\x89\\xE9\\x80\\x89"   // 拉选   ← 应为「勾选」
每一个字节都"合法"，合起来却是错字，而且**客户在界面上看得见**。

关键：**grep 汉字看不见它们** —— 文件里没有"已到时"这三个汉字的字面字节，
grep 必然零命中，**无论坏字在不在**（判据恒为绿，没有鉴别力）。
所以必须把字面量按真实字节解码出来再比对。

按**词组**判、不按单字判：时 / 拉 / 杂 本身都是常用字（超时、实时、拉、杂志），
按单字判会把一大片正常文案全报成坏字 —— 那是噪音，不是判据。

用法
----
    python scripts/scan_no_cjk_qstringliteral.py            # 扫 src/
    python scripts/scan_no_cjk_qstringliteral.py <目录>      # 扫指定目录

退出码：0 = 干净；1 = 有违规（并逐条列出）。
"""
import io
import os
import re
import sys

PAT = re.compile(r'QStringLiteral\(\s*"((?:[^"\\]|\\.)*)"\s*\)')
LIT = re.compile(r'"((?:[^"\\]|\\.)*)"')

# 坏词组 -> 正确词组
BAD_WORDS = {
    "本跑": "本次",
    "结杂": "结束",
    "已到时": "已到点",
    "一践": "一键",
    "拉选": "勾选",
    "拉入": "勾入",
    # 第二轮 UI 审计（findings/gui-click.md 的 F-1/F-2/F-5）到本轮才修掉的 3 条：
    # 每一条都是客户在界面上看得见的坏字，grep 汉字看不见，必须按字节解码才拦得住。
    "初化完成": "初始化完成",
    "上这个问题": "这个问题",
    "反配": "配置",
    "手动勾入": "手动取消",
    "手动写入的内容": "手动取消的文件",
}


def decode_escapes(raw):
    """把 C 字符串字面量里的 \\xNN / \\uNNNN / \\n 等还原成真实字节，再按 UTF-8 解。"""
    out = bytearray()
    i = 0
    while i < len(raw):
        c = raw[i]
        if c == "\\" and i + 1 < len(raw):
            nxt = raw[i + 1]
            if nxt == "x" and i + 3 < len(raw):
                try:
                    out.append(int(raw[i + 2:i + 4], 16))
                    i += 4
                    continue
                except ValueError:
                    pass
            mapping = {"n": 10, "t": 9, "r": 13, "\\": 92, '"': 34, "0": 0}
            if nxt in mapping:
                out.append(mapping[nxt])
                i += 2
                continue
            if nxt == "u":
                j = i + 2
                h = ""
                while j < len(raw) and len(h) < 4 and raw[j] in "0123456789abcdefABCDEF":
                    h += raw[j]
                    j += 1
                if len(h) == 4:
                    out.extend(chr(int(h, 16)).encode("utf-8"))
                    i = j
                    continue
            # 非 ASCII 的原样字符：按 UTF-8 字节进缓冲（不能 append(ord())，
            # ord() 对汉字会 >255，bytearray.append 会直接抛 ValueError）。
            out.extend(nxt.encode("utf-8"))
            i += 2
            continue
        out.extend(c.encode("utf-8"))
        i += 1
    try:
        return out.decode("utf-8")
    except UnicodeDecodeError:
        return None


def scan_cjk(root):
    """检查 1：QStringLiteral + 裸中文。"""
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "release", "debug")]
        for fn in filenames:
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                continue
            p = os.path.join(dirpath, fn)
            try:
                txt = io.open(p, "r", encoding="utf-8-sig").read()
            except UnicodeDecodeError:
                hits.append((p, 0, "<文件不是合法 UTF-8>"))
                continue
            for i, line in enumerate(txt.split("\n"), 1):
                for m in PAT.finditer(line):
                    s = m.group(1)
                    if any(ord(c) > 127 for c in s):
                        hits.append((p, i, s))
    return hits


def scan_bad_words(root):
    """检查 2：字面量按真实字节解码后含坏词组。"""
    hits = []
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "release", "debug")]
        for fn in sorted(filenames):
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                continue
            p = os.path.join(dirpath, fn)
            txt = io.open(p, "r", encoding="utf-8-sig", newline="").read()
            for ln, line in enumerate(txt.split("\n"), 1):
                for m in LIT.finditer(line):
                    s = decode_escapes(m.group(1))
                    if not s:
                        continue
                    bad = sorted({c for c in BAD_WORDS if c in s})
                    if bad:
                        hits.append((p, ln,
                                     " / ".join(bad),
                                     " / ".join(BAD_WORDS[c] for c in bad),
                                     s))
    return hits


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "src")
    if not os.path.isdir(root):
        print("目录不存在：%s" % root)
        return 2

    hits = scan_cjk(root)
    if hits:
        print("FAIL：发现 %d 处「QStringLiteral + 裸中文」" % len(hits))
        for p, i, s in hits:
            print("  %s:%d  %s" % (p, i, s))
        print()
        print("改法：QStringLiteral(\"中文\")  ->  QString::fromUtf8(u8\"中文\")")
        print("      （或用本工程已有的 U(\"\\xE6\\x97\\xA5\") 辅助函数）")
    else:
        print("OK：%s 下没有「QStringLiteral + 裸中文」" % root)

    bad = scan_bad_words(root)
    if bad:
        print()
        print("FAIL：发现 %d 处「转义里写错字节」的中文" % len(bad))
        for p, ln, b, g, s in bad:
            print("  %s:%d  坏词[%s] 应为[%s]  %s" % (p, ln, b, g, s))
        print()
        print("改法：把该字面量里对应的 \\xNN 字节换成正确汉字的 UTF-8 字节。")
        print("      注意：自检里匹配同一句文案的字符串要**同步改**，否则自检会反过来变红。")
    else:
        print("OK：%s 下没有「转义里写错字节」的中文" % root)

    return 1 if (hits or bad) else 0


if __name__ == "__main__":
    sys.exit(main())
