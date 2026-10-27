#pragma once

#include <memory>

#include <QWidget>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QByteArray>
#include <QVector>
#include <QList>
#include "Model.h"
#include "AutoSyncScheduler.h"

class QPushButton;
class QLabel;
class QProgressBar;
class QTextEdit;
class QCheckBox;
class QToolButton;
class QTableView;
class QHeaderView;
class QMenu;
class QThread;
class RsyncRunner;
class FileTableModel;
class ToolColumnDelegate;
class SettingsDialog;
// 注意：ScannerWorker 定义在 SyncPanel.cpp 的匿名 namespace 里，
// 这里**不能**前置声明 —— 前置声明指向的是全局作用域，
// 与匿名 namespace 里的同名类型是两个类型，编译器会报 C2872 不明确符号。

// 同步功能：扫描本地目录 -> 按后缀分组 -> 传到目标机。
//
// 结构照定稿原型，从上到下四块：
//   1) 文件清单卡片（后缀 Tab + 刷新 + 7 列表格）
//   2) 进度区（唯一一根进度条 + 状态 + 计数速率）
//   3) 底部操作条（仅同步新增 / 同步日志 / 开始⇄取消）
//   4) 日志抽屉（默认收起，展开 110px，2000 行 FIFO）
//
// 连接信息全部收在设置弹窗里（手册：根目录收纳于设置），本面板不再有字段区。
class SyncPanel : public QWidget
{
    Q_OBJECT

public:
    explicit SyncPanel(QWidget *parent = nullptr);
    ~SyncPanel() override;

    void saveConfig();
    void loadConfig();

    // 只给自动化测试用。
    TargetConfig effectiveTarget() const { return m_target; }
    int fileCountForTest() const;
    void setLocalRootForTest(const QString &v);
    void setHostForTest(const QString &v);
    void setUserForTest(const QString &v);
    void setKeyPathForTest(const QString &v);
    // 口令同理。定稿 UI 把它放进设置弹窗之后，自动化往 QLineEdit 里打字
    // 是**不生效**的 —— 产品只在点「保存」时才从弹窗取值。
    // 于是"错口令应该连不上"这个场景实际用的是空口令 + 私钥，一路成功，
    // 测出来的"什么都传过去了"看着像产品没拦，实际上根本没走错口令这条路径。
    void setPasswordForTest(const QString &v) { m_target.password = v; }
    // 触发一轮**真实同步**，走的是「开始同步」按钮的同一条链路。
    // 自动化探针用它反复跑同步，再回头检查临时目录里有没有明文口令残留。
    void startSyncForTest() { startOrCancel(); }

    // 自动化自检要拿到面板本身（设目标、触发同步）。
    SyncPanel *panelForTest() { return this; }
    void setRemoteRootForTest(const QString &v);
    void setPortForTest(int v) { m_target.port = v; }
    // 「仅同步新增文件」勾选框。默认是**不勾**的（见构造里读配置那一行），
    // 对应 rsync 的 --checksum：按内容逐个比对，只有新增或改过的文件才传。
    // 勾上则改成 --ignore-existing：远端已有的路径一律不碰。
    //
    // 为什么必须能切换：这两种模式对"同名文件内容变了"的结论完全相反 ——
    // 勾上时不会重传（按设计），取消勾选时走 --checksum 会重传。
    // 只测默认态会把设计当缺陷报，只测取消勾选又测不到客户开箱即用的那一档。
    // 只声明，定义放在 .cpp：这个头里 QCheckBox 只有前向声明，
    // 内联实现要解引用它，会在所有包含者身上报"使用了未定义类型 QCheckBox"。
    void setOnlyNewForTest(bool v);
    bool onlyNewForTest() const;
    void setAutoSyncForTest(bool v);
    bool autoSyncForTest() const;
    // 自动同步的调度状态（只读）。给自动化探针用：
    // 定时器类行为没法在 --selfcheck 里验（不跑事件循环），
    // 但"什么时候该跑""失败了排程推没推进"这些是纯状态，
    // 必须能被外部读到，否则自动同步只能靠人肉盯界面。
    QDateTime autoSyncNextAtForTest() const { return m_autoSyncNextAt; }

    // 面板存活令牌。**必须由析构函数显式置失效**，
    // 不能靠 QPointer 自己清零。
    //
    // 为什么（实测会稳定复现的 Qt 警告）：
    //   QPointer<QObject> 在 ~QObject 才清空，而 SyncPanel 的 vptr
    //   在 ~QWidget 就已经降级成 QWidget —— 两者之间有一个窗口。
    //   后台扫描线程在这个窗口里 invokeMethod 投递结果，
    //   Qt 在 QWidget 的 metaObject 里找不到 deliverScanResult，
    //   就打 "No such method QWidget::deliverScanResult" 的警告。
    //   自动同步本身就会起扫描，所以"自动同步期间关窗"必中这个窗口。
    //
    // 令牌由 shared_ptr 共享给后台线程：析构时先把它置失效，
    // 投递前再查一次 —— 这条路径**不依赖 Qt 的析构顺序**，
    // 窗口彻底消失。
    std::shared_ptr<bool> aliveToken() const { return m_aliveToken; }
    // 自动同步心跳定时器的当前周期（毫秒）。
    //
    // 给自动化判据用：心跳必须跟着间隔走，否则
    // "界面填 10 秒、实际 60 秒才跑"这件事没人能发现
    //（界面照样显示"下次同步 10s 后"，倒计时在走，就是不触发）。
    int autoSyncHeartbeatMsForTest() const
    { return m_autoSyncTimer ? m_autoSyncTimer->interval() : -1; }
    QDateTime autoSyncLastRunAtForTest() const { return m_autoSyncLastRunAt; }
    int         autoSyncIntervalSecForTest() const { return m_autoSyncIntervalSec; }
    bool        autoSyncInFlightForTest() const { return m_autoSyncInFlight; }
    // 手动触发一次心跳（走的是产品自己的 onAutoSyncTick 路径）。
    // 返回 true = 这次真的发起了自动同步。
    bool triggerAutoSyncTickForTest();
    // 直接跑一次"到点判定 + 触发"，不做时间等待。
    bool forceAutoSyncFireForTest();
    // 把下一次触发时间往前挪（模拟"时间到了"），用于测试等待场景。
    void setAutoSyncNextAtForTest(const QDateTime &dt) { m_autoSyncNextAt = dt; }
    void setRulesForTest(const QStringList &suffixes);
    // 灌工具规则。定稿 UI 的工具调用在**表格「工具调用」列**上（delegate 现画按钮），
    // 那条才是用户真实路径；旧的 ToolPanel 已不显示，只测它等于测一条没人走的路。
    // 要测操作列就得能把规则灌进去，否则探针只能去点那个隐藏面板。
    void setToolRulesForTest(const QVector<ToolRule> &rules);
    // 读回当前**生效中**的同步规则（含 remoteSubDir），只给探针用。
    //
    // 为什么需要：remoteSubDir 是配置里真实存在、且同步时真的生效的字段
    // （RsyncRunner 按它分目录落地），但定稿 UI 没有它的输入框，
    // 也就是说用户只能从旧配置继承它。继承有没有成功，
    // 从界面上**看不出来** —— 只能把内存里那条规则读出来对。
    // 只读，不改。
    QVector<SyncRule> syncRulesForTest() const { return m_syncRules; }
    void scanFilesForTest() { scanFiles(); }
    // 产品路径上的「重新扫描」。设置保存后要立刻按新目录/新后缀规则重扫一遍，
    // 不能让用户自己再点一次刷新。名字不带 Test 是因为 MainWindow 也在调。
    void rescanNow() { scanFiles(); }

    // 直接把一份文件清单灌进模型，跳过后台扫描。
    // 截图/断言探针要用它构造"表格里真的有行"的状态：走真实扫描会起后台线程，
    // 探针退出时卡在析构上（实测挂过一次），而比对只需要那个显示状态。
    void injectFiles(const QVector<FileItem> &files) { deliverScanResult(files, false, 0, 0); }

    // 设置弹窗由主窗口持有，这里只提供数据。
    TargetConfig targetConfig() const { return m_target; }
    QString localRoot() const { return m_localRoot; }
    QVector<ToolRule> toolRules() const { return m_toolRules; }
    QVector<SyncRule> syncRules() const { return m_syncRules; }
    void applyFromSettings(const TargetConfig &t, const QString &localRoot,
                           const QVector<ToolRule> &tools, const QVector<SyncRule> &rules);

    FileTableModel *tableModel() const { return m_model; }

    // 只给 offscreen 截图探针用。
    void toggleLogDrawerForProbe() { toggleLogDrawer(); }
    bool isLogOpenForTest() const { return m_logOpen; }

    // 自动化自检要能看见产品自己的日志原文。
    //
    // 没有它就出现了一个很难受的盲区：自检报"没有残留"，但同步可能压根没跑
    // （找不到 rsync / 没匹配到文件 / 口令错），三种失败在"临时目录干不干净"
    // 这一个指标上长得一模一样。实测就栽在这里：连报三轮 PASS，
    // 而接收端连目录都没建出来。判据必须能说出"它到底干了什么"。
    QStringList logTailForTest(int maxLines) const;
    // 每完成一轮同步 +1。自动化自检靠它判断"这一轮真的跑完了"。
    //
    // 之前用 isRunning() 当完成判据，结果 90 秒都等不到"空闲"：
    // 同步早就跑完了（接收端文件已落地、日志也停了），而 isRunning()
    // 在离屏模式下并不跟着落回去。判据绑在内部实现状态上就会这样。
    // 完成计数绑的是产品自己的 allFinished 信号，与实现无关。
    int syncFinishedCountForTest() const { return m_syncFinishedCount; }
    bool syncLastOkForTest() const { return m_syncLastOk; }

    int              m_syncFinishedCount = 0;
    bool             m_syncLastOk = false;
    // 本轮真的传输完成的文件数（区分于「已处理完」）。
    int              m_transferredThisRound = 0;
    // 上一轮是不是「成功但什么都没传」。
    bool             m_lastRoundNoChange = false;
    // 本轮是第几轮（0 = 还没开始过）。
    int              m_currentRoundNo = 0;
    // 「传输进程还在不在」。定稿 UI 把开始/取消合并成同一个按钮之后，
    // 按钮**不再承载**完成态（完成态在状态标签和进度条上），于是自动化
    // 再也没法靠按钮文案判断"跑完了没有"。这里给一个真相来源，
    // 比让每个脚本各自去猜按钮长什么样可靠得多。
    bool isSyncBusyForTest() const;

    // ---- 进度序列探针（只给自动化判据用）----
    //
    // 为什么必须有探针：进度条只有 6px 高，而且只在字节回调那一刻跳一次。
    // 截图能看见的那一格只证明"某时刻是几"，证明不了三件事 ——
    // 单调不减、过程中没有提前打满、终值恰好是 100。
    // 只有把整轮的真实取值序列导出来，判据才能逐项断言。
    //
    // 不用命令行开关：加开关要动 main.cpp（不在这次的范围里），
    // 这里照 --selfcheck-secrets 那套 SRT_SC_* 的约定用环境变量驱动。
    // 设了 SRT_PXP_REPORT 就会自动跑一轮并把序列写进那个文件，见 .cpp。
    int         progressValueForTest() const;
    QStringList progressTraceForTest() const { return m_progressTrace; }
    QString     statusTextForTest() const;
    QString     countTextForTest() const;
    QStringList fileStatesForTest() const;
    int         runDoneForTest() const { return m_runDone; }
    int         runTotalForTest() const { return m_runTotal; }

    // ---- 工具启动：预检判据（真实路径 launchToolForRow 用的就是这一份）----
    //
    // 判定与文案都放在类里、且是 static：判据调的是产品那一份，
    // 照抄一份副本的话，把产品改回原样判据照样全绿 —— 这个坑上一轮踩过
    // （收尾次序判据第一版是"模拟收尾"，变异测试当场证明它是自证的）。
    //
    // isExecutable 由调用方传进来而不是内部去 stat：Windows 上 Qt 5.9 的
    // isExecutable() 对任何存在的文件都返回 true，文件系统根本造不出
    // false 现场。判据要能在 Windows 上跑，就必须把它拆成纯函数；
    // Linux 容器里再用真 chmod -x 的文件验产品那一侧确实传了它。
    enum LaunchVerdict {
        LaunchOk = 0,
        LaunchNoTool,           // 这一行没配工具 / 取不到 exe
        LaunchSourceMissing,    // 源文件在本机不存在（缺陷：静默失败）
        LaunchToolMissing,      // 工具程序不存在
        LaunchToolNotExecutable // 工具程序没有执行权限（Linux）
    };
    static LaunchVerdict preflightLaunch(const QString &toolExe, const QString &sourceFile,
                                         bool isExecutable);
    static QString preflightMessage(LaunchVerdict v, const QString &toolExe,
                                    const QString &sourceFile, const QString &toolName);
    // ASCII 标签，判据用它断言分类，不去肉眼比中文。
    static QString launchVerdictTag(LaunchVerdict v);

    // 启动器型工具（xdg-open / gio open）的良性警告码。
    //
    // 实测（麒麟实机）：xdg-open 打开文件成功、stderr 打印 GLib 断言、
    // 退出码 3。原实现把非 0 一律报红色 ERR，用户看到的是"文件开了，
    // 下面一行却写着 ERR"。判据要求：**只有** stderr 命中这组特征且退出码非 0
    // 才降级，其余非 0 仍然报 ERR。
    static bool isBenignLauncherExit(int exitCode, const QByteArray &stderrTail);
    static QString benignLauncherMessage(const QString &toolName, int exitCode);

    // 启动防抖窗口（毫秒）。同一行在这个窗口内的重复点击直接忽略。
    static int launchDebounceMs() { return 500; }

    // ---- 工具启动探针 ----
    void     launchToolForTest(int row, int toolIndex) { launchToolForRow(row, toolIndex); }
    LaunchVerdict launchPreflightForTest(int row, int toolIndex) const;
    int      toolLaunchAttemptsForTest() const { return m_toolLaunchAttempts; }
    // 被防抖挡掉的次数。这是防抖的**可断言状态**：
    // 只靠"按钮被禁用"的话，判据只能在界面上看出按钮变灰，
    // 证明不了真的没起第二个进程。
    int      toolDebounceSuppressedForTest() const { return m_toolDebounceSuppressed; }
    int      toolFinishedCountForTest() const { return m_toolFinishedCount; }
    int      runningToolCountForTest() const { return m_toolProcs.size(); }
    void     resetToolLaunchStateForTest();
    // 最近若干条日志，每条 "TAG\t正文"（TAG 是 ASCII，正文是原文）。
    // 判据要断言"这一行到底是 ERR 还是 WARN"，
    // 只给 toPlainText 会连徽标底色一起剥掉、看不出是哪一档。
    QStringList logRecordsForTest() const { return m_logRecords; }
    // 日志上限。判据要能**灌满再读回**，不能只断言那行代码里的常量。
    void        appendLogLinesForTest(int n);
    int         logBlockCountForTest() const;
    int         logCapForTest() const;


signals:
    void settingsRequested();
    // 连接测试的异步结果。设置弹窗只负责显示，
    // 真正的连接逻辑在 RsyncRunner 那条已经测通的路径上。
    void testConnectionResult(const QString &text, bool ok, bool testing);

public slots:
    // 设置弹窗点了「测试网络连接」时由 MainWindow 调进来。
    void testConnectionAsync(QWidget *sink);
    void testConnectionWithTarget(const TargetConfig &t, QWidget *sink = nullptr);
    void appendNotice(const QString &text);
    // 往同步日志写一条 INFO。给 SyncPanel 之外的组件用
    // （MainWindow 的外观切换提示就走这条路）。
    //
    // 为什么不让外面直接调 appendLogLine：那个是**私有槽**，
    // 参数是内部的 tag / tagKind 编码，外部调用者不该知道这两个值怎么编码。
    // 这里只暴露"写一条普通信息"这一件事。
    void logInfo(const QString &msg);
    // 日志抽屉的开关。放 public 是因为 MainWindow 的 switchPageForTest()
    // 要在**状态已经对得上时不动作**（避免探针连调两次把抽屉反复开关），
    // 那是 private 就只能盲目 toggle。
    void toggleLogDrawer();

private slots:
    void refreshScanClicked();
    void onTabClicked();
    void onHeaderCheckToggled(bool on);
    void onToolButtonClicked(int row, int toolIndex);
    void onToolMoreClicked(int row, int firstHidden);
    void onRowDoubleClicked(const QModelIndex &index);
    void onSelectAllToggled(bool on);
    void startOrCancel();
    void startSyncInternal();
    void launchToolForRow(int row, int toolIndex);
    void onToolFinished(int exitCode, QProcess::ExitStatus status);
    void onToolError(QProcess::ProcessError error);
    // 工具的 stderr 现在是**自己收**的（不再 ForwardedChannels），
    // 因为启动器型工具的良性警告码只能从 stderr 认。
    // 收上来的内容原样转发到本进程的 stderr，一行都不少地还给用户。
    void onToolStderrReady();

    void copyLog();
    void clearLog();
    void appendLogLine(const QString &tag, const QString &msg, int tagKind);

    // RsyncRunner 回调
    void onFileStarted(const QString &relativePath);
    void onFileTransferred(const QString &relativePath, int done, int total);
    // 本组里被 rsync 报失败的文件：把已经被标成「已完成」的行改回「失败」。
    void onFileFailedInGroup(const QString &relativePath);
    void onByteProgress(int percent);
    void onByteStats(qint64 bytes, double bytesPerSec);
    void onAllFinished(bool ok);
    void onRunLog(const QString &text);
    void onScanFinished(const QVector<FileItem> &files);
    // ScannerWorker 在后台线程跑完，用排队连接把结果送回界面线程。
    // 必须注册 FileItem 元类型，否则 Qt 会把这个调用**直接丢掉** ——
    // 现象是点了刷新扫描永远没反应。
    Q_INVOKABLE void deliverScanResult(const QVector<FileItem> &files, bool truncated,
                                      int skippedLinks, int unreadableDirs);
    // ConnTestWorker 的回程。testing=false 表示这一条是最终结果。
    Q_INVOKABLE void deliverConnResult(const QString &detail, bool ok);

    void tickElapsed();
    void onAutoSyncToggled(bool on);
    void onAutoSyncTick();
    // 倒计时专用的 1 秒心跳（自动同步开着的时候才跑）。
    void onAutoSyncUiTick();

    // ---- 自动同步（需求 10）后台 ----
    //
    // 调度语义：每 intervalSec 秒跑一次，**从上一次真正开跑的时刻起算**。
    // 用绝对时间点（m_autoSyncNextAt）而不是倒计时计数器，原因：
    //   倒计时会在手动同步完成时被重置（onAllFinished 里），
    //   于是用户点一次开始同步就悄悄挪动了自动节奏，节奏变得不可预期。
    //   绝对时间点不会：手动同步不碰 m_autoSyncLastRunAt。
    //
    // 触发时**强制重扫 + 全选**，与手动共用 startSyncInternal 同一发起链路。
    // 不能沿用用户勾选：自动同步的语义是"把该传的都传了"，
    // 而用户上次手动取消勾选只是那一次的临时决定。
    void autoSyncScheduleNext(bool fromNow = false);
    // 把心跳周期对齐到当前间隔。
    //
    // 三处调用点（构造 / 勾选 / 保存设置）必须共用这一个，
    // 少一处就会出现"界面填 10 秒、实际 60 秒才跑"——
    // 界面照样显示"下次同步 10s 后"，倒计时在走，就是不触发，
    // 客户完全看不出哪里不对（实测探针：设 10 秒，59 秒后才跑）。
    void applyAutoSyncHeartbeat();
    // 到点真正开跑。返回 false 表示这次没跑成。
    //
    // 「正在忙」这个守卫**不在这里**，而在 onAutoSyncTick()（忙时跳过这一拍、
    // 下一拍再来）。这里只管"配置不全"这一类失败。
    bool autoSyncFire();
    // 记下"这一次真正开跑了"的时刻，并据此排下一次。
    // **只在自动路径上调用**，手动同步不碰它（手动不该挪动自动节奏）。
    //
    // 落点是"下一次触发"：间隔模式是 intervalSec 之后，
    // 每天定点模式是**下一个 HH:mm**（定点模式下间隔不参与计算）。
    void autoSyncMarkRunStarted();

    // 进度序列探针的起跑回调。**必须是 slot**：startProgressProbe() 里用
    // 老式 SIGNAL/SLOT 排队调用它，写成普通成员函数 moc 不会把它登记进
    // 元对象，连接静默失败 —— 现象是探针设了环境变量也永远不跑、程序挂着不退。
    void beginProgressProbeRun();

private:
    void buildUi();
    // 复位主按钮。文案、objectName、样式三样都要动，只改文案会让按钮
    // 永远停在"运行中"的红色上 —— 见实现里的说明。
    void setStartButtonIdle();
    void rebuildTabs();
    void applyFilter(const QString &suffixFilter);
    void startScanAsync();
    void setBusy(bool busy);
    void setStatusLine(const QString &text, int kind);   // 0 就绪 / 1 完成 / 2 取消 / 3 进行中
    // 计数行文本。tickElapsed 每秒调一次、onAllFinished 收尾还要补一次，
    // 两处共用它，"分子变了行上的数也跟着变"才成立。
    //
    // 这个声明原来漏了：.cpp 里定义并调用了，头里没有声明，
    // 于是整个工程根本编不过（error C2039: 不是 SyncPanel 的成员）。
    void renderCountLabel();
    void updateAutoSyncStatusUi();

    // ---- 进度口径。口径只在这一处定义，别的函数都调它 ----
    void rebuildGroupSizes(const QVector<FileItem> &picked);
    int  groupIndexOfSuffix(const QString &suffix) const;
    int  globalProgressPercent() const;
    // 进度条的**唯一**写入口。探针的取值序列也从这里取，
    // 保证导出来的就是真实显示值，不是绕过它改出来的值。
    void setProgressValue(int v, const char *tag);
    void startProgressProbe();
    void writeProgressProbeReport();
    void scanFiles();
    QString currentScanKey() const;
    QString absolutePathForRow(int row) const;
    ToolRule toolRuleForRow(int row) const;
    // 日志抽屉：2000 行 FIFO，超出淘汰最旧（手册 §3）。
    void pushLogLine(const QString &line);

    // ---- 带宽限制（底栏）----
    //
    // 菜单每次展开前重铺一遍：勾选状态必须跟着当前值走，
    // 不能靠"建菜单时记下的那个值"—— 那会漏掉别的窗口改过配置的情况。
    void rebuildBandwidthMenu();
    // 弹自定义输入框，收客户自己填的数值（值域由 ConfigStore 给）。
    void askCustomBandwidthLimit();
    // 应用一次档位变更：存盘 + 刷新按钮文字 + 写日志。
    void applyBandwidthLimit(int mbps, bool custom);
    // 档位 → 按钮上显示的文字（不限速 / 12 MB/s）。
    static QString bandwidthDisplayText(int mbps);

    FileTableModel   *m_model = nullptr;
    ToolColumnDelegate *m_toolDelegate = nullptr;
    QTableView       *m_table = nullptr;
    QHeaderView      *m_checkHeader = nullptr;
    QList<QPushButton*> m_tabs;
    QPushButton      *m_refresh = nullptr;

    QLabel           *m_statusLabel = nullptr;
    QLabel           *m_countLabel = nullptr;
    QProgressBar     *m_progress = nullptr;
    QCheckBox        *m_onlyNew = nullptr;
    QCheckBox        *m_chkAutoSync = nullptr;
    // 带宽限制选择器（QToolButton + QMenu，**不是** QComboBox）。
    //
    // 为什么不是下拉框：选项超过 3 个时只露 2 个、第 3 格是「更多…」，
    // QComboBox 的弹出列表绑死在它自己的 model 上，折叠得另写一套机制；
    // 用按钮 + 菜单可以直接表达"两个常用项 + 一个展开入口"，
    // 而且按钮上永远显示**当前值**，不受"当前值恰好不在露出的两项里"影响。
    //
    // 档位来自 ConfigStore::bandwidthLimitOptionsMBps()，不在这里另写一份；
    // 「自定义…」那一项由界面自己加，值域取自 ConfigStore 的上下限。
    QToolButton     *m_bwLimit = nullptr;
    QTimer           *m_autoSyncTimer = nullptr;
    // 「下次同步 N 秒」倒计时专用的 1 秒定时器。
    //
    // 为什么不能跟着心跳走：心跳周期 = clamp(间隔/2, 5, 60) 秒，
    // 间隔 15 秒时心跳 7 秒 —— 界面上的倒计时会连续 7 秒一动不动，
    // 然后突然跳一格，用户看着像卡住。
    QTimer           *m_autoSyncUiTimer = nullptr;
    int               m_autoSyncIntervalSec = AutoSyncScheduler::DefaultIntervalSec;
    // 下一次自动触发的**绝对**时刻。无效 = 当前没有排程。
    QDateTime         m_autoSyncNextAt;
    // 面板存活令牌。public 的 aliveToken() 把同一个 shared_ptr 交给后台线程，
    // 析构时显式置失效（不依赖 QPointer 的清零时机）。
    std::shared_ptr<bool>  m_aliveToken = std::shared_ptr<bool>(new bool(true));
    // 上一次**真正开跑**的时刻。用来算下一次（next = last + interval）。
    // 手动同步不写它，所以手动操作不会挪动自动节奏。
    QDateTime         m_autoSyncLastRunAt;
    // 自动触发这一轮已经发起（正在等扫描完成）。防止心跳在扫描期间
    // 重复发起 —— 一次自动同步只该跑一轮。
    bool              m_autoSyncInFlight = false;
    // 计数行尾巴「· 下次同步 Ns 后」。由 updateAutoSyncStatusUi() 写，
    // 由 renderCountLabel() 拼到行尾 —— 单一写入点，避免两处 setText
    // 互相覆盖导致这一行一秒出现一秒消失。
    QString           m_autoSyncSuffix;
    // 上一次自动同步**没能跑起来**的原因。用于日志去重：
    // 同一个原因只提示一次，不然后台日志会被同一条 WARN 刷爆
    // （实测：每秒一条，真正有用的报错被埋掉）。
    // 用户改完配置后要清掉它，这样下次失败会重新提示。
    QString           m_autoSyncLastFailReason;
    QPushButton      *m_startBtn = nullptr;
    QPushButton      *m_toggleLog = nullptr;
    QWidget          *m_logDrawer = nullptr;
    // 日志必须是 QTextEdit 而不是 QPlainTextEdit：原型每行是
    // [时间戳] + 带底色的小徽标 + 正文 三段，徽标要各自一块背景色，
    // 纯文本控件画不出来（只能靠空格对齐，反而在等宽字体下更歪）。
    QTextEdit        *m_logView = nullptr;
    QPushButton      *m_logToggleLabel = nullptr;

    TargetConfig      m_target;
    QString           m_localRoot;
    QVector<ToolRule> m_toolRules;
    QVector<SyncRule> m_syncRules;
    // m_syncRules 里这批是不是**程序扫目录认出来的**、而不是用户配的。
    // 为 true 时 saveConfig() 不把它们写回配置文件 —— 用户把同步规则留空
    // 本来就表示"传所有文件"，自动生成的那条只该在本次会话里管用，
    // 落盘就把这个意思永久改掉了。见 saveConfig 里的说明。
    bool              m_rulesAutoGenerated = false;

    RsyncRunner      *m_runner = nullptr;
    QThread          *m_scanThread = nullptr;
    QString           m_scanKey;
    bool              m_syncRequested = false;
    // 扫描是否在飞。用来挡住**并发扫描**。
    //
    // 原来 startScanAsync() 没有任何重入保护，而「开始同步」这条路径
    // （startSyncInternal 发现 m_scanKey 对不上 -> scanFiles()）完全绕过了
    // 刷新按钮上的 setEnabled(false)。于是扫描在飞的时候再点一次开始同步，
    // 会**并发拉起第二个 ScannerWorker**。慢盘上两次扫描都还在跑的时候，
    // 先回来的那个会 startSyncInternal 把传输拉起来；后回来的那个
    // 再跑一遍 onScanFinished()，把 m_model 的文件清单、m_runDone/m_runTotal、
    // 进度条和状态行**在传输进行中整个刷掉**。
    // 实测形状：扫描只要 1ms 时，第二击正好落在传输已启动的那 40ms 窗口里，
    // 于是变成"双击 = 启动后立刻取消"；扫描要几秒时，则是清单和进度被换掉。
    bool              m_scanning = false;
    bool              m_busy = false;
    bool              m_logOpen = false;
    QString           m_filterSuffix;
    int               m_currentTab = 0;
    int               m_runDone = 0;
    int               m_runTotal = 0;
    // 本轮按后缀分组后的「后缀 + 文件数」。RsyncRunner 内部也按同一算法分组，
    // 但 m_groups 是它的私有成员、面板拿不到 —— 而全局完成度必须知道每组多大，
    // 否则只能拿到"组内百分比"这一个没有分母的数（缺陷 2）。
    QVector<QString>  m_groupSuffixes;
    QVector<int>      m_groupSizes;
    // 当前正在传的组（-1 = 还没进任何组）和它的**组内**字节百分比。
    int               m_groupIndex = -1;
    int               m_groupPercent = 0;
    // 探针记录。m_progressTrace 每项是 "<取值>:<改它的原因>"，
    // 连续重复的同一项不留（否则一组几百个字节回调会把序列撑爆）。
    QStringList       m_progressTrace;
    // 非空时整轮跑完就把探针报告写到这个文件并退出进程（自动化用）。
    QString           m_probeReportPath;
    QStringList       m_runRelativeOrder;   // 传输回包的顺序，用于把状态落到正确的行
    // 本轮选中、还没拿到回包的行号。状态药丸靠它保证**同时只有一行在闪**：
    // 一收到"某个文件传完"就把该行置为已完成，再把下一行置为同步中。
    //
    // 为什么要单独维护：StateSyncing / StateFailed / StateSkipped 三个状态
    // 定义了、delegate 也画了，可全库原来只有一处 setFileState(StateOk) ——
    // 同步过程中所有行一直停在「待传输」，然后直接跳「已完成」。
    // 「同步中」药丸和那个脉冲点从来没在真实运行里出现过，失败的文件更是
    // 永远显示「待传输」，用户根本分不出哪个文件没传成功。
    QVector<int>      m_pendingRows;

    // 本轮里 rsync 明确报失败的文件（相对路径）。
    //
    // 为什么单独记一份：stdout 与 stderr 是两条管道、两个读线程，
    // 报错的行（stderr）可能先到、"下一个文件开始"的行（stdout）后到。
    // 迟到的 stdout 行会把刚判失败的行又改回「✓ 已完成」——
    // 而它远端确实一个字节都没有。实测（80 个文件全部写失败）：
    // 偶发 1 个文件显示成 ok。
    //
    // 所以失败要**在收尾时再钉一次**（见 onAllFinished）。
    QSet<QString>    m_failedPaths;

    QTimer           *m_elapsedTimer = nullptr;
    qint64            m_runStartMs = 0;
    // 最近一次速率和已过秒数：tickElapsed 每秒刷一次，没有缓存就只能读一次算一次，
    // 而速率只随字节进度回调更新，不缓存会在两次回调之间显示成 0。
    double            m_lastBytesPerSec = 0.0;
    // 最近一次字节进度里的"已传总量"。和速率同一个道理：不缓存的话，
    // tickElapsed 在两次字节回调之间只能显示 0 GB。
    qint64            m_lastBytes = 0;
    // 已经结束的那些组累计下来的字节。rsync 每组都从 0 重新报本组字节
    // （RsyncRunner::onByteStats 发的是单次调用的进度），直接赋给
    // m_lastBytes 的话，组一切换显示就从上一组的总量掉回本组的小数字。
    // 实测：3 组（1GB x3 + 小文件组）传完 3.22 GB，计数栏显示「已传 39.1 MB」，
    // 正好是最后一组的量 —— 和真实传输量差 8.7 万倍。
    qint64            m_groupBytesBase = 0;
    qint64            m_elapsedSecs = 0;

    // 手动拉起工具的进程。独立拉起：不 waitForFinished，工具之间并行。
    QVector<QProcess *> m_toolProcs;
    QHash<QProcess *, QString> m_toolNames;
    // 每个工具进程收到的 stderr 尾部。只用于判退出码的性质
    // （良性警告码 vs 真失败），不拿它当用户可见日志。
    QHash<QProcess *, QByteArray> m_toolStderr;
    // 同一行上一次**真正启动**的时刻（毫秒，QElapsedTimer 基准）。
    // 连点「打开」会起一串进程：工具窗口开两份、文件被同一工具打开两次，
    // 而界面毫无异常。这里挡住的是**真的多起进程**，
    // 不是把按钮禁用 —— 按钮禁用的那 500ms 里用户看不见发生了什么。
    QHash<int, qint64>  m_toolLaunchAtMs;
    int                 m_toolLaunchAttempts = 0;      // 真正 start() 过的次数
    int                 m_toolDebounceSuppressed = 0; // 被防抖挡掉的次数
    int                 m_toolFinishedCount = 0;      // 走到 finished 的次数
    // 日志的「TAG + 正文」流水（判据用），只留最近 500 条。
    QStringList         m_logRecords;
    QElapsedTimer       m_toolClock;

};
