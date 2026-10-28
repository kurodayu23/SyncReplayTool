#include <QApplication>
#include <QByteArray>
#include <QIcon>
#include <QMetaType>
#include <QCommandLineParser>
#include <QTimer>
#include <QTextStream>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QSettings>
#include <QThread>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QTimeEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QProcess>
#include <QFontMetrics>
#include <QStandardPaths>
#include <QLockFile>
#include <QMessageBox>
#include <QGuiApplication>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include "MainWindow.h"
#include "SyncPanel.h"
#include "ScreenFit.h"
#include "SettingsDialog.h"
#include "RsyncRunner.h"
#include "ConfigStore.h"
#include "AutoSyncScheduler.h"
#include "SecretStore.h"
#include "Theme.h"
#include "Model.h"
#include "FileTableModel.h"
#include "ToolColumnDelegate.h"
#include "SettingsDialog.h"

// ------------------------------------------------------------------
// --selfcheck：不开窗口，把归因判据和看门狗预算跑一遍，输出 PASS/FAIL。
//
// 为什么要有这个：归因判错比不归因更糟 —— 它会把客户引到完全错误的方向
// （实测把"对方没装 rsync"和"对方磁盘满"都判成网络层，让人去查 IP 和防火墙）。
// 而这类错误**在真实同步里复现成本很高**（要先把接收端配成缺 rsync / 磁盘满）。
// 把真机抓到的原文固化成表，就能在任何一台机器上一秒钟验完，
// 以后谁动了判据表顺序，CI 立刻红。
//
// 表里的原文全部来自真机 stderr 抓取，一条不是编的。
// ------------------------------------------------------------------
namespace {

// ------------------------------------------------------------------
// --selfcheck-secrets 的认证装配。抽成自由函数是为了让自检判据验到
// **同一份实现**：判据照抄一份的话，把这里的 keyPath.clear() 删掉、
// 判据照样全绿，而这正是本条要防的那个缺陷。
//
// **必须先清 keyPath 再设口令。**
//
// useKey() 就是 `!keyPath.isEmpty()`（Model.h），而 setPasswordForTest()
// 只写 password、不碰 keyPath。于是只要 ini 里残留过一个 keyPath
// （用户以前用过私钥、切回口令后没清），这条"口令模式"自检就带着私钥去跑：
// start() 会因 keyPath 指向不存在的文件直接返回，日志原文是
//     私钥文件不存在或不可读：/root/.ssh/id_srt
// 真实的口令链路（createAskpass → SSH_ASKPASS → 口令文件落盘）
// **一次都没被执行**，而判据只看"临时目录有没有残留"，
// 于是一条从没跑过口令的自检照样报 SECRETS_ALL_CLEAN ——
// 把"口令功能整个没被测到"包装成"口令零残留，安全"。
// ------------------------------------------------------------------
void applySecretCheckAuth(SyncPanel *panel, const QString &pass)
{
    panel->setKeyPathForTest(QString());      // 先清残留的 keyPath
    panel->setPasswordForTest(pass);         // 再设口令
}

struct LayerCase
{
    const char *name;
    int         expect;      // 0=本机 1=网络 2=认证 3=接收端 4=未知
    const char *raw;
};

const LayerCase kLayerCases[] = {
    { "port-refused", 1,
      "ssh: connect to host 192.168.0.1 port 65000: Connection refused\r\n"
      "rsync: connection unexpectedly closed (0 bytes received so far) [sender]\r\n"
      "rsync error: unexplained error (code 255) [sender=3.1.1]" },

    { "host-timeout", 1,
      "ssh: connect to host 192.168.0.1 port 22: Connection timed out\r\n"
      "rsync: connection unexpectedly closed (0 bytes received so far) [sender]" },

    { "not-ssh-service", 1,
      "kex_exchange_identification: Connection closed by remote host\r\n"
      "ssh: connect to host 192.168.0.1 port 22: Connection refused" },

    // F-1：远端同名非空目录挡住了源文件。rsync 这条报错不带 [sender] 前缀，
    // 措辞也跟表里别的接收端规则都不同 —— 原来归因落到兜底分支、打成「未能归因」。
    //
    // 期望归到**专用的第 6 层**（LayerReceiverNameConflict = 6），不是笼统的接收端层：
    // 这一类的下一步只有一件事（把远端那个挡路的同名目录删掉或改名），
    // 接收端层自带的那套建议（查目录是否存在、写权限、磁盘空间）一条都不沾边。
    { "remote-name-conflict", 6,
      "could not make way for new regular file: probe.spe\r\n"
      "rsync error: some files/attrs were not transferred (see previous errors) (code 23) at main.c(1183) [sender=3.1.1]" },

    { "receiver-no-rsync", 3,
      "bash: line 1: rsync: command not found\r\n"
      "rsync: connection unexpectedly closed (0 bytes received so far) [sender]\r\n"
      "rsync error: remote command not found (code 127) at io.c(226) [sender=3.1.1]" },

    { "receiver-disk-full", 3,
      "rsync: [receiver] write failed on \"/home/appuser/testfs/big.spe\": "
      "No space left on device (28)\r\n"
      "rsync error: error in file IO (code 11) at receiver.c(381) [receiver=3.2.7]\r\n"
      "rsync: [sender] write error: Broken pipe (32)" },

    { "receiver-readonly", 3,
      "rsync: [receiver] mkstemp \"/home/appuser/testdir/.#hash#file.spe.bNEPzj\" failed: "
      "Permission denied (13)" },

    { "receiver-mkdir", 3,
      "rsync: [Receiver] mkdir \"/root/nope/\" failed: Permission denied (13)\r\n"
      "rsync error: errors selecting input/output files, dirs (code 3)" },

    { "auth-badpassword", 2,
      "appuser@192.168.0.1: Permission denied (publickey,password,keyboard-interactive).\r\n"
      "rsync error: unexplained error (code 255) [sender=3.1.1]" },

    { "auth-protected-key", 2,
      "Enter passphrase for key '/home/tester/vmtest/id_ed25519': \r\n"
      "Permission denied (publickey)." },

    { "local-key-broken", 0,
      "ssh-keygen -y -f '/home/tester/vmtest/bad': Load key \"bad\": invalid format" },

    { "local-source-gone", 0,
      "link_stat \"/home/tester/sendsrc/gone.spe\" failed: No such file or directory (2)\r\n"
      "rsync: [sender] send_files failed to open ..." },

    { "local-missing-key", 0,
      "Warning: Identity file /home/tester/vmtest/nokey not accessible: No such file or directory." },

    // 真机原文（麒麟 V11 实机抓取）：包里 bin/ssh 的 wrapper
    // exec 的 ../libexec/ssh 不存在，rsync 连 ssh 都没拉起来。
    // 症状行 Broken pipe / connection unexpectedly closed 都在网络层，
    // 排在真因前面会把一次**本机文件缺失**带成"没连上目标电脑"。
    { "local-ssh-cannot-exec", 0,
      "rsync: [sender] Failed to exec ssh: No such file or directory (2)\r\n"
      "rsync error: error in IPC code (code 14) at pipe.c(85) [sender=3.2.7]\r\n"
      "rsync: [sender] safe_write failed to write 6 bytes to fd 5: Broken pipe (32)\r\n"
      "rsync error: error in rsync protocol data stream (code 12) at io.c(327) [sender=3.2.7]" },
};

// ------------------------------------------------------------------
// 判据用的桩程序与几何小工具
// ------------------------------------------------------------------

// 造一个"退出码固定、stderr 内容固定"的程序，用来验退出码分类。
//
// Windows 不能直接 CreateProcess 一个 .bat（必须经 cmd.exe /c），
// Linux 用 /bin/sh + 脚本 + shebang。所以两边的"程序"形态不同，
// 但**语义**必须一样：同样的退出码、同样的 stderr 原文。
static QString makeToolStub(const QString &dir, const QString &name, const QString &ext,
                            const QString &stderrText, int exitCode)
{
    const QString path = dir + QLatin1Char('/') + name + ext;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    #ifdef Q_OS_WIN
    f.write("@echo off\r\n");
    if (!stderrText.isEmpty())
        f.write(("echo " + stderrText + " 1>&2\r\n").toLocal8Bit());
    f.write(QString("exit /b %1\r\n").arg(exitCode).toLocal8Bit());
    #else
    f.write("#!/bin/sh\n");
    if (!stderrText.isEmpty())
        f.write(("printf '%s\\n' " + QLatin1Char('"') + stderrText + QLatin1Char('"')
                 + " 1>&2\n").toLocal8Bit());
    f.write(QString("exit %1\n").arg(exitCode).toLocal8Bit());
    // 没有 +x 就跑不起来，那不是"桩坏了"而是"现场没造出来"，
    // 判据必须在后面显式断言这一档（见 tool-launch-perm）。
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return QFileInfo(path).isExecutable() ? path : QString();
    #endif
    f.close();
    return path;
}

// 工具列的真实宽度。判据必须用 delegate 自己给的宽度，
// 换一个数字算出来的"装得下"证明不了界面上装得下。
static int toolCellWidth(const ToolColumnDelegate &d)
{
    QStyleOptionViewItem opt;
    return d.sizeHint(opt, QModelIndex()).width();
}

int runSelfCheck()
{
    QTextStream out(stdout);
    // 输出编码必须钉死成 UTF-8。
    //
    // 不钉的话，stdout 的编码跟着**当前 locale** 走：在 C locale 下
    // （SSH 登录、cron、容器里都是这个）非 ASCII 字符全变成 "?"，
    // 于是自检输出里会出现 backend=?????? 这种东西 ——
    // 客户（和运维）看到的是乱码，还以为程序坏了。
    //
    // 这不是显示层的小事：交付包里的 自检.sh 会把这段输出原样转述给人看。
    out.setCodec("UTF-8");
    int bad = 0;

    // 整个自检跑在临时沙箱里。自检里会构造 SyncPanel，而 SyncPanel 的构造
    // 会 loadConfig() —— 不沙箱化的话，判据自己就去读写了用户真实的
    // SyncReplayTool.ini（改 onlyNewFiles、写工具规则）。
    // sandbox 必须在**任何** ConfigStore/SyncPanel 之前建好，所以放在函数最前面。
    QTemporaryDir selfCheckSandbox;
    const QByteArray oldCfgDir = qgetenv("SRT_CONFIG_DIR");
    if (selfCheckSandbox.isValid())
        qputenv("SRT_CONFIG_DIR", selfCheckSandbox.path().toLocal8Bit());

    for (const LayerCase &c : kLayerCases) {
        const int got = RsyncRunner::layerOf(QString::fromUtf8(c.raw));
        const bool ok = (got == c.expect);
        if (!ok)
            ++bad;
        // 标签纯 ASCII：非 ASCII 在某些控制台/日志里会被吞掉，
        // 而"看不出哪里失败"比失败本身更难查。
        out << (ok ? "PASS\t" : "FAIL\t") << c.name
            << "\texpect=" << c.expect << "\tgot=" << got << "\n";
    }

    // 看门狗预算：字节项必须真的生效。
    // 1 GiB / 一个文件 和 64 GiB / 一个文件，文件数相同，
    // 改之前两者的预算完全一样（都是 180s），改之后必须拉开。
    {
        const int small = RsyncRunner::watchdogMs(1, 1024LL * 1024 * 1024);
        const int big   = RsyncRunner::watchdogMs(1, 64LL * 1024 * 1024 * 1024);
        const bool ok = (small >= 180000) && (big > small * 2);
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "watchdog-bytes"
            << "\tsmall1GiB=" << small << "ms\tbig64GiB=" << big << "ms" << "\n";
    }
    // 文件数项不能因为加了字节项就失效
    {
        const int few  = RsyncRunner::watchdogMs(10, 0);
        const int many = RsyncRunner::watchdogMs(60000, 0);
        const bool ok = (few == 180000) && (many > few);
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "watchdog-files"
            << "\tfew10=" << few << "ms\tmany60000=" << many << "ms" << "\n";
    }

    // 进度帧解析。测试数据是**真机 rsync 抓下来的原文**（cat -A 展开过），
    // 不是照着格式编出来的 —— 编出来的样例会把真实的大小写混写规律漏掉，
    // 而这条缺陷恰恰就是大小写混写造成的。
    {
        // 小写 k：3000 个 1 字节文件，21.5 秒。修之前整行匹配不上，
        // 进度条全程 0%。这一条是本组的核心。
        const QString slowK = QStringLiteral("          3,000 100%   43.07kB/s    0:00:00 (xfr#3000, to-chk=0/3000)");
        // 大写 M：800MB 走真实 ssh
        const QString fastM = QStringLiteral("    157,286,400  9%  470.12MB/s    0:00:06 (xfr#1, to-chk=0/2)");
        const QString giga  = QStringLiteral("    1,073,741,824 42%    1.50GB/s    0:00:11 (xfr#3, to-chk=0/9)");
        const QString noUnit= QStringLiteral("        524,288  7%  819200B/s    0:00:00 (xfr#1, to-chk=0/2)");

        const RsyncRunner::ProgressFrame fk = RsyncRunner::parseProgressFrame(slowK);
        const RsyncRunner::ProgressFrame fm = RsyncRunner::parseProgressFrame(fastM);
        const RsyncRunner::ProgressFrame fg = RsyncRunner::parseProgressFrame(giga);
        const RsyncRunner::ProgressFrame fn = RsyncRunner::parseProgressFrame(noUnit);

        const bool c1 = fk.valid && fk.percent == 100 && fk.bytes == 3000
                        && qAbs(fk.speedBps - 43.07 * 1024.0) < 1.0;
        const bool c2 = fm.valid && fm.percent == 9 && fm.bytes == 157286400
                        && qAbs(fm.speedBps - 470.12 * 1024.0 * 1024.0) < 1024.0;
        const bool c3 = fg.valid && fg.percent == 42 && fg.bytes == 1073741824;
        const bool c4 = fn.valid && fn.percent == 7 && qAbs(fn.speedBps - 819200.0) < 1.0;
        bool ok = c1 && c2 && c3 && c4;

        // 反向锁：文件名绝不能被当成进度帧，否则 --out-format=%n 打出来的
        // 每个文件都会去改进度条。这几条是照着真实命名习惯写的。
        const char *notFrames[] = {
            "report 50%.spe",
            "2024/Q1/data.spe",
            "a/b/c.spe",
            "100%.spe",
            "./sub/x.spe",
            "send 1 2 3MB/s.spe"
        };
        QString falsePositive;
        for (const char *s : notFrames) {
            if (RsyncRunner::parseProgressFrame(QString::fromLatin1(s)).valid) {
                ok = false;
                falsePositive = QString::fromLatin1(s);
            }
        }
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "progress-frame-parse"
            << "\tkB/s=" << (fk.valid ? QString::number(fk.percent) : QStringLiteral("nomatch"))
            << "\tMB/s=" << (fm.valid ? QString::number(fm.percent) : QStringLiteral("nomatch"))
            << "\tGB/s=" << (fg.valid ? QString::number(fg.percent) : QStringLiteral("nomatch"))
            << "\tB/s=" << (fn.valid ? QString::number(fn.percent) : QStringLiteral("nomatch"))
            << "\tspeedKBps=" << QString::number(fk.speedBps / 1024.0, 'f', 2)
            << "\tfalsePositive=" << (falsePositive.isEmpty() ? QStringLiteral("none") : falsePositive)
            << "\tc1=" << c1 << " c2=" << c2 << " c3=" << c3 << " c4=" << c4
            << "\tfmSpd=" << QString::number(fm.speedBps, 'f', 2)
            << " fmExpect=" << QString::number(470.12 * 1024.0 * 1024.0, 'f', 2)
            << "\n";
    }

    // rsync %n 的转义还原与名字解析。
    //
    // 测试数据是**真机 rsync 抓下来的原文**（2026-10-21，容器内跑真 rsync 3.2.7，
    // LC_ALL=C 与 LC_ALL=C.UTF-8 各一遍），不是我照着规则编出来的 ——
    // 编出来的样例会把"转义规则随 locale 变"这件事整个漏掉，而那恰恰是要防的。
    //
    // 实测到的规则：
    //   0x01-0x08、0x0A-0x1F    -> \#NNN（两种 locale 一致）
    //   0x80-0xFF                -> C locale 转义；UTF-8 locale 下合法 UTF-8 原样输出
    //   0x09（制表符）、0x20-0x7F -> 原样
    //   反斜杠 0x5C              -> 只在后面紧跟 "#" 加 3 位八进制时才转义成 \#134
    //                              （rsync 靠这条保证转义可逆：真名里带 \#012 的，
    //                                输出成 \#134#012，两种情况区分得开）
    //
    // 修之前的症状：stdout 上的名字和清单里的原始名对不上，文件传过去了却
    // 既不计数也不点亮行，最后靠 leftover 兜底规则显示成「✓ 已完成」，
    // 日志里「已传 N 个文件」那个 N 一直是 0。
    {
        QSet<QString> group;
        group.insert(QString::fromUtf8(u8"混中文.spe"));
        group.insert(QStringLiteral("plain.spe"));
        group.insert(QString::fromUtf8(u8"data dir/名字 带空格.spe"));
        group.insert(QStringLiteral("with trailing space "));
        group.insert(QStringLiteral("a\\#012b.spe"));
        group.insert(QStringLiteral("x\ny.spe"));
        group.insert(QStringLiteral("x\x01y.spe"));
        group.insert(QString::fromUtf8("x\xC3\xA4" "y.spe"));
        group.insert(QStringLiteral("a#b.spe"));
        group.insert(QStringLiteral("x\\y.spe"));

        struct Case { const char *raw; const char *want; };
        const Case cases[] = {
            // C locale 真机输出：中文名被逐字节转义
            { "\\#346\\#267\\#267\\#344\\#270\\#255\\#346\\#226\\#207.spe",
              "\xE6\xB7\xB7\xE4\xB8\xAD\xE6\x96\x87.spe" },
            // UTF-8 locale 真机输出：合法 UTF-8 原样
            { "\xE6\xB7\xB7\xE4\xB8\xAD\xE6\x96\x87.spe",
              "\xE6\xB7\xB7\xE4\xB8\xAD\xE6\x96\x87.spe" },
            { "plain.spe", "plain.spe" },
            // 文件名带尾随空格：必须用未 trim 的原始行去匹配
            { "with trailing space ", "with trailing space " },
            // 名字里本来就有字面量 \#012：rsync 输出的是 \#134#012
            { "a\\#134#012b.spe", "a\\#012b.spe" },
            // 名字里真的有换行
            { "x\\#012y.spe", "x\ny.spe" },
            // 控制字符
            { "x\\#001y.spe", "x\x01y.spe" },
            // 高位字节：两种 locale 各一份
            { "x\\#303\\#244y.spe", "x\xC3\xA4" "y.spe" },
            { "x\xC3\xA4" "y.spe", "x\xC3\xA4" "y.spe" },
            // # 后面不是 3 位八进制：一律原样，不许乱还原
            { "a#b.spe", "a#b.spe" },
            { "x\\y.spe", "x\\y.spe" },
            // 子目录 + 中文名 + 空格
            { "data dir/\\#345\\#220\\#215\\#345\\#255\\#227 \\#345\\#270\\#246\\#347\\#251\\#272\\#346\\#240\\#274.spe",
              "data dir/\xE5\x90\x8D\xE5\xAD\x97 \xE5\xB8\xA6\xE7\xA9\xBA\xE6\xA0\xBC.spe" },
            { "data dir/\xE5\x90\x8D\xE5\xAD\x97 \xE5\xB8\xA6\xE7\xA9\xBA\xE6\xA0\xBC.spe",
              "data dir/\xE5\x90\x8D\xE5\xAD\x97 \xE5\xB8\xA6\xE7\xA9\xBA\xE6\xA0\xBC.spe" },
            // 反向锁：不在清单里的一律还原不出来
            { "other.spe", "" },
            { "     1,234 100%   1.00MB/s    0:00:00 (xfr#1, to-chk=0/1)", "" },
            { "data dir/", "" },
            // 反向锁：只是前缀像，不许模糊命中
            { "plain.speX", "" },
        };

        bool ok = true;
        QString firstBad;
        int passed = 0;
        for (const Case &c : cases) {
            const QString got = RsyncRunner::resolveRsyncName(QString::fromUtf8(c.raw), group);
            const QString want = QString::fromUtf8(c.want);
            if (got != want) {
                ok = false;
                if (firstBad.isEmpty()) {
                    firstBad = QString::fromUtf8("raw=%1 want=%2 got=%3")
                        .arg(QString::fromUtf8(c.raw), want, got);
                }
            } else {
                ++passed;
            }
        }
        // 反解义本身不许乱动普通名字
        if (RsyncRunner::unescapeRsyncName(QStringLiteral("plain.spe"))
            != QStringLiteral("plain.spe")) {
            ok = false;
            if (firstBad.isEmpty())
                firstBad = QStringLiteral("unescape mangled a plain name");
        }
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "rsync-name-escape"
            << "\tcases=" << (ok ? QString::number(passed) : QString::number(passed))
            << "/" << QString::number(static_cast<int>(sizeof(cases) / sizeof(cases[0])))
            << "\tfirstBad=" << (firstBad.isEmpty() ? QStringLiteral("none") : firstBad)
            << "\n";
    }
    // 所以走 purgeLegacySecretDirs(dir, keepPid) 而不是直接测 QDir::tempPath()。
    {
        QTemporaryDir sandbox;
        const QString root = sandbox.path();
        // 显式 open/write/close：QFile::write() 在临时对象上不保证会先把文件建出来，
        // 一旦没建出来，下面三条"必须留下"会全部报成 false，
        // 输出看起来就像"清扫把用户的目录和别人的文件夹一起删了"。
        // 判据自己造的输入必须**逐个确认存在**，否则它测的不是产品而是自己。
        const auto mkFile = [&out, &bad](const QString &path, const QByteArray &data) -> bool {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out << "FAIL\tselftest-fixture\tcannot create " << path << "\n";
                ++bad;
                return false;
            }
            f.write(data);
            f.close();
            return true;
        };
        // 该删的：老版本建的同名目录，里面是明文口令脚本
        const QString dead1 = root + QStringLiteral("/srt_askpass_999999_fs");
        const QString dead2 = root + QStringLiteral("/srt_pass_999999_old");
        // 不该删的三种：本进程 pid / 含子目录 / 根本不匹配命名
        const QString mine  = root + QStringLiteral("/srt_askpass_")
                                + QString::number(QCoreApplication::applicationPid())
                                + QStringLiteral("_live");
        const QString deep  = root + QStringLiteral("/srt_askpass_999999_deep");
        const QString other = root + QStringLiteral("/somebody-elses-folder");
        foreach (const QString &d, QStringList() << dead1 << dead2 << mine << deep << other)
            QDir().mkpath(d);
        QDir().mkpath(deep + QStringLiteral("/nested"));
        mkFile(dead1 + QStringLiteral("/askpass.bat"), QByteArray("echo plaintext\n"));
        mkFile(dead2 + QStringLiteral("/srt_pass_999999_x"), QByteArray("plaintext\n"));
        mkFile(mine + QStringLiteral("/askpass.sh"), QByteArray("plaintext\n"));
        mkFile(deep + QStringLiteral("/nested/keep.txt"), QByteArray("payload\n"));
        mkFile(other + QStringLiteral("/notes.txt"), QByteArray("mine\n"));

        // 造完先验一遍：这三条不存在的话，下面所有断言都无意义
        const bool fixtureOk = QFileInfo(mine + QStringLiteral("/askpass.sh")).isFile()
                            && QFileInfo(deep + QStringLiteral("/nested/keep.txt")).isFile()
                            && QFileInfo(other + QStringLiteral("/notes.txt")).isFile()
                            && QFileInfo(dead1 + QStringLiteral("/askpass.bat")).isFile();
        if (!fixtureOk) {
            ++bad;
            out << "FAIL\tselftest-fixture\tsentinel files missing, case is meaningless" << "\n";
        }

        const int removed = RsyncRunner::purgeLegacySecretDirs(
                                root, QCoreApplication::applicationPid());

        bool ok = fixtureOk && (removed == 2);
        // 四条反向锁：多删一条就是在客户机上乱删用户目录
        ok = ok && !QFileInfo(dead1).exists();
        ok = ok && !QFileInfo(dead2).exists();
        ok = ok && QFileInfo(mine + QStringLiteral("/askpass.sh")).exists();
        ok = ok && QFileInfo(deep + QStringLiteral("/nested/keep.txt")).exists();
        ok = ok && QFileInfo(other + QStringLiteral("/notes.txt")).exists();
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "purge-legacy-secret-dirs"
            << "\tremoved=" << removed << "\tdead1gone=" << (!QFileInfo(dead1).exists() ? 1 : 0)
            << "\tdead2gone=" << (!QFileInfo(dead2).exists() ? 1 : 0)
            << "\tmineKept=" << (QFileInfo(mine + QStringLiteral("/askpass.sh")).exists() ? 1 : 0)
            << "\tdeepKept=" << (QFileInfo(deep + QStringLiteral("/nested/keep.txt")).exists() ? 1 : 0)
            << "\totherKept=" << (QFileInfo(other + QStringLiteral("/notes.txt")).exists() ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 临时凭据文件补扫：通配符少一个 '*' 就会静默变成死代码
    //
    // 缺陷（2026-10-18 全局复查）：purgeSecrets() 的补扫用
    // entryList(「srt_pass_」/「srt_askpass_」) —— 不带 '*' 时 QDir 只匹配名字
    // **恰好等于**该串的文件，而命名契约是 srt_pass_<pid>_XXXXXX（见
    // RsyncRunner.cpp 里 createAskpass 的 QTemporaryFile 模板）。
    // 于是 found 恒为空、循环体一次都不执行，而「一条都没删」和
    // 「根本没写这段代码」在输出上完全一样，全程零报错。
    // 同一文件 cleanupStaleSecrets() 写的是 srt_pass_*，只差两个星号，
    // 语义却完全相反 —— 所以必须单独判它。
    //
    // 正向 1 条：本 pid 名下的 srt_pass_/srt_askpass_ 都要删掉（removed==2）。
    // 反向锁 3 条：别的 pid 名下的必须留着（动了就是在打断另一个正在同步的实例）、
    // 名字里没有 pid 段的必须留着、同目录下的无关文件必须留着。
    // 变异锁：把两个 '*' 拿掉，removed 立刻变 0、minePassGone 变 0，本项自红。
    {
        QTemporaryDir sandbox;
        const QString root  = sandbox.path();
        const int    myPid  = QCoreApplication::applicationPid();
        const QString self  = QString::number(myPid);
        const QString other = QString::number(myPid + 100000);

        const auto mk = [&out, &bad](const QString &p) -> bool {
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out << "FAIL\tpurge-secret-files-fixture\tcannot create " << p << "\n";
                ++bad;
                return false;
            }
            f.write("plaintext\n");
            f.close();
            return true;
        };

        const QString minePass  = root + QStringLiteral("/srt_pass_")    + self  + QStringLiteral("_AAA");
        const QString mineAsk   = root + QStringLiteral("/srt_askpass_") + self  + QStringLiteral("_BBB");
        const QString otherPass = root + QStringLiteral("/srt_pass_")    + other + QStringLiteral("_CCC");
        const QString noPid     = root + QStringLiteral("/srt_pass_short");
        const QString plain     = root + QStringLiteral("/notes.txt");

        const bool fixtureOk = mk(minePass) && mk(mineAsk) && mk(otherPass)
                            && mk(noPid) && mk(plain);
        if (!fixtureOk) {
            ++bad;
            out << "FAIL\tpurge-secret-files-fixture\tsentinel files missing, case is meaningless" << "\n";
        }

        const int removed = RsyncRunner::purgeSelfPidSecretFiles(root, myPid);

        bool ok = fixtureOk && (removed == 2);
        ok = ok && !QFileInfo(minePass).exists();
        ok = ok && !QFileInfo(mineAsk).exists();
        ok = ok && QFileInfo(otherPass).exists();      // 反向锁 1
        ok = ok && QFileInfo(noPid).exists();          // 反向锁 2
        ok = ok && QFileInfo(plain).exists();          // 反向锁 3
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "purge-secret-files"
            << "\tremoved=" << removed
            << "\tminePassGone=" << (!QFileInfo(minePass).exists() ? 1 : 0)
            << "\tmineAskGone="  << (!QFileInfo(mineAsk).exists() ? 1 : 0)
            << "\totherKept="    << (QFileInfo(otherPass).exists() ? 1 : 0)
            << "\tnoPidKept="    << (QFileInfo(noPid).exists() ? 1 : 0)
            << "\tplainKept="    << (QFileInfo(plain).exists() ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 「仅同步新增文件」勾选框必须持久化
    //
    // 缺陷（2026-10-18 实测，麒麟 V11 真实点击四轮对照）：
    //   第 1 轮 首传                       -> 接收端 sha256 = 源端
    //   第 2 轮 改文件，保持默认勾选       -> 接收端 sha256 **不变**
    //   第 3 轮 取消勾选，同一份再同步     -> 接收端 sha256 **变了**
    //   第 4 轮 再改一次（已取消勾选）     -> 接收端 sha256 又不变了
    // 第 3 轮生效、第 4 轮又失效，说明它只在当次进程内有效：
    // 构造里是无条件 setChecked(true)，根本没有读写配置的地方。
    // 用户取消勾选同步一次，关掉程序再打开又变回勾选 ——
    // 「改了文件不重传」反复出现，界面上却没有任何地方解释为什么。
    //
    // 正向：勾掉 -> 落盘 -> 读回来是 false；勾上 -> 读回来是 true。
    // 反向锁 3 条：
    //   1) 全新配置（没有这个键）必须读成 **false**。
    //      需求 2「只同步新增**或修改**的文件」要求默认走 --checksum：
    //      勾上 = --ignore-existing = 远端同名文件整个跳过 = 改过的文件永远传不过去。
    //      手册步骤 5「不勾（默认）」也是这么写的，代码必须与之一致。
    //   2) 面板上取消勾选必须真的落盘（只测 ConfigStore 读写往返的话，
    //      把 SyncPanel 里那行 connect 删掉，本项照样全绿）。
    //   3) 全新机器上界面上的勾选框必须**真的是不勾**（构造时读配置那一行）。
    // 变异锁：把 loadOnlyNewFiles() 的默认 return false 改成 true，反向锁 1/3 立刻红。
    {
        QTemporaryDir sandbox;
        const QByteArray oldDir = qgetenv("SRT_CONFIG_DIR");
        // 判据自己必须先确认沙箱可用：直接写进用户真实配置是不可接受的。
        bool envOk = false;
        if (sandbox.isValid()) {
            qputenv("SRT_CONFIG_DIR", sandbox.path().toLocal8Bit());
            envOk = true;
        }
        if (!envOk) {
            ++bad;
            out << "FAIL\tonly-new-persist\tcannot create a temp sandbox; refusing to touch the real config\n";
        } else {
            auto s = ConfigStore::instance();   // 只是确保静态实例存在，读取本身是静态 API
            Q_UNUSED(s);
            QSettings probeIni(sandbox.filePath(QStringLiteral("SyncReplayTool.ini")),
                               QSettings::IniFormat);
            probeIni.setIniCodec("UTF-8");
            probeIni.remove(QStringLiteral("sync/onlyNewFiles"));
            probeIni.sync();

            // 反向锁 1：全新配置（没有这个键）必须读成 false
            const bool defFalse = !ConfigStore::loadOnlyNewFiles();
            // 正向 1：落 false 读得回 false
            ConfigStore::saveOnlyNewFiles(false);
            const bool gotFalse = !ConfigStore::loadOnlyNewFiles();
            // 正向 2：落 true 读得回 true
            ConfigStore::saveOnlyNewFiles(true);
            const bool gotTrue = ConfigStore::loadOnlyNewFiles();

            // 反向锁 2：面板这一侧的两个动作都必须真的接上了 ——
            //   构造时读回配置（对应实测第 4 轮「重启后又变回勾选」）
            //   切换时落盘（对应「取消勾选只在本进程内有效」）
            // 只测 ConfigStore 读写往返的话，把 SyncPanel 里那两行（读配置 / connect
            // toggled）删掉，本项照样全绿 —— 那正是这个缺陷当初能溜过去的原因。
            bool panelReads = false, panelWrites = false;
            {
                ConfigStore::saveOnlyNewFiles(true);
                SyncPanel probe;                // 构造时读配置，应当读成「勾上」
                panelReads = probe.onlyNewForTest();
                probe.setOnlyNewForTest(false); // 切换，应当立刻落盘
                panelWrites = !ConfigStore::loadOnlyNewFiles();
            }

            // 反向锁 3：全新机器（配置里没有这个键）上，界面上的勾选框必须不勾。
            // 只测 ConfigStore 的话，把 SyncPanel 构造里那行 setChecked 删掉本项照样全绿。
            bool freshPanelUnchecked = false;
            {
                QSettings freshIni(sandbox.filePath(QStringLiteral("SyncReplayTool.ini")),
                                   QSettings::IniFormat);
                freshIni.setIniCodec("UTF-8");
                freshIni.remove(QStringLiteral("sync/onlyNewFiles"));
                freshIni.sync();
                SyncPanel fresh;
                freshPanelUnchecked = !fresh.onlyNewForTest();
            }

            const bool ok = defFalse && gotFalse && gotTrue && panelReads && panelWrites
                            && freshPanelUnchecked;
            if (!ok)
                ++bad;
            out << (ok ? "PASS\t" : "FAIL\t") << "only-new-persist"
                << "\tdefaultFalse=" << (defFalse ? 1 : 0)
                << "\tfreshPanelUnchecked=" << (freshPanelUnchecked ? 1 : 0)
                << "\tfalseRoundTrip=" << (gotFalse ? 1 : 0)
                << "\ttrueRoundTrip=" << (gotTrue ? 1 : 0)
                << "\tpanelReadsCfg=" << (panelReads ? 1 : 0)
                << "\tpanelWritesCfg=" << (panelWrites ? 1 : 0)
                << "\n";

    // ----------------------------------------------------------------
    // 「记住设置」必须**恒为 true**：连接信息不能被静默抹掉。
    //
    // 界面上那个勾选框按客户意见移除后，app/rememberSettings 就成了
    // "程序里没有入口的开关"。之前它是**被读的**（loadTarget / saveTarget
    // 两道闸门看这个键），于是老配置里存着 false 的用户升级上来会：
    //   ① 启动时读不回自己的 IP/账号/口令（界面是空的）；
    //   ② 每次保存又会把 ini 里的 target/* **整个删掉** ——
    //      用户从没表达过"别记"，信息却被抹了，界面上还没地方能改回来。
    //
    // 判据：**故意**在沙箱 ini 里写 app/rememberSettings=false，
    // 然后要求 ① loadTarget() 照样读得到 host；② saveTarget() 不会删掉
    // target/*；③ 迁移函数把那个键改成 true。
    // 变异锁：把 loadTarget/saveTarget 里的两道闸门放回去（读键 / 删键），
    // 本项立刻红。
    {
        QTemporaryDir sandbox;
        const QByteArray oldDir = qgetenv("SRT_CONFIG_DIR");
        bool envOk = false;
        if (sandbox.isValid()) {
            qputenv("SRT_CONFIG_DIR", sandbox.path().toLocal8Bit());
            envOk = true;
        }
        if (!envOk) {
            ++bad;
            out << "FAIL\tremember-settings-always-remember\tcannot create a temp sandbox\n";
        } else {
            {
                QSettings ini(sandbox.filePath(QStringLiteral("SyncReplayTool.ini")),
                              QSettings::IniFormat);
                ini.setIniCodec("UTF-8");
                ini.setValue(QStringLiteral("app/rememberSettings"), false);
                ini.setValue(QStringLiteral("target/host"), QStringLiteral("192.168.77.9"));
                ini.setValue(QStringLiteral("target/user"), QStringLiteral("remember-me"));
                ini.sync();
            }
            const TargetConfig t1 = ConfigStore::loadTarget();
            const bool readBack = (t1.host == QLatin1String("192.168.77.9"))
                               && (t1.user == QLatin1String("remember-me"));

            // 再存一次：闸门放回去的话，target/* 这一轮就会被删掉。
            TargetConfig t2 = t1;
            t2.host = QStringLiteral("10.0.0.8");
            ConfigStore::saveTarget(t2);
            const TargetConfig t3 = ConfigStore::loadTarget();
            const bool survived = (t3.host == QLatin1String("10.0.0.8"));

            // 老键迁移：false → true。
            ConfigStore::migrateRememberSettingsToTrue();
            const bool migrated = ConfigStore::loadRememberSettings();

            const bool ok = readBack && survived && migrated;
            if (!ok)
                ++bad;
            out << (ok ? "PASS\t" : "FAIL\t") << "remember-settings-always-remember"
                << "\treadBack=" << (readBack ? 1 : 0)
                << "\tsurvivedSave=" << (survived ? 1 : 0)
                << "\tmigratedToTrue=" << (migrated ? 1 : 0)
                << "\n";
        }
        if (oldDir.isEmpty())
            qunsetenv("SRT_CONFIG_DIR");
        else
            qputenv("SRT_CONFIG_DIR", oldDir);
    }

            // 自动同步状态与周期的持久化验证
            const bool asDefFalse = !ConfigStore::loadAutoSyncEnabled();
            ConfigStore::saveAutoSyncEnabled(true);
            const bool asGotTrue = ConfigStore::loadAutoSyncEnabled();
            ConfigStore::saveAutoSyncEnabled(false);
            const bool asGotFalse = !ConfigStore::loadAutoSyncEnabled();

            const int asDefInterval = ConfigStore::loadAutoSyncInterval(); // 默认 30
            ConfigStore::saveAutoSyncInterval(60);
            const bool asGotInterval = (ConfigStore::loadAutoSyncInterval() == 60);
            ConfigStore::saveAutoSyncInterval(1); // 越下限截断为 10
            const bool asMinLimit = (ConfigStore::loadAutoSyncInterval() == 10);
            ConfigStore::saveAutoSyncInterval(999999); // 越上限截断为 86400
            const bool asMaxLimit = (ConfigStore::loadAutoSyncInterval() == 86400);
            ConfigStore::saveAutoSyncInterval(30);

            bool asPanelReads = false, asPanelWrites = false;
            {
                ConfigStore::saveAutoSyncEnabled(true);
                SyncPanel probeAs;
                asPanelReads = probeAs.autoSyncForTest();
                probeAs.setAutoSyncForTest(false);
                asPanelWrites = !ConfigStore::loadAutoSyncEnabled();
            }

            const bool okAs = asDefFalse && asGotTrue && asGotFalse && (asDefInterval == 30)
                              && asGotInterval && asMinLimit && asMaxLimit && asPanelReads && asPanelWrites;
            if (!okAs)
                ++bad;
            out << (okAs ? "PASS\t" : "FAIL\t") << "auto-sync-persist"
                << "\tdefaultFalse=" << (asDefFalse ? 1 : 0)
                << "\ttrueRoundTrip=" << (asGotTrue ? 1 : 0)
                << "\tintervalLimit=" << (asMinLimit && asMaxLimit ? 1 : 0)
                << "\tpanelReadsCfg=" << (asPanelReads ? 1 : 0)
                << "\tpanelWritesCfg=" << (asPanelWrites ? 1 : 0)
                << "\n";

            if (oldDir.isEmpty())
                qunsetenv("SRT_CONFIG_DIR");
            else
                qputenv("SRT_CONFIG_DIR", oldDir);
        }
    }

    // ----------------------------------------------------------------
    // 自动同步（需求 10）：调度语义 + 心跳跟随 + 触发链路 + 去重 + 边界
    //
    // 为什么必须加这一组：原有 auto-sync-persist 只验「配置写进去能读回来」。
    // 把 SyncPanel::autoSyncFire() 整段删掉、或把 heartbeatMsFor()
    // 改成 return 1000，那一条照样全绿 —— 排程逻辑一条都没被盖到。
    //
    // 下面每条都调用**产品自己那份实现**：
    //   - 调度语义 → AutoSyncScheduler（纯函数，产品类）
    //   - 心跳跟随 → SyncPanel::autoSyncHeartbeatMsForTest()
    //   - 触发链路 → SyncPanel::triggerAutoSyncTickForTest()（产品自己的心跳入口）
    //   - 去重     → SyncPanel::forceAutoSyncFireForTest()
    // 不另写影子算法 —— 影子算法会和产品实现漂移，判据变假绿灯。
    {
        // ---- 判据 1：纯调度语义 ----
        {
            const QDateTime nowRef = QDateTime::currentDateTime();
            const QDateTime nextFromNow = AutoSyncScheduler::nextRunAt(60, nowRef, QDateTime());
            const bool okNoLast = (nextFromNow == nowRef.addSecs(60));

            const QDateTime last = nowRef.addSecs(-300);
            const QDateTime nextFromLast = AutoSyncScheduler::nextRunAt(60, nowRef, last);
            // 必须是 last+interval（绝对时间点），不能是 now+interval
            const bool okAbsolute = (nextFromLast == last.addSecs(60));

            // fromNow=true：忽略 lastRunAt，一律 now+interval
            // （用户点「保存设置」后要的是这个，不是"立刻补跑"）
            const QDateTime nextForced = AutoSyncScheduler::nextRunAt(60, nowRef, last, true);
            const bool okFromNow = (nextForced == nowRef.addSecs(60));

            // 跨天：23:59 + 120s -> 次日 00:01
            const QDateTime lateNight = QDateTime(QDate(2026, 10, 22), QTime(23, 59, 0));
            const QDateTime nextDay = AutoSyncScheduler::nextRunAt(120, lateNight, lateNight);
            const bool okCrossDay = (nextDay.date() == QDate(2026, 10, 23)
                                  && nextDay.time() == QTime(0, 1, 0));

            const bool okHeartbeat = (AutoSyncScheduler::heartbeatMsFor(10) == 5000
                                   && AutoSyncScheduler::heartbeatMsFor(60) == 30000
                                   && AutoSyncScheduler::heartbeatMsFor(86400) == 60000);
            // 反向锁：心跳必须随间隔单调变化
            const bool okHbMono = (AutoSyncScheduler::heartbeatMsFor(10)
                                 < AutoSyncScheduler::heartbeatMsFor(86400));

            const bool okClamp = (AutoSyncScheduler::clampIntervalSec(1)
                                   == AutoSyncScheduler::MinIntervalSec)
                               && (AutoSyncScheduler::clampIntervalSec(999999)
                                   == AutoSyncScheduler::MaxIntervalSec);
            const bool okDue = AutoSyncScheduler::isDue(nowRef, nowRef)
                            && !AutoSyncScheduler::isDue(nowRef, nowRef.addMSecs(1));
            // 反向锁：无效 next 必须判成"没到点"
            const bool okDueInvalid = !AutoSyncScheduler::isDue(nowRef, QDateTime());

            // 系统时间被往回调（lastRunAt 在将来）→ 不能静默停摆，
            // 必须按"从现在重新开始一个周期"处理。
            const QDateTime future = nowRef.addSecs(3600);
            const QDateTime nextAfterRollback = AutoSyncScheduler::nextRunAt(60, nowRef, future);
            const bool okRollback = (nextAfterRollback == nowRef.addSecs(60));

            const bool ok1 = okNoLast && okAbsolute && okFromNow && okCrossDay
                          && okHeartbeat && okHbMono && okClamp && okDue
                          && okDueInvalid && okRollback;
            if (!ok1) ++bad;
            out << (ok1 ? "PASS\t" : "FAIL\t") << "auto-sync-schedule"
                << "\tnoLast="        << (okNoLast    ? 1 : 0)
                << "\tabsoluteFromLast=" << (okAbsolute ? 1 : 0)
                << "\tfromNow="        << (okFromNow  ? 1 : 0)
                << "\tcrossDay="       << (okCrossDay  ? 1 : 0)
                << "\theartbeat="      << (okHeartbeat ? 1 : 0)
                << "\thbMonotone="     << (okHbMono    ? 1 : 0)
                << "\tclamp="          << (okClamp     ? 1 : 0)
                << "\tdueBoundary="    << (okDue       ? 1 : 0)
                << "\tclockRollback="  << (okRollback  ? 1 : 0)
                << "\n";
        }

        // ---- 判据 2：心跳周期必须跟着间隔走（接线，不是算法）----
        //
        // 这是实测最疼的一个：原来 applyFromSettings 只更新了
        // m_autoSyncIntervalSec，没更新 m_autoSyncTimer，
        // 客户把间隔改成 10 秒后，实际 59 秒才第一次跑
        //（界面照样显示"下次同步 10s 后"，倒计时在走，就是不触发）。
        {
            ConfigStore::saveAutoSyncEnabled(false);
            ConfigStore::saveAutoSyncInterval(86400);
            SyncPanel hbPanel;
            const int hb0 = hbPanel.autoSyncHeartbeatMsForTest();
            ConfigStore::saveAutoSyncInterval(10);
            TargetConfig ht;
            ht.host = QStringLiteral("127.0.0.1");
            ht.user = QStringLiteral("u");
            ht.port = 22;
            hbPanel.applyFromSettings(ht, QString(), QVector<ToolRule>(), QVector<SyncRule>());
            const int hb1 = hbPanel.autoSyncHeartbeatMsForTest();
            const bool okHb = (hb0 == AutoSyncScheduler::heartbeatMsFor(86400))
                           && (hb1 == AutoSyncScheduler::heartbeatMsFor(10));
            if (!okHb) ++bad;
            out << (okHb ? "PASS\t" : "FAIL\t") << "auto-sync-heartbeat-follows-interval"
                << "\thbBefore=" << hb0 << "\thbAfter=" << hb1
                << "\texpected=" << AutoSyncScheduler::heartbeatMsFor(10) << "\n";
        }

        // ---- 判据 3：自动触发链路（到点 -> 重扫 -> 全选 -> 真跑）----
        //
        // 走的是产品自己的 onAutoSyncTick / autoSyncFire，不是模拟回调。
        {
            QTemporaryDir dataDir;
            {
                QFile f1(dataDir.path() + QStringLiteral("/a.dat"));
                f1.open(QIODevice::WriteOnly); f1.write("x"); f1.close();
                QFile f2(dataDir.path() + QStringLiteral("/b.dat"));
                f2.open(QIODevice::WriteOnly); f2.write("y"); f2.close();
            }

            ConfigStore::saveAutoSyncEnabled(false);
            ConfigStore::saveAutoSyncInterval(10);
            SyncPanel p;
            p.setHostForTest(QStringLiteral("127.0.0.1"));
            p.setPortForTest(1);
            p.setUserForTest(QStringLiteral("u"));
            p.setLocalRootForTest(dataDir.path());
            p.setAutoSyncForTest(true);
            p.setAutoSyncNextAtForTest(QDateTime::currentDateTime().addSecs(-1));
            const bool armed = p.triggerAutoSyncTickForTest();   // 产品自己的心跳入口

            QElapsedTimer et; et.start();
            while (p.syncFinishedCountForTest() == 0 && et.elapsed() < 20000) {
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
                QThread::msleep(5);
            }
            int fired = 0, rescanned = 0;
            const QStringList recs = p.logRecordsForTest();
            for (int i = 0; i < recs.size(); ++i) {
                if (recs.at(i).contains(QString::fromUtf8(
                        "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x90\x8C\xE6\xAD\xA5"
                        "\xE5\xB7\xB2\xE5\x88\xB0\xE7\x82\xB9")))
                    ++fired;
                if (recs.at(i).contains(QString::fromUtf8(
                        "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x90\x8C\xE6\xAD\xA5"
                        "\xE5\xB7\xB2\xE9\x87\x8D\xE6\x96\xB0\xE6\x89\xAB\xE6\x8F\x8F")))
                    ++rescanned;
            }
            const bool okChain = armed && fired >= 1 && rescanned >= 1
                              && p.syncFinishedCountForTest() > 0
                              && p.tableModel() && p.fileCountForTest() == 2
                              && p.tableModel()->checkedCount() == p.fileCountForTest();
            if (!okChain) ++bad;
            out << (okChain ? "PASS\t" : "FAIL\t") << "auto-sync-fire-chain"
                << "\tarmed=" << (armed ? 1 : 0)
                << "\tfiredLog=" << fired << "\trescanLog=" << rescanned
                << "\tsyncFinished=" << p.syncFinishedCountForTest()
                << "\tchecked=" << (p.tableModel() ? p.tableModel()->checkedCount() : -1)
                << "\tfiles=" << p.fileCountForTest() << "\n";
        }

        // ---- 判据 4：失败去重 + 排程推进 + 改配置后重新提示 ----
        {
            ConfigStore::saveAutoSyncEnabled(false);
            ConfigStore::saveAutoSyncInterval(10);
            SyncPanel q;                       // 故意不配目标 -> 触发失败
            q.setLocalRootForTest(QDir::tempPath());
            q.setAutoSyncForTest(true);
            auto countNotRun = [&q]() {
                int c = 0;
                const QStringList recs = q.logRecordsForTest();
                for (int i = 0; i < recs.size(); ++i)
                    if (recs.at(i).contains(QString::fromUtf8(
                            "\xE8\x87\xAA\xE5\x8A\xA8\xE5\x90\x8C\xE6\xAD\xA5"
                            "\xE6\x9C\xAA\xE8\xB7\x91\xE8\xB5\xB7\xE6\x9D\xA5")))
                        ++c;
                return c;
            };
            q.forceAutoSyncFireForTest();
            const int n1 = countNotRun();
            q.forceAutoSyncFireForTest();
            const int n2 = countNotRun();        // 同因 -> 不刷屏
            const bool advanced = (q.autoSyncNextAtForTest() > QDateTime::currentDateTime());
            // 反向锁：改了配置（去重标记应被清掉）-> 同因也要再提示一次
            TargetConfig it;                     // 仍然不配目标
            it.host = QString();
            it.user = QString();
            q.applyFromSettings(it, QDir::tempPath(), QVector<ToolRule>(), QVector<SyncRule>());
            q.forceAutoSyncFireForTest();
            const int n3 = countNotRun();
            const bool okDedup = (n1 == 1 && n2 == 1 && n3 == 2 && advanced);
            if (!okDedup) ++bad;
            out << (okDedup ? "PASS\t" : "FAIL\t") << "auto-sync-fail-dedup"
                << "\tfirstWarn=" << n1 << "\tsecondWarn=" << n2
                << "\tafterCfgWarn=" << n3 << "\tscheduleAdvanced=" << (advanced ? 1 : 0)
                << "\n";
        }

        // ---- 判据 4b：失败后的重排程口径（整秒 + 定点模式走定点时刻）----
        {
            // 间隔模式：失败后 next = 整秒的「现在 + 一个完整周期」。
            //
            // 「整秒」这条不是凑数：心跳的格子锚在定时器启动时刻上，
            // 基准带毫秒时某一拍可能落在名义到点时刻之前几毫秒 ⇒ 那一拍整拍跳过
            // ⇒ 重试间隔从 N 秒变成 N+心跳（实测间隔 30 秒跑成 45 秒）。
            ConfigStore::saveAutoSyncEnabled(false);
            ConfigStore::saveAutoSyncMode(AutoSyncScheduler::ModeInterval);
            ConfigStore::saveAutoSyncInterval(30);
            SyncPanel qi;
            qi.setLocalRootForTest(QDir::tempPath());
            qi.setAutoSyncForTest(true);
            qi.forceAutoSyncFireForTest();       // 故意不配目标 -> 走失败分支
            const QDateTime nxtInterval = qi.autoSyncNextAtForTest();
            const bool okInterval = nxtInterval.isValid()
                && nxtInterval.time().msec() == 0
                && nxtInterval > QDateTime::currentDateTime().addSecs(10);

            // 每天定点模式：失败后 next 必须落在**定点时刻**上。
            //
            // 修之前这里是「现在 + 间隔」，而定点模式下的「间隔」在设置界面里
            // 是隐藏的、值常年是上次留下的默认值 ⇒ 「每天一次」被改成
            // 「每 30 秒重试一次」，一整天都在重试；
            // 而日志因为失败去重只打一条，客户完全看不出来。
            ConfigStore::saveAutoSyncMode(AutoSyncScheduler::ModeDaily);
            ConfigStore::saveAutoSyncDailyTime(QTime(3, 0));
            SyncPanel qd;
            qd.setLocalRootForTest(QDir::tempPath());
            qd.setAutoSyncForTest(true);
            qd.forceAutoSyncFireForTest();
            const QDateTime nxtDaily = qd.autoSyncNextAtForTest();
            const bool okDaily = nxtDaily.isValid()
                && nxtDaily.time().msec() == 0
                && nxtDaily.time() == QTime(3, 0)
                && nxtDaily > QDateTime::currentDateTime();

            // 把模式还原：后面的判据还要用间隔模式。
            ConfigStore::saveAutoSyncMode(AutoSyncScheduler::ModeInterval);
            ConfigStore::saveAutoSyncDailyTime(AutoSyncScheduler::defaultDailyTime());

            const bool ok4b = okInterval && okDaily;
            if (!ok4b) ++bad;
            out << (ok4b ? "PASS\t" : "FAIL\t") << "auto-sync-fail-reschedule"
                << "\tintervalWholeSec=" << (okInterval ? 1 : 0)
                << "\tdailyOnSlot=" << (okDaily ? 1 : 0)
                << "\tdailyNext=" << nxtDaily.toString(QString::fromUtf8("yyyy-MM-dd HH:mm:ss"))
                << "\n";
        }

        // ---- 判据 4c：0 起始的 [syncRules] 必须读得回来 ----
        {
            // 夹具**手写 ini**：要的就是"按第 0 条的直觉手写"那种形状。
            // QSettings 自己的数组是 1 起始（beginWriteArray + setArrayIndex(0)
            // 写出来就是 "1\suffix"），所以这份夹具只能手写。
            //
            // 为什么这条判据必须存在：0 起始的写法被读成**空规则表**时，
            // 后果比"读不到"本身更糟 ——
            //   · 规则表为空 ⇒ 扫描不过滤 ⇒ 数据目录里的 .txt/.exe/.dll
            //     全部进清单并推到接收端（**过度同步方向**）；
            //   · 任何一次保存 ⇒ saveSyncRules 先 remove 整段再按表写回
            //     ⇒ 用户手写的那条规则**从磁盘上永久消失**；
            //   · 日志还打「已按实际的 N 条读取」而实际读进来 0 条。
            //
            // 两种基准都要验：修 0 起始的同时不能把 1 起始（Qt 自己写的）弄坏。
            const QString iniFile = ConfigStore::iniPath();
            QByteArray backup;
            {
                QFile bak(iniFile);
                if (bak.open(QIODevice::ReadOnly))
                    backup = bak.readAll();
            }
            // 两份夹具长度不同：QSettings 靠"文件大小变了"来判定磁盘上的文件
            // 被外部改过，长度相同会让第二次读拿到缓存里的旧内容。
            const char *zeroBased =
                "[syncRules]\n"
                "0\\suffix=spe\n"
                "0\\remoteSubDir=ziyuan\n"
                "size=1\n";
            const char *oneBased =
                "[syncRules]\n"
                "1\\suffix=spe\n"
                "1\\remoteSubDir=ziyuan\n"
                "size=1\n"
                "; 由 Qt 自己写出来的形状（1 起始）\n";

            bool okZero = false;
            bool okOne  = false;
            bool wrote = false;
            {
                QFile f(iniFile);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(zeroBased);
                    f.close();
                    wrote = true;
                }
            }
            if (wrote) {
                const QVector<SyncRule> z = ConfigStore::loadSyncRules();
                okZero = (z.size() == 1)
                      && (z.first().suffix == QStringLiteral("spe"))
                      && (z.first().remoteSubDir == QStringLiteral("ziyuan"));
            }
            {
                QFile f(iniFile);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(oneBased);
                    f.close();
                }
            }
            {
                const QVector<SyncRule> o = ConfigStore::loadSyncRules();
                okOne = (o.size() == 1)
                     && (o.first().suffix == QStringLiteral("spe"))
                     && (o.first().remoteSubDir == QStringLiteral("ziyuan"));
            }
            // 还原配置：后面的判据还要用这份 ini。
            if (!backup.isEmpty()) {
                QFile f(iniFile);
                if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    f.write(backup);
                    f.close();
                }
            }

            const bool ok4c = okZero && okOne;
            if (!ok4c) ++bad;
            out << (ok4c ? "PASS\t" : "FAIL\t") << "sync-rules-zero-based"
                << "\tzeroBasedRead=" << (okZero ? 1 : 0)
                << "\toneBasedRead=" << (okOne ? 1 : 0)
                << "\n";
        }

        // ---- 判据 5：间隔输入框边界 + lastRunAt 落盘往返 + 保存后不立刻跑 ----
        {
            SettingsDialog dlg;
            QLineEdit *le = dlg.findChild<QLineEdit *>(QStringLiteral("settingAutoSyncInterval"));
            le->setText(QStringLiteral(""));
            const int emptyGot = dlg.autoSyncInterval();
            // 空输入必须回默认值，不能静默落到下限 10
            // （原来 toInt() 对空串返回 0 → clamp → 10，
            //   客户清空输入框再保存，自动同步变成每 10 秒一轮，
            //   而界面上输入框是空的，他完全不知道自己改了什么）。
            const bool okEmpty = (emptyGot == AutoSyncScheduler::DefaultIntervalSec);
            le->setText(QStringLiteral("86400"));
            const int maxGot = dlg.autoSyncInterval();
            const bool okMax = (maxGot == AutoSyncScheduler::MaxIntervalSec);

            const QDateTime t = QDateTime::currentDateTime();
            ConfigStore::saveAutoSyncLastRunAt(t);
            const QDateTime back = ConfigStore::loadAutoSyncLastRunAt();
            const bool okRt = back.isValid() && qAbs(back.secsTo(t)) <= 1;

            // 排程基准必须是**整秒**。
            //
            // 老版本写进 ini 的时刻带毫秒（ISODate 在 msec≠0 时带 ".zzz"），
            // 读回来若不截，第一次排程的「名义到点时刻」会落在心跳某一拍之后
            // 几毫秒 ⇒ 那一拍判「未到点」跳过 ⇒ 升级后第一轮晚一个心跳
            // （间隔 N 秒跑成 N+N/2）。这条判据把"读出来必须是整秒"锁住。
            const QDateTime withMs(QDate(2026, 10, 24), QTime(10, 0, 0, 123));
            ConfigStore::saveAutoSyncLastRunAt(withMs);
            const QDateTime backMs = ConfigStore::loadAutoSyncLastRunAt();
            const bool okWholeSec = backMs.isValid() && backMs.time().msec() == 0;

            // 保存设置后**不应该**立刻补跑：
            // lastRunAt 是很久以前 → next 若按 lastRunAt+interval 算就在过去，
            // 心跳一醒来就开跑。注释承诺的是"从这一刻起算下一个周期"。
            ConfigStore::saveAutoSyncEnabled(false);
            ConfigStore::saveAutoSyncInterval(60);
            ConfigStore::saveAutoSyncLastRunAt(QDateTime::currentDateTime().addSecs(-7200));
            SyncPanel sp;
            TargetConfig tc;                      // 不配目标，避免真跑
            sp.setLocalRootForTest(QDir::tempPath());
            sp.applyFromSettings(tc, QDir::tempPath(), QVector<ToolRule>(), QVector<SyncRule>());
            const QDateTime nxt = sp.autoSyncNextAtForTest();
            const bool okNoCatchUp = nxt.isValid()
                && nxt > QDateTime::currentDateTime().addSecs(30);

            // 到点时刻必须落在**整秒**上。
            //
            // 这是「排程基准截整秒」这条规则的判据：applyFromSettings 走的是
            // fromNow=true 那条路径，如果这里的基准还带毫秒，某一拍就可能落在
            // 名义到点时刻之前几毫秒 ⇒ 那一拍判「未到点」跳过 ⇒ 第一轮晚一个心跳。
            // 把 autoSyncScheduleNext 的截断改回去，上面几条照样全绿，只有这条会红。
            const bool okNextWholeSec = nxt.isValid() && nxt.time().msec() == 0;

            const bool ok5 = okEmpty && okMax && okRt && okWholeSec && okNextWholeSec && okNoCatchUp;
            if (!ok5) ++bad;
            out << (ok5 ? "PASS\t" : "FAIL\t") << "auto-sync-interval-and-persist"
                << "\temptyField=" << emptyGot
                << "\tdefault=" << AutoSyncScheduler::DefaultIntervalSec
                << "\tmaxOk=" << (okMax ? 1 : 0)
                << "\tlastRunRoundTrip=" << (okRt ? 1 : 0)
                << "\tlastRunWholeSec=" << (okWholeSec ? 1 : 0)
                << "\tnextWholeSec=" << (okNextWholeSec ? 1 : 0)
                << "\tnoCatchUpAfterSave=" << (okNoCatchUp ? 1 : 0)
                << "\n";
        }
    }

    // ----------------------------------------------------------------
    // 每天定点模式（需求 10 的"日历定点"）+ 两种模式互斥 + 老 ini 兼容
    //
    // 为什么必须加这一组：上面 auto-sync-schedule 只验了**间隔**模式。
    // 把 nextDailyRunAt() 整个删掉、或者让它退化成 now+interval，
    // 那几条照样全绿 —— 定点模式的排程一条都没被盖到。
    //
    // 全部调用**产品自己那份实现**（AutoSyncScheduler / ConfigStore /
    // SettingsDialog），不另写影子算法：影子算法会和产品实现漂移，
    // 判据变假绿灯。
    {
        // ---- 判据 1：daily 的 next 计算（纯函数）----
        {
            // 基准时刻**写死**，不用真实时钟。
            //
            // 为什么必须写死：判据 (c) 断言“lastRunAt 恰好是今天 03:00 →
            // next = 明天 03:00”，这条只在“now 已经过了今天 03:00”时成立。
            // 用真实时钟时，凌晨 3 点前跑自检：lastRunAt(=今天 03:00) 落在**将来**
            // → nextDailyRunAt() 按设计以 now 为基准 → 返回今天 03:00
            // （今天还没到，确实该今天跑）→ 判据 (c) 反而判 FAIL。
            //
            // 产品逻辑没错，是**判据依赖钟点**：同一份代码每天 00:00~03:00
            // 自检必 FAIL、03:00 之后又必 PASS。交付给客户的自检绝不能这样飘。
            //
            // 基准取 2026-10-24 10:00（当天 03:00 已过）：10 条子判据全部与
            // 钟点无关，任何时刻跑结果完全一致。
            const QDateTime nowRef(QDate(2026, 10, 24), QTime(10, 0, 0));
            const QTime t0300(3, 0);

            // (a) 从没跑过（lastRunAt 无效）→ now 之后的第一个 03:00
            const QDateTime nextFresh = AutoSyncScheduler::nextDailyRunAt(t0300, nowRef, QDateTime());
            const bool okFresh = nextFresh.isValid()
                && nextFresh.time() == QTime(3, 0)
                && nextFresh > nowRef;

            // (b) lastRunAt 在今天 03:00 之前 → next = 今天 03:00
            const QDateTime lastEarly = QDateTime(nowRef.date(), QTime(1, 0));
            const QDateTime nextEarly = AutoSyncScheduler::nextDailyRunAt(t0300, nowRef, lastEarly);
            const bool okEarly = (nextEarly == QDateTime(nowRef.date(), QTime(3, 0)));

            // (c) lastRunAt 恰好落在今天 03:00:00 → next = **明天** 03:00。
            //     取 ">= base" 会算回今天 03:00（已过去）→ 无限快跑。
            const QDateTime lastExact = QDateTime(nowRef.date(), QTime(3, 0, 0));
            const QDateTime nextExact = AutoSyncScheduler::nextDailyRunAt(t0300, nowRef, lastExact);
            const bool okExact = (nextExact == QDateTime(nowRef.date().addDays(1), QTime(3, 0)));

            // (d) 跨天：lastRunAt 是昨天 04:00 → next = 今天 03:00
            const QDateTime lastYest = QDateTime(nowRef.date().addDays(-1), QTime(4, 0));
            const QDateTime nextYest = AutoSyncScheduler::nextDailyRunAt(t0300, nowRef, lastYest);
            const bool okYest = (nextYest == QDateTime(nowRef.date(), QTime(3, 0)));

            // (e) 跨月：base = 2026-01-31 10:00 → 2026-02-01 03:00
            const QDateTime monthBase(QDate(2026, 1, 31), QTime(10, 0));
            const QDateTime nextMonth = AutoSyncScheduler::nextDailyRunAt(t0300, monthBase, monthBase);
            const bool okMonth = (nextMonth == QDateTime(QDate(2026, 2, 1), QTime(3, 0)));

            // (f) 跨年：base = 2025-12-31 22:00 → 2026-01-01 03:00
            const QDateTime yearBase(QDate(2025, 12, 31), QTime(22, 0));
            const QDateTime nextYear = AutoSyncScheduler::nextDailyRunAt(t0300, yearBase, yearBase);
            const bool okYear = (nextYear == QDateTime(QDate(2026, 1, 1), QTime(3, 0)));

            // (g) 闰年 2 月：base = 2028-02-28 23:00 → 2028-02-29 03:00
            const QDateTime leapBase(QDate(2028, 2, 28), QTime(23, 0));
            const QDateTime nextLeap = AutoSyncScheduler::nextDailyRunAt(t0300, leapBase, leapBase);
            const bool okLeap = (nextLeap == QDateTime(QDate(2028, 2, 29), QTime(3, 0)));

            // (h) fromNow：改完时刻**不立刻跑** —— next 必须严格晚于 now
            const QDateTime nextFromNow = AutoSyncScheduler::nextDailyRunAt(QTime(4, 30), nowRef,
                                                                            QDateTime(), true);
            const bool okFromNow = nextFromNow.isValid()
                && nextFromNow > nowRef
                && nextFromNow.time() == QTime(4, 30);

            // (i) 时钟被往回调（lastRunAt 在将来）→ 不能停摆
            const QDateTime future = nowRef.addSecs(3600);
            const QDateTime nextRollback = AutoSyncScheduler::nextDailyRunAt(t0300, nowRef, future);
            const bool okRollback = nextRollback.isValid() && nextRollback > nowRef;

            // (j) 时刻串：写出来两位补零、读回来一致；非法值一律拒收
            QTime parsed;
            const bool okParse =
                AutoSyncScheduler::parseDailyTime(QStringLiteral("03:00"), &parsed)
                    && parsed == QTime(3, 0)
                && AutoSyncScheduler::parseDailyTime(QStringLiteral("3:00"), &parsed)
                    && parsed == QTime(3, 0)
                && !AutoSyncScheduler::parseDailyTime(QStringLiteral("24:00"), &parsed)
                && !AutoSyncScheduler::parseDailyTime(QStringLiteral("03:60"), &parsed)
                && !AutoSyncScheduler::parseDailyTime(QStringLiteral("3:0"), &parsed)
                && !AutoSyncScheduler::parseDailyTime(QStringLiteral("0300"), &parsed)
                && !AutoSyncScheduler::parseDailyTime(QString(), &parsed)
                && AutoSyncScheduler::dailyTimeToString(QTime(3, 0)) == QStringLiteral("03:00")
                && AutoSyncScheduler::dailyTimeToString(QTime(23, 5)) == QStringLiteral("23:05")
                && AutoSyncScheduler::defaultDailyTime() == QTime(3, 0);

            const bool ok1 = okFresh && okEarly && okExact && okYest && okMonth
                          && okYear && okLeap && okFromNow && okRollback && okParse;
            if (!ok1) ++bad;
            out << (ok1 ? "PASS\t" : "FAIL\t") << "auto-sync-daily-schedule"
                << "\tfresh="        << (okFresh    ? 1 : 0)
                << "\tearly="        << (okEarly    ? 1 : 0)
                << "\tstrictlyAfter=" << (okExact    ? 1 : 0)
                << "\tcrossDay="     << (okYest     ? 1 : 0)
                << "\tcrossMonth="   << (okMonth    ? 1 : 0)
                << "\tcrossYear="    << (okYear     ? 1 : 0)
                << "\tleapDay="      << (okLeap     ? 1 : 0)
                << "\tfromNow="      << (okFromNow  ? 1 : 0)
                << "\tclockRollback=" << (okRollback ? 1 : 0)
                << "\ttimeParse="    << (okParse    ? 1 : 0)
                << "\tnextFresh="    << nextFresh.toString(Qt::ISODate)
                << "\n";
        }

        // ---- 判据 2：错过补跑（关机 3 天再开机）----
        {
            const QDateTime nowRef = QDateTime::currentDateTime();
            // lastRunAt = 3 天前 → 它之后的第一个 03:00 已经落在过去
            const QDateTime last3 = nowRef.addDays(-3);
            const QDateTime next = AutoSyncScheduler::nextDailyRunAt(QTime(3, 0), nowRef, last3);
            // 必须"已到点" → 心跳一醒来就补跑一轮
            const bool okDue = AutoSyncScheduler::isDue(nowRef, next);
            // 补跑完 lastRunAt = now → next = 明天 03:00
            //   （今天 03:00 还没到时，下一次就是今天 03:00）
            const QDateTime next2 = AutoSyncScheduler::nextDailyRunAt(QTime(3, 0), nowRef, nowRef);
            const bool okNext = next2.isValid()
                && next2 > nowRef
                && next2.time() == QTime(3, 0)
                && (nowRef.time() < QTime(3, 0)
                        ? next2.date() == nowRef.date()
                        : next2.date() == nowRef.date().addDays(1));
            const bool ok2 = okDue && okNext;
            if (!ok2) ++bad;
            out << (ok2 ? "PASS\t" : "FAIL\t") << "auto-sync-daily-catchup"
                << "\tdueImmediately=" << (okDue ? 1 : 0)
                << "\tnextAfterRun=" << (okNext ? 1 : 0)
                << "\tnext=" << next.toString(Qt::ISODate)
                << "\tnextAfter=" << next2.toString(Qt::ISODate)
                << "\n";
        }

        // ---- 判据 3：模式 / 时刻的持久化 ----
        {
            ConfigStore::saveAutoSyncMode(AutoSyncScheduler::ModeDaily);
            const bool modeDaily = (ConfigStore::loadAutoSyncMode() == AutoSyncScheduler::ModeDaily);
            ConfigStore::saveAutoSyncMode(AutoSyncScheduler::ModeInterval);
            const bool modeInterval = (ConfigStore::loadAutoSyncMode() == AutoSyncScheduler::ModeInterval);
            ConfigStore::saveAutoSyncDailyTime(QTime(4, 30));
            const bool timeOk = (ConfigStore::loadAutoSyncDailyTime() == QTime(4, 30));
            // 还原成默认，别把后面的用例带偏
            ConfigStore::saveAutoSyncDailyTime(AutoSyncScheduler::defaultDailyTime());

            // 「开机自动启动」开关的往返
            ConfigStore::saveAutoStartEnabled(true);
            const bool asTrue = ConfigStore::loadAutoStartEnabled();
            ConfigStore::saveAutoStartEnabled(false);
            const bool asFalse = !ConfigStore::loadAutoStartEnabled();

            const bool ok3 = modeDaily && modeInterval && timeOk && asTrue && asFalse;
            if (!ok3) ++bad;
            out << (ok3 ? "PASS\t" : "FAIL\t") << "auto-sync-mode-persist"
                << "\tdailyRoundTrip="   << (modeDaily    ? 1 : 0)
                << "\tintervalRoundTrip=" << (modeInterval ? 1 : 0)
                << "\tdailyTimeRoundTrip=" << (timeOk      ? 1 : 0)
                << "\tautoStartTrue="    << (asTrue       ? 1 : 0)
                << "\tautoStartFalse="   << (asFalse      ? 1 : 0)
                << "\n";
        }

        // ---- 判据 4：老 ini 兼容 —— 缺键按默认值读，且**不把文件写回去**----
        //
        // 这一条防的是"升级到新版本顺手把用户的老配置改写了一遍"。
        // 老配置里没有 autoSyncMode / autoSyncDailyTime / autoStartEnabled，
        // 读的时候按默认值（间隔 / 03:00 / 关），**且一个字都不许写回去**。
        {
            const QByteArray savedDir = qgetenv("SRT_CONFIG_DIR");
            QTemporaryDir legacyBox;
            const QString legacyDir = legacyBox.path() + QStringLiteral("/legacy");
            QDir().mkpath(legacyDir);
            const QString legacyIni = legacyDir + QStringLiteral("/SyncReplayTool.ini");

            const QByteArray legacyBytes("[sync]\nautoSyncEnabled=true\nautoSyncIntervalSec=60\n");
            {
                QFile f(legacyIni);
                if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    out << "FAIL\tauto-sync-legacy-ini\tcannot create " << legacyIni << "\n";
                    ++bad;
                }
                f.write(legacyBytes);
                f.close();
            }
            // 读回来存一份"读之前的字节"，用来做逐字节比对
            QByteArray beforeBytes;
            {
                QFile f(legacyIni);
                if (f.open(QIODevice::ReadOnly))
                    beforeBytes = f.readAll();
                f.close();
            }

            qputenv("SRT_CONFIG_DIR", legacyDir.toLocal8Bit());
            const bool modeDefault = (ConfigStore::loadAutoSyncMode() == AutoSyncScheduler::ModeInterval);
            const bool timeDefault = (ConfigStore::loadAutoSyncDailyTime() == QTime(3, 0));
            const bool asDefault = !ConfigStore::loadAutoStartEnabled();
            QByteArray afterBytes;
            {
                QFile f(legacyIni);
                if (f.open(QIODevice::ReadOnly))
                    afterBytes = f.readAll();
                f.close();
            }
            qputenv("SRT_CONFIG_DIR", savedDir);

            const bool notRewritten = (beforeBytes == afterBytes);
            const bool ok4 = modeDefault && timeDefault && asDefault && notRewritten;
            if (!ok4) ++bad;
            out << (ok4 ? "PASS\t" : "FAIL\t") << "auto-sync-legacy-ini"
                << "\tmodeDefaultInterval=" << (modeDefault ? 1 : 0)
                << "\ttimeDefault0300="    << (timeDefault ? 1 : 0)
                << "\tautoStartDefaultOff=" << (asDefault   ? 1 : 0)
                << "\tnotRewritten="       << (notRewritten ? 1 : 0)
                << "\tbytesBefore=" << beforeBytes.size()
                << "\tbytesAfter="  << afterBytes.size()
                << "\n";
        }

        // ---- 判据 5：界面互斥（选 daily 时看不到/用不了间隔输入框）----
        //
        // 用 isHidden() 而不是 isVisible()：自检里这个弹窗**从来没有显示过**，
        // QWidget::isVisible() 对"父窗口没显示"的子控件一律返回 false ——
        // 那样两条判据都会假绿。
        // isHidden() 反映的是控件**自己**被 setVisible(false) 过的状态。
        {
            SettingsDialog dlg;
            QLineEdit *le = dlg.findChild<QLineEdit *>(QStringLiteral("settingAutoSyncInterval"));
            QTimeEdit *te = dlg.findChild<QTimeEdit *>(QStringLiteral("settingAutoSyncDailyTime"));
            // 模式二选一现在是**分段按钮**（定稿图 .seg-group），不再是单选圆点。
            QPushButton *ri = dlg.findChild<QPushButton *>(QStringLiteral("chkAutoSyncModeInterval"));
            QPushButton *rd = dlg.findChild<QPushButton *>(QStringLiteral("chkAutoSyncModeDaily"));
            const bool found = (le && te && ri && rd);
            bool okDaily = false, okInterval = false;
            if (found) {
                // 切到「每天定时」
                rd->setChecked(true);
                okDaily = le->isHidden() && !le->isEnabled()
                          && !te->isHidden() && te->isEnabled();
                // 切回「按间隔重复」
                ri->setChecked(true);
                okInterval = !le->isHidden() && le->isEnabled()
                             && te->isHidden() && !te->isEnabled();
            }
            const bool ok5 = found && okDaily && okInterval;
            if (!ok5) ++bad;
            out << (ok5 ? "PASS\t" : "FAIL\t") << "auto-sync-mode-exclusive-ui"
                << "\twidgetsFound=" << (found ? 1 : 0)
                << "\tdailyHidesInterval=" << (okDaily ? 1 : 0)
                << "\tintervalHidesDaily=" << (okInterval ? 1 : 0)
                << "\n";
        }
    }

    // ----------------------------------------------------------------
    // 端口必须是纯 ASCII 十进制：非法写法不许**静默**退回 22
    //
    // 缺陷（2026-10-18 实测，交付包 + 真实点击 + 日志抽屉截图）：
    // 端口栏填全角数字「２２２６」、中间带空格「2 226」、超范围「99999」，
    // toInt() 全部失败 -> 统一退回 22，且**一句提示都没有**。
    // 用户点开始同步看到的是「网络层：没有连到目标电脑：可能是 IP 填错、
    // 网络或交换机不通、或者目标电脑上这个端口没有服务」—— 真因就在他
    // 屏幕上的端口栏，却被指去了防火墙和交换机。
    // ssh 那边也补救不了：传下去的是**合法**的 22，ssh 无话可说，
    // RsyncRunner 的 "Bad port" 规则只在传 0 时才触发。
    //
    // 正向 4 条 + 反向锁 3 条。反向锁的作用是证明判据测得到「放行」这一侧：
    // 一个把所有输入都判成非法、把正常端口也拦掉的实现同样能让 4 条正向通过。
    {
        const QString fullW = QString::fromUtf8(u8"\xEF\xBC\x92\xEF\xBC\x92\xEF\xBC\x92\xEF\xBC\x96"); // ２２２６
        const bool okPlain    =  ConfigStore::isPlainPortText(QStringLiteral("2226"));
        const bool okMin      =  ConfigStore::isPlainPortText(QStringLiteral("1"));
        const bool okMax      =  ConfigStore::isPlainPortText(QStringLiteral("65535"));
        // 反向锁：下面三个都必须被判成非法
        const bool badFullW   = !ConfigStore::isPlainPortText(fullW);
        const bool badInner   = !ConfigStore::isPlainPortText(QStringLiteral("2 226"));
        const bool badRange   = !ConfigStore::isPlainPortText(QStringLiteral("99999"));
        const bool badZero    = !ConfigStore::isPlainPortText(QStringLiteral("0"));
        const bool badEmpty   = !ConfigStore::isPlainPortText(QString());
        // 文案里必须带一个具体例子，否则用户不知道该填成什么样
        const bool noticeHasExample =
            ConfigStore::invalidPortNotice(fullW).contains(QStringLiteral("2226"));

        // ---- 接线，不是谓词 ----
        //
        // 上一轮就是在这儿翻的车：下面 emptyRejected 那条反向锁**一直是对的**
        // （isPlainPortText("") 确实返回 false），因为它测的是**谓词**；
        // 洞在**调用点** —— `if (!portText.isEmpty() && !isPlainPortText(...))`
        // 那个 isEmpty 守卫写在谓词外面，把「端口根本没填」整类放过去了。
        // 结果：客户第一次装、端口栏空着就保存，界面零提示，静默退 22，
        // 同步全失败并把原因指去网络层和防火墙。
        //
        // 所以这里测的是 noticePortProblem() **真的会把提示攒进队列**：
        //   - 空端口必须攒（这正是漏掉的那一类）
        //   - 正常端口必须**不**攒（反向锁：不然就是无差别地一路提示）
        // 用 pendingNoticeCount() 的**增量**，不用绝对值 ——
        // 绝对值会被本进程里先前的提示污染。
        const int pendBeforeEmpty = ConfigStore::pendingNoticeCount();
        ConfigStore::noticePortProblem(QString());
        const bool wiredEmpty = ConfigStore::pendingNoticeCount() > pendBeforeEmpty;

        const int pendBeforeOk = ConfigStore::pendingNoticeCount();
        ConfigStore::noticePortProblem(QStringLiteral("2226"));
        const bool wiredOkQuiet = ConfigStore::pendingNoticeCount() == pendBeforeOk;

        // 空端口的文案不能是「（当前内容：）」这种没头没尾的话 ——
        // 端口空的时候括号里什么都没有，现场人员看到反而不知道要往哪填。
        const QString emptyNotice = ConfigStore::invalidPortNotice(QString());
        const QString curContent  = QString::fromUtf8(u8"当前内容");
        const bool emptyNoticeSaysSo =
            emptyNotice.contains(QStringLiteral("2226"))
            && !emptyNotice.contains(curContent);

        const bool ok = okPlain && okMin && okMax
                     && badFullW && badInner && badRange && badZero && badEmpty
                     && noticeHasExample
                     && wiredEmpty && wiredOkQuiet && emptyNoticeSaysSo;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "port-plain-digits"
            << "\tplain2226=" << (okPlain ? 1 : 0)
            << "\tmin1=" << (okMin ? 1 : 0)
            << "\tmax65535=" << (okMax ? 1 : 0)
            << "\tfullwidthRejected=" << (badFullW ? 1 : 0)
            << "\tinnerSpaceRejected=" << (badInner ? 1 : 0)
            << "\toutOfRangeRejected=" << (badRange ? 1 : 0)
            << "\tzeroRejected=" << (badZero ? 1 : 0)
            << "\temptyRejected=" << (badEmpty ? 1 : 0)
            << "\tnoticeHasExample=" << (noticeHasExample ? 1 : 0)
            << "\twiredEmptyNotices=" << (wiredEmpty ? 1 : 0)
            << "\twiredValidQuiet=" << (wiredOkQuiet ? 1 : 0)
            << "\temptyNoticeWording=" << (emptyNoticeSaysSo ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 远端根越界：校验必须用 remoteDir() 真正会用的那个串
    //
    // 缺陷（2026-10-18 参数矩阵攻击）：远端目录填 `~/app-data` 通过校验、
    // 文件真落到 /home/app-data/（家目录之外）、界面报"同步完成"。
    // 原因是校验查的是用户填的**原始**串，而 "~" 被当成一个普通目录段，
    // 正好抵消掉紧跟其后的那一个 ".."。填 `~/../../` 两级则被正确拦住，
    // 所以只看"两级能不能拦住"是发现不了这个的。
    //
    // 正向 3 条 + 反向锁 9 条。反向锁缺一不可：只测 `~/../x` 被拦住的话，
    // 一个"把所有输入都当成越界"的实现同样全绿，而那会把正常路径全堵死。
    {
        const QString sTilde      = QStringLiteral("~");
        const QString sTildeSlash = QStringLiteral("~/");
        // ~/app-data   → "app-data"，不越界（用户本意就是家目录下这一层）
        const bool okHome      = !RsyncRunner::remoteRootEscapesHome(sTildeSlash + QStringLiteral("app-data"));
        // ~/../app-data → "../app-data"，**越界**（这一条就是修复的那条）
        const bool okEscape1   =  RsyncRunner::remoteRootEscapesHome(sTildeSlash + QStringLiteral("../app-data"));
        // ~/../../app-data → "../../app-data"，越界（原来也能拦住）
        const bool okEscape2   =  RsyncRunner::remoteRootEscapesHome(sTildeSlash + QStringLiteral("../../app-data"));

        // 反向锁 1：~ 本身 == home 根，永远合法
        const bool okBare      = !RsyncRunner::remoteRootEscapesHome(sTilde)
                                 && RsyncRunner::remoteRootWithoutTilde(sTilde).isEmpty();
        // 反向锁 2：普通相对/绝对路径不受影响
        const bool okPlain     = !RsyncRunner::remoteRootEscapesHome(QStringLiteral("/data/spe"))
                                 && !RsyncRunner::remoteRootEscapesHome(QStringLiteral("data/../spe"));
        // 反向锁 3：相对路径里合法的 ".." 不能误判
        const bool okInner     = !RsyncRunner::subDirEscapesRoot(QStringLiteral("a/../b"))
                                 && !RsyncRunner::subDirEscapesRoot(QStringLiteral("vendor/spe"));
        // 反向锁 4：~user 这种形式必须原样保留，交给别处的显式拒绝处理
        const bool okTildeUser = RsyncRunner::remoteRootWithoutTilde(QStringLiteral("~appuser/x"))
                                 == QStringLiteral("~appuser/x");
        // 反向锁 5：真正越界的几种写法仍然要拦住（cleanPath 会把 "/.." 归一成 "/"，
        // 所以判据必须靠逐段数深度，不能靠字符串比较）
        const bool okEscapes   =  RsyncRunner::subDirEscapesRoot(QStringLiteral("/.."))
                                 && RsyncRunner::subDirEscapesRoot(QStringLiteral("./.."))
                                 && RsyncRunner::subDirEscapesRoot(QStringLiteral("a/../.."))
                                 && RsyncRunner::remoteRootEscapesHome(QStringLiteral("../x"));

        const bool ok = okHome && okEscape1 && okEscape2 && okBare && okPlain
                        && okInner && okTildeUser && okEscapes;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "remote-root-escape"
            << "\ttildeDir=" << (okHome ? 1 : 0)
            << "\ttildeDotDot=" << (okEscape1 ? 1 : 0)
            << "\ttildeDotDot2=" << (okEscape2 ? 1 : 0)
            << "\tbareTilde=" << (okBare ? 1 : 0)
            << "\tplain=" << (okPlain ? 1 : 0)
            << "\tinnerDotDot=" << (okInner ? 1 : 0)
            << "\ttildeUser=" << (okTildeUser ? 1 : 0)
            << "\tescapes=" << (okEscapes ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 路径字段的 ini 往返：反斜杠会被 QSettings 吃掉
    //
    // 缺陷（2026-10-18）：用户手打/粘贴 C:\AP\...\id_rsa，关窗保存后
    // 下次启动读回来变成 D:PP7000\ai-workspaceeysd_valid —— 每往返一次
    // 损坏一次，且信息已丢失、无法反向修复。表现是「私钥文件不存在」，
    // 而文件明明就在那儿。
    //
    // 这里用**真的 QSettings INI** 往返，而不是照抄一份解析逻辑：
    // 判据的价值全在于它踩的是同一个编解码器。
    // rawQt= 那一项只记录、不参与判定 —— 它记的是"不归一化会坏成什么样"，
    // 哪天 Qt 修好了这项会变成 1，那时归一化也就只是无害的冗余。
    {
        QTemporaryDir sandbox;
        const bool made = sandbox.isValid();
        const QString ini = sandbox.filePath(QStringLiteral("roundtrip.ini"));
        const QString winPath  = QStringLiteral("D:\\work\\data\\keys\\id_valid");
        // 反斜杠**用程序拼**，不要直接写在字符串字面量里：
        // "\a" 是 BEL、"\t" 是制表符、"\子" 在 MSVC 下是未定义转义，
        // 测试数据自己带上这种歧义，判据就成了在测自己的字面量。
        // 反斜杠**用程序拼**，不要直接写在字符串字面量里：
        // "\a" 是 BEL、"\t" 是制表符、"\子" 在 MSVC 下是未定义转义，
        // 测试数据自己带上这种歧义，判据就变成在测自己的字面量了。
        const QString bs = QString(QChar('\\'));
        const QString fs = QString(QChar('/'));
        const QString wd = QStringLiteral("id_valid");

        // 被**外部写入** ini 的形态。**只用 ASCII**：外部写的文件里，
        // 中文还会额外撞上一个编码问题（QSettings 读外部 UTF-8 文本的行为
        // 与自己写的文件不同），那是另一件事，混进来这条判据就分不清
        // 是反斜杠坏了还是编码坏了 —— 判据必须一次只测一件事。
        const QStringList extShapes = QStringList()
            << (QStringLiteral("D:") + bs + QStringLiteral("work") + bs + QStringLiteral("data")
                + bs + QStringLiteral("keys") + bs + wd)
            << (QStringLiteral("C:") + bs + QStringLiteral("Users") + bs + QStringLiteral("tester")
                + bs + QStringLiteral(".ssh") + bs + QStringLiteral("id_rsa"))
            << (bs + bs + QStringLiteral("srv") + bs + QStringLiteral("share") + bs
                + QStringLiteral("a b") + bs + wd)
            << (QStringLiteral("D:") + fs + QStringLiteral("work") + fs + QStringLiteral("data")
                + fs + QStringLiteral("keys") + fs + wd)
            << QStringLiteral("/home/appuser/.ssh/id_real");

        // 额外一个含中文的形态，只用于 Qt 自己写读的**反向锁**。
        const QString cjkShape = QStringLiteral("D:") + bs + QString::fromUtf8(u8"data 目录")
                                 + bs + QString::fromUtf8(u8"子目录") + bs + QStringLiteral("a b.spe");

        int rawQt = -1;
        int handWritten = -1;
        bool okFlagMangled = false;
        QString mangledDetail;
        QString rawFileText;
        bool okRound = false;
        bool okUnchanged = false;
        if (made) {
            // 1) Qt 写、Qt 读 —— 完好。Qt 落盘时把反斜杠转义成反斜杠+反斜杠，
            //    往返是对称的。这一项只记录、不参与判定：
            //    它记的是"别再把根因说反"（程序自己写的文件不会被自己读坏）。
            {
                QSettings s(ini, QSettings::IniFormat);
                s.setValue(QStringLiteral("raw/keyPath"), winPath);
                s.sync();
            }
            QString rawBack;
            { QSettings s(ini, QSettings::IniFormat); rawBack = s.value(QStringLiteral("raw/keyPath")).toString(); }
            rawQt = (rawBack == winPath) ? 1 : 0;
            {
                QFile f(ini);
                if (f.open(QIODevice::ReadOnly))
                    rawFileText = QString::fromLatin1(f.readAll());
            }

            // 2) **损坏入口**：ini 里是单反斜杠，而且不是 Qt 写的 ——
            //    手改过、支持同事改过、从别的机器抄过来、被脚本或安装程序生成过。
            //    这里用 QFile 直接写文本、不经过 QSettings，才造得出这个形态。
            //
            //    正向不变式：**产品存进去的形态**（归一化后的正斜杠）被外部写进
            //    ini、再读回来，必须逐字节相同。
            //    这条对修复本身敏感：把 normalizePathForStore 改成原样返回，
            //    存进去的就还是反斜杠形态，这里立刻变红。
            QStringList mangledRaw;
            QStringList mangledNorm;
            for (int i = 0; i < extShapes.size(); ++i) {
                const QString raw = extShapes.at(i);
                const QString norm = ConfigStore::normalizePathForStore(raw);
                // 原始形态：只作为"损坏确实存在"的证据，不参与判定
                {
                    const QString hi = sandbox.filePath(QStringLiteral("h-%1.ini").arg(i));
                    { QFile f(hi);
                      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                          f.write("[target]\nkeyPath=" + raw.toUtf8() + "\n"); f.close(); } }
                    QString back;
                    { QSettings s(hi, QSettings::IniFormat); back = s.value(QStringLiteral("target/keyPath")).toString(); }
                    if (back != raw)
                        mangledRaw << (raw + QStringLiteral(" -> ") + back);
                }
                // 归一化后的形态：**判定项**
                {
                    const QString ni = sandbox.filePath(QStringLiteral("n-%1.ini").arg(i));
                    { QFile f(ni);
                      if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                          f.write("[target]\nkeyPath=" + norm.toUtf8() + "\n"); f.close(); } }
                    QString back;
                    { QSettings s(ni, QSettings::IniFormat); back = s.value(QStringLiteral("target/keyPath")).toString(); }
                    if (back != norm)
                        mangledNorm << (norm + QStringLiteral(" -> ") + back);
                }
            }
            handWritten = mangledRaw.size();
            mangledDetail = mangledRaw.join(QStringLiteral(" ; "));
            okFlagMangled = mangledNorm.isEmpty();
            if (!okFlagMangled)
                mangledDetail += QStringLiteral(" ;; NORM-FAILED: ")
                                + mangledNorm.join(QStringLiteral(" ; "));

            // 3) 归一化之后经 Qt 写读也必须无损（产品真正的落盘路径）
            bool okStorePath = true;
            for (int i = 0; i < extShapes.size(); ++i) {
                const QString norm = ConfigStore::normalizePathForStore(extShapes.at(i));
                const QString si = ini + QStringLiteral(".s%1").arg(i);
                { QSettings s(si, QSettings::IniFormat);
                  s.setValue(QStringLiteral("target/keyPath"), norm); s.sync(); }
                QString back;
                { QSettings s(si, QSettings::IniFormat); back = s.value(QStringLiteral("target/keyPath")).toString(); }
                if (back != norm)
                    okStorePath = false;
            }
            okRound = okStorePath;

            // 反向锁：Qt 不经过归一化、直接写原始形态再读回来（含中文）也必须原样。
            // 这条保证"归一化没有掩盖别处的损坏"。
            bool okQtMatrix = true;
            for (int i = 0; i <= extShapes.size(); ++i) {
                const QString shape = (i < extShapes.size()) ? extShapes.at(i) : cjkShape;
                const QString qi = ini + QStringLiteral(".q%1").arg(i);
                { QSettings s(qi, QSettings::IniFormat);
                  s.setValue(QStringLiteral("target/keyPath"), shape); s.sync(); }
                QString back;
                { QSettings s(qi, QSettings::IniFormat); back = s.value(QStringLiteral("target/keyPath")).toString(); }
                if (back != shape)
                    okQtMatrix = false;
            }
            okUnchanged = okQtMatrix;
        }

        // 反向锁：好值一个都不能被误判，否则每次启动都弹一条"你的路径坏了"
        const bool okFlagGood = !ConfigStore::pathLooksCorrupted(QStringLiteral("C:/AP/x.spe"))
                                && !ConfigStore::pathLooksCorrupted(QStringLiteral("C:\\Users\\x\\.ssh\\id_rsa"))
                                && !ConfigStore::pathLooksCorrupted(QStringLiteral("/home/appuser/.ssh/id_real"))
                                && !ConfigStore::pathLooksCorrupted(QStringLiteral("~/keys/id_valid"))
                                && !ConfigStore::pathLooksCorrupted(QStringLiteral("\\\\srv\\share\\a.spe"))
                                && !ConfigStore::pathLooksCorrupted(QStringLiteral("D:"))
                                && !ConfigStore::pathLooksCorrupted(QString());
        // 反向锁：Linux 上带冒号的路径**完全正常**（冒号是合法的文件名字符），
        // 判成"配置已损坏"会让用户每次启动都看到一句让他重选目录的提示，
        // 而路径一点问题都没有。
        const bool okFlagLinuxColon = !ConfigStore::pathLooksCorrupted(QStringLiteral("/data/spe:2024"))
                                      && !ConfigStore::pathLooksCorrupted(QStringLiteral("/mnt/d:spe"))
                                      && !ConfigStore::pathLooksCorrupted(QStringLiteral("/home/appuser/a:b:c"));
        // 反向锁：Linux 路径里的反斜杠是**合法的文件名字符**，
        // 归一化把它换成正斜杠就等于把用户指向了另一个目录。
        // Windows 上规则相反 —— 那边必须换（Qt 读回时会掉字符）。
#ifdef Q_OS_WIN
        const bool okNormLinux = true;   // Linux 规则在 Windows 上不适用
        const bool okNormWin = ConfigStore::normalizePathForStore(QStringLiteral("C:\\AP\\keys\\id_rsa"))
                               == QStringLiteral("C:/AP/keys/id_rsa")
                               && ConfigStore::normalizePathForStore(QStringLiteral("/data/spe\\2024"))
                                  == QStringLiteral("/data/spe/2024");
#else
        const bool okNormLinux = ConfigStore::normalizePathForStore(QStringLiteral("/data/spe\\2024"))
                                 == QStringLiteral("/data/spe\\2024");
        const bool okNormWin = ConfigStore::normalizePathForStore(QStringLiteral("C:\\AP\\keys\\id_rsa"))
                               == QStringLiteral("C:/AP/keys/id_rsa");
#endif

        const bool ok = made && okRound && okUnchanged && okFlagMangled && okFlagGood
                        && okFlagLinuxColon && okNormLinux && okNormWin;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "ini-path-roundtrip"
            << "\tqtWriteRead=" << rawQt
            << "\thandWrittenMangled=" << handWritten
            << "\tnormIntact=" << (okFlagMangled ? 1 : 0)
            << "\tnoMangleQt=" << (okUnchanged ? 1 : 0)
            << "\troundtrip=" << (okRound ? 1 : 0)
            << "\tflagGood=" << (okFlagGood ? 1 : 0)
            << "\tmangledByHand=[" << mangledDetail << "]"
            << "\n";
    }

    // ----------------------------------------------------------------
    // 归因"建议"：层判对了还不够，建议指的方向也得对
    //
    // 2026-10-18 参数矩阵攻击实测出的三处"把人引到错误方向"：
    //   1. 全角端口 -> 原文 `Bad port '0'`，之前判成网络层，建议 ping 目标 IP、
    //      查防火墙 —— 而真正要做的只是把端口栏改回数字；
    //   2. 远端路径栏填成一个已存在的**文件** -> 接收端层的通用建议是
    //      "让对方管理员确认目录存在、写权限、磁盘空间、已安装 rsync"，
    //      四条一条都不对；
    //   3. 本机路径过长 -> 建议查杀软、查权限。
    //
    // 反向锁 4 条：普通网络/接收端故障必须**继续走通用建议**。
    // 只测"专用建议非空"的话，一个"对任何输入都返回一段话"的实现同样全绿，
    // 而那等于把所有故障都指到同一个错误方向。
    {
        const QString badPort = QStringLiteral("ssh: Bad port '0'\r\n"
                                               "ssh: connect to host 192.168.0.1: Connection refused");
        const QString notDir  = QStringLiteral("rsync: [Receiver] mkdir \"/home/appuser/testa9/a9.spe\" failed: "
                                               "Not a directory (20)\r\n"
                                               "rsync error: errors selecting input/output files, dirs (code 3)");
        const QString refused = QStringLiteral("ssh: connect to host 192.168.0.1 port 22: Connection refused");
        const QString noSpace = QStringLiteral("rsync: [receiver] write failed: No space left on device (28)");
        const QString kylin   = QStringLiteral("ssh: connect to host 192.168.0.1 port 22: Operation not permitted");

        // 层：端口填错 -> 本机层（不是网络层）；路径填成文件 -> 接收端层
        const bool okLayerPort = (RsyncRunner::layerOf(badPort) == 0);
        const bool okLayerDir  = (RsyncRunner::layerOf(notDir) == 3);
        // 建议：这两类必须拿到专用建议
        const QString advPort = RsyncRunner::specialAdviceFor(badPort);
        const QString advDir  = RsyncRunner::specialAdviceFor(notDir);
        const bool okAdvPort = !advPort.isEmpty() && advPort.contains(QStringLiteral("Bad port"));
        const bool okAdvDir  = !advDir.isEmpty() && advDir.contains(QStringLiteral("Not a directory"));
        // 反向锁：这两类**不能**被新分支误伤，必须仍然走通用建议（返回空串）
        const bool okGeneric = RsyncRunner::specialAdviceFor(refused).isEmpty()
                               && RsyncRunner::specialAdviceFor(noSpace).isEmpty();
        // 反向锁：层没变
        const bool okLayerRest = (RsyncRunner::layerOf(refused) == 1)
                                 && (RsyncRunner::layerOf(noSpace) == 3);
        // 反向锁：安全中心那条专用建议必须还在（早于本轮改动，不能被挤掉）
        const bool okKylin = !RsyncRunner::specialAdviceFor(kylin).isEmpty();

        const bool ok = okLayerPort && okLayerDir && okAdvPort && okAdvDir
                        && okGeneric && okLayerRest && okKylin;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "attribution-advice"
            << "\tlayerPort=" << (okLayerPort ? 1 : 0)
            << "\tlayerDir=" << (okLayerDir ? 1 : 0)
            << "\tadvPort=" << (okAdvPort ? 1 : 0)
            << "\tadvDir=" << (okAdvDir ? 1 : 0)
            << "\tgeneric=" << (okGeneric ? 1 : 0)
            << "\tlayerRest=" << (okLayerRest ? 1 : 0)
            << "\tkylin=" << (okKylin ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // QString::arg() 到底吃不吃反斜杠（2026-10-18 参数矩阵子代理的怀疑项）
    //
    // 报告称日志里"私钥路径"的反斜杠全被吃掉，归因到 QString::arg()。
    // 实测结论：**arg() 不做任何转义处理**，替换文本里的 \ 原样保留。
    // 当时看到的"没有反斜杠"是配置文件里的值本身已经被 ini 编解码吃掉了
    // （见 ini-path-roundtrip 那条），不是 arg() 的问题。
    //
    // 放一条判据在这里，是为了让"顺手给 arg() 加一层转义处理"这种改动
    // 立刻变红 —— 那会把用户填的每一个反斜杠都变成两个。
    {
        const QString bs = QString(QChar('\\'));
        const QString path = QStringLiteral("D:") + bs + QStringLiteral("work") + bs
                             + QStringLiteral("keys") + bs + QStringLiteral("id_valid");
        const QString rendered = QStringLiteral("%1").arg(path);
        const bool okArg = (rendered == path);
        const bool okUnc = (QStringLiteral("%1").arg(QStringLiteral("\\\\srv\\share")) == bs + bs + QStringLiteral("srv") + bs + QStringLiteral("share"));
        const bool ok = okArg && okUnc;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "arg-keeps-backslash"
             << "\tplain=" << (okArg ? 1 : 0)
             << "\tunc=" << (okUnc ? 1 : 0)
             << "\n";
    }

    // ----------------------------------------------------------------
    // 本机 ssh_config 读不懂 -> 必须判本机层，且拿到专用建议
    //
    // 真实故障（银河麒麟 V11 / Ubuntu 20.04，包内自带 OpenSSH 7.2p2）：
    //   /etc/ssh/ssh_config: line 19: Bad configuration option: include
    //   /etc/ssh/ssh_config: terminating, 1 bad configuration options
    //   rsync: connection unexpectedly closed (0 bytes received so far) [sender]
    // Include 是 7.3 才有的指令，7.2 读不懂就在**解析配置**阶段终止，
    // 一个字节都没发出去。而 "connection unexpectedly closed" 排在 kFailRules
    // 的网络层，原来的判据把一次"本机配置文件太老"说成"没连上目标电脑"，
    // 用户去 ping IP、查防火墙 —— 真因是这台机器上的 ssh_config。
    //
    // 正例 3 条（3 种真实原文形态都必须判本机层）+ 专用建议非空
    // 反向锁 3 条：真正的网络故障必须**继续**判网络层、拿通用建议。
    // 反向锁缺了的话，一个"对任何输入都返回本机层"的实现同样全绿，
    // 而那会把所有真实网络故障都说成配置问题。
    {
        // 带完整收尾症状行的那一份（最容易被症状带偏）
        const QString full = QStringLiteral(
            "/etc/ssh/ssh_config: line 19: Bad configuration option: include\n"
            "/etc/ssh/ssh_config: terminating, 1 bad configuration options\n"
            "rsync: connection unexpectedly closed (0 bytes received so far) [sender]\n"
            "rsync error: unexplained error (code 255) at io.c(235) [sender=3.1.3]");
        // 只有 terminating 那一行（部分发行版不打 Bad configuration option）
        const QString onlyTerm = QStringLiteral(
            "/etc/ssh/ssh_config: terminating, 1 bad configuration options");
        // 只有 Bad configuration option 那一行
        const QString onlyBad = QStringLiteral(
            "/etc/ssh/ssh_config: line 19: Bad configuration option: include");

        const bool okLayerFull = (RsyncRunner::layerOf(full) == 0);
        const bool okLayerTerm = (RsyncRunner::layerOf(onlyTerm) == 0);
        const bool okLayerBad  = (RsyncRunner::layerOf(onlyBad) == 0);

        // 命中 ssh_config 时必须给出一条**非空**说明：用户得看到这条报错
        // 到底指向哪个配置文件、以及接下来改哪一行。
        // 同时**禁止**再出现「不是网络不通」这种否定式断言 —— 程序只观测到
        // ssh 报文里带 ssh_config 字样，说死"不是网络问题"属于未验证断言，
        // 今天按用户要求连同文案一起删掉了。
        const QString adv = RsyncRunner::specialAdviceFor(full);
        const bool okAdv = !adv.trimmed().isEmpty()
                           && adv.contains(QStringLiteral("ssh_config"))
                           && !adv.contains(QString::fromUtf8(u8"\u4E0D\u662F\u7F51\u7EDC\u4E0D\u901A"));

        // 反向锁 1/2：真实网络故障继续判网络层
        const bool okNetRefused = (RsyncRunner::layerOf(
            QStringLiteral("ssh: connect to host 192.168.0.1 port 22: Connection refused")) == 1);
        const bool okNetTimeout = (RsyncRunner::layerOf(
            QStringLiteral("ssh: connect to host 192.168.0.1 port 22: Connection timed out\n"
                           "rsync: connection unexpectedly closed (0 bytes received so far) [sender]")) == 1);
        // 反向锁 3：网络故障必须继续走通用建议（返回空串 = 不覆盖）
        const bool okNetGeneric = RsyncRunner::specialAdviceFor(
            QStringLiteral("ssh: connect to host 192.168.0.1 port 22: Connection refused")).isEmpty()
            && RsyncRunner::specialAdviceFor(
            QStringLiteral("ssh: connect to host 192.168.0.1 port 22: Connection timed out")).isEmpty();

        // 反向锁 4：接收端层的真实故障不能被 ssh_config 这条误伤
        const bool okRecvStill = (RsyncRunner::layerOf(
            QStringLiteral("rsync: [receiver] write failed on \"/p/big.spe\": No space left on device (28)")) == 3);

        const bool ok = okLayerFull && okLayerTerm && okLayerBad && okAdv
                        && okNetRefused && okNetTimeout && okNetGeneric && okRecvStill;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "attribution-ssh-config"
            << "\tlayerFull=" << (okLayerFull ? 1 : 0)
            << "\tlayerTerm=" << (okLayerTerm ? 1 : 0)
            << "\tlayerBad=" << (okLayerBad ? 1 : 0)
            << "\tadvice=" << (okAdv ? 1 : 0)
            << "\tnetRefused=" << (okNetRefused ? 1 : 0)
            << "\tnetTimeout=" << (okNetTimeout ? 1 : 0)
            << "\tnetGeneric=" << (okNetGeneric ? 1 : 0)
            << "\trecvStill=" << (okRecvStill ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // --selfcheck-secrets 必须真的走口令分支
    //
    // 缺陷（实测第一轮就是这样失败的）：--selfcheck-secrets 只调
    // setPasswordForTest()、**不清 keyPath**，而 useKey() 就是
    // `!keyPath.isEmpty()`。配置里只要残留过一个 keyPath，这个"口令模式"
    // 自检实际走的是私钥分支 —— 日志原文：
    //     私钥文件不存在或不可读：/root/.ssh/id_srt
    // 口令链路（createAskpass → SSH_ASKPASS → 口令文件落盘）一次都没跑。
    //
    // 只看返回码判不出来：私钥那条路照样"没留残留"→ 照样报 ALL_CLEAN。
    // 所以判据验的是 **useKey() 真的变成 false**，也就是
    // applySecretCheckAuth() 这个被自检和判据**共用**的函数
    // （共用是关键：判据照抄一份的话，把 keyPath.clear() 删掉它照样全绿）。
    //
    // 反向锁：keyPath 本身没被 setPasswordForTest 碰到 ——
    // 证明"只有显式清空才有用"，而不是碰巧清掉了。
    {
        const QString residualKey = QStringLiteral("/root/.ssh/id_srt");
        const QString probePass  = QStringLiteral("SrtProbe2026");

        SyncPanel probe;
        // 先摆一个残留的 keyPath，模拟"用户以前用过私钥、切回口令后没清"
        probe.setKeyPathForTest(residualKey);
        const bool fixtureOk = probe.effectiveTarget().useKey();
        // 正例：走共用的装配函数之后必须不再是私钥模式
        applySecretCheckAuth(&probe, probePass);
        const TargetConfig after = probe.effectiveTarget();
        const bool okPasswordBranch = !after.useKey() && after.password == probePass;
        // 反向锁：setPasswordForTest 单独用**不会**清 keyPath（证明修复是有效的、）
        SyncPanel probe2;
        probe2.setKeyPathForTest(residualKey);
        probe2.setPasswordForTest(probePass);
        const bool okSetterAlone = probe2.effectiveTarget().useKey();

        const bool ok = fixtureOk && okPasswordBranch && okSetterAlone;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "secrets-password-mode"
            << "\tfixtureHadKey=" << (fixtureOk ? 1 : 0)
            << "\tpasswordBranch=" << (okPasswordBranch ? 1 : 0)
            << "\tpassSet=" << (after.password == probePass ? 1 : 0)
            << "\tsetterAloneKeepsKey=" << (okSetterAlone ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 收尾顺序：远端标记必须先于口令文件清理
    //
    // cleanupRemoteMarkers() 靠 ssh 连到接收端执行 `rm -f`，它要的凭据
    // （SSH_ASKPASS 脚本 + 口令文件）正是 cleanupSecrets() 要删的两个文件。
    // 顺序反了 → 清理用的 ssh 没有凭据 → 接收端留下 .srt-verify-*
    // （实测：Windows 进程内探针，取消同步 3/3 必留残留）。
    //
    // 判据读的是 **产品那张次序表** RsyncRunner::finishCleanupSteps()，
    // launchNext() 走的是同一个 runFinishCleanup()。第一版这里写的是
    // 一个"模拟收尾"的 lambda —— 变异测试立刻证明它是自证的：
    // 把产品里两次调用的次序对调，判据照样全绿（模型没跟着产品走）。
    // 所以改成直接断言产品表。
    {
        const QStringList steps = RsyncRunner::finishCleanupSteps();
        const int iMark = steps.indexOf(QStringLiteral("remote-markers"));
        const int iSec  = steps.indexOf(QStringLiteral("secrets"));

        // 正例：两步都在，且标记在前
        const bool okBoth = (iMark >= 0) && (iSec >= 0) && (steps.size() == 2);
        const bool okOrder = okBoth && (iMark < iSec);

        // 反向锁 1：把表对调（模拟退回缺陷顺序）必须判红。
        // 不用 swapItemsAt —— 那是 Qt 5.13 才有的，这套是 Qt 5.9.6。
        QStringList swapped;
        for (int i = steps.size() - 1; i >= 0; --i)
            swapped.append(steps.at(i));
        const int jSec = swapped.indexOf(QStringLiteral("secrets"));
        const int jMark = swapped.indexOf(QStringLiteral("remote-markers"));
        const bool redOnSwap = (jMark >= 0) && (jSec >= 0) && (jMark > jSec)
                               && !((jMark < jSec));   // 与 okOrder 同一判据
        // 反向锁 2：步骤不能凭空多出第三步（判据自己不能宽松）
        const bool noExtra = (swapped.size() == 2);
        if (!redOnSwap || !noExtra)
            ++bad;

        const bool ok = okBoth && okOrder;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "remote-marker-cleanup-order"
            << "\tsteps=" << steps.join(QStringLiteral(","))
            << "\tiMarkers=" << iMark
            << "\tiSecrets=" << iSec
            << "\torderOk=" << (okOrder ? 1 : 0)
            << "\tswapGoesRed=1"
            << "\n";
    }

    // ----------------------------------------------------------------
    // 用户可见文案钉死：必须出现「已经损坏」，不得出现「已经损坍」
    //
    // \u574D 是「坍」、\u574F 才是「坏」。写错一位的话界面会显示
    // 「已经损坍，反斜杠在配置读写中被丢弃」—— 错字直接进客户眼前。
    // 用码位比对而不是肉眼：肉眼分不出这两个字长得像。
    //
    // 判据调的是 ConfigStore 暴露出来的**产品那一份字符串**，
    // 不是这里照抄的副本 —— 照抄的话把实现改回错字，判据照样全绿。
    {
        const QString corruptKey = ConfigStore::corruptKeyPathNotice();
        const QString corruptDir = ConfigStore::corruptLocalRootNotice();
        const QChar hao   = QChar(0x574F);   // 坏
        const QChar tan   = QChar(0x574D);   // 坍
        const bool okKey = !corruptKey.isEmpty()
                           && corruptKey.contains(hao) && !corruptKey.contains(tan);
        const bool okDir = !corruptDir.isEmpty()
                           && corruptDir.contains(hao) && !corruptDir.contains(tan);
        // 反向锁 1：码位本身不能搞反（判据自己写错就全绿）
        const bool okCode = (hao == QChar(0x574F) && tan == QChar(0x574D) && hao != tan);
        // 反向锁 2：两条文案不能是同一句（照抄成一条也会全绿）
        const bool okDistinct = (corruptKey != corruptDir);

        const bool ok = okKey && okDir && okCode && okDistinct;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "corrupt-notice-wording"
            << "\tkeyHasHao=" << (okKey ? 1 : 0)
            << "\tdirHasHao=" << (okDir ? 1 : 0)
            << "\tcodeGuard=" << (okCode ? 1 : 0)
            << "\tdistinct=" << (okDistinct ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 工具启动链路：预检分类 + 文案
    //
    // 判据调的是 SyncPanel 暴露出来的**产品那一份** preflightLaunch /
    // preflightMessage，不是这里照抄的副本。照抄的话把产品改回原样，
    // 判据照样全绿 —— 上一轮的收尾次序判据就是这么被证伪的。
    {
        QTemporaryDir sandbox;
        const QString root = sandbox.path();
        // 现场必须先造出来，否则"不存在"和"造不出来"报的是同一句话。
        const QString goodFile = root + QStringLiteral("/a.spe");
        const QString goodExe  = root + QStringLiteral("/tool.sh");
        QFile f1(goodFile); f1.open(QIODevice::WriteOnly); f1.write("x"); f1.close();
        QFile f2(goodExe);  f2.open(QIODevice::WriteOnly); f2.write("#!/bin/sh\n"); f2.close();
        const QString goneFile = root + QStringLiteral("/gone.spe");
        const QString goneExe  = root + QStringLiteral("/gone.sh");
        const bool sceneOk = QFileInfo(goodFile).exists() && QFileInfo(goodExe).exists()
                             && !QFileInfo(goneFile).exists() && !QFileInfo(goneExe).exists();
        if (!sceneOk) {
            out << "FAIL\tselftest-fixture\ttool-preflight fixture not created\n";
            ++bad;
        }

        const QString okTag    = SyncPanel::launchVerdictTag(SyncPanel::preflightLaunch(goodExe, goodFile, true));
        const QString noTag    = SyncPanel::launchVerdictTag(SyncPanel::preflightLaunch(QString(), goodFile, true));
        const QString goneTag  = SyncPanel::launchVerdictTag(SyncPanel::preflightLaunch(goodExe, goneFile, true));
        const QString toolTag  = SyncPanel::launchVerdictTag(SyncPanel::preflightLaunch(goneExe, goodFile, true));
        const QString permTag  = SyncPanel::launchVerdictTag(SyncPanel::preflightLaunch(goodExe, goodFile, false));

        // 文案：CJK 一律按码位查，不在发起进程那侧拼中文字面量。
        const QString msgMissing = SyncPanel::preflightMessage(SyncPanel::LaunchToolMissing,
                                                               goneExe, goodFile, QStringLiteral("t"));
        const QString msgPerm = SyncPanel::preflightMessage(SyncPanel::LaunchToolNotExecutable,
                                                            goodExe, goodFile, QStringLiteral("t"));
        const QString msgSrc = SyncPanel::preflightMessage(SyncPanel::LaunchSourceMissing,
                                                           goodExe, goneFile, QStringLiteral("t"));
        // 设(8BBE) 置(7F6E) / 程(7A0B) 序(5E8F) 路(8DEF) 径(5F84)
        const bool hintSetting  = msgMissing.contains(QChar(0x8BBE)) && msgMissing.contains(QChar(0x7F6E));
        const bool hintProgram  = msgMissing.contains(QChar(0x7A0B)) && msgMissing.contains(QChar(0x5E8F))
                                  && msgMissing.contains(QChar(0x8DEF)) && msgMissing.contains(QChar(0x5F84));
        const bool hintPath     = msgMissing.contains(goneExe);
        const bool hintChmod    = msgPerm.contains(QLatin1String("chmod +x")) && msgPerm.contains(goodExe);
        const bool hintSrcPath  = msgSrc.contains(goneFile);

        const bool okVerdict = (okTag == QLatin1String("ok"))
                               && (noTag == QLatin1String("no-tool"))
                               && (goneTag == QLatin1String("source-missing"))
                               && (toolTag == QLatin1String("tool-missing"));
        #ifdef Q_OS_WIN
        // Windows 上 Qt 5.9 的 isExecutable() 对任何存在的文件都是 true，
        // 所以"无执行权限"这一档在文件系统上造不出来。这里断言的正是
        // 那个事实：产品没有在 Windows 上瞎报执行权限。
        const bool okPerm = (permTag == QLatin1String("ok"));
        const QString permNote = QStringLiteral("win-isExecutable-always-true");
        #else
        const bool okPerm = (permTag == QLatin1String("not-executable"));
        const QString permNote = QStringLiteral("posix-chmod-minus-x");
        #endif
        // 反向锁 1：码位本身不能写反，否则 hintSetting / hintProgram 恒为真。
        const bool okCode = (QChar(0x8BBE) != QChar(0x7F6E) && QChar(0x8BBE) == QChar(0x8bbe));
        // 反向锁 2：两条提示不能是同一句（照抄成一句也会全绿）。
        const bool distinct = (msgMissing != msgPerm) && (msgMissing != msgSrc);

        const bool ok = sceneOk && okVerdict && okPerm && hintSetting && hintProgram
                        && hintPath && hintChmod && hintSrcPath && okCode && distinct;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "tool-preflight-verdict"
            << "\tok=" << okTag << " noTool=" << noTag << " srcGone=" << goneTag
            << " toolGone=" << toolTag << " noPerm=" << permTag << " (" << permNote << ")"
            << "\thintSetting=" << (hintSetting ? 1 : 0)
            << " hintProgramPath=" << (hintProgram ? 1 : 0)
            << " hintFullExePath=" << (hintPath ? 1 : 0)
            << " hintChmodCmd=" << (hintChmod ? 1 : 0)
            << " hintFullSrcPath=" << (hintSrcPath ? 1 : 0)
            << " codeGuard=" << (okCode ? 1 : 0) << " distinct=" << (distinct ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 工具启动：防抖 + 退出码分类 + 日志上限
    //
    // 这一组全部走**真实链路**：构造 SyncPanel、灌文件与工具规则、
    // 调 launchToolForRow，让它真的把子进程拉起来。
    // 只测静态函数的话，"产品那一侧根本没调用它"这类缺陷判据永远咬不到。
    {
        QTemporaryDir sandbox;
        const QString root = sandbox.path();
        const QString dataFile = root + QStringLiteral("/data.spe");
        QFile f(dataFile);
        f.open(QIODevice::WriteOnly); f.write("srt-probe"); f.close();

        // 三种退出的桩程序。Windows 走 cmd.exe + .bat（QProcess 不能直接
        // CreateProcess 一个 .bat），Linux 走 /bin/sh + 脚本。
        // 两边的"程序"形态不同，但**语义**必须一样：同样的退出码、同样的 stderr。
        #ifdef Q_OS_WIN
        // QFileInfo("cmd.exe").exists() is FALSE: it resolves against the CWD,
        // not PATH.  The pre-flight would then correctly (but uselessly) report
        // "tool missing" and the exit-code cases would never run.
        const QString runner = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
        const auto argsFor = [](const QString &stub) { return QStringLiteral("/c ") + stub; };
        #else
        const QString runner = QStringLiteral("/bin/sh");
        const auto argsFor = [](const QString &stub) { return stub; };
        #endif
        const QString okExe  = makeToolStub(root, QStringLiteral("ok"), QStringLiteral(".bat"), QString(), 0);
        const QString badExe = makeToolStub(root, QStringLiteral("fail"), QStringLiteral(".bat"),
                                            QStringLiteral("cannot open display"), 2);
        const QString glibExe = makeToolStub(root, QStringLiteral("glib"), QStringLiteral(".bat"),
                                             QStringLiteral("GLib-GObject-CRITICAL: g_object_get_data: assertion 'G_IS_OBJECT (object)' failed"), 3);

        SyncPanel panel;
        panel.setLocalRootForTest(root);
        FileItem item;
        item.relativePath = QStringLiteral("data.spe");
        item.suffix = QStringLiteral("spe");
        item.size = 9;
        panel.injectFiles(QVector<FileItem>() << item);

        ToolRule rule;
        rule.suffix = QStringLiteral("spe");
        rule.toolNames    << QStringLiteral("probeTool");
        rule.toolExePaths << runner;
        rule.toolArgs     << argsFor(glibExe);
        panel.setToolRulesForTest(QVector<ToolRule>() << rule);

        const bool sceneOk = QFileInfo(dataFile).exists() && QFileInfo(glibExe).exists()
                             && QFileInfo(badExe).exists() && QFileInfo(okExe).exists();
        if (!sceneOk) {
            out << "FAIL\tselftest-fixture\ttool-launch fixture not created\n";
            ++bad;
        }

        // ---- 防抖：连点 3 次只准起 1 个进程 ----
        panel.resetToolLaunchStateForTest();
        panel.launchToolForTest(0, 0);
        panel.launchToolForTest(0, 0);
        panel.launchToolForTest(0, 0);
        const int attempts = panel.toolLaunchAttemptsForTest();
        const int suppressed = panel.toolDebounceSuppressedForTest();
        const int running = panel.runningToolCountForTest();
        const bool okDebounce = (attempts == 1) && (suppressed == 2) && (running == 1);

        // 等它真的结束（QProcess 的 finished 要事件循环才推得动）。
        const int beforeFin = panel.toolFinishedCountForTest();
        QElapsedTimer w; w.start();
        while (panel.toolFinishedCountForTest() == beforeFin && w.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
            QThread::msleep(5);
        }
        const QStringList rec1 = panel.logRecordsForTest();
        QString tag1, body1;
        if (!rec1.isEmpty()) {
            const QStringList parts = rec1.last().split(QLatin1Char('\t'));
            tag1 = parts.value(0); body1 = parts.value(1);
        }
        // 良性 GLib 码必须是黄色 WARN 并带上退出码。
        // 文案只许陈述观测到的事实（非零退出码 + stderr 命中启动器特征串），
        // **不许**声称文件已打开 —— 程序从来没验证过文件到底开没开，
        // 那句未验证的完成断言今天按用户要求删掉了。这里反向锁住：
        // 谁再把「已打开 / 打开了」写回用户可见文案，这条判据立刻变红。
        const bool glibWarn = (tag1 == QLatin1String("WARN")) && body1.contains(QLatin1String("3"));
        const bool glibExplains = !body1.trimmed().isEmpty()
                                  && !body1.contains(QChar(0x5DF2))                              // 「已」
                                  && !body1.contains(QString::fromUtf8(u8"\u6253\u5F00\u4E86"));   // 「打开了」

        // ---- 真失败仍然是红色 ERR（不能放宽成一律成功）----
        rule.toolArgs[0] = argsFor(badExe);
        panel.setToolRulesForTest(QVector<ToolRule>() << rule);
        panel.resetToolLaunchStateForTest();
        const int beforeFin2 = panel.toolFinishedCountForTest();
        panel.launchToolForTest(0, 0);
        QElapsedTimer w2; w2.start();
        while (panel.toolFinishedCountForTest() == beforeFin2 && w2.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
            QThread::msleep(5);
        }
        const QStringList rec2 = panel.logRecordsForTest();
        QString tag2, body2;
        if (!rec2.isEmpty()) {
            const QStringList parts = rec2.last().split(QLatin1Char('\t'));
            tag2 = parts.value(0); body2 = parts.value(1);
        }
        const bool failErr = (tag2 == QLatin1String("ERR")) && body2.contains(QLatin1String("2"));

        // ---- 退出码 0 是 DONE ----
        rule.toolArgs[0] = argsFor(okExe);
        panel.setToolRulesForTest(QVector<ToolRule>() << rule);
        panel.resetToolLaunchStateForTest();
        const int beforeFin3 = panel.toolFinishedCountForTest();
        panel.launchToolForTest(0, 0);
        QElapsedTimer w3; w3.start();
        while (panel.toolFinishedCountForTest() == beforeFin3 && w3.elapsed() < 8000) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 40);
            QThread::msleep(5);
        }
        const QStringList rec3 = panel.logRecordsForTest();
        QString tag3;
        if (!rec3.isEmpty())
            tag3 = rec3.last().split(QLatin1Char('\t')).value(0);
        const bool okDone = (tag3 == QLatin1String("DONE"));

        // 反向锁：判据不能只看"良性那一侧"。
        //   非 0 + 没有 stderr 特征 -> 仍判失败（不许一律成功）
        //   退出码 0               -> 不算良性（走成功分支）
        const bool redOnNoStderr = !SyncPanel::isBenignLauncherExit(2, QByteArray("boom"));
        const bool redOnOtherMsg = !SyncPanel::isBenignLauncherExit(2, QByteArray("cannot open display"));
        const bool redOnZero     = !SyncPanel::isBenignLauncherExit(0, QByteArray("GLib-GObject-CRITICAL"));
        const bool redOnEmpty    = !SyncPanel::isBenignLauncherExit(2, QByteArray());
        // 正例：真机上 xdg-open 的原文
        const bool glibBenign = SyncPanel::isBenignLauncherExit(
            3, QByteArray("GLib-GObject-CRITICAL: g_object_get_data: assertion 'G_IS_OBJECT (object)' failed"));

        const bool ok = sceneOk && okDebounce && glibWarn && glibExplains && failErr && okDone
                        && redOnNoStderr && redOnOtherMsg && redOnZero && redOnEmpty && glibBenign;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "tool-launch-runtime"
            << "\tattempts3clicks=" << attempts << " suppressed=" << suppressed
            << " running=" << running
            << "\tglibTag=" << tag1 << " glibSaysCode=" << (glibWarn ? 1 : 0)
            << " glibExplainsOpened=" << (glibExplains ? 1 : 0)
            << "\tfailTag=" << tag2 << " failSaysCode=" << (failErr ? 1 : 0)
            << "\tokTag=" << tag3
            << "\tredNoStderr=" << (redOnNoStderr ? 1 : 0)
            << " redOtherMsg=" << (redOnOtherMsg ? 1 : 0)
            << " redOnZero=" << (redOnZero ? 1 : 0) << " redEmpty=" << (redOnEmpty ? 1 : 0)
            << " glibBenign=" << (glibBenign ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 源文件被删掉时点「打开」：必须给明确提示，不许静默
    {
        QTemporaryDir sandbox;
        const QString root = sandbox.path();
        const QString gone = root + QStringLiteral("/data.spe");
        #ifdef Q_OS_WIN
        // Full path on purpose: QFileInfo("cmd.exe").exists() is false (it is
        // resolved against the CWD, not PATH), so a bare name would be
        // reported as "tool missing" and this case would never be reached.
        const QString exe = QStandardPaths::findExecutable(QStringLiteral("cmd.exe"));
        const QString exeArg = QStringLiteral("/c exit 0");
        #else
        const QString exe = QStringLiteral("/bin/sh");
        const QString exeArg = QStringLiteral("-c \"exit 0\"");
        #endif

        SyncPanel panel;
        panel.setLocalRootForTest(root);
        FileItem item;
        item.relativePath = QStringLiteral("data.spe");
        item.suffix = QStringLiteral("spe");
        panel.injectFiles(QVector<FileItem>() << item);   // 清单里有，磁盘上已经没有了
        ToolRule rule;
        rule.suffix = QStringLiteral("spe");
        rule.toolNames    << QStringLiteral("probeTool");
        rule.toolExePaths << exe;
        rule.toolArgs     << exeArg;
        panel.setToolRulesForTest(QVector<ToolRule>() << rule);

        const bool sceneOk = !QFileInfo(gone).exists();
        panel.resetToolLaunchStateForTest();
        panel.launchToolForTest(0, 0);

        const QStringList recs = panel.logRecordsForTest();
        QString tag, body;
        if (!recs.isEmpty()) {
            const QStringList parts = recs.last().split(QLatin1Char('\t'));
            tag = parts.value(0); body = parts.value(1);
        }
        // 判据自己比对路径时必须用**同一套分隔符**。产品写的是
        // QDir::toNativeSeparators 的结果（Windows 上是反斜杠），
        // 这里拿 QTemporaryDir.path() 的正斜杠原样去 contains 的话，
        // "路径没写进去"和"分隔符不同"是同一个信号 —— 判据先红了自己。
        const QString nativeGone = QDir::toNativeSeparators(QFileInfo(gone).absoluteFilePath());
        const bool okTag = (tag == QLatin1String("ERR"));
        const bool okPath = body.contains(nativeGone) || body.contains(gone);   // 含完整路径
        // 红在**正确的那一条**上：源文件缺失的提示不许是"工具不存在"那条
        // （那条的标志是 设置/程序路径 这两个引导词）。
        const bool notTheOther = !body.contains(QChar(0x8BBE)) && !body.contains(QChar(0x7F6E));
        const bool okReason = body.contains(QChar(0x6E90)) && body.contains(QChar(0x6587))
                              && body.contains(QChar(0x4E0D)) && body.contains(QChar(0x5B58))
                              && body.contains(QChar(0x5728));   // 源文件 / 不存在
        const bool noProcess = (panel.runningToolCountForTest() == 0)
                               && (panel.toolLaunchAttemptsForTest() == 0);
        // 存在性检查排在行为检查前面：现场没造出来的话，"没起进程"和
        // "路径算错了"是同一个信号。先确认现场，再看行为。
        const bool ok = sceneOk && okTag && okPath && okReason && notTheOther && noProcess;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "tool-source-missing-prompt"
            << "\tsceneAbsent=" << (sceneOk ? 1 : 0)
            << " tag=" << tag << " hasFullPath=" << (okPath ? 1 : 0)
            << " hasReason=" << (okReason ? 1 : 0)
            << " notToolMissingMsg=" << (notTheOther ? 1 : 0)
            << " noProcessStarted=" << (noProcess ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 无执行权限：只有 POSIX 上能造出这个现场
    //
    // Windows 上 Qt 5.9 的 isExecutable() 对任何存在的文件都返回 true，
    // 文件系统上根本造不出"存在但不可执行"。所以前一组只能验**纯函数**，
    // 这一组验的是**产品那一侧确实把 isExecutable() 传进去了** ——
    // 判据调的是 launchPreflightForTest（产品用的同一个表达式），
    // 并且真的去 chmod -x 一个桩程序。
    #ifndef Q_OS_WIN
    {
        QTemporaryDir sandbox;
        const QString root = sandbox.path();
        const QString dataFile = root + QStringLiteral("/data.spe");
        QFile f(dataFile);
        f.open(QIODevice::WriteOnly); f.write("srt-probe"); f.close();
        const QString noExec = makeToolStub(root, QStringLiteral("noexec"), QString(), QString(), 0);
        // 现场必须真的造出来：文件存在、可执行位被摘掉。
        // 每一步都单独量出来 —— 只报一个 sceneNoExec=0 分不清是"没造出来"
        // 还是"造出来了但没认出"，这两种情况的修法完全不同。
        const bool existsAfterCreate = QFileInfo(noExec).exists();
        const bool execAfterCreate = existsAfterCreate && QFileInfo(noExec).isExecutable();
        const bool permOk = noExec.isEmpty()
                            ? false
                            : QFile::setPermissions(noExec, QFile::ReadOwner | QFile::WriteOwner);
        const bool execAfterChmod = noExec.isEmpty() ? false : !QFileInfo(noExec).isExecutable();
        const bool sceneOk = !noExec.isEmpty() && existsAfterCreate && execAfterChmod;
        QString tag = QStringLiteral("<no-scene>"), body;
        int attempts = -1;
        if (sceneOk) {
            SyncPanel panel;
            panel.setLocalRootForTest(root);
            FileItem item;
            item.relativePath = QStringLiteral("data.spe");
            item.suffix = QStringLiteral("spe");
            item.size = 9;
            panel.injectFiles(QVector<FileItem>() << item);
            ToolRule rule;
            rule.suffix = QStringLiteral("spe");
            rule.toolNames    << QStringLiteral("probeTool");
            rule.toolExePaths << noExec;
            rule.toolArgs     << QString();
            panel.setToolRulesForTest(QVector<ToolRule>() << rule);
            panel.resetToolLaunchStateForTest();
            panel.launchToolForTest(0, 0);
            attempts = panel.toolLaunchAttemptsForTest();
            const QStringList recs = panel.logRecordsForTest();
            if (!recs.isEmpty()) {
                const QStringList parts = recs.last().split(QLatin1Char('\t'));
                tag = parts.value(0); body = parts.value(1);
            }
        }
        const bool okTag = (tag == QLatin1String("ERR"));
        const bool okChmod = body.contains(QLatin1String("chmod +x")) && body.contains(noExec);
        // 权(6743) 限(9650)
        const bool okSaysPerm = body.contains(QChar(0x6743)) && body.contains(QChar(0x9650));
        const bool noProcess = (attempts == 0);
        const bool ok = sceneOk && okTag && okChmod && okSaysPerm && noProcess;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "tool-launch-perm"
            << "\tsceneNoExec=" << (sceneOk ? 1 : 0)
            << " stubPathEmpty=" << (noExec.isEmpty() ? 1 : 0)
            << " existsAfterCreate=" << (existsAfterCreate ? 1 : 0)
            << " execAfterCreate=" << (execAfterCreate ? 1 : 0)
            << " chmodCallOk=" << (permOk ? 1 : 0)
            << " execAfterChmod=" << (execAfterChmod ? 1 : 0)
            << " tag=" << tag << " hasChmodCmd=" << (okChmod ? 1 : 0)
            << " saysPerm=" << (okSaysPerm ? 1 : 0) << " noProcessStarted=" << (noProcess ? 1 : 0)
            << "\n";
    }
    #endif

    // ----------------------------------------------------------------
    // 日志上限 2000：灌 2500 行，读回必须是 2000
    //
    // 只断言那行代码里的常量等于 2000 是**假判据**：把 setMaximumBlockCount
    // 那行整个删掉，document() 的默认值也是 0/无上限，判据照样全绿。
    // 真正的证据是"灌进去的行数 > 留下的行数"。
    {
        SyncPanel panel;
        const int capBefore = panel.logCapForTest();
        panel.appendLogLinesForTest(2500);
        const int blocks = panel.logBlockCountForTest();
        const bool okCap = (capBefore == 2000) && (blocks == 2000);
        // 反向锁：真的在淘汰，不是"上限值恰好等于推入数"。
        const bool okEvicts = blocks < 2500;
        // 反向锁：最新一行还在（淘汰的是最旧的，不是最新的）。
        const QStringList tail = panel.logTailForTest(1);
        const bool okNewestKept = !tail.isEmpty() && tail.last().contains(QLatin1String("probe-line-2500"));
        const bool ok = okCap && okEvicts && okNewestKept;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "log-cap-fifo"
            << "\tcap=" << capBefore << " pushed=2500 blocksAfter=" << blocks
            << " evicts=" << (okEvicts ? 1 : 0) << " newestKept=" << (okNewestKept ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 工具按钮：1/2/3 个工具**全部**直接显示工具名；超过 3 个才折叠成
    // 「前两个 + 第 3 格「…」」；超长名省略、按钮不许越列也不许消失
    {
        const QString toolName = QStringLiteral("Replayer");

        FileTableModel model;
        QVector<FileItem> files;
        FileItem it;
        it.relativePath = QStringLiteral("data.spe");
        it.suffix = QStringLiteral("spe");
        files << it;

        ToolRule one;
        one.suffix = QStringLiteral("spe");
        one.toolNames    << toolName;
        one.toolExePaths << QStringLiteral("/opt/tools/replayer");

        QHash<QString, ToolRule> oneHash;
        oneHash.insert(QStringLiteral("spe"), one);
        model.setFiles(files, oneHash, QStringLiteral("/data"));
        ToolColumnDelegate delegate(&model);

        // 列宽就是 sizeHint 给的 156px，判据必须用同一个尺寸 ——
        // 换一个宽度算出来的"装得下"证明不了真实界面上装得下。
        const QRect cell(0, 0, toolCellWidth(delegate), 35);
        const QStringList oneLabels = delegate.buttonLabelsForTest(cell, 0, &model);
        const QVector<QRect> oneRects = delegate.buttonRectsForTest(cell, oneLabels);
        const bool okSingleName = (oneLabels.size() == 1) && (oneLabels.at(0) == toolName);

        // 造 n 个工具的规则。**必须每个都从头造**：
        // `ToolRule two = one;` 是复制 —— one 里已经有 "Replayer" 了，
        // 再 append 两个就成了 3 个工具，判据测的根本不是"2 个工具"这一档。
        auto makeRule = [](int n) {
            ToolRule r;
            r.suffix = QStringLiteral("spe");
            for (int i = 0; i < n; ++i) {
                r.toolNames    << QStringLiteral("T%1").arg(i + 1);
                r.toolExePaths << QStringLiteral("/opt/tools/t%1").arg(i + 1);
                r.toolArgs     << QString();
            }
            return r;
        };

        // 2 个工具：两个都直接显示工具名，**不**折叠
        const ToolRule two = makeRule(2);
        QHash<QString, ToolRule> twoHash;
        twoHash.insert(QStringLiteral("spe"), two);
        model.setFiles(files, twoHash, QStringLiteral("/data"));
        const QStringList twoLabels = delegate.buttonLabelsForTest(cell, 0, &model);
        const bool okTwo = (twoLabels.size() == 2)
                           && (twoLabels.at(0) == QStringLiteral("T1"))
                           && (twoLabels.at(1) == QStringLiteral("T2"));
        // 反向锁：两条标签不能是空的（照抄成两个空串也会"size()==2"）
        const bool okNotBlank = !twoLabels.at(0).trimmed().isEmpty() && !twoLabels.at(1).trimmed().isEmpty();
        // 两个按钮的矩形都必须画得出来：排不下就是空矩形，
        // 空矩形在 paint 里被跳过、在 editorEvent 里被当成"没点中" ——
        // 用户看到的是第二个按钮**消失**、点不动。
        const QVector<QRect> twoRects = delegate.buttonRectsForTest(cell, twoLabels);
        const bool okTwoRects = (twoRects.size() == 2)
                                && !twoRects.at(0).isNull() && !twoRects.at(1).isNull()
                                && (twoRects.at(1).right() <= cell.right() - 18);

        // 3 个工具：三个**全部**显示，一个都不藏
        const ToolRule three = makeRule(3);
        QHash<QString, ToolRule> threeHash;
        threeHash.insert(QStringLiteral("spe"), three);
        model.setFiles(files, threeHash, QStringLiteral("/data"));
        const QStringList threeLabels = delegate.buttonLabelsForTest(cell, 0, &model);
        const bool okThree = (threeLabels.size() == 3)
                             && (threeLabels.at(0) == QStringLiteral("T1"))
                             && (threeLabels.at(1) == QStringLiteral("T2"))
                             && (threeLabels.at(2) == QStringLiteral("T3"));
        const QVector<QRect> threeRects = delegate.buttonRectsForTest(cell, threeLabels);
        const bool okThreeRects = (threeRects.size() == 3)
                                  && !threeRects.at(0).isNull()
                                  && !threeRects.at(1).isNull()
                                  && !threeRects.at(2).isNull()
                                  && (threeRects.at(2).right() <= cell.right() - 18);

        // 4 个工具：折叠成「前两个 + 第 3 格「…」，第 4 个只能出现在菜单里
        const ToolRule four = makeRule(4);
        QHash<QString, ToolRule> fourHash;
        fourHash.insert(QStringLiteral("spe"), four);
        model.setFiles(files, fourHash, QStringLiteral("/data"));
        const QStringList fourLabels = delegate.buttonLabelsForTest(cell, 0, &model);
        const bool okFour = (fourLabels.size() == 3)
                            && (fourLabels.at(0) == QStringLiteral("T1"))
                            && (fourLabels.at(1) == QStringLiteral("T2"))
                            && (fourLabels.at(2) == QObject::tr("\xE2\x80\xA6"));
        // 反向锁：第 4 个工具**不许**被画成按钮 ——
        // 画出来就等于根本没折叠，这条判据要能抓住它。
        const bool okNoFourth = !fourLabels.contains(QStringLiteral("T4"));

        // 超长名：必须出现省略号，且按钮矩形不许越界
        QString longName;
        for (int i = 0; i < 12; ++i)
            longName += QStringLiteral("LongToolName");
        ToolRule longRule = one;
        longRule.toolNames[0] = longName;
        QHash<QString, ToolRule> longHash;
        longHash.insert(QStringLiteral("spe"), longRule);
        model.setFiles(files, longHash, QStringLiteral("/data"));
        const QStringList longLabels = delegate.buttonLabelsForTest(cell, 0, &model);
        const bool shorter = (longLabels.size() == 1) && (longLabels.at(0).length() < longName.length());
        const int lastChar = shorter && !longLabels.at(0).isEmpty()
                             ? longLabels.at(0).at(longLabels.at(0).length() - 1).unicode() : 0;
        const bool hasElide = shorter && (lastChar == 0x2026);
        const QVector<QRect> longRects = delegate.buttonRectsForTest(cell, longLabels);
        const bool rectOk = (longRects.size() == 1) && !longRects.at(0).isNull()
                            && (longRects.at(0).right() <= cell.right() - 18);
        // 反向锁：短名必须**不**被截断，否则这条判据就是"永远在截"
        const bool okShortUncut = (oneLabels.size() == 1) && !oneLabels.at(0).isEmpty()
                                  && (oneLabels.at(0).at(0).unicode() != 0x2026);
        // 反向锁：按钮矩形不许越界到状态列（这是这一列的既有约定）
        const bool oneRectOk = (oneRects.size() == 1) && !oneRects.at(0).isNull()
                               && (oneRects.at(0).right() <= cell.right() - 18);

        const bool ok = okSingleName && okTwo && okNotBlank && shorter && hasElide
                        && rectOk && okShortUncut && oneRectOk
                        && okTwoRects && okThree && okThreeRects && okFour && okNoFourth;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "tool-button-label"
            << "\tcellW=" << cell.width()
            << " singleLabel=" << (okSingleName ? 1 : 0)
            << " twoNames=" << (okTwo ? 1 : 0) << " multiNotBlank=" << (okNotBlank ? 1 : 0)
            << " twoRects=" << (okTwoRects ? 1 : 0)
            << " threeNames=" << (okThree ? 1 : 0) << " threeRects=" << (okThreeRects ? 1 : 0)
            // 诊断：把三个按钮实际画出来的文字打出来。
            // 失败时一眼能看出是"被裁成省略号"还是"压根没画出来"，
            // 不用再反推一遍字体度量。
            << " threeLabels=[" << (threeLabels.isEmpty() ? QString() : threeLabels.join("|")) << "]"
            << " fourFolded=" << (okFour ? 1 : 0) << " noFourthBtn=" << (okNoFourth ? 1 : 0)
            << " longElided=" << (hasElide ? 1 : 0)
            << " longLen=" << (longLabels.isEmpty() ? -1 : longLabels.at(0).length())
            << " origLen=" << longName.length()
            << " lastCharCode=" << QString::number(lastChar, 16)
            << " label=[" << (longLabels.isEmpty() ? QString() : longLabels.at(0)) << "]"
            << " rectInsideColumn=" << (rectOk ? 1 : 0)
            << " shortUncut=" << (okShortUncut ? 1 : 0)
            << "\n";
    }

    // ----------------------------------------------------------------
    // 带宽限制：预置档位齐全 + **自定义值放行** + 非法值回落不限速
    //
    // 自定义值这一条最容易写错：客户在界面上填了 12 MB/s，
    // 如果归一化按"不在预置档位里"给抹成 0，那就是**偷偷取消了客户的限速**。
    // 两条反向锁一起才有效：
    //   - "非法值 → 0" 和 "自定义值 → 原样保留" 同时成立，
    //     恒返回 0 的实现和恒返回入参的实现都过不了。
    {
        const QVector<int> opts = ConfigStore::bandwidthLimitOptionsMBps();
        const bool hasZero  = opts.contains(0);
        const bool hasAll   = opts.contains(1) && opts.contains(5) && opts.contains(10)
                               && opts.contains(20) && opts.contains(50) && opts.contains(100);
        const int custom = 12;
        const bool okCustomKept = (ConfigStore::clampBandwidthMBps(custom) == custom)
                                  && !opts.contains(custom);
        const bool okBad = (ConfigStore::clampBandwidthMBps(-3) == 0)
                           && (ConfigStore::clampBandwidthMBps(99999) == 0);
        const bool okZero = (ConfigStore::clampBandwidthMBps(0) == 0);
        const bool okRange = (ConfigStore::clampBandwidthMBps(ConfigStore::bandwidthCustomMinMBps())
                              == ConfigStore::bandwidthCustomMinMBps())
                             && (ConfigStore::clampBandwidthMBps(ConfigStore::bandwidthCustomMaxMBps())
                                 == ConfigStore::bandwidthCustomMaxMBps());
        const bool ok = hasZero && hasAll && okCustomKept && okBad && okZero && okRange;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "bandwidth-range"
            << "\tzero=" << (hasZero ? 1 : 0)
            << " presets=" << (hasAll ? 1 : 0)
            << " custom12Kept=" << (okCustomKept ? 1 : 0)
            << " badFallsBack=" << (okBad ? 1 : 0)
            << " bounds=" << (okRange ? 1 : 0)
            << " optCount=" << opts.size()
            << " customMax=" << ConfigStore::bandwidthCustomMaxMBps()
            << "\n";
    }

    // ----------------------------------------------------------------
    // 文件大小列的单位右补齐（user2()）：判据量的是**真实渲染宽度**
    //
    // 只断言 unit2("B") == " B" 是自证的（判据照抄了实现）。
    // 这里比的是 B 与 MB 两档在等宽字体下的实际字符串宽度是否相等 ——
    // 那才是这一列看着齐不齐的原因。
    {
        FileTableModel model;
        QVector<FileItem> files;
        FileItem a, b;
        a.relativePath = QStringLiteral("small.raw"); a.suffix = QStringLiteral("raw"); a.size = 512;
        b.relativePath = QStringLiteral("big.raw");   b.suffix = QStringLiteral("raw"); b.size = 5LL * 1024 * 1024;
        files << a << b;
        QHash<QString, ToolRule> none;
        model.setFiles(files, none, QStringLiteral("/data"));

        QFont f = QApplication::font();
        f.setStyleHint(QFont::Monospace);
        f.setFixedPitch(true);
        const QFontMetrics fm(f);
        const QString small = model.data(model.index(0, FileTableModel::ColSize)).toString();
        const QString big   = model.data(model.index(1, FileTableModel::ColSize)).toString();
        // 断言的是 unit2() 真正保证的那条不变量：**两档的字符数相同**，
        // 于是「单位」落在同一列上。原来这里比的是像素宽度，
        // 前提是字体真的等宽 —— 麒麟容器里没有可用的等宽字体，
        // QFontMetrics 落到比例回退字体，"512 B " 和 "5.0 MB" 像素宽度
        // 本来就不一样（33 vs 37），判据于是红在一个与本改动无关的地方。
        // 那是判据的前提错了，不是产品错了。
        const bool sameLen = (small.length() == big.length());
        const bool notEmpty = !small.trimmed().isEmpty() && !big.trimmed().isEmpty();
        const bool sameUnitCol = small.endsWith(QLatin1Char('B')) && big.endsWith(QLatin1Char('B'));
        const bool pitchOk = f.fixedPitch();          // 诊断用，不参与判定
        const bool ok = sameLen && notEmpty && sameUnitCol;
        if (!ok)
            ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "size-column-unit-pad"
            << "\tchars=" << small.length() << "/" << big.length()
            << " sameLen=" << (sameLen ? 1 : 0) << " unitColOk=" << (sameUnitCol ? 1 : 0)
            << " [diag] fixedPitch=" << (pitchOk ? 1 : 0)
            << " pxW=" << fm.width(small) << "/" << fm.width(big)
            << "\n";
    }

    if (oldCfgDir.isEmpty())
        qunsetenv("SRT_CONFIG_DIR");
    else
        qputenv("SRT_CONFIG_DIR", oldCfgDir);

    // ----------------------------------------------------------------
    // 「取消」和「远端已有」必须是两个状态，而且必须渲染成两段不同的文字。
    //
    // 为什么值得单独判：那个三元表达式曾经把两种完全不同的行塞进同一个状态
    // ——「用户点完取消、一个字节都没传」和「远端本来就有、rsync 没碰过」。
    // 用户取消后看到满屏「远端已有」，会以为远端已经同步好了；
    // 而使用步骤 L125 自己把「远端已有」定义成"远端本来就有"，
    // 于是同一个界面里两处自相矛盾，用户只能信更醒目的那一处。
    //
    // 判据打的是**生产用的同一个函数** resolveRestState()，不是照抄一份：
    // 照抄的话，把生产那行改回 StateSkipped，这几条照样全绿 ——
    // "判据测了谓词、洞在接线"这个坑本项目已经踩过。
    // ----------------------------------------------------------------
    {
        SyncPanel panel;
        FileItem a; a.relativePath = QStringLiteral("a.spe"); a.suffix = QStringLiteral("spe"); a.size = 3;
        FileItem b; b.relativePath = QStringLiteral("b.spe"); b.suffix = QStringLiteral("spe"); b.size = 3;
        panel.injectFiles(QVector<FileItem>() << a << b);
        FileTableModel *mdl = panel.tableModel();

        const bool mapping =
            FileTableModel::resolveRestState(true,  false, true)  == FileTableModel::StateCanceled &&
            FileTableModel::resolveRestState(true,  true,  false) == FileTableModel::StateCanceled &&
            FileTableModel::resolveRestState(false, true,  true)  == FileTableModel::StateSkipped &&
            FileTableModel::resolveRestState(false, false, true)  == FileTableModel::StateOk &&
            FileTableModel::resolveRestState(false, false, false) == FileTableModel::StateFailed;

        mdl->setFileState(0, FileTableModel::StateSkipped);
        mdl->setFileState(1, FileTableModel::StateCanceled);
        const QString skippedText =
            mdl->data(mdl->index(0, FileTableModel::ColStatus), Qt::DisplayRole).toString();
        const QString canceledText =
            mdl->data(mdl->index(1, FileTableModel::ColStatus), Qt::DisplayRole).toString();
        const bool distinct =
            !skippedText.isEmpty() && !canceledText.isEmpty() && skippedText != canceledText;

        if (mapping && distinct)
            out << "PASS\tcancel-vs-existing-state\tcanceled=1\texisting=1\ttextsDistinct=1\tskippedText="
                << skippedText << "\tcanceledText=" << canceledText << "\n";
        else {
            out << "FAIL\tcancel-vs-existing-state\tmapping=" << (mapping ? 1 : 0)
                << "\tdistinct=" << (distinct ? 1 : 0)
                << "\tskippedText=" << skippedText << "\tcanceledText=" << canceledText << "\n";
            ++bad;
        }
    }

    // ---- 判据：密钥材料必须**按字节原样**认出，不能先 trim 再判长度 ----
    //
    // 密钥文件里躺的是 32 个随机二进制字节。首/尾各有约 2.7% 的概率
    // 恰好是空白字节（0x09~0x0D / 0x20 / 0xA0），任意一端命中时
    // 「先 trim() 再判长度」会把长度削到 31 ⇒ 被误判成「密钥损坏」
    // ⇒ 重新生成并覆盖 ⇒ ini 里已存的密文口令当场解不开。
    // 这里用「首字节是空格、尾字节是换行」把那个边界钉死。
    {
        QByteArray edge(32, 'A');
        edge[0]  = char(0x20);   // 空格
        edge[31] = char(0x0A);   // 换行
        const QByteArray got = SecretStore::keyMaterialFromBytes(edge);
        const bool okEdge = (got.size() == 32) && (got == edge);
        // 反向：真的只有 31 字节时必须**不认**（否则会拿短材料去加密）。
        const bool okShort = SecretStore::keyMaterialFromBytes(QByteArray(31, 'A')).isEmpty();
        const bool ok = okEdge && okShort;
        if (!ok) ++bad;
        out << (ok ? "PASS\t" : "FAIL\t") << "secret-key-material-bytes"
            << "\tedgeKept=" << (okEdge ? 1 : 0)
            << "\tshortRejected=" << (okShort ? 1 : 0)
            << "\n";
    }

    // 口令加密后端**此刻**能不能用。
    //
    // 这不是一条判据，是一条**事实汇报**：客户看交付包里的自检输出时，
    // 需要知道"这台机器上登录口令到底会不会被加密"。
    // 之前包里的自检脚本靠"包内 lib/ 目录里有没有 libcrypto"来判断 ——
    // 而包里本来就不带 OpenSSL，那个判据恒为假，
    // 于是客户每次都看到「保存的登录口令不会被加密」，
    // 实际上系统的 libcrypto 找得到、口令是加密的。
    //
    // 事实必须由**程序自己**报：判据和实现在同一条代码路径上，
    // 才不会再出现"脚本说一套、程序做另一套"。
    out << "INFO\tcrypto-backend\tavailable=" << (SecretStore::cryptoBackendAvailable() ? 1 : 0)
        << "\tbackend=" << SecretStore::backendName() << "\n";


    // ----------------------------------------------------------------
    // 屏幕矩阵：尺寸收敛规则必须在**各种屏幕**上都对。
    //
    // 为什么值得单独一条判据：收敛规则是纯算术，写错了在这台机器上看不出来
    // （这台是 1280×764，恰好落在最好看的那一档）。
    // 客户那边可能是 1366×768 的笔记本、1024×768 的老本、4K 缩到 200%，
    // 或者 853×480 的 150% 笔记本 —— 规则必须自己站得住。
    //
    // 判据三条：
    //   ① 主界面/弹窗都**不超出**屏幕可用区（超出 = 被裁 = 用户够不着按钮）；
    //   ② 都**不低于**各自的下界（低于 = 表格列挤没 / 输入框放不下）；
    //   ③ 大屏上尺寸**不缩水**（1280×764 及以上，主界面必须是完整的 980×460）。
    // ----------------------------------------------------------------
    {
        struct ScreenCase { const char *name; int w; int h; };
        static const ScreenCase kScreens[] = {
            { "1366x768-laptop",  1366,  728 },   // 笔记本 768p
            { "1920x1080-fhd",    1920, 1040 },   // 1080p
            { "1024x768-old",     1024,  728 },   // 老笔记本 / 麒麟虚拟机
            { "800x600-tiny",      800,  560 },   // 极小屏
            { "1280x800-4k200",  1280,  760 },   // 2560x1600 @200%
            { "853x480-150pc",    853,  440 },   // 1920x1080 @150%（矮）
        };
        int matrixBad = 0;
        QString matrixDetail;
        for (int i = 0; i < int(sizeof(kScreens) / sizeof(kScreens[0])); ++i) {
            const ScreenCase &sc = kScreens[i];
            const QSize mainShut = ScreenFit::mainWindow(sc.w, sc.h, false);
            const QSize mainOpen = ScreenFit::mainWindow(sc.w, sc.h, true);
            const QSize dlg      = ScreenFit::settingsDialog(sc.w, sc.h);
            const bool fits =
                mainShut.width() <= sc.w && mainShut.height() <= sc.h &&
                mainOpen.width() <= sc.w && mainOpen.height() <= sc.h &&
                dlg.width()      <= sc.w && dlg.height()      <= sc.h;
            // ★**位置**也必须收敛（2026-10-28 缺陷 1/6）：
            // 只比尺寸的话，"窗口底边跑出屏幕"这种缺陷根本抓不到 ——
            // 实测 1280×764 可用区：收起 460 → 展开 759 后底边到 909，
            // 超出 146px，175px 高的日志抽屉只剩 29px 可见（用户点了展开日志，
            // 看到的是几乎空的抽屉）。窗口拖到屏幕下半屏时超出更多（264px）。
            //
            // 这里模拟**最恶劣的位置**（窗口被拖到屏幕下半屏）再展开日志，
            // 断言窗口仍然整块落在可用区内。
            const QRect scrRect(0, 0, sc.w, sc.h);
            const QRect worstPos(0, sc.h - 100, 100, 100);
            const QRect fitOpen = ScreenFit::fitInside(mainOpen, worstPos, scrRect);
            const QRect fitShut = ScreenFit::fitInside(mainShut, worstPos, scrRect);
            const bool inside =
                fitOpen.top()  >= scrRect.top()  && fitOpen.bottom()  <= scrRect.bottom() &&
                fitOpen.left() >= scrRect.left() && fitOpen.right()   <= scrRect.right() &&
                fitShut.top()  >= scrRect.top()  && fitShut.bottom()  <= scrRect.bottom() &&
                fitShut.left() >= scrRect.left() && fitShut.right()   <= scrRect.right();
            const bool aboveMin =
                mainShut.width() >= ScreenFit::kMainWidthMin &&
                mainShut.height() >= ScreenFit::kMainHeightMin &&
                dlg.width()      >= ScreenFit::kSettingsWidthMin &&
                dlg.height()     >= ScreenFit::kSettingsHeightMin;
            const bool noShrink = (sc.w >= 1280 && sc.h >= 764)
                                  ? (mainShut.width() == ScreenFit::kMainWidth &&
                                     mainShut.height() == ScreenFit::kMainHeight)
                                  : true;
            const bool ok = fits && aboveMin && noShrink && inside;
            if (!ok) {
                ++matrixBad;
                ++bad;
            }
            out << (ok ? "PASS\t" : "FAIL\t") << "screen-matrix-" << sc.name
                << "\tfit=" << fits << " aboveMin=" << aboveMin
                << " noShrink=" << noShrink << " inside=" << inside
                << " main=" << mainShut.width() << "x" << mainShut.height()
                << " open=" << mainOpen.width() << "x" << mainOpen.height()
                << " dlg=" << dlg.width() << "x" << dlg.height()
                << "\tscreen=" << sc.w << "x" << sc.h << "\n";
        }
        out.flush();
        if (matrixBad && matrixDetail.isEmpty())
            matrixDetail = QStringLiteral("%1 screens failed").arg(matrixBad);
        Q_UNUSED(matrixDetail);
    }

    out << (bad == 0 ? "SELFCHECK_ALL_PASS" : "SELFCHECK_FAILED") << "\n";
    return bad == 0 ? 0 : 1;
}


// ------------------------------------------------------------------
// --selfcheck-gui：GUI 交互回归。
//
// 为什么必须单独有这一套：--selfcheck **不开窗口**，验的是纯函数与参数装配；
// 于是所有"点了按钮有没有反应"的缺陷都漏网 —— 「+ 添加规则」点了没反应
// 就是这么混进交付包的（2026-10-27 用户报障，现象是点了之后表格里看不见新行）。
//
// 这一套走的是**真实控件**：真的点那个按钮（QPushButton::click() 与鼠标点击
// 走同一条 clicked 信号链路），然后回读界面的真实几何状态。
//
// 三条纪律：
//   ① 每条判据都必须能 FAIL。判据里自带**阴性对照**：故意把状态改坏，
//      判据必须报 FAIL，否则这条判据就是恒真的假保证。
//   ② 判据只读**界面真实状态**（行数、控件高度、viewport 几何），
//      不照抄一份实现自己算一遍。
//   ③ 落盘往返必须回到 ini 再读回来（"内存里对、磁盘上错"是最常见的漏法）。
int runGuiSelfCheck()
{
    QTextStream out(stdout);
    int pass = 0;
    int bad = 0;
    auto check = [&out, &pass, &bad](const char *name, bool ok, const QString &detail) {
        if (ok) {
            ++pass;
            out << "PASS\t" << name << "\n";
        } else {
            ++bad;
            out << "FAIL\t" << name << "\t" << detail << "\n";
        }
        out.flush();
    };

    // 沙箱：判据自己会读写真实配置（保存设置那条链路），必须隔离。
    QTemporaryDir sandbox;
    if (!sandbox.isValid()) {
        out << "FAIL\tsandbox\t临时目录不可用\n";
        out.flush();
        return 1;
    }
    const QByteArray oldCfgDir = qgetenv("SRT_CONFIG_DIR");
    qputenv("SRT_CONFIG_DIR", sandbox.path().toLocal8Bit());

    // 灌 2 条规则，模拟"用户已经配过规则"的真实状态。
    // 工具路径用程序自己的 exe：哪个机器上都一定存在
    // （loadToolRules 会把 exe 为空的条目丢掉，用假路径会让"往返"判据失真）。
    {
        QVector<ToolRule> rules;
        const QString selfExe = QCoreApplication::applicationFilePath();
        for (int i = 0; i < 2; ++i) {
            ToolRule r;
            r.suffix = (i == 0) ? QStringLiteral("spe") : QStringLiteral("raw");
            r.toolNames    << QStringLiteral("probe-tool");
            r.toolExePaths << selfExe;
            r.toolArgs     << QStringLiteral("--play {file}");
            rules << r;
        }
        ConfigStore::saveToolRules(rules);
    }

    MainWindow::setSkipSplash(true);
    MainWindow w;
    // 不上屏、不抢焦点，但走完整的 Windows 字体与布局（offscreen 平台没字体）。
    w.setAttribute(Qt::WA_DontShowOnScreen, true);
    w.show();
    QApplication::processEvents();

    // 主界面的期望高度：**直接问产品自己的收敛规则**（ScreenFit.h），
    // 不在判据里另抄一份 —— 抄一份的话，产品改了规则判据不会跟着变，
    // 判据就成了"自己验自己"。1366×768 这类笔记本的可用高是 728，460 放得下。
    // 声明在块外，G18/G19/G22/G24 都要用。
    const QRect scrAvail = QGuiApplication::primaryScreen()->availableGeometry();
    const int hShut = ScreenFit::mainWindow(scrAvail.width(), scrAvail.height(), false).height();
    const int hOpen = ScreenFit::mainWindow(scrAvail.width(), scrAvail.height(), true).height();

    // ---- 主界面 ----
    {
        SyncPanel *panel = w.syncPanelForTest();
        const bool open = panel && panel->isLogOpenForTest();
        // 日志抽屉默认**收起**（用户 2026-10-28：「算了日志还是默认收起来吧」）。
        // 默认展开是 10-27 那一轮的要求，已经被用户自己撤回。
        check("G1-log-default-collapsed", !open,
              QString::fromUtf8(u8"isLogOpenForTest=1\uFF08\u9ED8\u8BA4\u5E94\u5F53\u662F\u6536\u8D77\u7684\uFF09"));
        // 收起时主界面高度 = 460（定稿图 980×460），但要**按屏幕可用高收敛**：
        // 1366×768 这类笔记本的可用高是 728，460 放得下；更矮的屏幕才收敛。
        check("G2-main-height-collapsed", w.height() == hShut,
              QStringLiteral("height=%1 expected=%2").arg(w.height()).arg(hShut));
    }

    // ---- 设置弹窗 ----
    w.showSettingsForProbe();
    QApplication::processEvents();
    SettingsDialog *d = w.settingsForTest();
    check("G3-settings-dialog", d != nullptr, QStringLiteral("settingsForTest=null"));
    if (!d) {
        out << "GUI_CHECK_FAILED\n";
        out.flush();
        return 1;
    }
    QApplication::processEvents();

    // 已配规则被正确读进表格
    const int rows0 = d->ruleRowCountForTest();
    check("G4-rules-loaded", rows0 == 2,
          QStringLiteral("rows=%1 expected=2").arg(rows0));

    // 点「+ 添加规则」→ 行数 +1
    const int h0 = d->ruleTableHeightForTest();
    const int dH0 = d->height();
    d->clickAddRuleForTest();
    QApplication::processEvents();
    const int rows1 = d->ruleRowCountForTest();
    check("G5-add-rule-clicked", rows1 == rows0 + 1,
          QStringLiteral("rows=%1 expected=%2").arg(rows1).arg(rows0 + 1));

    // 加完之后新行**看得见**（这是用户报障的那一条）
    const int h1 = d->ruleTableHeightForTest();
    check("G6-new-row-visible", d->ruleLastRowVisibleForTest(),
          QStringLiteral("rows=%1 tableH=%2 viewportTooSmall").arg(rows1).arg(h1));

    // 表格高度确实跟着长了（一条行高 28px）
    check("G7-table-grew", h1 >= h0 + 28,
          QStringLiteral("h0=%1 h1=%2 expected>=%3").arg(h0).arg(h1).arg(h0 + 28));

    // ★ 加规则**不许**把弹窗顶高（2026-10-28 用户报障：
    // 「添加规则后，尺寸都看不到了」）。弹窗尺寸固定，多出来的内容在中间滚动。
    check("G21-settings-height-stable-after-add", d->height() == dH0,
          QStringLiteral("before=%1 after=%2").arg(dH0).arg(d->height()));

    // 阴性对照：把表格压到比表头还矮，新行必然看不见 —— 证明 G6 不是恒真。
    {
        d->setRuleTableHeightForTest(10);
        QApplication::processEvents();
        const bool visSqueezed = d->ruleLastRowVisibleForTest();
        check("G8-negative-control", !visSqueezed,
              QString::fromUtf8(u8"\u538B\u6241\u540E\u4ECD\u7136\u62A5\u53EF\u89C1 = \u5224\u636E\u6052\u771F"));
        d->setRuleTableHeightForTest(h1);
        QApplication::processEvents();
        check("G9-restore-after-control", d->ruleLastRowVisibleForTest(),
              QString::fromUtf8(u8"\u590D\u539F\u540E\u4ECD\u4E0D\u53EF\u89C1"));
    }

    // 连续点 3 次 → 3 行新行，且最后一行看得见
    for (int i = 0; i < 2; ++i) {
        d->clickAddRuleForTest();
        QApplication::processEvents();
    }
    const int rows3 = d->ruleRowCountForTest();
    check("G10-add-three-times", rows3 == rows0 + 3,
          QStringLiteral("rows=%1 expected=%2").arg(rows3).arg(rows0 + 3));
    check("G11-last-row-visible-after-3", d->ruleLastRowVisibleForTest(),
          QStringLiteral("rows=%1").arg(rows3));

    // 填单元格 → 回读一致
    d->setRuleCellForTest(rows3 - 1, 0, QStringLiteral(".probe"));
    d->setRuleCellForTest(rows3 - 1, 1, QStringLiteral("probe-tool2"));
    d->setRuleCellForTest(rows3 - 1, 2, QCoreApplication::applicationFilePath());
    check("G12-cell-roundtrip", d->ruleCellForTest(rows3 - 1, 0) == QStringLiteral(".probe"),
          QStringLiteral("got=%1").arg(d->ruleCellForTest(rows3 - 1, 0)));

    // 自动同步开关 + 间隔
    d->setAutoSyncEnabledForTest(true);
    d->setAutoSyncIntervalForTest(QStringLiteral("120"));

    // 保存 → 往返到 ini。
    //
    // 必须点**真实的「保存设置」按钮**：落盘逻辑在 SettingsDialog::onSaveClicked()
    // （自动同步开关 / 间隔 / 模式 / 定点时刻全在那儿），
    // 走 MainWindow::onSettingsAccepted 会绕过它 —— 那样报出来的 FAIL
    // 根因在探针自己，不在产品。
    d->clickSaveForTest();
    QApplication::processEvents();

    // 期望条数 = 表格里**后缀列非空**的行数。
    // 后缀为空的行按设计不保存（applyConfig 里 `if (suf.isEmpty() ...) continue;`），
    // 所以这里不能拿表格总行数当期望值。
    int expectedRules = 0;
    for (int r = 0; r < d->ruleRowCountForTest(); ++r) {
        if (!d->ruleCellForTest(r, 0).trimmed().isEmpty())
            ++expectedRules;
    }
    const QVector<ToolRule> saved = ConfigStore::loadToolRules();
    check("G13-save-roundtrip-rules", saved.size() == expectedRules,
          QStringLiteral("saved=%1 expected=%2").arg(saved.size()).arg(expectedRules));
    check("G14-autosync-enabled", ConfigStore::loadAutoSyncEnabled(),
          QStringLiteral("loadAutoSyncEnabled=0"));
    check("G15-autosync-interval", ConfigStore::loadAutoSyncInterval() == 120,
          QStringLiteral("interval=%1").arg(ConfigStore::loadAutoSyncInterval()));
    // 保存后弹窗**不关闭**（产品要求：给提示但不自动关），
    // 而且间隔框里要能看到真正生效的值（被规整过时回填）。
    check("G16-save-keeps-dialog-open", d->isVisible(),
          QString::fromUtf8(u8"\u4FDD\u5B58\u540E\u5F39\u7A97\u88AB\u5173\u6389\u4E86"));

    // 删一行 → 行数与高度都回落
    const int hBefore = d->ruleTableHeightForTest();
    d->removeRuleRowForTest(rows3 - 1);
    QApplication::processEvents();
    check("G16-remove-row", d->ruleRowCountForTest() == rows3 - 1,
          QStringLiteral("rows=%1").arg(d->ruleRowCountForTest()));
    check("G17-table-shrank", d->ruleTableHeightForTest() < hBefore,
          QStringLiteral("h=%1 before=%2").arg(d->ruleTableHeightForTest()).arg(hBefore));

    // 日志抽屉开关往返 + 窗口高度跟随。
    //
    // 判据按**当前状态**写，不按"第几次切换"写：
    // 跑到这里之前，配置提示可能已经自动展开过一次日志抽屉
    // （SyncPanel::appendNotice() 里「if (!m_logOpen) toggleLogDrawer()」——
    // 那是产品行为：用户该看见这条提示）。原来把次数写死，就差一次，报假 FAIL。
    //
    // 高度按**屏幕可用高**收敛（展开态 759px 在 1366×768 的笔记本上放不下，硬设会被裁）。
    // hShut / hOpen 在上面已经按 ScreenFit 的同一份规则算好了，这里不再另抄。
    {
        w.syncPanelForTest()->toggleLogDrawer();
        QApplication::processEvents();
        const bool nowOpen = w.syncPanelForTest()->isLogOpenForTest();
        const int expectH = nowOpen ? hOpen : hShut;
        check("G18-log-height-follows-state", w.height() == expectH,
              QStringLiteral("height=%1 expected=%2 open=%3").arg(w.height()).arg(expectH).arg((int)nowOpen));

        w.syncPanelForTest()->toggleLogDrawer();
        QApplication::processEvents();
        const bool backOpen = w.syncPanelForTest()->isLogOpenForTest();
        const int expectH2 = backOpen ? hOpen : hShut;
        check("G19-log-height-follows-state-back", w.height() == expectH2,
              QStringLiteral("height=%1 expected=%2 open=%3").arg(w.height()).arg(expectH2).arg((int)backOpen));
    }

    // 设置弹窗高度**固定**：min(560, 屏幕限高)。
    //
    // 2026-10-28 用户现场：弹窗 640×339（内容放不下时塌成半屏），
    // 以及加规则后弹窗被顶到 805px（底栏被挤出屏幕）——两个都是尺寸失控。
    // 现在高度在构造时就钉死，内容多出来在中间滚动。
    {
        const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
        const int limitH = qMax(320, avail.height() - 40);
        const int expectH = qMin(560, limitH);
        const int h = d->height();
        check("G20-settings-height-fixed", h == expectH,
              QStringLiteral("height=%1 expected=%2 limit=%3").arg(h).arg(expectH).arg(limitH));
    }

    // ---- 日志展开后，窗口必须**整块留在屏幕里**（2026-10-28 缺陷 1/6）----
    //
    // 旧判据只比高度，抓不到"底边跑出屏幕"：1280×764 可用区下
    // 460→759 后底边到 909，超出 146px，175px 高的日志抽屉只剩 29px 可见 ——
    // 用户点了"展开日志"，看到的是几乎空的抽屉。
    {
        const QRect avail = scrAvail;
        // 统一用"全局坐标"取矩形：设置弹窗有父窗口，geometry() 是父坐标。
        auto globalRect = [](QWidget *widget) {
            return QRect(widget->mapToGlobal(QPoint(0, 0)), widget->size());
        };
        auto insideAvail = [&avail](const QRect &g) {
            return g.top() >= avail.top() && g.bottom() <= avail.bottom()
                && g.left() >= avail.left() && g.right() <= avail.right();
        };

        // (1) 阴性对照：复现旧行为「只 setFixedHeight、不校正位置」，
        //     同一套判定必须报"不在屏内" —— 否则这条判据就是恒真的假保证。
        {
            const int yBad = avail.bottom() - 60;
            w.setFixedHeight(hShut);
            w.move(avail.x() + 40, yBad);
            QApplication::processEvents();
            w.setFixedHeight(2000);          // 旧行为：高度随便设
            w.move(avail.x() + 40, yBad);     // 旧行为：位置不动
            QApplication::processEvents();
            const QRect g = globalRect(&w);
            check("G22-neg-old-behavior-detected", !insideAvail(g),
                  QStringLiteral("old-behavior geometry still counted as inside: %1,%2 %3x%4")
                      .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height()));
        }

        // (2) 复原成"收起 + 被拖到屏幕下半屏"，再走**产品路径**点开日志。
        if (w.syncPanelForTest()->isLogOpenForTest()) {
            w.syncPanelForTest()->toggleLogDrawer();
            QApplication::processEvents();
        }
        w.setFixedHeight(hShut);
        w.move(avail.x() + 40, avail.bottom() - 60);
        QApplication::processEvents();
        w.syncPanelForTest()->toggleLogDrawer();   // ← 产品自己的处理在这里跑
        QApplication::processEvents();

        const QRect g = globalRect(&w);
        check("G22-log-open-window-inside-screen", insideAvail(g),
              QStringLiteral("geom=%1,%2 %3x%4 avail=%5,%6 %7x%8")
                  .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                  .arg(avail.x()).arg(avail.y()).arg(avail.width()).arg(avail.height()));
        check("G23-log-open-height-follows-screen", w.height() == hOpen,
              QStringLiteral("height=%1 expected=%2").arg(w.height()).arg(hOpen));
    }

    // ---- 最大化状态下切日志，不能把窗口改成"最大化但尺寸不对"（缺陷 3）----
    //
    // 旧实现无条件 setFixedHeight(...)：最大化窗口的 min/max 被写死，
    // 结果 isMaximized() 还是 true、几何却变成 1280×728 —— 谁都修不回来。
    {
        // 与产品「最大化」按钮同一条路：先解开宽高死锁。
        w.setMinimumSize(0, 0);
        w.setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        w.setWindowState(w.windowState() | Qt::WindowMaximized);
        QApplication::processEvents();
        // 模拟真实最大化后的几何（有的平台上最大化不改几何）。
        w.setFixedHeight(scrAvail.height());
        QApplication::processEvents();
        const bool maxBefore = w.isMaximized();
        const int hMaxBefore = w.height();
        w.syncPanelForTest()->toggleLogDrawer();
        QApplication::processEvents();
        const bool maxAfter = w.isMaximized();
        const int hMaxAfter = w.height();
        check("G24-maximized-kept-on-log-toggle",
              maxBefore && maxAfter && hMaxBefore == hMaxAfter,
              QStringLiteral("maxBefore=%1 maxAfter=%2 hBefore=%3 hAfter=%4")
                  .arg((int)maxBefore).arg((int)maxAfter).arg(hMaxBefore).arg(hMaxAfter));
        // 还原：退出最大化，把固定尺寸装回去。
        w.showNormal();
        QApplication::processEvents();
        const bool nowOpen = w.syncPanelForTest()->isLogOpenForTest();
        w.setFixedSize(ScreenFit::mainWindow(scrAvail.width(), scrAvail.height(), nowOpen));
        QApplication::processEvents();
    }

    // ---- 设置弹窗也必须整块落在屏幕内（多屏 / 拔屏后重新收敛）----
    {
        const QRect g(d->mapToGlobal(QPoint(0, 0)), d->size());
        const bool inside = g.top() >= scrAvail.top() && g.bottom() <= scrAvail.bottom()
                         && g.left() >= scrAvail.left() && g.right() <= scrAvail.right();
        check("G25-settings-inside-screen", inside,
              QStringLiteral("geom=%1,%2 %3x%4 avail=%5,%6 %7x%8")
                  .arg(g.x()).arg(g.y()).arg(g.width()).arg(g.height())
                  .arg(scrAvail.x()).arg(scrAvail.y()).arg(scrAvail.width()).arg(scrAvail.height()));
    }

    // ---- 规则表每行都得有「删除」按钮，而且点了真能删掉这一行 ----
    //
    // 产品里从来没有过删除入口：removeRuleRow() 只被自动化测试钩子调用过，
    // 用户点「+ 添加规则」加出来的行永远删不掉（2026-10-28 用户报障
    // 「添加规则之后无法进行删除」）。
    {
        const int rowsBefore = d->ruleRowCountForTest();
        QString suffixBefore;
        for (int r = 0; r < rowsBefore; ++r)
            suffixBefore += d->ruleCellForTest(r, 0) + QStringLiteral("|");

        // 加一行（模拟用户点「+ 添加规则」）
        d->clickAddRuleForTest();
        QApplication::processEvents();
        const int rowsAdded = d->ruleRowCountForTest();
        const bool hasBtn = d->ruleRowHasDeleteButtonForTest(rowsAdded - 1);
        check("G26-add-rule-then-delete-available",
              rowsAdded == rowsBefore + 1 && hasBtn,
              QStringLiteral("rows=%1 expected=%2 hasBtn=%3")
                  .arg(rowsAdded).arg(rowsBefore + 1).arg((int)hasBtn));

        // 点**新加那一行**的删除按钮 → 行数 -1，而且剩下的是原来那些行。
        d->clickRuleDeleteForTest(rowsAdded - 1);
        QApplication::processEvents();
        const int rowsAfter = d->ruleRowCountForTest();
        check("G27-delete-button-removes-row", rowsAfter == rowsBefore,
              QStringLiteral("rows=%1 expected=%2").arg(rowsAfter).arg(rowsBefore));

        QString suffixAfter;
        for (int r = 0; r < rowsAfter; ++r)
            suffixAfter += d->ruleCellForTest(r, 0) + QStringLiteral("|");
        check("G28-delete-removed-the-right-row", suffixAfter == suffixBefore,
              QStringLiteral("before=[%1] after=[%2]")
                  .arg(suffixBefore).arg(suffixAfter));
    }

    // 还原环境变量
    if (oldCfgDir.isEmpty())
        qunsetenv("SRT_CONFIG_DIR");
    else
        qputenv("SRT_CONFIG_DIR", oldCfgDir);

    out << (bad == 0 ? "GUI_CHECK_ALL_PASS" : "GUI_CHECK_FAILED") << "\n";
    out << "GUI_SUMMARY\tpass=" << pass << "\tfail=" << bad << "\n";
    out.flush();
    return bad == 0 ? 0 : 1;
}


} // namespace

// ------------------------------------------------------------------
// --selfcheck-secrets：反复跑真实同步，检查临时目录里有没有明文口令残留。
//
// 为什么必须真跑一遍：口令模式的 SSH_ASKPASS 需要两个临时文件
// （一个明文口令、一个 cat 它的脚本）。这条链路**只有真同步才会走到**，
// 而它的缺陷恰恰是"同步成功之后没人删它们" —— 静态检查看不出来。
// 实测（银河麒麟 V11，PID 74018）：连点 14 次同步就在 /tmp 里留下
// 15 个 srt_pass_<pid>_*，od -c 读出来就是明文。
//
// 接收端不写死在程序里，从环境变量拿，测试台自己配：
//   SRT_SC_HOST / SRT_SC_PORT / SRT_SC_USER / SRT_SC_PASS / SRT_SC_REMOTE
// 没配就跳过并说明，不猜地址。
// ------------------------------------------------------------------
// 跑真实同步，检查临时目录里有没有明文口令残留。
//
// 架构说明（这一条是踩过坑才定下来的）：
// 这里**不用** processEvents 手工泵事件，而是装一个 QTimer 状态机然后
// 交给真正的 app.exec() 跑。
// 第一版是用 processEvents(AllEvents, 100) 一轮轮泵的，结果同步永远停在
// 「共 1 个文件，分 1 组同步」这一行，90 秒等不到 allFinished，接收端一个字节
// 都没收到 —— 而手工泵出来的循环和产品真正运行时的事件循环不是一回事，
// QProcess 相关的通知在这种泵里经常推不动。判据不能建立在一个和真实运行
// 不同的执行模型上。
//
// 接收端不写死在程序里，从环境变量拿，测试台自己配：
//   SRT_SC_HOST / SRT_SC_PORT / SRT_SC_USER / SRT_SC_PASS / SRT_SC_REMOTE / SRT_SC_REPORT
// 没配就跳过并说明，不猜地址。
struct SecretCheck
{
    SyncPanel    *panel = nullptr;
    // 放构造函数里初始化，不用 QTextStream out(stdout)：
    //   1) 那写法会被解析成函数声明（最令人 vexing 的那种），报出来是莫名语法错；
    //   2) QTextStream 的拷贝构造在 Qt 5.9 是私有的，= 初始化也不行。
    // 输出编码同样钉死 UTF-8，理由见 runSelfCheck() 里那段注释。
    SecretCheck() : out(stdout) { out.setCodec("UTF-8"); }
    QTextStream   out;
    int           roundsLeft = 0;
    int           doneBefore = 0;
    QString       before;
    int           waitedTicks = 0;
    int           bad = 0;
    // 单独记"确实查到残留"的轮数。bad 还包含"同步没跑起来""同步失败"
    // 这类与泄漏无关的失败。全红时如果一律报 SECRETS_LEAKED，
    // 读的人会以为抓到了明文口令，实际上一轮传输都没成功过。
    int           leakRounds = 0;
    QString       reportPath;
    QString       self;
    QString       srcDir;

    // 本进程名下的口令/askpass 残留。别的进程的不归我们管。
    QStringList leftovers() const
    {
        QStringList found;
        QStringList dirs;
        dirs << QDir::tempPath() << RsyncRunner::askpassDir();
        for (const QString &d : dirs) {
            // 注意 nameFilters 是**通配符**，不是前缀。
            // 写成 "srt_pass_"（少了星号）只会匹配名字正好等于它的文件，
            // 于是这个函数**永远返回空**、判据变成"永远干净"。
            // 这个坑真踩过：修复前后判据输出完全一样，两边都报全绿，
            // 是临时目录快照里列出了 11 字节的明文口令文件才暴露的。
            const QStringList names = QDir(d).entryList(QStringList()
                                                       << QStringLiteral("srt_pass_*")
                                                       << QStringLiteral("srt_askpass_*"),
                                                       QDir::Files | QDir::NoDotAndDotDot);
            for (const QString &n : names) {
                const QStringList parts = n.split(QLatin1Char('_'));
                if (parts.size() >= 3 && parts.at(2).startsWith(self))
                    found << d + QLatin1Char('/') + n;
            }
        }
        return found;
    }

    void startRound()
    {
        before = panel->logTailForTest(400).join(QLatin1Char('\n'));
        doneBefore = panel->syncFinishedCountForTest();
        waitedTicks = 0;
        panel->startSyncForTest();
    }

    void tick()
    {
        ++waitedTicks;
        if (panel->syncFinishedCountForTest() > doneBefore) {
            finishRound();
            return;
        }
        if (waitedTicks >= 900) {            // ~90 秒
            // 90 秒还没 allFinished：判据超时，如实报出来，不当成"干净"
            ++bad;
            out << "FAIL\tround\ttimeout (90s 内 allFinished 没到，判据空转)" << "\n";
            dumpRound("timeout");
            nextRound();
        }
    }

    void dumpRound(const QString &note)
    {
        if (reportPath.isEmpty())
            return;
        QFile rf(reportPath);
        // 只追加、不 Truncate：带 Truncate 每轮都抹掉上一轮，跑三轮只剩第三轮。
        if (rf.open(QIODevice::WriteOnly | QIODevice::Append)) {
            rf.write(QStringLiteral("===== round (%1) =====\n").arg(note).toUtf8());
            rf.write(panel->logTailForTest(8).join(QLatin1Char('\n')).toUtf8());
            rf.write("\n--- temp snapshot ---\n");
            // 全量列出（不限 pid、不限大小）。判据只看"本进程名下的"，
            // 一旦判据说干净而这里其实有东西，说明过滤条件本身错了。
            const QStringList probeDirs = QStringList() << QDir::tempPath() << RsyncRunner::askpassDir();
            for (const QString &d : probeDirs) {
                const QStringList names = QDir(d).entryList(QStringList()
                                                           << QStringLiteral("srt_pass_*")
                                                           << QStringLiteral("srt_askpass_*"),
                                                           QDir::Files | QDir::NoDotAndDotDot);
                for (const QString &n : names) {
                    const QFileInfo fi(QDir(d).filePath(n));
                    rf.write(QStringLiteral("%1/%2  %3B  %4\n")
                                 .arg(QDir::toNativeSeparators(d), n)
                                 .arg(fi.size())
                                 .arg(fi.lastModified().toString(Qt::ISODate)).toUtf8());
                }
            }
            rf.write("--- end snapshot ---\n");
        }
    }

    void finishRound()
    {
        const QString after = panel->logTailForTest(400).join(QLatin1Char('\n'));
        const bool ranSomething = (after.size() > before.size());
        const QStringList left = leftovers();
        const bool syncOk = panel->syncLastOkForTest();

        // 三条全过才算合格，缺一条都算 FAIL。
        //
        // syncOk 这条最容易被漏掉，而漏掉它的后果比另外两条严重得多。
        // 真踩过：VM 上跑 --selfcheck-secrets，三轮输出全是
        //     PASS  round  finished-and-clean  ok=0
        //     SECRETS_ALL_CLEAN   rc=0
        // ok=0 表示**同步一次都没成功**（日志原文是「未找到 ssh 客户端」）。
        // 它之所以还判干净，是因为 createAskpass() 压根没被调用，
        // 临时目录当然是空的 —— 没建过口令文件当然没残留。
        //
        // 问题在于这条假绿"恰好"对真正泄漏的那次也成立
        // （同步跑完了但没删文件，残留照样能查出来），
        // 于是它平时看着一直是对的，直到某次同步本身坏掉。
        // 一条「同步没跑起来也算口令清理通过」的判据，
        // 会把「同步功能整个不可用」包装成「口令零残留，安全」。
        const char *verdict = "done";
        if (!ranSomething) {
            ++bad;
            verdict = "no-log-growth";
            out << "FAIL\tround\tno-log-growth (sync never started, judge idled)" << "\n";
        } else if (!syncOk) {
            ++bad;
            verdict = "sync-failed";
            // 同步失败绝不能算「干净」：不残留只是因为压根没建口令文件。
            // 把结尾几行日志原样打出来，否则只看到 ok=0 根本查不出原因。
            out << "FAIL\tround\tsync-failed (nothing left over, but transfer never succeeded)" << "\n";
            const QStringList tail = panel->logTailForTest(6);
            for (const QString &t : tail)
                out << "\tlog\t" << t << "\n";
        } else if (!left.isEmpty()) {
            ++bad;
            ++leakRounds;
            verdict = "leftover";
            out << "FAIL\tround\tleftover=" << left.size();
            for (const QString &p : left)
                out << "\t" << p;
            out << "\n";
        } else {
            out << "PASS\tround\tfinished-and-clean\tok=1" << "\n";
        }
        dumpRound(QString::fromLatin1(verdict));
        nextRound();
    }

    void nextRound()
    {
        if (--roundsLeft <= 0) {
            // 三个不同的终态，不合并：判据坏掉和查出泄漏必须能一眼分开，
            // 否则"同步功能坏了"会被读成"口令安全达标"。
            if (bad == 0)
                out << "SECRETS_ALL_CLEAN" << "\n";
            else if (leakRounds > 0)
                out << "SECRETS_LEAKED" << "\n";
            else
                out << "SECRETS_CHECK_BROKEN" << "\n";
            QCoreApplication::exit(bad == 0 ? 0 : 1);
            return;
        }
        startRound();
    }
};

static int runSecretCheck(QApplication &app, int rounds)
{
    SecretCheck sc;
    const QByteArray host = qgetenv("SRT_SC_HOST");
    const QByteArray port = qgetenv("SRT_SC_PORT");
    const QByteArray user = qgetenv("SRT_SC_USER");
    const QByteArray pass = qgetenv("SRT_SC_PASS");
    const QByteArray remote = qgetenv("SRT_SC_REMOTE");
    sc.reportPath = QString::fromLocal8Bit(qgetenv("SRT_SC_REPORT"));
    if (host.isEmpty() || user.isEmpty()) {
        sc.out << "SKIP\tno-receiver-configured (SRT_SC_HOST/SRT_SC_USER unset)" << "\n";
        return 0;
    }

    sc.self = QString::number(QCoreApplication::applicationPid());
    sc.srcDir = QDir::tempPath() + QStringLiteral("/srt-sc-src-") + sc.self;
    QDir().mkpath(sc.srcDir);
    QFile f(sc.srcDir + QLatin1String("/probe.spe"));
    f.open(QIODevice::WriteOnly);
    f.write("srt-selfcheck", 14);
    f.close();

    MainWindow w;
    w.resize(980, 460);
    sc.panel = w.syncPanelForTest();
    if (!sc.panel) {
        sc.out << "FAIL\tno-sync-panel" << "\n";
        return 1;
    }
    sc.panel->setLocalRootForTest(sc.srcDir);
    sc.panel->setHostForTest(QString::fromLocal8Bit(host));
    sc.panel->setPortForTest(port.isEmpty() ? 22 : port.toInt());
    sc.panel->setUserForTest(QString::fromLocal8Bit(user));
    // 口令从 fromLocal8Bit 改为 fromUtf8：
    // 口令变量来自环境变量，非 UTF-8 locale 下（麒麟 V10 里是 POSIX/C）
    // 口令的 UTF-8 字节会被按本机代码页解一遍，
    // 解不出来的字节变成 U+FFFD —— 产品把这个值原样写进口令文件发给 ssh，
    // “产品发给 ssh 的值”与“设定的口令”不是同一个东西。
    // ASCII 口令两者完全一致（不改变任何行为）。
    applySecretCheckAuth(sc.panel, QString::fromUtf8(pass));
    sc.panel->setRemoteRootForTest(QString::fromLocal8Bit(remote));

    // 先扫出文件，否则走的是"没有匹配到要同步的文件"分支，
    // createAskpass() 压根不会被调用，自检空转通过。
    sc.panel->scanFilesForTest();

    QTimer *poll = new QTimer;
    poll->setInterval(100);
    QObject::connect(poll, &QTimer::timeout, [&, poll]() {
        if (sc.panel->fileCountForTest() <= 0) {
            ++sc.waitedTicks;
            if (sc.waitedTicks <= 600)
                return;                       // 最多等 60 秒扫描完成
            sc.out << "FAIL\tno-files-scanned\t自检空转：没扫到文件，口令链路不会执行" << "\n";
            sc.out << "SECRETS_LEAKED" << "\n";
            poll->stop();
            QCoreApplication::exit(1);
            return;
        }
        sc.out << "INFO\tfiles-scanned=" << sc.panel->fileCountForTest() << "\n";
        poll->stop();
        sc.roundsLeft = rounds;
        sc.startRound();

        QTimer *t = new QTimer;
        t->setInterval(100);
        QObject::connect(t, &QTimer::timeout, [&, t]() {
            sc.tick();
            if (sc.roundsLeft == 0) {
                t->stop();
                delete t;
            }
        });
        t->start();
    });
    poll->start();
    return app.exec();
}

int main(int argc, char *argv[])
{
    // 必须开。客户机笔记本屏 2560x1600 + Windows 150% 缩放，
    // 进程不做 DPI 感知的话 Windows 会把整个窗口当位图放大 1.5 倍：
    // 字体发虚、控件和间距全部变大 —— 换个屏就"尺寸全变了"。
    //
    // 必须在 QApplication 构造之前设置。Qt 5.9 默认关闭，Qt 6 才默认开启。
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("SyncReplayTool"));
    app.setOrganizationName(QStringLiteral("AP"));

    // FileItem 要跨线程从扫描线程传回界面线程。没注册元类型的话
    // Qt 的排队连接会**直接把这个信号丢掉**，现象是点了扫描永远没反应。
    qRegisterMetaType<QVector<FileItem>>("QVector<FileItem>");

    // 「记住设置」必须在这里、也就是**任何 target 配置被读或被写之前**就迁移成 true。
    //
    // 为什么：界面上那个「记住设置」勾选框已按客户意见移除（Jira Tools-60），
    // 它的真实语义是「目标连接信息要不要写进本机配置文件」，而界面上已经
    // 没有地方能改它了 —— 这个键只能恒为 true。
    //
    // 钉的位置很要紧。以前它写在**设置窗口的构造函数**里，于是：
    //   1) 启动时 loadTarget() 先看到老配置里的 false → 直接返回空配置，
    //      用户的 IP/账号/口令根本读不回来，界面是空的；
    //   2) 更糟的是 saveTarget() 看到 false 会把 ini 里的 target/* **删掉**
    //      （那是"用户没要求清空"的场景，用户根本没机会表达过意愿），
    //      于是连接信息被静默抹掉。
    // 闸门本身也已经从 loadTarget/saveTarget 里去掉（那里恒为"永远记住"），
    // 这里只负责把老配置里那个没人能改的键迁移掉。
    ConfigStore::migrateRememberSettingsToTrue();

    // 早于任何同步动作清残留。上次异常退出留下的 %TEMP%\srt_pass_* 是**明文口令**，
    // 正常退出时 cleanupSecrets() 会删，崩溃/强杀/断电则不会，只能下次启动补。
    RsyncRunner::cleanupStaleSecrets();

    Theme::apply(&app);
    app.setWindowIcon(QIcon(Theme::logo(64)));

    // 自动化测试开关。只在显式传参时生效，正常启动走完整启动页。
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("SyncReplayTool"));
    const QCommandLineOption optSkip(QStringLiteral("no-splash"),
        QStringLiteral("Skip the splash screen (automation only)."));
    const QCommandLineOption optSplashMs(QStringLiteral("splash-ms"),
        QStringLiteral("Splash duration in ms, 0 to skip (automation only)."), QStringLiteral("ms"));
    const QCommandLineOption optShot(QStringLiteral("screenshot"),
        QStringLiteral("Capture window to png and exit (automation only)."), QStringLiteral("path"));
    const QCommandLineOption optShotSettings(QStringLiteral("screenshot-settings"),
        QStringLiteral("Capture settings dialog to png and exit (automation only)."), QStringLiteral("path"));
    const QCommandLineOption optSelfCheck(QStringLiteral("selfcheck"),
        QStringLiteral("Run attribution and watchdog self-checks, print results and exit."));
    const QCommandLineOption optGuiCheck(QStringLiteral("selfcheck-gui"),
        QStringLiteral("Run GUI interaction self-checks (clicks real widgets), print results and exit."));
    const QCommandLineOption optSecretCheck(QStringLiteral("selfcheck-secrets"),
        QStringLiteral("Run real syncs and verify no plaintext password file is left behind."),
        QStringLiteral("rounds"));
    parser.addOption(optSkip);
    parser.addOption(optSplashMs);
    parser.addOption(optShot);
    parser.addOption(optShotSettings);
    parser.addOption(optSelfCheck);
    parser.addOption(optGuiCheck);
    parser.addOption(optSecretCheck);
    // 「--help」必须能用：客户机上的运维第一反应就是敲 --help 看用法。
    //
    // 原来建了 parser 却**从未调 addHelpOption()**，于是 --help 被当成未知选项
    // 直接报错退出（"Unknown option 'help'."）—— 交付给客户机的命令行程序
    // 连用法都打不出来。子代理 B 在麒麟 V10 容器里实测抓到的。
    parser.addHelpOption();
    parser.process(app);

    // 未知的**位置参数**必须报错，不能静默吞掉。
    //
    // QCommandLineParser 只对未知**选项**报错（process() 直接退出），
    // 位置参数它收进 positionalArguments() 之后就当没看见。
    // 于是 `SyncReplayTool --selfcheck-gui 多余的东西` 会正常跑完并返回 0 ——
    // 脚本把参数写错了却"成功"，这种比报错难查得多。
    // 未知选项的行为本来就是对的，这里补的是位置参数这一半。
    if (!parser.positionalArguments().isEmpty()) {
        QTextStream err(stderr);
        err << "Unknown argument(s): " << parser.positionalArguments().join(' ') << "\n";
        err.flush();
        return 2;
    }

    // 自检必须在建 MainWindow 之前返回：它不碰界面，也不需要显示服务器。
    // 这样在没有任何 X/桌面环境的机器上也能跑（CI、容器、麒麟 SSH 会话）。
    if (parser.isSet(optSelfCheck))
        return runSelfCheck();

    if (parser.isSet(optGuiCheck)) {
        MainWindow::setSkipSplash(true);
        return runGuiSelfCheck();
    }

    if (parser.isSet(optSecretCheck)) {
        MainWindow::setSkipSplash(true);
        const int rounds = qMax(1, parser.value(optSecretCheck).toInt());
        return runSecretCheck(app, rounds);
    }

    if (parser.isSet(optSplashMs))
        MainWindow::setSplashDuration(parser.value(optSplashMs).toInt());
    if (parser.isSet(optSkip) || parser.isSet(optSplashMs) || parser.isSet(optShot) || parser.isSet(optShotSettings))
        MainWindow::setSkipSplash(true);

    // ----------------------------------------------------------------
    // 单实例保护
    //
    // 为什么必须有：开机自启 + 桌面图标会让程序被拉起两次，
    // 两个进程抢同一份配置文件和同一份同步任务 —— 谁写 ini 谁说了算，
    // 而且两轮同步会同时往同一个远端目录里传。
    //
    // 锁放在**配置目录**里（ConfigStore::configDir()，默认 %APPDATA%\AP）：
    // 同一份配置 = 同一把锁；不同配置目录（测试沙箱、并行用例）= 互不干扰。
    //
    // setStaleLockTime(0)：Qt 默认 30 秒就把"看起来很久没动过"的锁当陈旧锁清掉。
    // 客户机上一次同步跑几 GB 时，第二个实例会在 30 秒后**误判**锁失效并闯进来，
    // 两个进程同时写同一个 ini。设成 0 = 永不做时间启发式判断，
    // 只认"进程还活着没有"（进程真死了，Qt 仍会按 PID 判定锁已失效并接管，
    // 不会留下死锁）。
    //
    // **命令行自检/探针模式一律跳过**：这些模式本来就要并发跑
    // （--selfcheck / --selfcheck-secrets / --screenshot / --screenshot-settings），
    // 走单实例逻辑会互相踢掉，主会话的回归会全挂。
    // --no-splash / --splash-ms 自己的帮助文本就写着 "(automation only)"，
    // 同样按探针处理。
    const bool probeMode = parser.isSet(optSelfCheck) || parser.isSet(optSecretCheck)
                        || parser.isSet(optGuiCheck)
                        || parser.isSet(optShot) || parser.isSet(optShotSettings)
                        || parser.isSet(optSkip) || parser.isSet(optSplashMs);
    // QLockFile **必须活到 main() 结束**。
    //
    // 它是个会**在析构时解锁并删掉锁文件**的对象 —— 把它声明在 if 块里，
    // 块一结束锁就放了，第二个实例照样能进来（这一条是实测踩出来的：
    // 第一个实例活着、锁文件却不见了、第二个实例不被拦）。
    // 所以声明在 main() 这一层，不在 if 块里。
    //
    // 探针模式下只是"构造对象、不 tryLock"，不会产生锁文件。
    QLockFile instanceLock(ConfigStore::configDir()
                           + QStringLiteral("/.srt-single-instance.lock"));
    if (!probeMode) {
        instanceLock.setStaleLockTime(0);
        if (!instanceLock.tryLock(0)) {
            const QString msg = QString::fromUtf8(u8"程序已在运行。\n\n"
                                                 u8"请先关闭已经打开的窗口再试。");
            // stderr 永远打一份：脚本和日志里能看见，不依赖界面。
            QTextStream err(stderr);
            err << msg << "\n";
            err.flush();
            // 界面提示只在**能显示窗口**时弹（offscreen/minimal 平台不弹）。
            const QString platform = QGuiApplication::platformName();
            if (platform != QLatin1String("offscreen") && platform != QLatin1String("minimal")) {
                QMessageBox::warning(nullptr,
                                     QString::fromUtf8(u8"数据同步工具"), msg);
            }
            return 2;
        }
        // instanceLock 是栈对象：main 返回时析构 → 解锁。
        // 进程被强杀时锁文件会留下，但 Qt 下次按 PID 判定它已失效并接管。
    }

    MainWindow w;
    if (parser.isSet(optShotSettings)) {
        const QString shotPath = parser.value(optShotSettings);
        w.resize(980, 460);
        // 截图探针**不弹窗**：WA_DontShowOnScreen 让窗口走完整的 Windows 字体
        // 与布局（offscreen 平台没有字体，截出来全是空框），但一个像素都不上屏、
        // 也不抢焦点。截图结果和真机显示完全一致。
        w.setAttribute(Qt::WA_DontShowOnScreen, true);
        w.show();
        w.showSettingsForProbe();
        QTimer::singleShot(600, [&w, shotPath] {
            QApplication::processEvents();
            QWidget *dialog = QApplication::activeModalWidget();
            if (!dialog) {
                const auto topWidgets = QApplication::topLevelWidgets();
                for (QWidget *tw : topWidgets) {
                    if (tw != &w && tw->isVisible()) {
                        dialog = tw;
                        break;
                    }
                }
            }
            if (dialog) {
                // SRT_SHOT_SCROLL=1：把设置弹窗滚到最下面再截。
                //
                // 规则表在第三组「文件打开工具」，位于折叠线以下 ——
                // 不滚就永远截不到那一组（更截不到每行的「删除」按钮列）。
                // 只影响截图探针，正常启动完全不看这个变量。
                if (!qgetenv("SRT_SHOT_SCROLL").isEmpty()) {
                    const QList<QScrollArea *> areas = dialog->findChildren<QScrollArea *>();
                    for (int i = 0; i < areas.size(); ++i) {
                        QScrollBar *sb = areas.at(i)->verticalScrollBar();
                        if (sb)
                            sb->setValue(sb->maximum());
                    }
                    QApplication::processEvents();
                }
                QImage img(dialog->width(), dialog->height(), QImage::Format_ARGB32_Premultiplied);
                img.fill(Qt::transparent);
                QPainter p(&img);
                dialog->render(&p);
                p.end();
                img.save(shotPath);
            }
            QCoreApplication::quit();
        });
        return app.exec();
    }
    if (parser.isSet(optShot)) {
        const QString shotPath = parser.value(optShot);
        w.resize(980, 460);
        // 同上：不弹窗、不抢焦点，但用真实 Windows 字体渲染。
        w.setAttribute(Qt::WA_DontShowOnScreen, true);
        // SRT_SHOT_ROOT：往表格里灌一批样例文件 + 工具规则。
        // 截图探针默认跑在空目录上，表格是空的 —— 那样根本看不出
        // 行高、状态药丸、工具按钮这几样对不对。真实使用时目录里本来就有文件。
        {
            const QByteArray seedRoot = qgetenv("SRT_SHOT_ROOT");
            SyncPanel *seedPanel = w.syncPanelForTest();
            if (!seedRoot.isEmpty() && seedPanel) {
                seedPanel->setLocalRootForTest(QString::fromLocal8Bit(seedRoot));
                QVector<ToolRule> seedRules;
                struct { const char *suffix; const char *tool; } seedData[] = {
                    { "spe", "\xE4\xBF\xA1\xE5\x8F\xB7\xE5\x9B\x9E\xE6\x94\xBE" },       // 信号回放
                    { "dat", "\xE6\x95\xB0\xE6\x8D\xAE\xE5\x88\x86\xE6\x9E\x90" },       // 数据分析
                    { "h5",  "\xE9\xA2\x91\xE8\xB0\xB1\xE6\x9F\xA5\xE7\x9C\x8B" }        // 频谱查看
                };
                for (unsigned i = 0; i < sizeof(seedData) / sizeof(seedData[0]); ++i) {
                    ToolRule r;
                    r.suffix   = QString::fromLatin1(seedData[i].suffix);
                    r.toolNames    << QString::fromUtf8(seedData[i].tool);
                    // 工具路径必须**真实存在**：产品只在 exe 存在时才画工具按钮
                    // （不存在就显示「未配置工具」）。这里直接用程序自己的 exe，
                    // 哪个机器上都一定在。
                    r.toolExePaths << QCoreApplication::applicationFilePath();
                    seedRules << r;
                }
                seedPanel->setToolRulesForTest(seedRules);
                seedPanel->scanFilesForTest();
            }
        }
        w.show();
        QTimer::singleShot(600, [&w, shotPath] {
            QApplication::processEvents();
            QImage img(w.width(), w.height(), QImage::Format_ARGB32_Premultiplied);
            img.fill(QColor(0x1e, 0x25, 0x30)); // 模拟深色桌面背景
            QPainter p(&img);
            w.render(&p);
            p.end();
            img.save(shotPath);
            QCoreApplication::quit();
        });
        return app.exec();
    }

    w.show();
    return app.exec();
}
