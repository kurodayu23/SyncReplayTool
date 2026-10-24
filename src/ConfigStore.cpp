#include "ConfigStore.h"

#include <QSettings>
#include <QTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QTextCodec>
#include <memory>
#include "SecretStore.h"
#include "AutoSyncScheduler.h"

#ifdef _WIN32
// 写注册表要 windows.h。
//
// 为什么**不用** QSettings(NativeFormat) 写 Run 键：
// 那个后端会把该键下**所有**已有值读进来再整体写回，且一律按 REG_SZ 写 ——
// 别人的 REG_EXPAND_SZ（带 %ProgramFiles% 的那种）会被写死成 REG_SZ，
// 那条自启就此报废，而且是**别的软件**的，出了事没人会想到是这个程序干的。
// 直接用 RegSetValueExW 只动**我们自己那一个值**，别的值一个字节都不碰。
//
// NOMINMAX：windows.h 的 min/max 宏会跟 Qt 头文件打架。
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// advapi32.lib **不是** MSVC 的默认链接库（kernel32/user32/gdi32 才是），
// 而本工程的 .pro 里只显式加了 crypt32 —— 直接调 RegSetValueExW 会 LNK2019。
// 改 .pro 不在本次改动范围内，所以用 #pragma comment 在本文件里就地补上。
#pragma comment(lib, "advapi32.lib")
#endif

namespace {

// 配置文件路径。默认走 Qt 的标准位置，但允许用 SRT_CONFIG_DIR 环境变量指到别处。
//
// 为什么需要这个：自动化测试一直靠设 APPDATA 环境变量来隔离配置，
// 那在 Windows 上**根本不生效** —— QSettings(IniFormat, UserScope) 解析出的
// 路径来自 SHGetFolderPath(CSIDL_APPDATA)，它读注册表
// HKCU\...\Shell Folders\AppData，不看环境变量。
// 结果测试跑的是"沙箱配置"，产品写的却是 %APPDATA%\AP\SyncReplayTool.ini ——
// 开发机的真实配置。实测真被写坏过（host=中文 / port=65535 那份）。
// 沙箱目录还会**一个文件都不产生**，从产物上看不出重定向没生效。
//
// 只认显式的绝对目录，不做路径拼接魔法。空值 / 相对路径一律当没设。
QString configDirOverride()
{
    const QByteArray raw = qgetenv("SRT_CONFIG_DIR");
    if (raw.isEmpty())
        return QString();
    const QDir d(QString::fromLocal8Bit(raw));
    if (!d.isAbsolute())
        return QString();
    return d.absolutePath();
}

// QSettings 不可拷贝，按指针返回
std::unique_ptr<QSettings> openSettings()
{
    const QString dir = configDirOverride();

    // INI 的编解码器必须显式钉成 UTF-8，不依赖 Qt 的默认值。
    //
    // 实测（本机 Qt 5.9.6 msvc2015_64，同一个 QSettings(path, IniFormat) 构造，
    // 只改编解码器）：
    //   默认 codec 写 "远程服务器" -> host=\xe8\xbf\x9c\xe7\xa8\x8b...（字面文本）
    //   显式 UTF-8                -> host=<e8 bf 9c e7 a8 8b ...>（正确 UTF-8）
    //   显式 ISO-8859-1          -> host=?????（替换成问号）
    //
    // Qt 5.9.6 在没有 setIniCodec() 时，默认 INI 编解码器会把**每一个非 ASCII
    // 字符转义成字面 "\xNN"**，而不是替换成问号、也不是丢字节。
    //
    // 后果：用户填的中文远端目录、中文工具 exe 路径、中文子目录，在**第一次保存**
    // 就被写成一串 \xNN 字面量；下次启动读回来仍是这串字面量 —— 幂等、不可逆、
    // 界面上零提示。中文客户必中，而且同步会带着这个乱码路径去连远端。
    const QByteArray codecName("UTF-8");

    if (!dir.isEmpty()) {
        QDir().mkpath(dir);
        std::unique_ptr<QSettings> s(new QSettings(
            QDir(dir).absoluteFilePath(QStringLiteral("SyncReplayTool.ini")),
            QSettings::IniFormat));
        s->setIniCodec(QTextCodec::codecForName(codecName));
        return s;
    }
    std::unique_ptr<QSettings> s(new QSettings(
        QSettings::IniFormat, QSettings::UserScope,
        QStringLiteral("AP"), QStringLiteral("SyncReplayTool")));
    s->setIniCodec(QTextCodec::codecForName(codecName));
    return s;
}

// 最近一次落盘失败的原因。空串 = 上次写成功了。
//
// 为什么要有它：QSettings 的错误是**延迟**报的 —— setValue 全部返回 void，
// 磁盘满 / 目录只读 / 配置文件被别的程序锁住时，它一声不吭，
// 只有 sync() 之后 status() 才说得出真话。
//
// 原来整个工程没有一处检查 status()，于是设置弹窗无条件显示
// 「✓ 设置已保存」，实际一个字节都没落盘。用户关掉重开，
// IP / 端口 / 密码 / 规则全回到旧值 —— 界面上却什么异常都没有。
static QString g_lastWriteError;

// 落盘并把结果记下来。
void flushSettings(QSettings *s)
{
    s->sync();
    if (s->status() == QSettings::NoError) {
        g_lastWriteError.clear();
        return;
    }
    // Qt 5.9 的 QSettings 没有 errorString()，只能按 status() 分形态给话。
    // （这一点是编译期才发现的：QSettings::errorString() 是 Qt 6 才有的。）
    QString detail;
    switch (s->status()) {
    case QSettings::AccessError:
        detail = QString::fromUtf8(u8"没有权限写入配置文件（目录可能不可写，或文件正被别的程序占用）");
        break;
    case QSettings::FormatError:
        detail = QString::fromUtf8(u8"配置文件格式异常，写入被拒绝");
        break;
    default:
        detail = QString::fromUtf8(u8"写入配置文件失败（磁盘可能已满）");
        break;
    }
    g_lastWriteError = detail;
    // 提示走日志抽屉（notice 的通道），不当场弹框 ——
    // 弹框会卡在关窗路径的嵌套事件循环里，表现为"关不掉"。
    ConfigStore::notice(QString::fromUtf8(u8"设置没有保存到 %1：%2")
                        .arg(ConfigStore::iniPath()).arg(g_lastWriteError));
}

// 配置里存着乱码时把它还原回原文。
//
// 历史上有过一段时间，界面文案里的中文被编译器按本地代码页(GBK)读成了错字，
// 用户在界面上看到的就是这串错字；界面把它当正常内容存回了配置文件，
// 于是坏值就一直留在配置里，换新版本也不会自己好。
//
// 判据：乱码的形状是「UTF-8 字节被当成 GBK 解了一次」，所以反过来再做一次就能还原：
//   toLocal8Bit() 拿到当初被误读的那串字节，再 fromUtf8() 就回到原文。
// 判定必须严格，不能误伤正常中文：
//   1. 还原结果不能含 U+FFFD（含了就说明这串本来就不是 UTF-8 字节的产物）；
//   2. 还原结果必须真的和原值不同；
//   3. 还原结果必须含中日韩字符（纯 ASCII 值不受影响，直接原样返回）。
//
// 必须知道的局限：toLocal8Bit() 用的是**当前系统**的 8 位编码。
// 中文 Windows（CP936/GBK）上能还原；locale 是 UTF-8 的 Linux 上，
// toLocal8Bit() 就是原样转成 UTF-8，再 fromUtf8 回来还是原值，
// 判据 2 挡住不修 —— 这条自愈在 Linux 上等于没生效。
// 这是保守方向：那边用户看到的是坏值，而不是被改坏的正常值。
// 想跨平台都自愈，就得把"当初按哪个编码写的"一起存进配置，那属于换格式。
bool hasCjkText(const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        // 只查 \u4E00-\u9FFF 是不够的：CJK 扩展 A（生僻汉字）落在 \u3400-\u4DBF，
        // 兼容表意文字落在 \uF900-\uFAFF。一整串扩展区汉字的坏值会被
        // 当成"不含中文"直接放过去，这正是这个函数最该拦的那类值。
        if ((u >= 0x4E00 && u <= 0x9FFF)    // CJK 统一表意文字
            || (u >= 0x3400 && u <= 0x4DBF) // CJK 扩展 A
            || (u >= 0xF900 && u <= 0xFAFF) // CJK 兼容表意文字
            || (u >= 0x3000 && u <= 0x303F) // CJK 标点
            || (u >= 0xFF00 && u <= 0xFFEF) // 全角字符
            ) {
            return true;
        }
    }
    return false;
}

QString repairMojibake(const QString &s)
{
    if (s.isEmpty())
        return s;
    if (!hasCjkText(s))
        return s;

    const QString fixed = QString::fromUtf8(s.toLocal8Bit());
    if (fixed.isEmpty() || fixed == s || fixed.contains(QChar(0xFFFD)))
        return s;

    // ---- 下面这两条是 2026-10-15 交付前审计补的，缺了会**反向破坏好数据** ----
    //
    // 原先只有上面三道判据，全部只看"原值是不是乱码"，于是它们在
    // **破坏方向上也是通的**。两个实测会触发反向破坏的环境：
    //
    //  a) locale 不是 UTF-8 的 Linux（LANG=C / POSIX，Qt 的 codecForLocale()
    //     落到 ISO-8859-1）：toLocal8Bit() 把每个汉字换成 '?'，
    //        "/data/子目录"  ->  "/data/???"
    //     不含 U+FFFD、不等于原值、原值含中文 → 三道判据全放行 →
    //     把**正确**的中文改坏，saveTarget 写回 ini，永久丢失且零提示。
    //     麒麟/统信的容器和 systemd 服务默认就是 LANG=C，这不是边缘场景。
    //
    //  b) CP936：CJK 区有 1920/20992 个汉字的 GBK 字节恰好是合法 UTF-8，
    //     还原后变成别的文字（"专" = D7A8 → U+05E8 希伯来字母）。
    //     常用词安全，生僻字/人名/地名中招 —— 而那正是这条函数最该拦的一类。
    //
    // 两条的产物都不是中文：一个是 '?'，一个是别文字系统的字母。
    // 所以对 fixed 本身也做一次 CJK 判定就能一刀全挡。
    if (!hasCjkText(fixed))
        return s;

    // 再补一道编码器替换指纹：Qt 的编码器在无法表示某个字符时只可能写成 '?'。
    // 还原前后问号数量变了，说明这次转码丢过东西，结果不可信。
    if (s.count(QLatin1Char('?')) != fixed.count(QLatin1Char('?')))
        return s;

    return fixed;
}

// 这条路径是不是 Windows 形态：带盘符（"C:\..." / "C:/..."），或者是 UNC（"\\srv\share"）。
//
// 为什么需要它：Linux 上**反斜杠是合法的文件名字符**，
// "/data/spe\2024" 是一个真目录，不是 Windows 路径。
// 把两种形态一视同仁地处理（换反斜杠、判"盘符冒号后面不是分隔符就是坏了"），
// 在麒麟上会天天误伤用户填的正常路径。
bool looksLikeWindowsPath(const QString &p)
{
    if (p.startsWith(QLatin1String("\\\\")))
        return true;                                   // UNC：\\srv\share
    if (p.size() >= 2 && p.at(0).isLetter() && p.at(1) == QLatin1Char(':'))
        return true;                                   // 盘符：C:\ 或 C:/
    return false;
}

// ---------------------------------------------------------------------------
// 路径字段写进 ini 之前，先把反斜杠换成正斜杠。
//
// **先把机制说准**（2026-10-18 实测，Qt 5.9.6 QSettings INI，往返用真文件验证）：
//
//   Qt **写**的时候是会转义的。setValue("D:\path\to\keys\id_rsa") 落盘得到的是
//       keyPath=D:\\path\\to\\keys\\id_rsa
//   所以**程序自己存、自己读**这一轮往返是完好的 —— 程序写出来的文件不会被
//   自己读坏。这一点一开始被判断反了，判据里也踩过（rawQt 应当是 1）。
//
//   Qt **读**的时候不对称：ini 里出现**单**反斜杠时，它按 C 转义解，
//   只认 a b f n r t v 七个，剩下的连反斜杠一起丢掉：
//       C:\Users\x\.ssh\id_rsa  ->  C:sersssshid_rsa
//   实测逐字验证过：`\A` \`\P` \`\k` \`\y` \`\e` \`\i` \`\d` \`\_` 全部消失，
//   `\1` 这种按八进制解码再按 \x 写回。也就是说**任何不是 Qt 写出来的 ini**
//   —— 用户手工改过、支持同事拿记事本改过、从别的机器抄过来、
//   被某个脚本或安装程序生成过 —— 里面的 Windows 路径都会被吃掉字符，
//   而且信息丢失、无法反向还原。
//
// 那为什么还要归一化：存进去的值里**一个反斜杠都没有**，就不存在"单反斜杠"
// 这个易碎形态了。之后无论这份 ini 被谁用什么工具读写，路径都不会再变形。
// 正斜杠在 Windows 上完全等价（Win32 和 Qt 都认），跨 Windows/麒麟 通用。
QString normalizePathForStore(const QString &p)
{
#ifdef Q_OS_WIN
    // Windows 上：凡是有反斜杠的路径都换。
    // 相对路径（"keys\id_rsa"）也要换 —— 它同样是 Qt 读回时会掉字符的那种形态。
    if (!p.contains(QLatin1Char('\\')))
        return p;
    QString s = p;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return s;
#else
    // 麒麟/Linux 上：反斜杠是**合法的文件名字符**。
    // 把它换成正斜杠，等于把用户指向了另一个目录：
    //   原路径 /data/spe\2024  ->  存成 /data/spe/2024
    // 下次启动读回来就是一个不存在的目录，界面报「本地根目录不存在或不可读」，
    // 用户重选一次存一次，**永远同步不了**，而且界面一个解释都没有。
    //
    // 只有确实是 Windows 形态的路径（盘符 / UNC）才换 ——
    // 那种路径搬到麒麟上也仍然是 Windows 路径，按 Windows 规则处理才对。
    if (!looksLikeWindowsPath(p))
        return p;
    if (!p.contains(QLatin1Char('\\')))
        return p;
    QString s = p;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return s;
#endif
}

// 已经损坏的旧值长什么样，用一个高精度的判据认出来：
//
//   好的   C:/AP/xxx      C:\Users\x\.ssh\id_rsa     /home/u/.ssh/id_rsa
//   坏的   C:sersssshid_rsa                        D:PP7000\ai-workspaceeysd
//
// 损坏吃掉的是**盘符冒号后面紧跟的那个分隔符**，于是冒号后面直接变成了
// 普通字符。这正是唯一稳定可判的特征：健康的 Windows 路径里 `C:` 后面
// 一定是分隔符，坏掉的一定不是。带分隔符的坏值（上面第二行还剩几个 斜杠
// 是因为那几处命中了能识别的转义）也照样能认出来。
//
// **绝不能反过来去"修"坏掉的旧值**：\s \. \i 被丢掉之后，
// C:sersssshid_rsa 无法还原成原来那条路径 —— 信息已经没了，
// 任何猜测都会把用户带到另一个文件上去。唯一正确的处理是提示重填。
bool pathLooksCorrupted(const QString &p)
{
    // 只有**盘符形态**的路径才可能是被吃坏了的。
    //
    // 原来只要看到冒号就判，于是 Linux 上完全正常的路径
    //   /data/spe:2024   /mnt/d:spe
    // 也被判成「配置里的本机目录已经被损坏」，每次启动都报一句
    // 「请到设置里重新选择一次本机目录」—— 而路径一点问题都没有。
    // 用户重选一次，提示照旧出现（判的是字符串形态，不是文件系统），
    // 于是他以为软件坏了。
    //
    // 被吃掉分隔符的形态只可能出现在 Windows 路径上：盘符冒号后面
    // 直接跟了普通字符。判据收窄到「首字符是字母 + 第二字符是冒号」，
    // 正好只覆盖这一种。
    if (p.size() < 3)
        return false;
    if (!p.at(0).isLetter() || p.at(1) != QLatin1Char(':'))
        return false;
    const QChar next = p.at(2);
    return next != QLatin1Char('/') && next != QLatin1Char('\\');
}

} // namespace

// 公开静态：实现就在上面那个匿名命名空间里，这里只是把它接出来，
// 让 saveTarget() 和自检判据走同一条代码路径（理由见 ConfigStore.h 的注释）。
QString ConfigStore::normalizePathForStore(const QString &path)
{
    return ::normalizePathForStore(path);
}

bool ConfigStore::pathLooksCorrupted(const QString &path)
{
    return ::pathLooksCorrupted(path);
}

// 最近一次落盘失败的原因。空串 = 上次写成功了。
// 实现必须在匿名命名空间**外面** —— 命名空间里不能定义外部类的成员
// （MSVC 报 C2888）。
QString ConfigStore::lastWriteError()
{
    return g_lastWriteError;
}

// 「路径已被损坏」这两条用户可见文案。抽成函数是为了让自检判据验到
// **同一份字符串**（见 ConfigStore.h 的说明）。
QString ConfigStore::invalidPortNotice(const QString &rawPort)
{
    // 空值单独说一句话。原措辞是「（当前内容：XXX）」，端口空的时候括号里什么都没有，
    // 现场人员看到的是一句没头没尾的话，反而不知道要往那一栏里填。
    if (rawPort.isEmpty()) {
        return QString::fromUtf8(u8"配置里还没有 SSH 端口（当前按默认端口 22 处理）。"
                                 u8"请填 1-65535 的纯数字，例如 2226。");
    }
    return QString::fromUtf8(u8"设置里的 SSH 端口不是有效的端口号（当前内容：")
         + rawPort
         + QString::fromUtf8(u8"）。程序会按默认端口 22 处理。"
                               u8"请改成 1-65535 的纯数字，例如 2226。");
}

QString ConfigStore::corruptKeyPathNotice()
{
    return QString::fromUtf8(u8"\u914D\u7F6E\u6587\u4EF6\u91CC\u7684\u79C1\u94A5\u8DEF\u5F84"
                                 u8"\u5DF2\u7ECF\u635F\u574F\uFF0C\u53CD\u659C\u6760\u5728\u914D"
                                 u8"\u7F6E\u8BFB\u5199\u4E2D\u88AB\u4E22\u5F03\uFF0C\u65E0\u6CD5"
                                 u8"\u81EA\u52A8\u8FD8\u539F\u3002\u8BF7\u5230\u8BBE\u7F6E\u91CC"
                                 u8"\u91CD\u65B0\u9009\u62E9\u4E00\u6B21\u79C1\u94A5\u6587"
                                 u8"\u4EF6\u3002");
}

QString ConfigStore::corruptLocalRootNotice()
{
    return QString::fromUtf8(u8"\u914D\u7F6E\u6587\u4EF6\u91CC\u7684\u672C\u673A\u76EE"
                                 u8"\u5F55\u5DF2\u7ECF\u635F\u574F\uFF0C\u53CD\u659C\u6760"
                                 u8"\u5728\u914D\u7F6E\u8BFB\u5199\u4E2D\u88AB\u4E22\u5F03"
                                 u8"\uFF0C\u65E0\u6CD5\u81EA\u52A8\u8FD8\u539F\u3002"
                                 u8"\u8BF7\u5230\u8BBE\u7F6E\u91CC\u91CD\u65B0\u9009\u62E9"
                                 u8"\u4E00\u6B21\u672C\u673A\u76EE\u5F55\u3002");
}

ConfigStore *ConfigStore::instance()
{
    static ConfigStore inst;
    return &inst;
}

void ConfigStore::notice(const QString &message)
{
    ConfigStore *self = instance();

    // SyncPanel 在构造函数里先 loadTarget()、后 connect()，直接 emit 会被丢掉。
    // 没人接的时候先攒着，等事件循环转一圈再发。
    if (self->receivers(SIGNAL(configNotice(QString))) == 0) {
        self->m_pending.append(message);
        if (!self->m_flushScheduled) {
            self->m_flushScheduled = true;
            QTimer::singleShot(0, self, [self] { self->flushPending(); });
        }
        return;
    }
    emit self->configNotice(message);
}

// 端口这一栏有问题就说一句。判断在函数里面，调用点无条件调 —— 详见
// ConfigStore.h 里这段注释：上一轮把判断留在调用点，于是"端口为空"
// 这一整类从守卫的缝里漏了过去，而谓词级的反向锁一条都没测到。
void ConfigStore::noticePortProblem(const QString &rawPort)
{
    if (isPlainPortText(rawPort)) {
        return;
    }
    notice(invalidPortNotice(rawPort));
}

int ConfigStore::pendingNoticeCount()
{
    return instance()->m_pending.size();
}

void ConfigStore::flushPending()
{
    m_flushScheduled = false;
    const QStringList pending = m_pending;
    m_pending.clear();
    for (const QString &msg : pending)
        emit configNotice(msg);
}

QString ConfigStore::iniPath()
{
    return openSettings()->fileName();
}

// 键名抽成常量，读写两侧共用 —— 照抄一份出来必然跟着实现漂移，
// 而这种漂移的表现是「保存了但读不回来」，判据照样可能全绿。
static const char *kOnlyNewKey = "sync/onlyNewFiles";
static const char *kRememberKey = "app/rememberSettings";
static const char *kAutoSyncEnabledKey = "sync/autoSyncEnabled";
static const char *kAutoSyncIntervalKey = "sync/autoSyncIntervalSec";
// 上一次**真正开跑**的时刻。用绝对时间点算下一次，
// 这样"程序关掉三天后再开"会立刻补跑一次，而不是再等一个完整周期。
static const char *kAutoSyncLastRunKey = "sync/autoSyncLastRunAt";
// 自动同步模式：interval（按间隔重复）/ daily（每天定点）。默认 interval。
// 每天定点的时刻："HH:mm"。默认 03:00。
//
// 注意这两个键在 **sync/** 段（和已有的 autoSyncEnabled / autoSyncIntervalSec 同段），
// 而「开机自动启动」在 **app/** 段 —— 两类东西不要混。
static const char *kAutoSyncModeKey = "sync/autoSyncMode";
static const char *kAutoSyncDailyTimeKey = "sync/autoSyncDailyTime";
// 「开机自动启动」的开关。它是**用户意图**；系统里实际有没有落地
// 由 applyAutoStart() / isAutoStartActive() 单独回答。
static const char *kAutoStartKey = "app/autoStartEnabled";

// 自启项在 Windows 注册表里的位置与值名。
#ifdef _WIN32
static const wchar_t *kWinRunSubKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t *kWinRunValue  = L"SyncReplayTool";
#endif

// Linux 自启项文件（XDG autostart 规范：~/.config/autostart/*.desktop）。
QString autostartFilePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/autostart/syncreplaytool.desktop");
}

bool ConfigStore::loadOnlyNewFiles()
{
    auto s = openSettings();
    // 键不存在 = 还没配过 → 默认**不勾**，走 --checksum（按内容比对）。
    //
    // 为什么默认不勾：需求 2「只同步新增**或修改**的文件」——
    // "修改"这两个字决定了远端同名文件内容变了必须重传。
    // 勾上「仅同步新增文件」= --ignore-existing，远端只要有同名路径就整个跳过，
    // **改过的文件永远传不过去**，接收端（B 包）拿到的永远是旧内容。
    //
    // 手册《使用步骤与注意事项.md》步骤 5 写的也是「不勾（默认）= 按内容逐个比对」，
    // 交付包里代码和手册必须一致，不能各说各话。
    if (!s->contains(QLatin1String(kOnlyNewKey)))
        return false;
    return s->value(QLatin1String(kOnlyNewKey), false).toBool();
}

void ConfigStore::saveOnlyNewFiles(bool onlyNew)
{
    auto s = openSettings();
    s->setValue(QLatin1String(kOnlyNewKey), onlyNew);
}

bool ConfigStore::loadRememberSettings()
{
    auto s = openSettings();
    if (!s->contains(QLatin1String(kRememberKey)))
        return true; // 默认记住
    return s->value(QLatin1String(kRememberKey), true).toBool();
}

void ConfigStore::saveRememberSettings(bool remember)
{
    auto s = openSettings();
    s->setValue(QLatin1String(kRememberKey), remember);
}

bool ConfigStore::loadAutoSyncEnabled()
{
    auto s = openSettings();
    if (!s->contains(QLatin1String(kAutoSyncEnabledKey)))
        return false;
    return s->value(QLatin1String(kAutoSyncEnabledKey), false).toBool();
}

void ConfigStore::saveAutoSyncEnabled(bool enabled)
{
    auto s = openSettings();
    s->setValue(QLatin1String(kAutoSyncEnabledKey), enabled);
}

int ConfigStore::loadAutoSyncInterval()
{
    auto s = openSettings();
    // 键不存在 = 还没配过。用默认值，不写盘（保持"没配就是没配"的事实）。
    if (!s->contains(QLatin1String(kAutoSyncIntervalKey)))
        return AutoSyncScheduler::DefaultIntervalSec;
    int sec = s->value(QLatin1String(kAutoSyncIntervalKey),
                       AutoSyncScheduler::DefaultIntervalSec).toInt();
    // 范围**必须**与 UI 输入框的校验范围一致。
    // 这里曾经是 [5, 3600]，而 UI 的 QIntValidator 也是 [5, 3600] ——
    // 两侧一致，但这个范围本身是错的：5 秒一次全目录扫描 + 起 rsync
    // + 建 SSH，会把客户机器打满；3600 秒上限又让"每天同步一次"
    // （86400 秒）在界面上根本填不出来。
    // 现在两边都引用 AutoSyncScheduler 的同一份常量，改一处两边同时变，
    // 不会出现"界面填 5、后台存 60"这种客户完全察觉不到的错位。
    return AutoSyncScheduler::clampIntervalSec(sec);
}

void ConfigStore::saveAutoSyncInterval(int seconds)
{
    seconds = AutoSyncScheduler::clampIntervalSec(seconds);
    auto s = openSettings();
    s->setValue(QLatin1String(kAutoSyncIntervalKey), seconds);
}

QDateTime ConfigStore::loadAutoSyncLastRunAt()
{
    auto s = openSettings();
    if (!s->contains(QLatin1String(kAutoSyncLastRunKey)))
        return QDateTime();
    // 存的是 ISO 字符串。用 QDateTime::fromString 而不是 toSecsSinceEpoch，
    // 因为后者对超出 2038 的时间会溢出，而客户机器的时间不一定靠谱。
    QString raw = s->value(QLatin1String(kAutoSyncLastRunKey)).toString();
    if (raw.isEmpty())
        return QDateTime();
    QDateTime dt = QDateTime::fromString(raw, Qt::ISODate);
    if (!dt.isValid())
        return QDateTime();
    // 统一成本地时区比较。
    //
    // 存的是 **UTC**（写侧 toUTC()），读出来再 toLocalTime()。
    //
    // 为什么不能存本地墙钟：客户改系统时区（或跨夏令时）时，
    // 裸墙钟串 "2026-10-22T18:16:39" 会被**按新时区**重新解释，
    // 下次触发时刻最多偏 ±14 小时，方向还可能相反。
    // 存 UTC 则只有一个绝对时刻，改时区不影响排程。
    //
    // 兼容旧数据：老版本存的是裸本地串，fromString 解析出来没有时区信息，
    // Qt 按 LocalTime 解释 —— 读回来再 toLocalTime() 结果不变，
    // 旧配置不会被这次改动弄坏。
    const QDateTime local = dt.toLocalTime();

    // **截到整秒**：排程基准必须是整秒（原因见 SyncPanel::autoSyncMarkRunStarted）。
    //
    // 老版本写进 ini 的时刻带毫秒（Qt::ISODate 在 msec≠0 时会带 ".zzz"），
    // 读回来若不截，第一次排程的「名义到点时刻」就落在某一拍之后几毫秒 ⇒
    // 那一拍醒来时 now < next ⇒ 判「未到点」跳过 ⇒ 升级后第一轮晚一个心跳。
    //
    // 截断只舍去毫秒，不改变日期与秒数 ⇒ 不影响定点模式的跨天判定。
    return local.addMSecs(-local.time().msec());
}

void ConfigStore::saveAutoSyncLastRunAt(const QDateTime &dt)
{
    auto s = openSettings();
    if (!dt.isValid()) {
        s->remove(QLatin1String(kAutoSyncLastRunKey));
        return;
    }
    // 存 UTC：见 loadAutoSyncLastRunAt() 里关于时区的说明。
    // Qt::ISODate 对 UTC 会带 "Z" 后缀，fromString 能正确还原成 UTC。
    s->setValue(QLatin1String(kAutoSyncLastRunKey), dt.toUTC().toString(Qt::ISODate));
}

// ------------------------------------------------------------------
// 自动同步的两种模式：按间隔重复 / 每天定点
// ------------------------------------------------------------------

AutoSyncScheduler::Mode ConfigStore::loadAutoSyncMode()
{
    auto s = openSettings();
    // 老 ini 没有这个键 → 老版本的行为（按间隔），**一个字都不写回去**。
    if (!s->contains(QLatin1String(kAutoSyncModeKey)))
        return AutoSyncScheduler::ModeInterval;
    const QString raw = s->value(QLatin1String(kAutoSyncModeKey)).toString().trimmed().toLower();
    if (raw == QLatin1String("daily"))
        return AutoSyncScheduler::ModeDaily;
    // 认不出来的值一律当 interval：宁可退回老行为，也不要停摆。
    return AutoSyncScheduler::ModeInterval;
}

void ConfigStore::saveAutoSyncMode(AutoSyncScheduler::Mode mode)
{
    auto s = openSettings();
    s->setValue(QLatin1String(kAutoSyncModeKey),
                mode == AutoSyncScheduler::ModeDaily ? QStringLiteral("daily")
                                                     : QStringLiteral("interval"));
}

QTime ConfigStore::loadAutoSyncDailyTime()
{
    auto s = openSettings();
    // 老 ini 没有这个键 → 默认 03:00，且**不写回去**。
    if (!s->contains(QLatin1String(kAutoSyncDailyTimeKey)))
        return AutoSyncScheduler::defaultDailyTime();
    QTime t;
    // 存的值认不出来（有人手改坏了）→ 退回默认值，同样不写回去。
    if (!AutoSyncScheduler::parseDailyTime(
            s->value(QLatin1String(kAutoSyncDailyTimeKey)).toString(), &t))
        return AutoSyncScheduler::defaultDailyTime();
    return t;
}

void ConfigStore::saveAutoSyncDailyTime(const QTime &t)
{
    auto s = openSettings();
    s->setValue(QLatin1String(kAutoSyncDailyTimeKey),
                AutoSyncScheduler::dailyTimeToString(t));
}

// ------------------------------------------------------------------
// 「开机自动启动」
// ------------------------------------------------------------------

bool ConfigStore::loadAutoStartEnabled()
{
    auto s = openSettings();
    // 老 ini 里没有这个键 → 视为 false（自启默认关闭）。
    // **不要因为读不到就把文件写回去**：只有用户真正点过勾选框才落盘。
    if (!s->contains(QLatin1String(kAutoStartKey)))
        return false;
    return s->value(QLatin1String(kAutoStartKey), false).toBool();
}

void ConfigStore::saveAutoStartEnabled(bool enabled)
{
    auto s = openSettings();
    s->setValue(QLatin1String(kAutoStartKey), enabled);
}

QString ConfigStore::configDir()
{
    // ini 实际所在目录 = 配置目录。锁文件放这里：
    // 同一份配置 = 同一把锁；不同配置目录（测试沙箱、并行用例）= 互不干扰。
    const QString dir = QFileInfo(iniPath()).absolutePath();
    QDir().mkpath(dir);
    return dir;
}

QString ConfigStore::autoStartCommand()
{
    const QString exe = QCoreApplication::applicationFilePath();
    if (exe.isEmpty())
        return QString();
#ifdef _WIN32
    // Windows：直接跑 exe。带引号 —— 装在 "C:\Program Files\..." 下时，
    // 不加引号注册表这一行会被 Windows 截断成 "C:\Program"。
    return QStringLiteral("\"%1\"").arg(exe);
#else
    // 麒麟交付包里**必须**走 run.sh：
    // 直接跑二进制会缺 LD_LIBRARY_PATH / QT_PLUGIN_PATH，
    // Qt 找不到平台插件直接 abort(134)，一个字符都不输出。
    const QString runSh = QDir::cleanPath(QCoreApplication::applicationDirPath()
                                         + QStringLiteral("/run.sh"));
    if (QFile::exists(runSh))
        return QStringLiteral("sh \"%1\"").arg(runSh);
    return QStringLiteral("\"%1\"").arg(exe);
#endif
}

QString ConfigStore::autoStartLocation()
{
#ifdef _WIN32
    return QString::fromUtf8(u8"注册表 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\SyncReplayTool");
#else
    return autostartFilePath();
#endif
}

bool ConfigStore::isAutoStartActive()
{
#ifdef _WIN32
    HKEY hKey = NULL;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kWinRunSubKey, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t buf[2048];
    DWORD type = 0;
    DWORD cb = sizeof(buf);
    if (RegQueryValueExW(hKey, kWinRunValue, NULL, &type,
                         reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS) {
        found = (type == REG_SZ || type == REG_EXPAND_SZ);
    }
    RegCloseKey(hKey);
    return found;
#else
    return QFile::exists(autostartFilePath());
#endif
}

bool ConfigStore::applyAutoStart(bool enable, QString *errorOut)
{
    QString err;
    bool ok = false;

#ifdef _WIN32
    HKEY hKey = NULL;
    // 只申请 KEY_SET_VALUE：只写值，不读整个键。
    const LSTATUS r = RegCreateKeyExW(HKEY_CURRENT_USER, kWinRunSubKey, 0, NULL,
                                      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                      NULL, &hKey, NULL);
    if (r != ERROR_SUCCESS) {
        err = QString::fromUtf8(u8"打不开注册表项 HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run（错误码 %1）")
                  .arg(static_cast<uint>(r));
    } else {
        if (enable) {
            const QString cmd = autoStartCommand();
            if (cmd.isEmpty()) {
                err = QString::fromUtf8(u8"取不到本程序的绝对路径，无法写自启项");
            } else {
                // Qt 5.9 的 QString::utf16() 返回 const ushort*（Qt 6 才改成 char16_t*）。
                const ushort *w = cmd.utf16();
                const LSTATUS wr = RegSetValueExW(
                    hKey, kWinRunValue, 0, REG_SZ,
                    reinterpret_cast<const BYTE *>(w),
                    static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
                if (wr != ERROR_SUCCESS) {
                    err = QString::fromUtf8(u8"写注册表自启项失败（错误码 %1）")
                              .arg(static_cast<uint>(wr));
                } else {
                    ok = true;
                }
            }
        } else {
            const LSTATUS dr = RegDeleteValueW(hKey, kWinRunValue);
            // ERROR_FILE_NOT_FOUND = 本来就没有 = 本来就处于"没开自启"，算成功。
            if (dr != ERROR_SUCCESS && dr != ERROR_FILE_NOT_FOUND) {
                err = QString::fromUtf8(u8"删除注册表自启项失败（错误码 %1）")
                          .arg(static_cast<uint>(dr));
            } else {
                ok = true;
            }
        }
        RegCloseKey(hKey);
    }
#else
    const QString file = autostartFilePath();
    if (enable) {
        const QDir dir = QFileInfo(file).absoluteDir();
        if (!dir.mkpath(QStringLiteral("."))) {
            err = QString::fromUtf8(u8"创建自启目录失败：%1").arg(dir.absolutePath());
        } else {
            QFile f(file);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                err = QString::fromUtf8(u8"写自启文件失败：%1").arg(file);
            } else {
                QString text;
                text += QStringLiteral("[Desktop Entry]\n");
                text += QStringLiteral("Type=Application\n");
                text += QString::fromUtf8(u8"Name=数据同步工具\n");
                text += QStringLiteral("Exec=") + autoStartCommand() + QStringLiteral("\n");
                text += QStringLiteral("X-GNOME-Autostart-enabled=true\n");
                text += QStringLiteral("Terminal=false\n");
                f.write(text.toUtf8());
                f.close();
                ok = true;
            }
        }
    } else {
        QFile f(file);
        if (!f.exists()) {
            ok = true;   // 本来就没有 = 本来就处于"没开自启"
        } else if (!f.remove()) {
            err = QString::fromUtf8(u8"删除自启文件失败：%1").arg(file);
        } else {
            ok = true;
        }
    }
#endif

    if (errorOut)
        *errorOut = err;
    return ok;
}

// 端口字符串是不是「合格的端口号」。
//
// 只有 1-5 位**纯 ASCII 十进制**才算数，值落在 1-65535。
// 不用 QChar::isDigit()：实测（本 Qt 5.9.6，变异锁实测过）它对全角数字
// 「２２２６」也返回 false，于是这条判据恰好能挡住全角 —— 但那是**碰巧**，
// 不同 Qt 版本对 Unicode Nd 的覆盖范围并不一致，不能作为依据。
// 统一成「逐字符比 ASCII 码」，读端和写端共用同一条判据，与 Qt 版本无关。
// 真正让全角端口变成 22 的是 QString::toInt() 对它返回 0（实测，见 git 里的缺陷记录）。
bool ConfigStore::isPlainPortText(const QString &text)
{
    if (text.isEmpty() || text.size() > 5) {
        return false;
    }
    for (int i = 0; i < text.size(); ++i) {
        const ushort c = text.at(i).unicode();
        if (c < '0' || c > '9') {
            return false;
        }
    }
    const int v = text.toInt();
    return v > 0 && v <= 65535;
}

TargetConfig ConfigStore::loadTarget()
{
    // 「记住设置」不勾 = 不恢复目标连接信息。
    //
    // 原来这个复选框只记录**自己**被勾没被勾，实际落盘与恢复走的是另一条路：
    //   落盘：saveTarget 无条件写 target/*
    //   恢复：loadTarget 无条件读 target/*
    // 于是用户取消勾选之后，IP/账号/密码照样写进配置文件、下次启动照样自动填回，
    // 界面上那个勾选框等于摆设。
    //
    // 这里必须在**读任何键之前**返回：不勾就不该把配置里的连接信息读出来。
    if (!loadRememberSettings()) {
        TargetConfig t;
        t.port = 22;
        t.autoAcceptHostKey = true;
        return t;
    }

    auto s = openSettings();

    TargetConfig t;
    // 所有**界面可填**的字段都要过一次自愈。原先只有 syncRules/remoteSubDir 走了
    // repairMojibake，host/user/keyPath/remoteRoot 这些同样是界面填进去、
    // 又被 saveTarget 原样写回来的值：坏值会一路躺在配置里，换多少个版本都
    // 不会自己好，而且用户看到的是"连不上 10.0.0.1"（host 里的中文被吃成
    // 一串错字），永远不会想到去检查配置文件。工具规则里的 exePath/argsTemplate
    // 同理，那两栏是用户从别的配置里抄过来的，最容易带进乱码。
    t.host              = repairMojibake(s->value(QStringLiteral("target/host")).toString());
    // 手改过 ini 或版本升级写出脏值时，toInt() 会静默给 0 或负数，
    // 界面显示 0 端口、同步直接失败却看不出原因。这里只接受 1-65535。
    //
    // 但**静默本身**就是要修的那一半。全角数字、带空格、字母都会让 toInt() 失败，
    // 于是一律退回 22，用户点开始同步看到的是「网络层：没有连到目标电脑，可能是
    // IP 填错、网络或交换机不通、或者目标电脑上这个端口没有服务」——
    // 真因就在他屏幕上的端口栏，却被指去了防火墙和交换机。
    // 而 ssh 那边给不了任何补救线索：传下去的是**合法**的 22，ssh 无话可说，
    // "Bad port" 那条规则只在传 0 时才触发。
    //
    // 所以这里原样取字符串再判一次：只有**纯 ASCII 十进制**（1-65535）才算合格。
    // 空值也不放过 —— 理由见下面那段。
    const QVariant rawPort  = s->value(QStringLiteral("target/port"));
    const QString  portText = rawPort.toString().trimmed();
    const int      port     = rawPort.toInt();
    t.port              = (port > 0 && port <= 65535) ? port : 22;
    // 这里无条件调 noticePortProblem()，判断在它里面。
    // 上一版写的是 `if (!portText.isEmpty() && !isPlainPortText(portText))`，
    // 那个 isEmpty 守卫写在**谓词外面**，把「根本没填端口」整类漏掉了 ——
    // 而这恰恰是最容易走到的入口：客户第一次装、端口栏空着就保存；
    // 或者 ini 是手写/别的工具生成的，压根没有 port 字段（QSettings 取回来是空串）。
    // 漏掉之后的症状和全角端口一模一样：静默退 22，界面一个提示都没有，
    // 同步时全部失败并把原因指去网络层和防火墙，真因就在他屏幕上那一栏。
    noticePortProblem(portText);
    t.user              = repairMojibake(s->value(QStringLiteral("target/user")).toString());
    t.keyPath           = repairMojibake(s->value(QStringLiteral("target/keyPath")).toString());

    const QString storedPw = s->value(QStringLiteral("target/password")).toString();
    t.password          = SecretStore::unprotect(storedPw);
    // 这里**不弹窗**，只置标记。原先是 notice() 走 MainWindow 的模态框，代价有两个：
    //   1) 启动第一步就弹一个用户还没法处理的错误框，必须先点掉才能用软件；
    //   2) 更要命的是，点掉 OK 之后关窗仍会走 saveConfig() -> saveTarget()，
    //      口令框是空的 -> 原密文被 remove() 掉，**再也回不来**。
    //      弹框反而给用户制造了"我已经处理过了"的错觉。
    // 数据丢失那一条已经由 passwordUnreadable 挡住了（见 saveTarget）。
    //
    // 但**不能就这么算了**：标记全项目只有这里写、saveTarget 里读，
    // 没有任何界面代码消费它 —— 也就是说注释里说的
    // 「常驻在口令栏旁边的提示」其实从来没实现。后果是：
    // 换机器 / 重装系统 / 复制过配置之后，配置文件里的密文本机解不开，
    // 用户看到的是一个**空口令框 + 零解释**，于是理所当然地认为"没配过密码"，
    // 不会去重填，之后每次同步都在认证这一步失败，
    // 而报错方向是网络或账号 —— 人会一直查错的地方。
    // 配置明明里有东西、界面却装作没有，这是最坏的一类静默。
    //
    // 提示走 notice() -> MainWindow::onConfigNotice -> SyncPanel::appendNotice
    // -> 日志抽屉里的 WARN 行（还会自动展开抽屉）。这是本项目早就定下的规矩：
    // 提示一律进日志抽屉，不再弹模态框（弹框会卡在关窗路径的嵌套事件循环里，
    // 表现为"关不掉"）。不加任何新控件。
    if (!storedPw.isEmpty() && t.password.isEmpty()) {
        t.passwordUnreadable = true;
        notice(QString::fromUtf8(u8"\u8BFB\u53D6\u5DF2\u4FDD\u5B58\u7684\u5BC6\u7801\u5931\u8D25\uFF0C"
                                 u8"\u672C\u673A\u65E0\u6CD5\u89E3\u5BC6\u914D\u7F6E\u6587\u4EF6\u91CC\u7684\u5BC6\u6587"
                                 u8"\uFF08\u914D\u7F6E\u672C\u8EAB\u6CA1\u4E22\uFF09\uFF0C"
                                 u8"\u8BF7\u5728\u8BBE\u7F6E\u91CC\u91CD\u65B0\u8F93\u5165\u767B\u5F55\u5BC6\u7801\u3002"));
    }

    t.remoteRoot        = repairMojibake(s->value(QStringLiteral("target/remoteRoot")).toString());
    t.localRoot         = repairMojibake(s->value(QStringLiteral("target/localRoot")).toString());

    // 已经被吃掉反斜杠的旧值：**认出来，告诉用户重填，但绝不去猜它原本是什么**。
    //
    // 写侧已经归一化了，从这一版开始不会再产生新的坏值；
    // 但升级上来的用户配置里已经躺着一批坏值，而坏值的表现是
    // 「私钥文件不存在或不可读」/「本机目录不存在」——
    // 文件明明就在那儿。不提示的话，用户会去反复挑私钥、重新生成密钥，
    // 怎么试都没用，因为错的不是他挑的路径，是配置文件里那个字符串。
    if (pathLooksCorrupted(t.keyPath))
        notice(corruptKeyPathNotice());
    if (pathLooksCorrupted(t.localRoot))
        notice(corruptLocalRootNotice());

    t.autoAcceptHostKey = s->value(QStringLiteral("target/autoAcceptHostKey"), true).toBool();
    return t;
}

void ConfigStore::saveTarget(const TargetConfig &target)
{
    auto s = openSettings();

    // 「记住设置」不勾 = 不保存目标连接信息，并且**清掉配置文件里原有的那些**。
    //
    // 清掉这一步是必须的：用户的直觉是"别把我的口令留在机器上"。
    // 只是不写、却把旧密文留在文件里，等于没兑现。
    //
    // 这个闸门必须放在**最前面**：
    //   1) 放在 password 分支之前 —— 不然「口令框是空的」那一支的
    //      s->remove("target/password") 会在用户**没要求清空**的情况下
    //      把那份"本机解不开、换台机器也许能解开"的密文删掉；
    //   2) 放在任何 setValue 之前 —— 不勾就不能写。
    if (!loadRememberSettings()) {
        static const char *kTargetKeys[] = {
            "host", "port", "user", "keyPath", "password",
            "remoteRoot", "localRoot", "autoAcceptHostKey"
        };
        bool had = false;
        for (unsigned i = 0; i < sizeof(kTargetKeys) / sizeof(kTargetKeys[0]); ++i) {
            const QString key = QStringLiteral("target/") + QLatin1String(kTargetKeys[i]);
            if (s->contains(key)) {
                s->remove(key);
                had = true;
            }
        }
        flushSettings(s.get());
        if (had) {
            notice(QString::fromUtf8(u8"按「记住设置」的当前状态，配置文件里原有的目标连接信息"
                                     u8"（IP、端口、账号、密码、私钥、目录）已经清除。"));
        }
        return;
    }

    s->setValue(QStringLiteral("target/host"), target.host);
    s->setValue(QStringLiteral("target/port"), target.port);
    s->setValue(QStringLiteral("target/user"), target.user);
    // keyPath / localRoot / remoteRoot 是本项目仅有的三个"用户手打 Windows 路径"的字段，
    // 也就是 normalizePathForStore 要保护的全部对象（工具 exePath 在 saveToolRules 里另行处理）。
    // 漏掉任何一个，它就会在"用户手打路径 → 关窗保存 → 下次启动"这一轮往返里被吃掉。
    // 归一化能挡住的是**程序自己存进去的值**：用户在弹窗里手打/粘贴
    // C:\keys\id_rsa，我们存成 C:/keys/id_rsa，之后无论谁读写都不会变形。
    //
    // 挡不住的是"ini 里本来就是手写的单反斜杠"那种：QSettings 一旦读过这个键，
    // 整个文件就会在 sync 时**从内存里的解析结果重新编码一遍**，
    // 于是跳过 setValue 也拦不住它把读坏的 C:P7000-keysd_rsa 写回去
    // （这一条实测踩过：守卫写了，文件照样每轮被覆盖一次）。
    // 而且损坏是**一次性的** —— 第二轮不会再继续劣化，因为已经没有反斜杠可吃了。
    // 所以对这种来源，唯一有效的处理是 loadTarget 里那条提示：让用户重选一次。
    s->setValue(QStringLiteral("target/keyPath"), normalizePathForStore(target.keyPath));

    if (target.password.isEmpty()) {
        if (target.passwordUnreadable) {
            // 口令框是空的**只是因为原来那条密文本机解不开**，不是用户想清空。
            // 这里的 remove() 是单向门：删掉之后用户再也没机会把它恢复出来，
            // 而界面上只剩一个空口令框，看不出配置里原本存着东西。
            // 用户重新输入后（passwordUnreadable 变 false）自然就写进去了，
            // 中间这段时间原密文必须原样留着。
            // 不弹窗：提示走日志抽屉，见 loadTarget 里的同名分支。
        } else {
            // 用户主动清空密码：删键，和"没设过密码"保持同一个状态
            s->remove(QStringLiteral("target/password"));
        }
    } else {
        const QString enc = SecretStore::protect(target.password);
        // protect() 失败返回空串：写进去就等于把用户之前存的密文抹成空，
        // 下次启动密码框空白且再也回不来。保留原密文，只提示。
        if (enc.isEmpty()) {
            notice(QString::fromUtf8(u8"\u5BC6\u7801\u52A0\u5BC6\u5931\u8D25\uFF0C"
                                     u8"\u672C\u6B21\u6CA1\u6709\u4FDD\u5B58\u5BC6\u7801\uFF0C"
                                     u8"\u914D\u7F6E\u6587\u4EF6\u4E2D\u5DF2\u6709\u7684\u5BC6\u6587\u4FDD\u6301\u4E0D\u53D8\u3002"
                                     u8"\u8BF7\u7A0D\u540E\u91CD\u8BD5\u4FDD\u5B58\u3002"));
        } else {
            s->setValue(QStringLiteral("target/password"), enc);
        }
    }

    // 远端路径也归一化：它是 POSIX 路径，反斜杠本来就没有意义，
    // 但用户会从自己的 Windows 记事本里复制过来，留着反斜杠就等于
    // 让它在 ini 里被吃掉一轮、再也回不来。归一化后两平台行为一致。
    s->setValue(QStringLiteral("target/remoteRoot"), normalizePathForStore(target.remoteRoot));
    s->setValue(QStringLiteral("target/localRoot"), normalizePathForStore(target.localRoot));
    s->setValue(QStringLiteral("target/autoAcceptHostKey"), target.autoAcceptHostKey);
    flushSettings(s.get());
}

// beginReadArray 返回的是 ini 里写的 size，QSettings 不会去核实真的有那么多条。
// 手改坏的文件里一个 size=99999999 就能让启动空转几十分钟。这里封顶。
// 写侧必须用同一个上限，否则攒到 512 条以上后，重启读不回来、
// 再保存一次就把多出来的规则永久覆盖掉。
const int kMaxRules = 512;

// 一个后缀最多配几个工具。读侧按它封顶、写侧按同一个上限截断，
// 理由与 kMaxRules 相同：ini 里手写的 toolCount=99999999 不能让启动空转几十分钟。
const int kMaxToolsPerRule = 512;

// 规则行里第 k 个工具的键名。读侧和写侧必须调**同一个函数**：
// 照抄一份键名出来的版本必然跟着实现漂移，改坏实现、判据照样全绿。
QString toolKey(int k, const char *field)
{
    return QStringLiteral("tool") + QString::number(k) + QString::fromLatin1(field);
}

// 数组下标的基准（0 或 1）。
//
// Qt 5.9.6 实测（探针链接产品 ConfigStore 直接跑，见
// ai-workspace/fix-toolexe/probe-before.txt）：INI 数组下标在磁盘上是
// **1 起始** —— beginWriteArray + setArrayIndex(0) 写出来就是 1\key，
// 读侧 setArrayIndex(0) 读的也是 1\key。读写两侧约定一致，
// 程序自己写的文件自己读得回来，所以这个 1 不是缺陷，别去"修"它。
//
// 但手工按「第 0 条」的直觉写的文件（0\key）Qt 读不到：规则被判成没配过，
// 界面显示「未配置工具」，而 saveToolRules 会整段重写 toolRules ——
// 这一保存就把用户手写的配置永久抹掉了，比"读不到"本身更糟。
// 所以这里把基准认出来：出现 0\suffix 就按 0 起始读，两种写法都能读出来。
int arrayIndexBase(QSettings *s, const QString &array)
{
    return s->contains(array + QLatin1String("/0/suffix")) ? 0 : 1;
}

// 数 ini 里某个数组段下**真实存在**的条目数。
//
// 不能用 beginReadArray 的返回值：它只是把 size 键原样读出来，
// QSettings 不会去核实真的有那么多条。手工改过配置、或者文件被别的程序截断过时，
// size 键和实际条数经常对不上 —— 而"是不是有东西被丢弃了"，
// 唯一可信的依据就是真实条数。
//
// 每条规则在 ini 里至少有一个 <array>/<n>/suffix 键（写侧无条件写），
// 所以数这个键的个数就是条数：既不受 size 键撒谎影响，
// 也不会因为某条规则少了 remoteSubDir 就少数一条。
//
// 下面两处"要不要提示用户规则被截断"的判断都用它，比较对象必须是 kMaxRules
// 而不是"这次准备写几条"(count)。用 count 会出现这种荒谬提示：
// 用户只是在弹窗里**删掉了一条规则**，磁盘 2 条、准备写 1 条，
// 于是每次保存都弹「配置文件里原有 2 条同步规则，超过上限 512 条…」——
// 2 条跟 512 扯不上任何关系，这句话会让人以为配置被截断，
// 转头去检查自己是不是写多了规则。
// 实测就是这么撞上的：只配了 1 条规则的 localedge 场景，
// 日志里赫然一条「配置文件里原有 2 条同步规则，超过上限 512 条」。
int countArrayEntries(const QSettings &s, const QString &array)
{
    const QString pfx = array + QLatin1Char('/');
    const QString sfx = QStringLiteral("/suffix");
    int n = 0;
    const QStringList keys = s.allKeys();
    for (int i = 0; i < keys.size(); ++i) {
        const QString k = keys.at(i);
        if (k.size() > pfx.size() && k.startsWith(pfx) && k.endsWith(sfx))
            ++n;
    }
    return n;
}

// 同时给出「size 键写了多少」与「磁盘上真实有多少」，由调用方决定信谁。
//
// beginReadArray 只把 size 键原样读出来，QSettings 不会去核实真的有那么多条。
// 手工改过配置、或者文件被别的程序截断过时，两个数字经常对不上：
//   * size 少报 -> 读出来的规则少了，用户丢掉规则，且界面毫无提示
//   * size 多报 -> 循环在空下标上空转（kMaxRules 封顶，但仍能空转很久）
//
// 真实条数 > 0 时以真实条数为准 —— 它同时修好上面两条路；
// 真实条数为 0 时回落到 size 键（空段本来就是 0/0）。
// 两条路都要过 kMaxRules 封顶，否则一个 size=99999999 的坏文件能空转几十分钟。
int readArrayCount(QSettings *s, const QString &array, int *declaredOut, int *realOut)
{
    // 顺序要紧：数真实条数必须在 beginReadArray **之前**做完。
    //
    // 实测踩出来的：先 beginReadArray 再 allKeys()，返回条数是对的（3），
    // 可紧跟着的 setArrayIndex(0) 读出来是空的 —— 数组的首条被跳过了，
    // 表现成"3 条规则只加载出 2 条，而且第一条的字段串到了第二条上"。
    // 也就是说 allKeys() 在数组读取上下文里走一遍，会把下标基址挪掉。
    // 先数、再开数组，就不会有这个问题。
    const int real = countArrayEntries(*s, array);
    const int declared = s->beginReadArray(array);
    if (declaredOut)
        *declaredOut = declared;
    if (realOut)
        *realOut = real;
    if (real > 0)
        return qMin(real, kMaxRules);
    return qMin(declared, kMaxRules);
}

// 只在「真实条数比 size 键多」时报不一致：那才是**条目正在被丢弃**的方向。
// size 多报只是空转，不值得打扰用户。
bool arraySizeMismatch(int declared, int real)
{
    return real > declared;
}

QVector<SyncRule> ConfigStore::loadSyncRules()
{
    auto s = openSettings();
    QVector<SyncRule> rules;
    int declaredSync = 0;
    int realSync = 0;
    const int count = readArrayCount(s.get(), QStringLiteral("syncRules"), &declaredSync, &realSync);
    if (arraySizeMismatch(declaredSync, realSync)) {
        notice(QString::fromUtf8(u8"\u914D\u7F6E\u6587\u4EF6\u91CC syncRules \u6BB5\u5199\u4E86 %1 \u6761\uFF0C"
                                 u8"\u5B9E\u9645\u6709 %2 \u6761\uFF0C"
                                 u8"\u5DF2\u6309\u5B9E\u9645\u7684 %2 \u6761\u8BFB\u53D6\u3002")
                  .arg(declaredSync).arg(realSync));
    }
    for (int i = 0; i < count; ++i) {
        s->setArrayIndex(i);
        SyncRule r;
        // trim 必须和 SyncPanel::collectRules() 做的一模一样。
        // 两处逻辑只要有一处不一致，就会出现"界面上写着一条规则、
        // 扫描时却按另一套取值走"的情况：配置文件里存的是 "  spe  "，
        // collectRules() 会归一成 spe 去扫，而这里原样返回给界面显示，
        // 用户看到一条空白规则、界面又报"未匹配到文件"。
        r.suffix       = s->value(QStringLiteral("suffix")).toString().trimmed().toLower();
        // 配置里可能存着带前导点的 ".spe"（用户手改 ini，或旧版本没归一化）。
        // QFileInfo::suffix() 是不带点的，全等比较匹配不上 → 一个文件都不传还显示成功。
        while (r.suffix.startsWith(QLatin1Char('.')))
            r.suffix.remove(0, 1);
        r.remoteSubDir = repairMojibake(s->value(QStringLiteral("remoteSubDir"))
                                            .toString().trimmed());
        if (!r.suffix.isEmpty())
            rules.append(r);
    }
    s->endArray();
    return rules;
}

void ConfigStore::saveSyncRules(const QVector<SyncRule> &rules)
{
    auto s = openSettings();
    const int count = qMin(rules.size(), kMaxRules);
    if (rules.size() > kMaxRules) {
        notice(QString::fromUtf8(u8"\u540C\u6B65\u89C4\u5219\u8D85\u8FC7 512 \u6761\uFF0C"
                                 u8"\u672C\u6B21\u53EA\u4FDD\u5B58\u524D 512 \u6761\uFF0C"
                                 u8"\u5176\u4F59\u89C4\u5219\u672A\u5199\u5165\u914D\u7F6E\u6587\u4EF6\u3002"));
    }
    // 读侧的 kMaxRules 截断会把 ini 里第 513 条之后的内容**在内存里抹掉**，
    // 于是 rules.size() 恰好等于 512，上面那个判断为假，界面一次提示都没有，
    // 而这一次保存就把磁盘上的多余规则永久删除了。
    // 所以还要拿**磁盘上的原始条数**再比一次 —— 同一个上限不够，
    // 判断的依据必须是"读之前有多少条"，不是"现在手里有多少条"。
    const int onDisk = countArrayEntries(*s, QStringLiteral("syncRules"));
    if (onDisk > kMaxRules) {
        notice(QString::fromUtf8(u8"\u914D\u7F6E\u6587\u4EF6\u91CC\u539F\u6709 %1 \u6761\u540C\u6B65\u89C4\u5219\uFF0C"
                                 u8"\u8D85\u8FC7\u4E0A\u9650 512 \u6761\uFF0C\u672C\u6B21\u53EA\u4FDD\u5B58\u524D 512 \u6761\uFF0C"
                                 u8"\u5176\u4F59\u89C4\u5219\u5DF2\u4ECE\u914D\u7F6E\u6587\u4EF6\u4E2D\u79FB\u9664\u3002")
                     .arg(onDisk));
    }
    // beginWriteArray 只覆盖**它自己写到的下标**，数组原来更长时，
    // 多出来的那些下标键会留在磁盘上。
    //
    // 实测（Qt 5.9.6 / MSVC2015 x64，逐进程跑，见 ai-workspace/wB-f3b2/test-b2.ps1）：
    //   · 存 3 条 -> 删光再存：组里只剩 [size] 是**做不到的**，
    //     1/suffix 1/remoteSubDir 2/… 3/… 6 个键全部残留；
    //   · 磁盘上原有 600 条、这次只存 512 条：513..600 共 88 个下标键全部残留。
    //
    // 读侧被 size 挡住，所以规则**不会**自己复活（loadSyncRules 只读到 size），
    // 但残留有两个真实后果：
    //   1) 用户已经删掉的规则、包括已撤掉的**工具 exe 路径**，永远留在配置文件里；
    //   2) countArrayEntries() 数的是 */suffix 键，残留会被一起数进去 ——
    //      于是上面那个 onDisk > 512 的提示会**永久卡住**：
    //      用户早就把规则删到 10 条了，每次保存还在报"原有 600 条，超过上限"。
    //      这条提示的整个前提就是"磁盘上真实条数"，前提错了它就恒为真。
    //
    // 位置很关键：必须在上面 countArrayEntries 之后，
    // 否则连"磁盘上原有几条"这个判据自己也一起抹掉了，
    // 反而永远看不到该提示。
    s->remove(QStringLiteral("syncRules"));
    s->beginWriteArray(QStringLiteral("syncRules"), count);
    for (int i = 0; i < count; ++i) {
        s->setArrayIndex(i);
        s->setValue(QStringLiteral("suffix"), rules.at(i).suffix);
        s->setValue(QStringLiteral("remoteSubDir"), rules.at(i).remoteSubDir);
    }
    s->endArray();
    flushSettings(s.get());
}

QVector<ToolRule> ConfigStore::loadToolRules()
{
    auto s = openSettings();
    QVector<ToolRule> rules;
    // 基准必须在 beginReadArray 之前问：进了数组上下文，
    // value()/contains() 都会被拼上当前下标前缀，问出来的是另一个键。
    const int base = arrayIndexBase(s.get(), QStringLiteral("toolRules"));
    int declaredTool = 0;
    int realTool = 0;
    const int count = readArrayCount(s.get(), QStringLiteral("toolRules"), &declaredTool, &realTool);
    if (arraySizeMismatch(declaredTool, realTool)) {
        notice(QString::fromUtf8(u8"\u914D\u7F6E\u6587\u4EF6\u91CC toolRules \u6BB5\u5199\u4E86 %1 \u6761\uFF0C"
                                 u8"\u5B9E\u9645\u6709 %2 \u6761\uFF0C"
                                 u8"\u5DF2\u6309\u5B9E\u9645\u7684 %2 \u6761\u8BFB\u53D6\u3002")
                  .arg(declaredTool).arg(realTool));
    }
    // 逐行按**绝对键名**读，不再用 setArrayIndex()：
    // 只有前者能同时吃下 0 起始（手写）和 1 起始（Qt 自己写的）两种格式，
    // setArrayIndex(0) 只认 1\key。退出数组上下文之后，
    // 两种基准就变成同一段代码，不需要分支。
    s->endArray();
    for (int i = 0; i < count; ++i) {
        const QString row = QStringLiteral("toolRules/%1/").arg(i + base);
        ToolRule r;
        // 归一化口径必须和 ToolPanel::collectRules() 一致，理由同 loadSyncRules。
        r.suffix       = s->value(row + QStringLiteral("suffix")).toString().trimmed().toLower();
        while (r.suffix.startsWith(QLatin1Char('.')))
            r.suffix.remove(0, 1);
        r.exePath      = repairMojibake(s->value(row + QStringLiteral("exePath")).toString().trimmed());
        // argsTemplate 不 trim：前后空格在这里是用户有意为之的占位，
        // collectRules() 同样原样保留，两边保持一致。
        r.argsTemplate = repairMojibake(s->value(row + QStringLiteral("argsTemplate")).toString());

        // 界面真正在用的是三个平行数组（设置弹窗规则表就是这三个字段），
        // 老配置只有上面两个标量。数组读回来，标量留作老配置的兜底：
        // ToolRule::nameAt/exeAt/argsAt 在数组为空时会自动回落到标量，
        // 所以"只有标量"的老文件读出来的行为和修复前完全一样。
        int toolCount = s->value(row + QStringLiteral("toolCount")).toInt();
        toolCount = qBound(0, toolCount, kMaxToolsPerRule);
        for (int k = 0; k < toolCount; ++k) {
            const QString name = repairMojibake(s->value(row + toolKey(k, "Name")).toString().trimmed());
            const QString exe  = repairMojibake(s->value(row + toolKey(k, "Exe")).toString().trimmed());
            // 参数同样不 trim，理由同 argsTemplate。
            const QString args = repairMojibake(s->value(row + toolKey(k, "Args")).toString());
            // 程序路径为空的条目直接丢。留着一个空 exe 的工具，
            // 工具列照样画按钮，点下去只会报「工具不存在：<空>」；
            // ToolPanel::collectRules() 也是这么丢的（见 ToolPanel.cpp），
            // 两侧口径必须一致。
            if (exe.isEmpty())
                continue;
            r.toolNames.append(name);
            r.toolExePaths.append(exe);
            r.toolArgs.append(args);
        }

        // 判据从「标量 exePath 非空」放宽成「至少有一个能用的 exe」。
        // 只判标量的话，设置弹窗存进去的规则（标量是空的）会被整条丢掉 ——
        // 这正是「工具列永远是未配置工具」的最后一段。
        bool usable = !r.exePath.isEmpty();
        for (int k = 0; k < r.toolExePaths.size() && !usable; ++k)
            usable = !r.toolExePaths.at(k).isEmpty();
        if (!r.suffix.isEmpty() && usable)
            rules.append(r);
    }
    return rules;
}

void ConfigStore::saveToolRules(const QVector<ToolRule> &rules)
{
    auto s = openSettings();
    const int count = qMin(rules.size(), kMaxRules);
    if (rules.size() > kMaxRules) {
        notice(QString::fromUtf8(u8"\u5DE5\u5177\u8C03\u7528\u89C4\u5219\u8D85\u8FC7 512 \u6761\uFF0C"
                                 u8"\u672C\u6B21\u53EA\u4FDD\u5B58\u524D 512 \u6761\uFF0C"
                                 u8"\u5176\u4F59\u89C4\u5219\u672A\u5199\u5165\u914D\u7F6E\u6587\u4EF6\u3002"));
    }
    // 工具规则是同一个 512 上限，但这一侧原本**一次提示都没有**：
    // loadToolRules 读的时候就已经截到 512，界面最多只能看到 512 条，
    // 再保存一次就把磁盘上第 513 条之后的内容永久删掉，用户全程不知情。
    // 判断口径和 saveSyncRules 一致：数磁盘上的真实条数，跟 kMaxRules 比。
    const int onDisk = countArrayEntries(*s, QStringLiteral("toolRules"));
    if (onDisk > kMaxRules) {
        notice(QString::fromUtf8(u8"\u914D\u7F6E\u6587\u4EF6\u91CC\u539F\u6709 %1 \u6761\u5DE5\u5177\u8C03\u7528\u89C4\u5219\uFF0C"
                                 u8"\u8D85\u8FC7\u4E0A\u9650 512 \u6761\uFF0C\u672C\u6B21\u53EA\u4FDD\u5B58\u524D 512 \u6761\uFF0C"
                                 u8"\u5176\u4F59\u89C4\u5219\u5DF2\u4ECE\u914D\u7F6E\u6587\u4EF6\u4E2D\u79FB\u9664\u3002")
                    .arg(onDisk));
    }
    // 同 saveSyncRules：beginWriteArray 不清旧下标，已删除的规则残留在磁盘上，
    // 工具规则这一侧残留的是**可执行文件路径**，更不该留着。
    // 同样必须放在 countArrayEntries 之后、beginWriteArray 之前。
    s->remove(QStringLiteral("toolRules"));
    s->beginWriteArray(QStringLiteral("toolRules"), count);
    for (int i = 0; i < count; ++i) {
        const ToolRule &r = rules.at(i);
        s->setArrayIndex(i);
        s->setValue(QStringLiteral("suffix"), r.suffix);

        // ---- 标量：老配置只认这两个键 ----
        // exePath / argsTemplate 过去是工具规则**唯一**的两个落盘字段，
        // 而设置弹窗的规则表填的是 ToolRule 的三个平行数组
        // （toolNames / toolExePaths / toolArgs，见 SettingsDialog::applyConfig），
        // 标量从来没被填过。于是用户填的「关联程序路径」在保存那一刻
        // 被静默丢弃：ini 里留下的是 exePath= 空串；重启后 loadToolRules
        // 又只认标量，于是工具列永远是「未配置工具」，程序永远起不来。
        //
        // 写侧必须把**生效的第一个工具**同时落到标量上：a) 老版本读得到，
        // b) 万一数组键读不出来，还有一份可用值。两个端口径从此对得上。
        const QString scalarExe  = r.exePath.isEmpty()       ? r.exeAt(0)     : r.exePath;
        const QString scalarArgs = r.argsTemplate.isEmpty()  ? r.argsAt(0)    : r.argsTemplate;
        // exePath 是第四个"用户手打 Windows 路径"的字段，而且是工具规则里
        // 唯一会被直接执行的路径 —— 被吃掉之后表现为「工具不存在」，
        // 用户会去重新找一遍 exe。漏掉它等于这条修复只做了一半。
        //
        // 这里**不做**"读坏了就不写回"的处理：saveToolRules 会整段重写 toolRules，
        // 少写一个键等于把磁盘上那份（虽然程序读不懂、人还读得懂）直接删掉，
        // 那比写坏更糟。工具路径只能靠提示用户重选来纠正。
        s->setValue(QStringLiteral("exePath"), normalizePathForStore(scalarExe));
        s->setValue(QStringLiteral("argsTemplate"), scalarArgs);

        // ---- 平行数组：界面真正在用的那三个字段 ----
        // 一个后缀可以配多个工具，所以要连名称和参数一起按序号落盘。
        // toolCount 是数组长度：读侧靠它知道要读几个（不能靠"有没有键"猜，
        // 因为参数本来就是允许为空的）。
        const int n = qBound(0, r.toolExePaths.size(), kMaxToolsPerRule);
        s->setValue(QStringLiteral("toolCount"), n);
        for (int k = 0; k < n; ++k) {
            const QString name = (k < r.toolNames.size()) ? r.toolNames.at(k) : QString();
            const QString args = (k < r.toolArgs.size())  ? r.toolArgs.at(k)  : QString();
            s->setValue(toolKey(k, "Name"), name);
            s->setValue(toolKey(k, "Exe"), normalizePathForStore(r.toolExePaths.at(k)));
            s->setValue(toolKey(k, "Args"), args);
        }
    }
    s->endArray();
    flushSettings(s.get());
}
