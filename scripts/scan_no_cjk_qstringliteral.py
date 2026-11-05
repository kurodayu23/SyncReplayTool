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


def strip_line_comment(line):
    """去掉行注释，但不动字符串里的内容。

    为什么需要（2026-11-05 实测）：检查 3 要在源码里找 u8"\\xNN" 这种形态，
    而**注释里恰恰要写这个形态当例子**（说明「这么写是错的」）。
    不去掉注释的话，扫描器会把自己写的说明当成违规 —— 实测就撞上了：
    判据的注释里写了 u8"\\xE5\\xA4\\x8D..."，构建前门禁直接红了。

    实现要点：逐字符扫，跟踪引号状态，只有**不在字符串里**的 // 才是注释起点。
    这样 URL（"http://…"）不会被误截断。
    """
    out = []
    i = 0
    n = len(line)
    quote = None
    while i < n:
        c = line[i]
        if quote:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(line[i + 1])
                i += 2
                continue
            if c == quote:
                quote = None
            i += 1
            continue
        if c in ("'", '"'):
            quote = c
            out.append(c)
            i += 1
            continue
        if c == "/" and i + 1 < n and line[i + 1] == "/":
            break
        out.append(c)
        i += 1
    return "".join(out)


def scan_u8_hex_escapes(root):
    """检查 3：u8"..." 字面量里不许用 \\xNN 字节转义写中文。

    为什么（2026-11-05 实测，Qt 5.9.6 / MSVC2015 /utf-8 探针）：
      QString::fromUtf8(u8"\\xE5\\xA4\\x8D\\xE5\\x88\\xB6...")  -> 15 个 Latin-1 字符（乱码）
      QString::fromUtf8(   "\\xE5\\xA4\\x8D\\xE5\\x88\\xB6...")  ->  5 个汉字（对）
      QObject::tr(          "\\xE5\\xA4\\x8D\\xE5\\x88\\xB6...") ->  5 个汉字（对）

    原因：u8 字面量里 \\xNN 表示**字符** U+00NN，整个字面量的字节是这些字符的
    UTF-8 编码（每个 2 字节）；而普通窄字面量里 \\xNN 直接是原始字节。
    ⇒ u8 字面量里写中文只能用 \\uXXXX（码位），用 \\xNN 一定解成乱码。

    危害是**静默**的：字节还是非 ASCII，交付门禁的「中文字符串齐全」照样过，
    但运行时界面上是乱码；而自检里比对同一句文案的判据也跟着一起错，
    于是「产品确实打了提示、判据却判 failMsg=0」——判据自己把 bug 盖住了。

    这条检查不依赖字面量里有没有汉字（那里根本没有汉字字节），所以必须扫源码。
    """
    hits = []
    pat = re.compile(r'u8"((?:[^"\\]|\\.)*)"')
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "release", "debug")]
        for fn in sorted(filenames):
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                continue
            p = os.path.join(dirpath, fn)
            txt = io.open(p, "r", encoding="utf-8-sig", newline="").read()
            for ln, line in enumerate(txt.split("\n"), 1):
                code = strip_line_comment(line)
                for m in pat.finditer(code):
                    s = m.group(1)
                    if re.search(r'\\x[0-9A-Fa-f]{2}', s):
                        hits.append((p, ln, s))
    return hits


def _iter_escapes(s):
    """逐个吐出字符串字面量里的转义序列，正确处理 \\\\ （转义的反斜杠）。

    为什么必须处理 \\\\：字面量里的 "\\\\Users" 会被正则当成"\\U"开头的 UCN，
    而 "U" 后面跟的是 "sers"（不是十六进制）⇒ 误报。
    转义的反斜杠要整对跳过，后面的 u/U 只是普通字母。
    """
    i = 0
    n = len(s)
    while i < n:
        c = s[i]
        if c == "\\" and i + 1 < n:
            yield (i, s[i + 1])
            i += 2
            continue
        i += 1


def scan_broken_ucn(root):
    """检查 4：字符串字面量里的 \\uXXXX / \\UXXXXXXXX 必须是**完整**的。

    为什么（2026-11-05 实测）：把一段中文拆成多个相邻字面量时，很容易在
    \\uXXXX 的中间断开，例如：

        u8"\\u8FD9\\u628A\\u94"      <- 只剩 2 位十六进制
        u8"A5\\u5319..."            <- 上一行的 \\u94 和这里的 A5 原本是一个 UCN

    后果分两种编译器：
      · GCC（麒麟交叉编译用的就是它）直接报错：
        error: incomplete universal character name \\u94
        ⇒ 麒麟版**编译不出来**，交付包里的二进制是旧的。
      · MSVC 2015 **照收不误**，Windows 版照编照过 ⇒ 只有编麒麟才炸。

    也就是说：只跑 Windows 编译的话，这个错会被完全放过；必须靠扫描器
    在 Windows 上就拦下来（或者每轮门禁都真编麒麟）。
    """
    hits = []
    # 匹配所有窄字面量（含 u8"..." / L"..." / "..."），u8 前缀在引号外面，
    # 落在字面量内容之外，不影响内容检查。
    pat = re.compile(r'"((?:[^"\\]|\\.)*)"')
    hexd = set("0123456789abcdefABCDEF")
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "release", "debug")]
        for fn in sorted(filenames):
            if not fn.endswith((".cpp", ".h", ".hpp", ".cc")):
                continue
            p = os.path.join(dirpath, fn)
            txt = io.open(p, "r", encoding="utf-8-sig", newline="").read()
            for ln, line in enumerate(txt.split("\n"), 1):
                code = strip_line_comment(line)
                for m in pat.finditer(code):
                    s = m.group(1)
                    for pos, nxt in _iter_escapes(s):
                        if nxt not in ("u", "U"):
                            continue
                        need = 4 if nxt == "u" else 8
                        tail = s[pos + 2: pos + 2 + need]
                        if len(tail) < need or any(ch not in hexd for ch in tail):
                            hits.append((p, ln, "\\%s%s" % (nxt, tail)))
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

    u8hits = scan_u8_hex_escapes(root)
    if u8hits:
        print()
        print("FAIL：发现 %d 处「u8 字面量里用 \\xNN 字节转义写中文」" % len(u8hits))
        for p, ln, s in u8hits:
            print("  %s:%d  %s" % (p, ln, s))
        print()
        print("改法：u8\"\\xNN...\"  ->  u8\"\\uNNNN...\"（码位，不是字节）")
        print("      实测：fromUtf8(u8\"\\xE5\\xA4\\x8D\") 解成 3 个 Latin-1 字符（乱码），")
        print("            fromUtf8(\"\\xE5\\xA4\\x8D\") 与 tr(\"\\xE5\\xA4\\x8D\") 都解成「复」。")
    else:
        print("OK：%s 下没有「u8 字面量里用 \\xNN 写中文」" % root)

    ucn = scan_broken_ucn(root)
    if ucn:
        print()
        print("FAIL：发现 %d 处「被截断的 \\uXXXX / \\UXXXXXXXX」" % len(ucn))
        for p, ln, s in ucn:
            print("  %s:%d  %s" % (p, ln, s))
        print()
        print("改法：转义序列不能跨行断开 —— 一个 \\uXXXX 必须 4 位十六进制写在**同一个**字面量里。")
        print("      GCC（麒麟交叉编译）会直接报 incomplete universal character name，")
        print("      而 MSVC 会照收不误 => 只编 Windows 看不出来。")
    else:
        print("OK：%s 下没有「被截断的 \\uXXXX / \\UXXXXXXXX」" % root)

    return 1 if (hits or bad or u8hits or ucn) else 0


if __name__ == "__main__":
    sys.exit(main())
