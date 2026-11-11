// probe.cpp - headless driver for SyncReplayTool's real sync path.
//
// Why: the GUI tests kept losing the app to a foreign mouse click (another
// agent shares this desktop). This probe uses the product's own test hooks
// (the same ones --selfcheck-secrets uses) so a test round needs no window,
// no mouse and no screen.
//
// It prints, in this order:
//   SCAN   files=<n> rules=<...> states=<...>
//   SYNC   ok=<0|1> done=<n> total=<n> count="<count label>" status="<status>"
//   LOG    <log lines>
//   ROWS   <index> <state>            (row order == scanner order)
//
// Usage:
//   probe --root <dir> [--host H --port P --user U --key K --remote R]
//         [--rules a,b,c] [--no-rules] [--only-new] [--sync] [--timeout S]
//         [--dump-rows] [--cancel-after MS]
//
// --cancel-after MS: 传输开始 MS 毫秒后再按一次开始/取消按钮（= 用户点取消）。
#include <QApplication>
#include <QTimer>
#include <QTextStream>
#include <QStringList>
#include <QFileInfo>
#include <QDir>

#include "MainWindow.h"
#include "SyncPanel.h"

static QTextStream out(stdout);

static void say(const QString &s)
{
    out << s << "\n";
    out.flush();
}

static QString argValue(const QStringList &a, const QString &key, const QString &dflt = QString())
{
    int i = a.indexOf(key);
    if (i >= 0 && i + 1 < a.size())
        return a.at(i + 1);
    return dflt;
}

static bool argFlag(const QStringList &a, const QString &key)
{
    return a.contains(key);
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    out.setCodec("UTF-8");

    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    const QString root      = argValue(args, "--root");
    const QString host      = argValue(args, "--host");
    const int    port      = argValue(args, "--port", "0").toInt();
    const QString user      = argValue(args, "--user");
    const QString key       = argValue(args, "--key");
    const QString password  = argValue(args, "--password");
    const QString remote    = argValue(args, "--remote");
    const QString rulesArg  = argValue(args, "--rules");
    const bool   noRules   = argFlag(args, "--no-rules");
    const bool   onlyNew   = argFlag(args, "--only-new");
    const bool   doSync    = argFlag(args, "--sync");
    const bool   dumpRows  = argFlag(args, "--dump-rows");
    const int    timeout   = argValue(args, "--timeout", "300").toInt();
    const int    cancelAfter = argValue(args, "--cancel-after", "0").toInt();

    MainWindow w;
    SyncPanel *panel = w.syncPanelForTest();
    if (!panel) {
        say("PROBE_FAIL no-sync-panel");
        return 3;
    }

    if (!root.isEmpty())
        panel->setLocalRootForTest(root);
    if (!host.isEmpty())
        panel->setHostForTest(host);
    if (port > 0)
        panel->setPortForTest(port);
    if (!user.isEmpty())
        panel->setUserForTest(user);
    if (!key.isEmpty())
        panel->setKeyPathForTest(key);
    // --password: switch to password authentication (the other, much less
    // covered auth path). The key path must be cleared, otherwise the
    // product takes the private-key branch and never uses the password at
    // all -- a test that looks like it covered password mode but did not.
    if (!password.isEmpty()) {
        panel->setPasswordForTest(password);
        panel->setKeyPathForTest(QString());
    }
    if (!remote.isEmpty())
        panel->setRemoteRootForTest(remote);
    if (!rulesArg.isEmpty()) {
        QStringList rl = rulesArg.split(QLatin1Char(','), QString::SkipEmptyParts);
        panel->setRulesForTest(rl);
    }
    if (noRules)
        panel->setRulesForTest(QStringList());
    if (onlyNew)
        panel->setOnlyNewForTest(true);

    panel->scanFilesForTest();

    const int doneBefore = panel->syncFinishedCountForTest();
    int ticks = 0;
    bool scanned = false;
    bool seenScanning = false;
    int syncStartTick = -1;
    bool cancelSent = false;

    QTimer *timer = new QTimer;
    timer->setInterval(200);
    QObject::connect(timer, &QTimer::timeout, [&, timer]() {
        ++ticks;
        if (!scanned) {
            // 判"扫描结束"不能要求 fileCount > 0：空目录时文件数恒为 0，
            // 原写法会让"扫描完成"永不成立，只能等 --timeout（默认 300 秒）才报
            // PROBE_FAIL scan-timeout —— 实测 20 秒不退出，被误判成"产品在空目录上挂死"。
            // 产品本身正常（isScanningForTest() 会从 true 正常变回 false）。
            // 正确判据：先看到过 scanning=true，再看它变回 false；另留 2 tick 兜底防极快扫描。
            if (panel->isScanningForTest())
                seenScanning = true;
            if ((seenScanning || ticks >= 2) && !panel->isScanningForTest()) {
                scanned = true;
                QStringList rules;
                const QVector<SyncRule> rs = panel->syncRulesForTest();
                for (int i = 0; i < rs.size(); ++i)
                    rules << rs.at(i).suffix;
                say(QString("SCAN files=%1 rules=[%2] states=[%3]")
                        .arg(panel->fileCountForTest())
                        .arg(rules.join(QLatin1Char(',')))
                        .arg(panel->fileStatesForTest().join(QLatin1Char(','))));
                if (dumpRows) {
                    const QStringList st = panel->fileStatesForTest();
                    for (int i = 0; i < st.size(); ++i)
                        say(QString("ROW %1 %2").arg(i).arg(st.at(i)));
                }
                if (!doSync) {
                    QCoreApplication::exit(0);
                    return;
                }
                panel->startSyncForTest();
                syncStartTick = ticks;
            }
            if (ticks * 200 > timeout * 1000) {
                say("PROBE_FAIL scan-timeout");
                QCoreApplication::exit(4);
                return;
            }
            return;
        }
        if (!cancelSent && cancelAfter > 0 && syncStartTick >= 0
            && (ticks - syncStartTick) * 200 >= cancelAfter) {
            cancelSent = true;
            say(QString("CANCEL sent after %1 ms").arg(cancelAfter));
            // 传输中再按一次开始/取消按钮 = 用户点取消
            panel->startSyncForTest();
        }
        if (panel->syncFinishedCountForTest() > doneBefore) {
            say(QString("SYNC ok=%1 done=%2 total=%3 count=\"%4\" status=\"%5\"")
                    .arg(panel->syncLastOkForTest() ? 1 : 0)
                    .arg(panel->runDoneForTest())
                    .arg(panel->runTotalForTest())
                    .arg(panel->countTextForTest())
                    .arg(panel->statusTextForTest()));
            const QStringList st = panel->fileStatesForTest();
            for (int i = 0; i < st.size(); ++i)
                say(QString("ROW %1 %2").arg(i).arg(st.at(i)));
            const QStringList lg = panel->logTailForTest(60);
            for (int i = 0; i < lg.size(); ++i)
                say(QString("LOG %1").arg(lg.at(i)));
            QCoreApplication::exit(panel->syncLastOkForTest() ? 0 : 1);
            return;
        }
        if (ticks * 200 > timeout * 1000) {
            say(QString("PROBE_FAIL sync-timeout status=\"%1\" count=\"%2\"")
                    .arg(panel->statusTextForTest())
                    .arg(panel->countTextForTest()));
            const QStringList lg = panel->logTailForTest(40);
            for (int i = 0; i < lg.size(); ++i)
                say(QString("LOG %1").arg(lg.at(i)));
            QCoreApplication::exit(5);
            return;
        }
    });
    timer->start();

    return app.exec();
}
