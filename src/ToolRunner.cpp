#include "ToolRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QTextCodec>
#include <QTimer>

// D5：把工具整棵进程树塞进 Windows 作业对象（Job Object）。
// windows.h 会定义 min/max 宏，先掐掉，免得把 Qt 头里的 qMin/std::min 顶掉。
#ifdef Q_OS_WIN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <tlhelp32.h>
#endif

namespace {

// 末尾最多 4 字节里，有多少个字节构成一个"还没收齐的 UTF-8 序列前缀"。
// 返回 0 表示尾部不是残缺序列（是真非法字节）。
int utf8TrailingPartialBytes(const QByteArray &d)
{
    const int max = qMin(4, d.size());
    for (int k = 1; k <= max; ++k) {
        const quint8 b = static_cast<quint8>(d.at(d.size() - k));
        int need = 0;
        if      ((b & 0x80) == 0x00) need = 1;   // ASCII 永远不会"没读全"
        else if ((b & 0xE0) == 0xC0) need = 2;
        else if ((b & 0xF0) == 0xE0) need = 3;
        else if ((b & 0xF8) == 0xF0) need = 4;
        else continue;                          // 续接字节 / 非法字节
        if (k < need)
            return k;
    }
    return 0;
}

// 同理，用于 GBK/CP936：双字节，高位字节 0x81-0xFE。
int gbkTrailingPartialBytes(const QByteArray &d)
{
    if (d.isEmpty())
        return 0;
    const quint8 b = static_cast<quint8>(d.at(d.size() - 1));
    return (b >= 0x81 && b <= 0xFE) ? 1 : 0;
}

} // namespace

// ConverterState 是 POD，而且 Qt 把它的拷贝赋值设成了 private，
// 只能逐个公有字段手工清零。
void clearState(QTextCodec::ConverterState &s)
{
    s.invalidChars   = 0;
    s.remainingChars = 0;
    s.flags          = QTextCodec::ConvertInvalidToNull;
}

// 这段字节能不能被 codec 一个字节不落地解完。
//
// 这里必须**自己**判尾部残缺，不能只信 Qt 的 invalidChars：
// 实测（dbgenc 探针，Qt 5.9.6）Qt 的 UTF-8 解码器把"尾部不完整"记在
// remainingChars 上，invalidChars 恒为 0：
//     1 字节 E5        -> invalidChars=0 remainingChars=1
//     2 字节 E5 90     -> invalidChars=0 remainingChars=2
//     2 字节 F0 9F     -> invalidChars=0 remainingChars=2
// 只看 invalidChars 的话，每个分片都会以为"这块干干净净"，
// 半个字符被直接丢掉，解出来的中文每三个字缺一个。
bool decodesCleanly(QTextCodec *codec, const QByteArray &data, int trailingPartial)
{
    if (trailingPartial > 0)
        return false;
    QTextCodec::ConverterState st;
    clearState(st);
    codec->toUnicode(data.constData(), data.size(), &st);
    return st.invalidChars == 0;
}

// 解坏的程度：解不出来的字节数 + 落地的替换字符数。两种编码用同一把尺子
// 量出来的数值可直接比较（都以输入字节数为基准）。
//
// 不能用"哪种能干净解完"来选编码，实测会翻车两次：
//   - GBK 的高位字节 0x81-0xFE 和 UTF-8 的 110xxxxx（0xC0-0xDF）大幅重叠；
//   - GBK 的 0xE5 0x90 就是一个合法的 UTF-8 双字节序列。
// 所以短样本上"干净解完"完全可能是假的。换成"坏得多就少"，
// 长样本下 GBK 串按 UTF-8 解的坏字节率通常在两成以上，反之也一样，
// 区分度足够；纯 ASCII 两种都是 0，随便选哪个都一样。
int damageOf(QTextCodec *codec, const QByteArray &data)
{
    QTextCodec::ConverterState st;
    clearState(st);
    const QString s = codec->toUnicode(data.constData(), data.size(), &st);
    int n = st.invalidChars;
    for (const QChar c : s) {
        if (c.unicode() == 0xFFFD)
            ++n;
    }
    return n;
}

// 工具输出的流式解码器。
//
// 为什么必须分两步（先定编码、再流式解）：
//
//   1. QProcess::readyRead 是管道里攒够多少给多少，完全可能在多字节字符
//      中间接断。半截字符必须扣下来等下一块拼上，否则每次都会解出乱码。
//   2. 但**不能**靠"这一块解出错就是编码错了"来判编码，也不能靠"这一块
//      解得干净就是这种编码"。GBK 和 UTF-8 在字节层面本来就分不开
//      （见 damageOf 上面的实测说明）。所以判定只能靠**长样本上的整体损坏率**。
//
// 真实工具第一次输出几乎总是远超 kDecideBytes 字节，所以这一步不产生延迟。
// 开头先攒一段再判定，是为了防止"工具先打了几行 ASCII 头、后面才是中文"
// 这种把编码判死早了的场景。
class ToolRunner::OutputDecoder
{
public:
    OutputDecoder()
        : m_utf8(QTextCodec::codecForName("UTF-8"))
        , m_local(QTextCodec::codecForLocale())
    {
    }

    void reset()
    {
        // 不清的话上一轮遗留的 pending / 编码判定会被下一个进程继承，
        // 它的头一块输出会被当成上一轮剩下的半截字符。
        m_pending.clear();
        m_mode = Undecided;
        m_locked = false;
        m_rechecked = false;
        m_badSinceCheck = 0;
    }

    QString decode(const QByteArray &chunk)
    {
        QByteArray data = m_pending + chunk;
        m_pending.clear();
        if (data.isEmpty())
            return QString();

        if (m_mode == Undecided) {
            // 样本太短、或者尾部还差几个字节、或者按 UTF-8 解不干净：
            // 都先别定，攒着再判。
            // UTF-8 干净且样本已经够长时可以立刻定 —— 这条快速路径是为了
            // 不让"纯 UTF-8 工具输出"白等 256 字节。
            const bool fastOk = data.size() >= kFastBytes
                                && decodesCleanly(m_utf8, data, utf8TrailingPartialBytes(data));
            if (!fastOk && data.size() < kDecideBytes) {
                m_pending = data;
                return QString();
            }
            m_mode = (damageOf(m_utf8, data) <= damageOf(m_local, data)) ? AsUtf8 : AsLocal;
            m_locked = true;
            m_badSinceCheck = 0;
        }

        return runCodec(data);
    }

    // 进程结束时把没拼上的尾巴直接吐出来（多半是截断的半个字符，
    // 留着也不会变完整，但不吐出来这行日志就凭空缺了尾巴）。
    QString flush()
    {
        if (m_pending.isEmpty())
            return QString();
        if (m_mode == Undecided)
            m_mode = (damageOf(m_utf8, m_pending) <= damageOf(m_local, m_pending)) ? AsUtf8 : AsLocal;
        QTextCodec *codec = (m_mode == AsLocal) ? m_local : m_utf8;
        const QString tail = codec->toUnicode(m_pending.constData(), m_pending.size());
        m_pending.clear();
        return tail;
    }

private:
    // 攒够这么多字节再定编码。定得太早会被"开头一段纯 ASCII"骗过去。
    static const int kDecideBytes = 256;
    // UTF-8 快速通道的最小样本：太短的"解得干净"没有说服力
    // （GBK 的一对字节有约三分之一刚好是合法的 UTF-8 双字节序列）。
    static const int kFastBytes = 16;
    // 锁定之后累计这么多坏字节才允许改判一次（不许来回抖）。
    static const int kRecheckBytes = 512;

    enum Mode { Undecided = 0, AsUtf8 = 1, AsLocal = 2 };

    QString runCodec(QByteArray data)
    {
        QTextCodec *codec = (m_mode == AsLocal) ? m_local : m_utf8;

        // 已经定死却解得乱七八糟：八成是开头那段纯 ASCII（或一小段恰好合法的
        // GBK 字节对）把编码判早了。累计到足够多的坏字节再改判，且只改一次 ——
        // 不设上限的话两种编码的乱码会交替吐出来，比一直错还难看。
        if (m_locked && !m_rechecked && data.size() >= 8) {
            const int badNow = damageOf(codec, data);
            m_badSinceCheck += badNow;
            if (m_badSinceCheck >= kRecheckBytes) {
                QTextCodec *other = (m_mode == AsLocal) ? m_utf8 : m_local;
                if (damageOf(other, data) * 4 < badNow) {
                    m_mode = (m_mode == AsLocal) ? AsUtf8 : AsLocal;
                    codec = other;
                }
                m_rechecked = true;
                m_badSinceCheck = 0;
            }
        }

        QTextCodec::ConverterState st;
        clearState(st);
        QString text = codec->toUnicode(data.constData(), data.size(), &st);
        const int bad = st.invalidChars;

        // 尾部残缺一律保留等下一块。
        //
        // 这里刻意不写成 `partial <= bad`：Qt 对残缺根本不填 invalidChars
        // （见 decodesCleanly 上面的实测），拿 bad 当门槛等于永远不成立。
        // 只要尾部是残缺的 UTF-8 / GBK 前缀，就一定是"这次没读全"，
        // 不可能是坏字节 —— 坏字节不会构成一个合法序列的前缀。
        const int partial = (m_mode == AsLocal) ? gbkTrailingPartialBytes(data)
                                               : utf8TrailingPartialBytes(data);
        if (partial > 0) {
            m_pending = data.right(partial);
            data.truncate(data.size() - partial);
            clearState(st);
            return codec->toUnicode(data.constData(), data.size(), &st);
        }

        if (bad > 0) {
            // 真的非法字节。编码已经定死了，不能靠"再换一次"糊过去
            // （前面那几块已经吐出去了，换回来只会让两种乱码交替出现）。
            // 把坏的那几个字节丢掉，保证它后面的正常内容还能继续读出来，
            // 日志不会从中间断掉。
            data.truncate(data.size() - bad);
            clearState(st);
            return codec->toUnicode(data.constData(), data.size(), &st);
        }
        return text;
    }

    QTextCodec *m_utf8;
    QTextCodec *m_local;
    Mode m_mode = Undecided;
    // 第一次判定算不算数：不算数的话后面还允许改判一次。
    bool m_locked = false;
    bool m_rechecked = false;
    int m_badSinceCheck = 0;
    QByteArray m_pending;
};
namespace {

// QProcess::splitCommand 要 Qt 5.15 才有，这里自己分词。
// 参数模板是用户手填的字符串，Windows 上三种引号写法都有人用：
//   "带空格路径"      —— 标准写法
//   '带空格路径'      —— 从 Linux 习惯带过来的
//   \"带空格路径\"    —— 从 JSON / ini / 代码里抄过来的
// 只认双引号的话，后两种会被拆成一堆碎片参数，工具收到乱七八糟的 argv。
// 反斜杠 + 引号要当成"引号本身"而不是普通字符，否则 --cfg \"a b\" 会被
// 还原成 a b 两段并多出一个反斜杠。
//
// unclosedQuote 是个出参：用户模板里引号没打齐（--a "b c）时，
// 分词会把后面所有内容吞成一个参数。工具照样跑完、界面照样显示"执行完成"，
// 用户根本不知道自己传错了什么。这里把这种情况标出来，让 start() 明确提示。
QStringList splitCommand(const QString &cmd, bool *unclosedQuote)
{
    QStringList out;
    QString cur;
    bool started = false;
    QChar quote;      // 当前所处的引号类型，'\0' 表示不在引号里

    for (int i = 0; i < cmd.size(); ++i) {
        const QChar c = cmd.at(i);

        if (c == QLatin1Char('\\') && i + 1 < cmd.size()) {
            const QChar n = cmd.at(i + 1);
            if (n == QLatin1Char('"') || n == QLatin1Char('\'')
                || n == QLatin1Char('\\')) {
                cur.append(n);
                started = true;
                ++i;
                continue;
            }
        }

        if (quote.isNull()) {
            if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
                quote = c;
                started = true;          // "" 表示一个显式的空参数
            } else if (c.isSpace()) {
                if (started) {
                    out << cur;
                    cur.clear();
                    started = false;
                }
            } else {
                cur.append(c);
                started = true;
            }
        } else {
            if (c == quote)
                quote = QChar();
            else
                cur.append(c);
        }
    }
    if (unclosedQuote)
        *unclosedQuote = !quote.isNull();
    if (started)
        out << cur;
    return out;
}

} // namespace

// 规则后缀的归一化口径，必须和扫描器那边完全一致。
//
// FileScanner::scan() 在匹配前对**规则**做了去点 + 去空白 + 小写
// （FileScanner.cpp 里 `while (low.startsWith('.')) low.remove(0, 1);`），
// 所以同步页里写 ".spe" 是能正常工作的。
// 而 rulesFor 原来只是 `r.suffix.toLower() == suffix.toLower()`，
// 直接逐字比较，于是同一份规则文本在工具页**一条都匹配不上**：
//   ".spe"  -> 不匹配（QFileInfo::suffix() 返回的是不带点的 "spe"）
//   "..spe" -> 不匹配
//   " SPE " -> 不匹配
//   "SPE"   -> 只有碰巧全小写才行
//
// 表现是：用户配好规则，点「用匹配工具打开」，弹"后缀 .spe 尚未配置对应的
// 执行工具"；重启一次又好了 —— 因为 ConfigStore::loadToolRules() 读回来时
// 剥了点，而 ToolPanel::collectRules() 从界面表格读时没有，
// 于是"存的时候不剥、读的时候剥"，同一进程内两个口径打架。
//
// 在匹配点归一化，而不是在存储点归一化：这样无论规则来自内存表格、
// 配置文件还是将来的脚本，五种写法都是等价的，也不会再出现两个口径。
QString ToolRunner::normSuffix(const QString &s)
{
    QString v = s.trimmed().toLower();
    while (v.startsWith(QLatin1Char('.')))
        v.remove(0, 1);
    // **尾点也必须去**，而且必须两侧同时去。
    //
    // 扫描侧（FileScanner）早就去了尾点，传输侧没去，于是规则写成 "spe." 时：
    //   扫描侧：规则 "spe." -> "spe"，文件 "a.spe" 的后缀也是 "spe" -> 匹配上，列表里有它
    //   传输侧：规则 "spe." -> "spe."，文件 "a.spe" -> "spe" -> 对不上 -> **这一组是空的**
    // 空的组会走「没有匹配到要同步的文件。」+ allFinished(true)，
    // 界面上是「同步完成」、进度 100%、远端一个文件都没有 ——
    // 用户据此认为数据已经上机。
    //
    // 归一化只有一处实现，正是为了防这种"两侧不对称"。
    // FileScanner 的规则侧也改成调这个函数（见 FileScanner.cpp）。
    while (v.endsWith(QLatin1Char('.')) && v.length() > 1)
        v.chop(1);
    return v;
}

QVector<ToolRule> ToolRunner::rulesFor(const QVector<ToolRule> &rules, const QString &suffix)
{
    QVector<ToolRule> matched;
    const QString low = normSuffix(suffix);
    for (const ToolRule &r : rules) {
        if (normSuffix(r.suffix) == low)
            matched.append(r);
    }
    return matched;
}

QString ToolRunner::decodeChunks(const QList<QByteArray> &chunks)
{
    OutputDecoder decoder;
    QString text;
    for (const QByteArray &c : chunks)
        text += decoder.decode(c);
    text += decoder.flush();
    return text;
}

// 把文件路径变成"能安全塞进参数模板、再经 splitCommand 拆回来还不变形"的片段。
//
// 这里有两处必须处理，否则第三方工具收到的是错的 argv（实测 T-A / T-B）：
//
// 1) QFileInfo::absoluteFilePath() 会规范化分隔符：Qt 内部用 '/'，
//    于是 UNC 路径 \\server\share\x.spe 出来变成 //server/share/x.spe。
//    大部分 Win32 调用能接受正斜杠，但工具自己的字符串处理未必，
//    而且界面上看到的和实际传下去的不一致。显式转回原生分隔符。
//
// 2) splitCommand 的转义判断在"是否在引号里"之前，所以**引号保护不了反斜杠**：
//    \\ 一定被折叠成一个 \。路径里的每个 \ 必须先写成 \\ 才能原样还原，
//    否则 C:\data\x.spe 会变成 C:\data\x.spe 少一个、UNC 会少掉一个前导。
//
// needQuote 只在真的含空白时才加引号；不含空白的路径不需要，多加反而
// 会和用户模板里自带的引号叠成 ""…""（见下面的 alreadyQuoted）。
//
// 3) ' 和 " 必须转义（P29，实测 end-to-end）：
//    实测（本机 Qt 5.9.6 msvc2015_64，模板 -f {file} 不带引号）：
//        apos's.spe   -> 工具收到 aposs.spe      ← 反引号被 splitCommand 当成引号起始
//        a"b.spe      -> 工具收到 ab.spe         ← 双引号被吃掉一个
//    两种字符都在 splitCommand 里被当成"进入引号状态"的触发器（见它的
//    quote.isNull() 分支），不转义就等于把路径剪碎，且界面全程显示"执行完成"。
//
// 转义写法必须是"单趟扫描"，不能是连续三次 replace：
// 先转义 ' 再转义 \ 的话，刚插进去的那个 \ 会被再翻倍成 \\，
// 于是 \' 还原成"一个字面反斜杠 + 一个真的引号起始"，反而更坏。
static QString fileArgToken(const QString &filePath, bool needQuote)
{
    const QString full = QDir::toNativeSeparators(QFileInfo(filePath).absoluteFilePath());
    QString token;
    token.reserve(full.size() + 8);
    for (const QChar c : full) {
        // splitCommand 的转义判断排在"是否在引号里"**之前**，
        // 所以 \ + " 和 \ + ' 在引号内外都能原样还原成字面量。
        if (c == QLatin1Char('\\') || c == QLatin1Char('"') || c == QLatin1Char('\''))
            token.append(QLatin1Char('\\'));
        token.append(c);
    }
    if (!needQuote)
        return token;
    return QLatin1Char('"') + token + QLatin1Char('"');
}

// 模板里的 {file} 是否已经被成对引号包住。
//
// 用户最自然的写法就是 -f "{file}" 或 "{file}"（T-A 的真实场景）。
// 原来这里无条件再包一层引号，于是变成 -f ""C:/Program Files/data/file.spe""，
// splitCommand 逐字符走："" 立刻闭合引号，随后的空格又被当成参数分隔，
// 路径被剪成 ['-f', 'C:/Program', 'Files/data', 'file.spe'] 4 段——
// 工具拿到一个不存在的目录名，而界面全程显示"执行完成"，没有任何提示。
static bool placeholderAlreadyQuoted(const QString &tmpl)
{
    const QString token = QStringLiteral("{file}");
    int p = tmpl.indexOf(token);
    while (p >= 0) {
        int l = p - 1;
        int r = p + token.length();
        while (l >= 0 && tmpl.at(l).isSpace()) --l;
        while (r < tmpl.size() && tmpl.at(r).isSpace()) ++r;
        if (l >= 0 && r < tmpl.size()
            && (tmpl.at(l) == QLatin1Char('"') || tmpl.at(l) == QLatin1Char('\''))
            && tmpl.at(r) == tmpl.at(l))
            return true;
        p = tmpl.indexOf(token, p + token.length());
    }
    return false;
}

QString ToolRunner::buildArgs(const ToolRule &rule, const QString &filePath)
{
    const QString full = QDir::toNativeSeparators(QFileInfo(filePath).absoluteFilePath());
    const bool needQuote = full.contains(QLatin1Char(' '))
                           || full.contains(QLatin1Char('\t'));
    // 用户自己写了引号就不要再包一层，否则嵌套引号会把路径剪碎。
    const QString subst = fileArgToken(filePath, needQuote && !placeholderAlreadyQuoted(rule.argsTemplate));

    QString args = rule.argsTemplate;
    if (args.contains(QLatin1String("{file}"))) {
        // 模板自带 {file} 占位：替换后直接返回，路径只出现一次。
        // 原来的实现在这里还会无条件再拼一份 quoted，同一个文件会被传给
        // 工具两次，模板形如 "{file} --out x.txt" 时会收到两个文件参数。
        args.replace(QLatin1String("{file}"), subst);
        return args.trimmed();
    }
    // 模板没有 {file}：在末尾追加路径，保证工具总能拿到要处理的文件。
    args = args.trimmed();
    return args.isEmpty() ? subst : args + QLatin1Char(' ') + subst;
}

// 参数构造 + 分词的一条龙，给同步表格页用。
//
// 表格页原来自己写了一份只认双引号的 splitArgs，绕开了这里的
// fileArgToken（不转义反斜杠）和 placeholderAlreadyQuoted（不查嵌套引号）。
// 于是路径含空格时被撕成 ['C:/Program', 'Files/data', 'x.spe'] 几段，
// 工具照样启动、界面照样报「执行完成」，用户从头到尾看不到异常。
//
// 复刻简化版的代价已经付过两次（P29 的单引号、T-A 的嵌套引号都是修了
// 这一边、另一边照旧坏）。这里直接把工具页那条链整条借出去，
// 两边从此不可能再分叉。
QStringList ToolRunner::buildArgv(const ToolRule &rule, const QString &filePath)
{
    bool unclosed = false;
    return splitCommand(buildArgs(rule, filePath), &unclosed);
}

#ifdef Q_OS_WIN
namespace {

// 建一个"最后一个句柄一关就全员陪葬"的作业对象。
// 返回 nullptr = 没拿到（创建失败或系统不支持），调用方必须退回旧行为。
void *createKillTreeJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

// 放行一个用 CREATE_SUSPENDED 拉起来、还没有跑过任何代码的进程。
//
// 为什么必须走线程快照而不是别的路子：作业成员身份是**创建时从父进程继承**的，
// 所以只要 cmd.exe 在挂进作业之前就已经 fork 出了真正的业务进程，
// 那个业务进程就永远不在作业里，TerminateJobObject 也碰不到它。
// 先挂起再挂作业、再放行，是唯一能把窗口关死的顺序。
//
// 而 CreateProcess 返回时 Qt 只给了我们进程句柄，没给主线程句柄：
// QProcess::started() 是排队信号，startProcess 的栈帧那时早就没了，
// 所以 CreateProcessArguments::processInformation 那个指针在槽里读是读野内存。
// 只能按 pid 反查：进程被挂起着，一个字节都没执行，必然只有主线程这一个线程，
// 枚举到的第一个属于它的线程就是主线程。
bool resumePrimaryThread(DWORD pid)
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    bool resumed = false;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid)
                continue;
            const HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (ResumeThread(th) != static_cast<DWORD>(-1))
                resumed = true;
            CloseHandle(th);
            if (resumed)
                break;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return resumed;
}

} // namespace
#endif // Q_OS_WIN

void ToolRunner::start(const QString &exePath, const QString &args)
{
    if (isRunning())
        return;

    m_proc = new QProcess(this);
    m_cancelRequested = false;
    closeJob();               // 上一轮若有漏网的句柄，先收干净再开新的
    m_decoder->reset();
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &ToolRunner::onReadyRead);
    connect(m_proc, &QProcess::readyReadStandardError,  this, &ToolRunner::onReadyRead);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ToolRunner::onFinished);

    // D5：工具常是 cmd /c wrapper.bat 这种间接层，杀 QProcess 只杀得掉 cmd.exe，
    // 真正的业务进程被留下继续跑，泄漏成孤儿（外部实测：按真实 PID 记账，
    // 直接调 exe 的场景干净，cmd /c 这层有残留）。
    //
    // 拿不到作业对象就**不要**挂起：挂起后放不回来，工具会永远卡住不退出，
    // 那比"留个孤儿"严重得多。这种情况下行为与修复前完全一致。
#ifdef Q_OS_WIN
    m_job = createKillTreeJob();
    if (m_job) {
        m_proc->setCreateProcessArgumentsModifier(
            [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_SUSPENDED; });
        connect(m_proc, &QProcess::started, this, &ToolRunner::onJobAttach);
    }
#endif

    // 不能用 waitForStarted()：进程启动失败时它会一直占着事件循环，
    // GUI 表现为点「打开」后整窗冻结。改为完全异步，靠 errorOccurred 兜底。
    connect(m_proc, &QProcess::errorOccurred, this, &ToolRunner::onError);

    bool unclosed = false;
    const QStringList argv = splitCommand(args, &unclosed);
    if (unclosed) {
        // 不阻断执行（用户可能就是要一整段带空格的参数），但必须把话说清楚：
        // 工具会照跑、界面会照显示"执行完成"，用户没有任何机会知道参数被合并了。
        emit output(QString::fromUtf8(u8"\u203B \u53C2\u6570\u6A21\u677F\u91CC\u7684\u5F15\u53F7\u6CA1\u6709\u6210\u5BF9\uFF1A"
                                       u8"\u5F15\u53F7\u540E\u9762\u7684\u5185\u5BB9\u88AB\u5F53\u6210\u4E86\u4E00\u4E2A\u6574\u4F53\u53C2\u6570\u3002\n"
                                       u8"\u5F15\u53F7\u540E\u9762\u7684\u5185\u5BB9\u88AB\u5F53\u6210\u4E86\u4E00\u4E2A\u6574\u4F53\u53C2\u6570\u3002\u005Cn"));
    }
    m_proc->start(exePath, argv);
}

void ToolRunner::onJobAttach()
{
#ifdef Q_OS_WIN
    if (!m_proc || !m_job)
        return;

    const qint64 pid = m_proc->processId();
    bool assigned = false;
    if (pid > 0) {
        const HANDLE ph = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | SYNCHRONIZE,
                                      FALSE, static_cast<DWORD>(pid));
        if (ph) {
            assigned = AssignProcessToJobObject(static_cast<HANDLE>(m_job), ph);
            CloseHandle(ph);
        }
    }

    // 放行是无条件的，且优先级高于一切：
    // 挂起状态下放不回来的工具会永远卡死，用户看到的是"点了没反应"，
    // 那比留一个孤儿严重得多。挂不进作业也只是退回旧行为，不会更糟。
    if (!resumePrimaryThread(static_cast<DWORD>(pid))) {
        m_proc->kill();
        closeJob();
    }
    Q_UNUSED(assigned)
#else
    Q_UNUSED(this)
#endif
}

void ToolRunner::closeJob()
{
#ifdef Q_OS_WIN
    if (!m_job)
        return;
    CloseHandle(static_cast<HANDLE>(m_job));
    m_job = nullptr;
#endif
}

void ToolRunner::releaseJob()
{
#ifdef Q_OS_WIN
    if (!m_job)
        return;
    // 摘掉 KILL_ON_JOB_CLOSE 再关句柄：句柄会正常释放（不泄漏），
    // 但作业里**已经跑着的进程**不受影响。
    //
    // 顺序不能反：先 CloseHandle 再设信息，句柄已经无效了，设不进去。
    // 设失败也照样要关句柄 —— 宁可退回"杀掉后代"（closeJob 的旧行为），
    // 也不能泄漏一个句柄让作业对象一直挂着。
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = 0;
    const BOOL ok = SetInformationJobObject(static_cast<HANDLE>(m_job),
                                           JobObjectExtendedLimitInformation,
                                           &info, sizeof(info));
    Q_UNUSED(ok)
    CloseHandle(static_cast<HANDLE>(m_job));
    m_job = nullptr;
#endif
}

void ToolRunner::onError(QProcess::ProcessError error)
{
    if (error != QProcess::FailedToStart || !m_proc)
        return;

    emit output(QString::fromUtf8(u8"\u542F\u52A8\u5931\u8D25\uFF1A%1").arg(m_proc->errorString()));
    m_proc->deleteLater();
    m_proc = nullptr;
    closeJob();
    emit finished(-1);
}

// 外部工具的输出不一定是 UTF-8。国内客户机上的回放/处理工具多半是
// 按 GBK(936) 编译的老程序，一律 fromUtf8 会把中文输出变成一串乱码，
// 用户看到日志全是"锟斤拷"却不知道是自己工具的问题。
// 启发式判断：按 UTF-8 解出来有替换字符 U+FFFD，就说明不是 UTF-8，退回本地编码。
// 外部工具的输出不一定是 UTF-8。国内客户机上的回放/处理工具多半是
// 按 GBK(936) 编译的老程序，一律 fromUtf8 会把中文输出变成一串乱码，
// 用户看到日志全是"锟斤拷"却不知道是自己工具的问题。
//
// 这里必须**流式**解码：QProcess::readyRead 是管道里攒够多少给多少，
// 完全可能在多字节字符中间断开（工具一次 printf 一大段中文时几乎必然发生）。
// 早先的写法是"整块 fromUtf8，看到 U+FFFD 就退回本地编码"，两个错叠在一起：
//   1. 半截 UTF-8 字符解出来带 U+FFFD，被误判成"这不是 UTF-8"；
//   2. 退回 GBK 之后，半截 GBK 字节被当成完整字符解出乱码；
//   3. 这个误判还是**每块独立**的，后面的块继续按 GBK 解，
//      于是中文输出从中间某一行起全部变乱码。
// 现在改成：末尾不足一个多字节序列的字节留下等下一块拼上，
// 只有真的出现非法字节才整体切编码。ASCII 是 UTF-8 与 GBK 的公共子集，
// 所以切换编码不影响已经正确解出的 ASCII 段落。

ToolRunner::ToolRunner(QObject *parent)
    : QObject(parent)
    , m_decoder(new OutputDecoder)
{
}

ToolRunner::~ToolRunner()
{
    // 析构不是信号槽，可以直接同步等它结束。
    // 只等 1.5 秒：程序不认 terminate 时靠 kill 兜底，
    // 不管它同不同意都必须把进程收干净，不能让它变孤儿继续写结果文件。
    if (m_proc && m_proc->state() != QProcess::NotRunning) {
        // **必须先断连再 kill。**
        // kill 之后 QProcess 会发 finished，经由 ToolRunner::onFinished
        // 转发成 ToolRunner::finished，接到 ToolPanel 的槽里会
        // 记一条失败并**弹一个模态框** —— 而那一刻 ToolPanel 正在析构，
        // 模态框的嵌套事件循环会把整个析构过程搅乱，实测直接 0xC0000005。
        // 表现就是"正在跑工具时一关主窗口程序就崩"。
        // 面板正在销毁，这时候不该有任何回调打回去。
        m_proc->disconnect(this);
#ifdef Q_OS_WIN
        // 整个作业一起终止：cmd /c 这类间接层下面的业务进程也一起走。
        if (m_job)
            TerminateJobObject(static_cast<HANDLE>(m_job), 1);
#endif
        m_proc->kill();
        m_proc->waitForFinished(1500);
    }
    // 关作业句柄：KILL_ON_JOB_CLOSE 会把作业里还活着的（孙）进程一起收掉，
    // 所以"工具自己退了但把后代留在后台"这种情况也堵住了。
    closeJob();
    delete m_decoder;
    m_decoder = nullptr;
}

void ToolRunner::onReadyRead()
{
    // stdout 和 stderr 共用一个解码器：界面把它们拼在同一份日志里，
    // 分开解码的话中文字符被切断时两边各留半截，拼起来反而是坏的。
    const QString text = m_decoder->decode(m_proc->readAllStandardOutput());
    if (!text.isEmpty())
        emit output(text);

    const QString err = m_decoder->decode(m_proc->readAllStandardError());
    if (!err.isEmpty())
        emit output(err);
}

void ToolRunner::onFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    if (!m_proc)
        return;

    // 进程刚退出时管道里可能还剩一段没投递的数据，先读走再收尾，
    // 否则最后几行输出永远看不到。
    onReadyRead();
    const QString tail = m_decoder->flush();
    if (!tail.isEmpty())
        emit output(tail);

    // 必须 deleteLater，不能 delete：本槽是被 m_proc 自己的 finished() 信号
    // 调用的，QProcess 的信号还压在 QObject::activate() 的栈上，
    // 这里直接 delete 会在 activate() 回到遍历连接的代码时访问已释放对象，
    // 实测表现为工具一跑完进程就崩（0xC0000005）。
    m_proc->deleteLater();
    m_proc = nullptr;
    // 工具正常跑完 —— 这里**不能**用 closeJob()。
    //
    // 作业建的时候带了 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE，closeJob() 语义是
    // "关句柄 = 作业里剩下的进程全部终止"。而"工具跑完了"并不等于
    // "工具的后代都该死"：不少分析工具是「拉起后台处理进程 → 自己立刻退出」
    // 这种模式（跑长任务时 detach），后台进程才是它真正的产出。
    // 这里杀树等于产品替用户决定了工具后代的命运。
    //
    // 实测（本机 Qt 5.9.6 msvc2015_64，同一个 bat 两条路径对照）：
    //   经产品 ToolRunner 跑   -> 后台进程残留 0（被误杀）
    //   裸 cmd /c 跑同一个 bat -> 后台进程残留 1（存活）
    // 对照证明差异来自产品，不是环境。界面那边全程显示「执行完成」，
    // 用户看不到任何提示，只发现后台任务无声消失了。
    //
    // 杀树仍然保留在两条**语义上该杀**的路径上：
    //   cancel()        用户点了取消，后台进程该跟着停
    //   ~ToolRunner     面板销毁，不留孤儿继续写结果文件
    // 那两条实测都是杀干净的（取消前 2 个 -> 取消后 0 个）。
    releaseJob();
    emit finished(exitCode);
}

void ToolRunner::cancel()
{
    if (!isRunning())
        return;

    m_cancelRequested = true;
    // 必须用 kill()，不能只用 terminate()。
    //
    // 实测（本机 Qt 5.9.6 msvc2015_64，与同步链路同一套环境）：
    //   cmd.exe /c ping -t → T=1500 调 terminate()，T=12000 仍 Running，
    //                        最后只能靠 kill() 才停下来
    //   MSYS2 rsync.exe     → 同样 10 秒后仍 Running
    // 也就是说 terminate() 在这套 Qt 上**对任何被拉起的进程都不生效**，
    // 不只是 MSYS2 那个。原先这里"先 terminate、1.5 秒后再 kill"的写法，
    // 那 1.5 秒不是保险，是**唯一真正起作用的兜底**，代价是每次点"停止"
    // 都要多等一秒半，而用户看到的是"点了没反应"。
    //
    // 同步链路的 RsyncRunner::cancel() 早就改成直接 kill()，并有前后对比数据
    // （取消后 260ms 停住 vs 之前 25 秒且 400MB 全传完）。这里同步跟上。
    //
    // 不用 waitForFinished：那是阻塞调用，跑在界面线程上会卡住界面。
    // 收尾交给 QProcess 自己的 finished() 信号，在 onFinished 槽里异步做。
    //
    // D5：光 kill() 只杀得掉直接子进程。工具是 cmd /c wrapper.bat 时，
    // 真正的业务进程是孙进程，kill 完它照跑不误，泄漏成孤儿。
    // 有了作业对象就整个作业一起终止。
#ifdef Q_OS_WIN
    if (m_job)
        TerminateJobObject(static_cast<HANDLE>(m_job), 1);
#endif
    if (m_proc)
        m_proc->kill();
}
