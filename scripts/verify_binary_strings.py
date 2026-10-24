#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
门禁：交付包里的二进制，必须与**当前源码**一致。

为什么
------
实测（2026-10-23，Tool-66 交付）：交付包是 01:44 打的，
而工作树里的源码一直改到 02:42 —— 包里的二进制比源码**老了近一小时**。
后果是包里**没有**"已到点，即将开始""同步进行中，结束后补跑"这两个
倒计时边界态，也没有新的间隔提示与新的同步完成文案；
客户拿到的程序和说明书对不上，而**没有任何一道门禁**发现这件事。

这道门禁有**两道**独立判据，缺一不可：

1. **时间戳**：二进制必须比所有源文件都新。
   —— 直接堵"包比源码旧"，且不依赖任何字符串抽取的正确性。
2. **文案存在性**：把源码里每条中文文案按真实字节取出来，逐条到二进制里找；
   缺一条即失败。 —— 堵"源码写了但没编进去"（旧二进制、漏文件、编译错文件）。

两条容易踩的坑
--------------
1. **源码里这些字是 \\xNN / \\uNNNN 逐字节转义存放的**，
   文件里没有汉字的字面字节 —— 必须先按真实字节解码再比对。
2. **注释里的引号内容不算文案**。不先去掉注释就会把
   `// 参考："已到点"` 当成一条必须出现在二进制里的文案 → 假失败。

判定：二进制里以 **UTF-8** 或 **UTF-16LE** 两种形态之一出现即算存在
（QString::fromUtf8(u8"…") 落 UTF-8；QStringLiteral 落 UTF-16）。

平台分支
--------
同一个源码在 Windows 与麒麟下编出两个不同的二进制，
`#ifdef Q_OS_WIN` / `#else` 两边的文案只有一边会进二进制。
所以必须**按目标平台求值预处理条件**再抽文案，
否则 Linux 专属文案会误报"Windows exe 里没有"（假失败）。
Qt 5.9 的宏表见 PLATFORM_MACROS。

用法
----
    python scripts/verify_binary_strings.py --src <src目录> --bin <二进制> --platform win|linux

退出码：0 = 通过；1 = 有缺失（并逐条列出）；2 = 参数/环境错误。
"""
import argparse
import io
import os
import re
import sys

# Qt 5.9.6 qsystemdetection.h 的真实定义（实测自本机 Qt 头文件）：
#   Q_OS_WIN   ← Q_OS_WIN32 || Q_OS_WIN64 || Q_OS_WINRT
#   Q_OS_UNIX  ← Windows 下被 #undef，Linux 下定义
PLATFORM_MACROS = {
    "win": {
        "_WIN32": True, "_WIN64": True, "WIN32": True,
        "Q_OS_WIN": True, "Q_OS_WIN32": True, "Q_OS_WIN64": True,
        "Q_OS_UNIX": False, "Q_OS_LINUX": False,
        "NOMINMAX": False,
    },
    "linux": {
        "_WIN32": False, "_WIN64": False, "WIN32": False,
        "Q_OS_WIN": False, "Q_OS_WIN32": False, "Q_OS_WIN64": False,
        "Q_OS_UNIX": True, "Q_OS_LINUX": True,
        "NOMINMAX": True,
    },
}

SRC_EXT = (".cpp", ".h", ".hpp", ".cc")


# --------------------------------------------------------------------------
# 注释剥离：把注释内容换成空格，保留字符串字面量与行结构
# --------------------------------------------------------------------------
def strip_comments(src):
    out = []
    i = 0
    n = len(src)
    in_str = False
    while i < n:
        c = src[i]
        if in_str:
            out.append(c)
            if c == "\\" and i + 1 < n:
                out.append(src[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
            i += 1
            continue
        if c == '"':
            in_str = True
            out.append(c)
            i += 1
            continue
        if c == "'":
            # 字符字面量：QLatin1Char('"') 里也有一个双引号，
            # 不处理的话它会被当成字符串开头，后面的注释就全被当成字符串了
            out.append(c)
            i += 1
            while i < n:
                out.append(src[i])
                if src[i] == "\\" and i + 1 < n:
                    out.append(src[i + 1])
                    i += 2
                    continue
                if src[i] == "'":
                    i += 1
                    break
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            while i < n and src[i] != "\n":
                out.append(" ")
                i += 1
            continue
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            while i < n and not (src[i] == "*" and i + 1 < n and src[i + 1] == "/"):
                out.append("\n" if src[i] == "\n" else " ")
                i += 1
            out.append("  ")
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


# --------------------------------------------------------------------------
# 预处理条件求值
# --------------------------------------------------------------------------
DIRECTIVE = re.compile(r"^[ \t]*#[ \t]*(ifdef|ifndef|if|elif|else|endif)\b[ \t]*(.*)$")


def _eval_expr(expr, macros):
    """求值 #if/#elif 表达式。支持 defined(X)、标识符、! && || ( ) 和 0/1。"""
    expr = expr.strip()
    if not expr:
        return False
    # defined(X) / defined X  -> 1 / 0
    expr = re.sub(r"defined\s*\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)",
                  lambda m: "1" if macros.get(m.group(1), False) else "0", expr)
    expr = re.sub(r"defined\s+([A-Za-z_][A-Za-z0-9_]*)",
                  lambda m: "1" if macros.get(m.group(1), False) else "0", expr)
    # 裸标识符 -> 1 / 0（未知宏按未定义处理）
    expr = re.sub(r"\b([A-Za-z_][A-Za-z0-9_]*)\b",
                  lambda m: "1" if macros.get(m.group(1), False) else "0", expr)
    expr = expr.replace("&&", " and ").replace("||", " or ").replace("!", " not ")
    expr = re.sub(r"\bnot\s*=", "!=", expr)
    try:
        return bool(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307 - 仅处理上面的字符集
    except Exception:
        return False


def active_mask(text, macros):
    """返回与 text 等长的布尔列表：True 表示该字符在目标平台下会被编译进去。"""
    mask = [False] * len(text)
    # 栈：每层是 [当前分支是否生效, 是否已有一个分支生效过]
    stack = []
    pos = 0
    n = len(text)
    while pos < n:
        nl = text.find("\n", pos)
        if nl < 0:
            nl = n
        line = text[pos:nl]
        m = DIRECTIVE.match(line)
        if not m:
            if not stack or all(s[0] for s in stack):
                for k in range(pos, nl):
                    mask[k] = True
            pos = nl + 1
            continue
        kind = m.group(1)
        rest = m.group(2).strip()
        parent_ok = all(s[0] for s in stack)
        if kind in ("ifdef", "ifndef", "if"):
            if not parent_ok:
                stack.append([False, True])
            else:
                if kind == "ifdef":
                    val = macros.get(rest, False)
                elif kind == "ifndef":
                    val = not macros.get(rest, False)
                else:
                    val = _eval_expr(rest, macros)
                stack.append([bool(val), bool(val)])
        elif kind == "elif":
            if not stack:
                continue
            taken = stack[-1][1]
            if not parent_ok or taken:
                stack[-1][0] = False
            else:
                val = _eval_expr(rest, macros)
                stack[-1][0] = bool(val)
                stack[-1][1] = taken or bool(val)
        elif kind == "else":
            if not stack:
                continue
            taken = stack[-1][1]
            if not parent_ok or taken:
                stack[-1][0] = False
            else:
                stack[-1][0] = True
                stack[-1][1] = True
        elif kind == "endif":
            if stack:
                stack.pop()
        pos = nl + 1
    return mask


# --------------------------------------------------------------------------
# 转义解码
# --------------------------------------------------------------------------
def decode_escapes(raw):
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
            out.extend(nxt.encode("utf-8"))
            i += 2
            continue
        out.extend(c.encode("utf-8"))
        i += 1
    try:
        return out.decode("utf-8")
    except UnicodeDecodeError:
        return None


# --------------------------------------------------------------------------
# 字面量抽取
# --------------------------------------------------------------------------
def iter_literals(text, mask):
    """产出 (起始下标, 字面量内容)。跳过字符字面量与原始字符串 R"delim(...)delim"。"""
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            # 原始字符串：R"delim( ... )delim"
            if i >= 1 and text[i - 1] == "R" and (i < 2 or not (text[i - 2].isalnum() or text[i - 2] == "_")):
                p = text.find("(", i + 1)
                if p > 0:
                    delim = text[i + 1:p]
                    term = ")" + delim + '"'
                    q = text.find(term, p)
                    if q >= 0:
                        i = q + len(term)
                        continue
            j = i + 1
            buf = []
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    buf.append(text[j])
                    buf.append(text[j + 1])
                    j += 2
                    continue
                if text[j] == '"':
                    break
                if text[j] == "\n":
                    break
                buf.append(text[j])
                j += 1
            if j < n and text[j] == '"':
                yield i, "".join(buf)
                i = j + 1
                continue
            i += 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\" and j + 1 < n:
                    j += 2
                    continue
                if text[j] == "'":
                    break
                if text[j] == "\n":
                    break
                j += 1
            i = j + 1
            continue
        i += 1


def collect_literals(src_root, platform):
    macros = PLATFORM_MACROS[platform]
    seen = {}
    for dirpath, dirnames, filenames in os.walk(src_root):
        dirnames[:] = [d for d in dirnames if d not in (".git", "build", "release", "debug")]
        for fn in sorted(filenames):
            if not fn.endswith(SRC_EXT):
                continue
            p = os.path.join(dirpath, fn)
            txt = io.open(p, "r", encoding="utf-8-sig").read()
            txt = strip_comments(txt)
            mask = active_mask(txt, macros)
            for start, raw in iter_literals(txt, mask):
                if not mask[start]:
                    continue
                s = decode_escapes(raw)
                if not s:
                    continue
                if not any("\u4e00" <= ch <= "\u9fff" for ch in s):
                    continue
                seen.setdefault(s, p)
    return seen


# --------------------------------------------------------------------------
# 例外清单
# --------------------------------------------------------------------------
def load_allowlist(src_root):
    path = os.path.join(src_root, "scripts", "verify_binary_strings.allow.txt")
    allow = set()
    if not os.path.isfile(path):
        return allow, path
    for line in io.open(path, "r", encoding="utf-8-sig").read().splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        allow.add(line)
    return allow, path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", required=True, help="源码目录")
    ap.add_argument("--bin", required=True, help="被检查的二进制")
    ap.add_argument("--platform", required=True, choices=sorted(PLATFORM_MACROS))
    ap.add_argument("--skip-mtime", action="store_true", help="跳过时间戳判据（只查文案）")
    args = ap.parse_args()

    src_root, binary = args.src, args.bin
    if not os.path.isdir(src_root):
        print("源码目录不存在：%s" % src_root)
        return 2
    if not os.path.isfile(binary):
        print("二进制不存在：%s" % binary)
        return 2

    failed = False

    # ---- 判据 1：时间戳（包不得比源码旧）----
    if not args.skip_mtime:
        bin_mtime = os.path.getmtime(binary)
        newest_src = None
        newest_path = None
        for dirpath, dirnames, filenames in os.walk(src_root):
            dirnames[:] = [d for d in dirnames if d not in (".git", "build", "release", "debug")]
            for fn in filenames:
                if not fn.endswith(SRC_EXT):
                    continue
                p = os.path.join(dirpath, fn)
                m = os.path.getmtime(p)
                if newest_src is None or m > newest_src:
                    newest_src, newest_path = m, p
        if newest_src is not None and bin_mtime < newest_src:
            import datetime
            fmt = lambda t: datetime.datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M:%S")
            print("FAIL：二进制比源码旧")
            print("  二进制 %s  %s" % (fmt(bin_mtime), os.path.basename(binary)))
            print("  最新源 %s  %s" % (fmt(newest_src), os.path.relpath(newest_path, src_root)))
            print("  含义：这个二进制不是当前源码编译出来的。重新编译后再打包。")
            failed = True

    # ---- 判据 2：中文文案存在性 ----
    blob = open(binary, "rb").read()
    lits = collect_literals(src_root, args.platform)
    allow, allow_path = load_allowlist(src_root)
    missing = []
    for text, path in sorted(lits.items()):
        if text in allow:
            continue
        u8 = text.encode("utf-8")
        u16 = text.encode("utf-16-le")
        if u8 not in blob and u16 not in blob:
            missing.append((text, path))

    if missing:
        failed = True
        print("FAIL：%d/%d 条中文文案在 %s（%s）里找不到"
              % (len(missing), len(lits), os.path.basename(binary), args.platform))
        for text, path in missing:
            print("  %s  %s" % (os.path.relpath(path, src_root), text))
        print()
        print("含义：这个二进制不是当前源码编译出来的（或者源码里有死代码）。")
        print("     重新编译 + 重新打包后再跑这道门禁。")
        if allow_path and os.path.isfile(allow_path):
            print("     确实属于平台差异的，写进 %s 并写明理由。" % allow_path)
    else:
        print("OK：%d 条中文文案在 %s（%s）里全部命中"
              % (len(lits), os.path.basename(binary), args.platform))

    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
