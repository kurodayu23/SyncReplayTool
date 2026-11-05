#pragma once

#include <QByteArray>
#include <cstdio>

// 控制台输出编码。
//
// 为什么需要它（2026-11-05 交付包审计 D7）：
//   中文版 Windows 的控制台默认输出代码页是 GBK(936)，而程序打的是 UTF-8 字节
//   —— 运维在 cmd 里敲 --help 看到的是满屏乱码。
//   客户的第一反应是"这软件输出坏了"。
//
// 为什么单独一个文件：<windows.h> 会把 `small` 定义成 `char`
// （rpcndr.h 干的），还会定义 min/max 宏 —— 把它塞进 main.cpp 会
// 污染那边成千上万行代码（实测直接把 `const int small = ...` 编译成
// `const int char = ...`）。隔离在一个只干这件事的小文件里最省事。
//
// 口径：
//   · 输出是**控制台** → 按控制台当前代码页转码后再写（人看的，要能显示）；
//   · 输出被**重定向**（> file、| 别的程序）→ 原样写 UTF-8
//     （机器/脚本读的，必须是 UTF-8，转了反而解析不了）。
//   · Linux / 麒麟：控制台本来就是 UTF-8，不做任何处理。
void writeConsoleText(FILE *stream, const QByteArray &utf8);
