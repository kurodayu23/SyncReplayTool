// 控制台输出编码 —— 头文件见 ConsoleOutput.h。
//
// 本文件**故意**只包含 <windows.h>：它会定义 `small`（= char）、min/max 等宏，
// 放进 main.cpp 会污染那边成千上万行代码（实测编译失败）。
// 所以隔离在这里，这个文件里不用 small / min / max 这些标识符。
//
// 非 Windows（麒麟 / Linux）不需要任何转码 —— 控制台本来就是 UTF-8，
// 而且这个文件会被编进麒麟包，所以 windows.h 必须条件包含。

#include "ConsoleOutput.h"

#ifdef Q_OS_WIN

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <QVector>

#include <io.h>
#include <stdio.h>

namespace {

// 传入的 FILE* 到底对应哪个 OS 句柄。
//
// 必须按**实际传入的 FILE*** 拿，不能一律用 STD_OUTPUT_HANDLE ——
// writeConsoleText 也会被用来写临时文件（自检判据 ③ 就是这么做的），
// 那种情况下句柄是文件，不是控制台。
inline HANDLE streamHandle(FILE *stream)
{
    if (!stream)
        return INVALID_HANDLE_VALUE;
    const int fd = _fileno(stream);
    if (fd < 0)
        return INVALID_HANDLE_VALUE;
    const intptr_t h = _get_osfhandle(fd);
    if (h == -1 || h == 0)
        return INVALID_HANDLE_VALUE;
    return reinterpret_cast<HANDLE>(h);
}

// stdout/stderr 是不是真的控制台（而不是重定向到文件/管道）。
inline bool streamIsConsole(FILE *stream)
{
    HANDLE h = streamHandle(stream);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return false;
    return GetFileType(h) == FILE_TYPE_CHAR;
}

// 用 WriteConsoleW 把 UTF-8 字节直写进控制台。
//
// 为什么优先走它而不是"转成控制台代码页"（2026-11-05）：
//   控制台的输出缓冲区本来就是 Unicode 的，WriteConsoleW 直接写 Unicode，
//   **完全不受 chcp / GetConsoleOutputCP() 影响**。
//   踩过的坑：用户在 Windows Terminal 里执行 chcp 936，Terminal 实际仍按
//   UTF-8 渲染，这时"转成 GBK 再写"就正好写出一屏乱码 ——
//   而 WriteConsoleW 在这种场合依然正确。
//   所以：Unicode 直写是主路，按代码页转码是兜底（WriteConsoleW 不可用时）。
//
// 返回 false 表示"没写成"，调用方改走转码兜底。
inline bool writeUnicodeToConsole(FILE *stream, const QByteArray &utf8)
{
    HANDLE h = streamHandle(stream);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return false;
    if (GetFileType(h) != FILE_TYPE_CHAR)
        return false; // 不是控制台（重定向/文件）—— WriteConsoleW 在这儿必然失败

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.constData(),
                                         utf8.size(), NULL, 0);
    if (wlen <= 0)
        return false;
    QVector<wchar_t> wbuf(wlen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.constData(), utf8.size(),
                            wbuf.data(), wlen) <= 0)
        return false;

    // 控制台缓冲区满时 WriteConsoleW 只会写进去一部分 —— 剩下的必须补写，
    // 否则输出被截断（自检有 70 行输出，长文本很容易触发）。
    int off = 0;
    while (off < wlen) {
        DWORD written = 0;
        if (!WriteConsoleW(h, wbuf.data() + off, (DWORD)(wlen - off),
                           &written, NULL))
            return false;
        if (written == 0)
            return false; // 卡住了，别死循环
        off += (int)written;
    }
    return true;
}

// 诊断：只有显式设了 SRT_CONSOLE_DIAG 才在 stderr 打一行 ASCII，
// 说明"这次输出到底走了哪条路"。默认完全不输出，不影响任何正常用法。
//
// 用途（2026-11-05 交付包审计 D7 的遗留验证）：
//   控制台分支没法在自动化环境里直接看屏幕，所以要能证明
//   "stdout 是控制台" 这个判定成立、并且走的是 Unicode 直写那条路。
//   用法：cmd /c "SyncReplayTool.exe --help > CONOUT$ 2> diag.txt"
//         然后看 diag.txt 里有没有 [console] path=WriteConsoleW
//
// 诊断行里带上 fd / filetype / cp，排障时能一眼看出
// "到底是没判成控制台" 还是 "判成了但走了别的路"。
inline void diagConsole(FILE *stream, const char *path, int chars)
{
    if (!getenv("SRT_CONSOLE_DIAG"))
        return;
    fprintf(stderr, "[console] path=%s chars=%d fd=%d filetype=%u cp=%u\n",
            path, chars, (int)_fileno(stream),
            (unsigned)GetFileType(streamHandle(stream)),
            (unsigned)consoleOutputCodePage());
    fflush(stderr);
}

} // namespace

unsigned int consoleOutputCodePage()
{
    // 没有附着控制台时 GetConsoleOutputCP() 返回 0 —— 正好就是"不需要转码"。
    const UINT cp = GetConsoleOutputCP();
    if (cp == 0)
        return 0;
    return static_cast<unsigned int>(cp);
}

bool consoleEncodingSupported()
{
    return true; // Windows：控制台代码页可能是 936，必须转
}

QByteArray encodeForCodePage(const QByteArray &utf8, unsigned int cp)
{
    if (utf8.isEmpty())
        return utf8;
    if (cp == 0 || cp == static_cast<unsigned int>(CP_UTF8))
        return utf8; // 本来就是 UTF-8，原样

    const int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.constData(),
                                         utf8.size(), NULL, 0);
    if (wlen <= 0)
        return utf8;
    QVector<wchar_t> wbuf(wlen);
    if (MultiByteToWideChar(CP_UTF8, 0, utf8.constData(), utf8.size(),
                            wbuf.data(), wlen) <= 0)
        return utf8;
    const int mlen = WideCharToMultiByte(cp, 0, wbuf.data(), wlen,
                                         NULL, 0, NULL, NULL);
    if (mlen <= 0)
        return utf8; // 该代码页装不下这些字符 —— 宁可乱码也不要空输出
    QByteArray mbuf(mlen, '\0');
    if (WideCharToMultiByte(cp, 0, wbuf.data(), wlen, mbuf.data(),
                            mlen, NULL, NULL) <= 0)
        return utf8;
    return mbuf;
}

QString decodeFromCodePage(const QByteArray &bytes, unsigned int cp)
{
    if (bytes.isEmpty())
        return QString();
    if (cp == 0 || cp == static_cast<unsigned int>(CP_UTF8))
        return QString::fromUtf8(bytes);

    const int wlen = MultiByteToWideChar(cp, 0, bytes.constData(),
                                         bytes.size(), NULL, 0);
    if (wlen <= 0)
        return QString();
    QVector<wchar_t> wbuf(wlen);
    if (MultiByteToWideChar(cp, 0, bytes.constData(), bytes.size(),
                            wbuf.data(), wlen) <= 0)
        return QString();
    return QString::fromWCharArray(wbuf.constData(), wlen);
}

void writeConsoleText(FILE *stream, const QByteArray &utf8)
{
    if (utf8.isEmpty())
        return;

    if (streamIsConsole(stream)) {
        // ① 主路：Unicode 直写（不受 chcp 影响）。
        const bool wrote = writeUnicodeToConsole(stream, utf8);
        diagConsole(stream, wrote ? "WriteConsoleW" : "codepage", utf8.size());
        if (wrote)
            return;
        // ② 兜底：按当前控制台代码页转码。
        const unsigned int cp = consoleOutputCodePage();
        if (cp != 0 && cp != static_cast<unsigned int>(CP_UTF8)) {
            const QByteArray encoded = encodeForCodePage(utf8, cp);
            fwrite(encoded.constData(), 1, encoded.size(), stream);
            fflush(stream);
            diagConsole(stream, "codepage-converted", encoded.size());
            return;
        }
        diagConsole(stream, "codepage-utf8", utf8.size());
    }

    // 输出被重定向 → 原样写 UTF-8（脚本要解析的就是它）。
    fwrite(utf8.constData(), 1, utf8.size(), stream);
    fflush(stream);
}

#else // !Q_OS_WIN

unsigned int consoleOutputCodePage()
{
    // Linux / 麒麟：控制台本来就是 UTF-8，不需要转码。
    return 0;
}

bool consoleEncodingSupported()
{
    return false; // Linux / 麒麟：不需要转码
}

QByteArray encodeForCodePage(const QByteArray &utf8, unsigned int cp)
{
    Q_UNUSED(cp);
    return utf8;
}

QString decodeFromCodePage(const QByteArray &bytes, unsigned int cp)
{
    Q_UNUSED(cp);
    return QString::fromUtf8(bytes);
}

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

// ---------------------------------------------------------------------------
// ConsoleDevice：跨平台部分（转码与否由 writeConsoleText 决定）
// ---------------------------------------------------------------------------

ConsoleDevice::ConsoleDevice(FILE *stream, QObject *parent)
    : QIODevice(parent)
    , m_stream(stream)
{
    // 必须在 m_stream 赋值之后 open：QIODevice::open() 之后才会调 writeData。
    open(QIODevice::WriteOnly | QIODevice::Unbuffered);
}

qint64 ConsoleDevice::readData(char *data, qint64 maxSize)
{
    Q_UNUSED(data);
    Q_UNUSED(maxSize);
    return -1; // 只写设备
}

qint64 ConsoleDevice::writeData(const char *data, qint64 len)
{
    if (!m_stream || len <= 0 || !data)
        return 0;
    writeConsoleText(m_stream, QByteArray(data, int(len)));
    return len;
}
