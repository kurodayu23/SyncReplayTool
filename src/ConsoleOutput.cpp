// 控制台输出编码（Windows 专有实现）—— 头文件见 ConsoleOutput.h。
//
// 本文件**故意**只包含 <windows.h>：它会定义 `small`（= char）、min/max 等宏，
// 放进 main.cpp 会污染那边成千上万行代码（实测编译失败）。
// 所以隔离在这里，这个文件里不用 small / min / max 这些标识符。
//
// 非 Windows（麒麟 / Linux）不需要任何转码 —— 控制台本来就是 UTF-8，
// 而且这个文件会被编进麒麟包，所以 windows.h 必须条件包含。

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "ConsoleOutput.h"

#include <QVector>

namespace {

// stdout/stderr 是不是真的控制台（而不是重定向到文件/管道）。
inline bool streamIsConsole(FILE *stream)
{
    const DWORD handle = (stream == stderr) ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE;
    HANDLE h = GetStdHandle(handle);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return false;
    return GetFileType(h) == FILE_TYPE_CHAR;
}

} // namespace

void writeConsoleText(FILE *stream, const QByteArray &utf8)
{
    if (utf8.isEmpty())
        return;

    if (streamIsConsole(stream)) {
        const UINT cp = GetConsoleOutputCP();
        // cp == CP_UTF8（Windows Terminal / PowerShell 7）时原样输出即可。
        if (cp != 0 && cp != CP_UTF8) {
            const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.constData(),
                                                 utf8.size(), NULL, 0);
            if (wlen > 0) {
                QVector<wchar_t> wbuf(wlen);
                MultiByteToWideChar(CP_UTF8, 0, utf8.constData(), utf8.size(),
                                    wbuf.data(), wlen);
                const int mlen = WideCharToMultiByte(cp, 0, wbuf.data(), wlen,
                                                     NULL, 0, NULL, NULL);
                if (mlen > 0) {
                    QByteArray mbuf(mlen, '\0');
                    WideCharToMultiByte(cp, 0, wbuf.data(), wlen, mbuf.data(),
                                        mlen, NULL, NULL);
                    fwrite(mbuf.constData(), 1, mbuf.size(), stream);
                    fflush(stream);
                    return;
                }
            }
        }
    }

    // 非 Windows，或输出被重定向 → 原样写 UTF-8。
    fwrite(utf8.constData(), 1, utf8.size(), stream);
    fflush(stream);
}

#else // !Q_OS_WIN

#include "ConsoleOutput.h"

void writeConsoleText(FILE *stream, const QByteArray &utf8)
{
    // Linux / 麒麟：控制台本来就是 UTF-8，原样写。
    // 输出被重定向时也保持 UTF-8 —— 脚本要解析的就是它。
    if (utf8.isEmpty())
        return;
    fwrite(utf8.constData(), 1, utf8.size(), stream);
    fflush(stream);
}

#endif // Q_OS_WIN
