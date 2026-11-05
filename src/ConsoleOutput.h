#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>
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
//   · **不改控制台代码页**（SetConsoleOutputCP 是控制台级设置，会波及
//     同一个控制台里的其它程序，也会影响用户自己正在做的事）。
//
// 头文件里只用 unsigned int 表示代码页，不用 UINT —— 后者是 windows.h 的
// 类型，而这个头文件会被 main.cpp 包含，main.cpp 不能碰 windows.h。

// 当前控制台输出代码页。
//   返回 0 表示"不需要转码"：没有控制台、或控制台本身就是 UTF-8(65001)。
unsigned int consoleOutputCodePage();

// 这个平台是否真的会做代码页转码。
//   Windows: 是（中文 cmd 是 936，必须转）。
//   Linux / 麒麟: 否 —— 控制台本来就是 UTF-8。
bool consoleEncodingSupported();

// 按给定代码页把 UTF-8 字节转成该代码页的字节。
//   cp == 0 或 cp == 65001 => 原样返回（UTF-8）。
//   转换失败（该代码页不支持某些字符）=> 也原样返回 UTF-8
//   （宁可乱码也不要输出空内容或崩溃）。
QByteArray encodeForCodePage(const QByteArray &utf8, unsigned int cp);

// 按给定代码页把字节解码回 Unicode（供自检做往返验证）。
//   cp == 0 或 cp == 65001 => 当 UTF-8 解。
QString decodeFromCodePage(const QByteArray &bytes, unsigned int cp);

// 把一段 UTF-8 字节写到控制台/文件。按上面的口径决定要不要转码。
void writeConsoleText(FILE *stream, const QByteArray &utf8);

// 一个"只写"的 QIODevice，把 QTextStream 的输出导向 FILE*，
// 并在写入前按上面的口径决定要不要转码。
//
// 用法（替代 `QTextStream out(stdout)`）：
//     ConsoleDevice console(stdout);
//     QTextStream out(&console);
//     out.setCodec("UTF-8");   // 仍然钉死 UTF-8 —— 设备层负责转码
//
// 注意：设备是顺序设备（isSequential == true），不提供读。
class ConsoleDevice : public QIODevice
{
public:
    explicit ConsoleDevice(FILE *stream, QObject *parent = nullptr);

    bool isSequential() const override { return true; }

protected:
    qint64 readData(char *data, qint64 maxSize) override;
    qint64 writeData(const char *data, qint64 len) override;

private:
    FILE *m_stream;
};
