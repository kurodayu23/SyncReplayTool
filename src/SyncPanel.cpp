#include "SyncPanel.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFrame>
#include <QHeaderView>
#include <QTableView>
#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QProgressBar>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QClipboard>
#include <QMenu>
#include <QCursor>
#include <QAction>
#include <QApplication>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QTimer>
#include <QStringList>
#include <QSet>
#include <QTime>
#include <QScrollBar>
#include <QStyle>
#include <QRunnable>
#include <QThreadPool>
#include <QPointer>
#include <cstdio>

#include "Theme.h"
#include "FileTableModel.h"
#include "ToolColumnDelegate.h"
#include "StatusPillDelegate.h"
#include "ConfigStore.h"
#include "RsyncRunner.h"
#include "FileScanner.h"
#include "ToolRunner.h"

namespace {

// 后台扫描。FileScanner::scan 是**静态方法**，既没有对象也没有信号，
// 十万级目录在主线程上递归十几秒，那段时间整个窗口没响应。
// 所以这里自己包一个 QRunnable，跑完把结果发回界面线程。
class ScannerWorker : public QRunnable
{
public:
    ScannerWorker(const QString &root, const QStringList &suffixes, QObject *sink)
        : m_root(root), m_suffixes(suffixes), m_sink(sink) {}

    void run() override
    {
        const QVector<FileItem> files = FileScanner::scan(m_root, m_suffixes);
        const bool truncated = FileScanner::wasTruncated();
        // 投递之前必须确认接收者还活着。
        //
        // 原来这里无条件 invokeMethod(m_sink, ...)：目录很大时扫描要几十秒，
        // 用户在这个窗口里点关闭，SyncPanel 析构、m_sink 变成野指针，
        // 后台线程再调一次 invokeMethod 就是 use-after-free ——
        // 表现是关闭时崩溃、退出码异常，麒麟上还可能给客户的运维留 core dump。
        //
        // 用 QPointer 而不是裸指针：对象析构时它自动置空，不用改任何生命周期代码。
        // 这里**不能**改成"析构里 waitForDone()" —— 那会把"关窗卡住"
        // 这个已经修过的问题带回来（FileScanner.cpp 里记着那个坑：
        // 扫描遇到目录环会一直不返回）。
        if (m_sink.isNull())
            return;
        QMetaObject::invokeMethod(m_sink, "deliverScanResult",
                                  Qt::QueuedConnection,
                                  Q_ARG(QVector<FileItem>, files),
                                  Q_ARG(bool, truncated));
    }

private:
    QString m_root;
    QStringList m_suffixes;
    QPointer<QObject> m_sink;
};

// 连接探测跑在后台线程：RsyncRunner::probeConnection 是**阻塞**的
// （要等 ssh 退出，最长 15 秒），放界面线程会让整个窗口在这段时间点不动，
// 用户会以为软件卡死了，然后狂点关闭。
class ConnTestWorker : public QRunnable
{
public:
    ConnTestWorker(const TargetConfig &t, RsyncRunner *runner, QObject *sink)
        : m_target(t), m_runner(runner), m_sink(sink) {}

    void run() override
    {
        // 同 ScannerWorker：探测最长要等 15 秒，这期间用户很可能已经关窗。
        // m_runner 指向的 RsyncRunner 也可能已经被析构，所以两处都要判空。
        if (m_sink.isNull() || m_runner.isNull())
            return;
        QString detail;
        const bool ok = m_runner->probeConnection(m_target, &detail);
        if (m_sink.isNull())
            return;
        QMetaObject::invokeMethod(m_sink, "deliverConnResult",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, detail),
                                  Q_ARG(bool, ok));
    }

private:
    TargetConfig m_target;
    QPointer<RsyncRunner> m_runner;
    QPointer<QObject> m_sink;
};

QString T(const char *s) { return QString::fromUtf8(s); }

// 命令行模板拆词：按空格分，但引号内的内容算一个整体。
// 使用手册 §4：「多个参数用空格分开」。用户会在参数里写 "C:\带空格的路径"，
// 按空格硬拆会把路径撕成两段，工具收到的就是错参数。
const char *kTabAll      = "\xE5\x85\xA8\xE9\x83\xA8";                      // 全部
const char *kRefresh     = "\xE5\x88\xB7\xE6\x96\xB0";                      // 刷新
const char *kOnlyNew     = "\xE4\xBB\x85\xE5\x90\x8C\xE6\xAD\xA5\xE6\x96\xB0\xE5\xA2\x9E\xE6\x96\x87\xE4\xBB\xB6";  // 仅同步新增文件
const char *kStartSync   = "\xE5\xBC\x80\xE5\xA7\x8B\xE5\x90\x8C\xE6\xAD\xA5";  // 开始同步
const char *kCancelSync  = "\xE5\x8F\x96\xE6\xB6\x88";                        // 取消
const char *kLogOpen     = "\xE5\x90\x8C\xE6\xAD\xA5\xE6\x97\xA5\xE5\xBF\x97 \xE2\x96\xBE";  // 同步日志 ▾
const char *kLogClose    = "\xE5\x90\x8C\xE6\xAD\xA5\xE6\x97\xA5\xE5\xBF\x97 \xE2\x96\xB4";  // 同步日志 ▴
const char *kLogTitle    = "\xE5\xAE\x9E\xE6\x97\xB6\xE4\xBC\xA0\xE8\xBE\x93\xE6\xB5\x81\xE6\xB0\xB4\xE6\x97\xA5\xE5\xBF\x97";  // 实时传输流水日志
const char *kCopyLog     = "\xE5\xA4\x8D\xE5\x88\xB6\xE6\x97\xA5\xE5\xBF\x97";  // 复制日志
const char *kClearLog    = "\xE6\xB8\x85\xE7\xA9\xBA";                      // 清空
const char *kStatusReady = "\xE7\x8A\xB6\xE6\x80\x81\xEF\xBC\x9A\xE5\xB0\xB1\xE7\xBB\xAA";  // 状态：就绪
const char *kStatusRun   = "\xE7\x8A\xB6\xE6\x80\x81\xEF\xBC\x9A\xE5\x90\x8C\xE6\xAD\xA5\xE4\xB8\xAD";  // 状态：同步中
const char *kStatusDone  = "\xE7\x8A\xB6\xE6\x80\x81\xEF\xBC\x9A\xE5\xB7\xB2\xE5\xAE\x8C\xE6\x88\x90";  // 状态：已完成
const char *kStatusStop  = "\xE7\x8A\xB6\xE6\x80\x81\xEF\xBC\x9A\xE5\xB7\xB2\xE5\x81\x9C\xE6\xAD\xA2";  // 状态：已停止
const char *kNoTool      = "\xE6\x9C\xAA\xE4\xB8\xBA\xE8\xAF\xA5\xE6\x96\x87\xE4\xBB\xB6\xE9\x85\x8D\xE7\xBD\xAE\xE8\xB0\x83\xE7\x94\xA8\xE5\xB7\xA5\xE5\x85\xB7";  // 未为该文件配置调用工具

class CheckHeaderView : public QHeaderView
{
public:
    explicit CheckHeaderView(Qt::Orientation orientation, QWidget *parent = nullptr)
        : QHeaderView(orientation, parent), m_checked(true) {}

    void setChecked(bool c) {
        if (m_checked != c) {
            m_checked = c;
            viewport()->update();
        }
    }
    bool isChecked() const { return m_checked; }

    std::function<void(bool)> onCheckToggled;

protected:
    void paintSection(QPainter *painter, const QRect &rect, int logicalIndex) const override
    {
        painter->save();
        QHeaderView::paintSection(painter, rect, logicalIndex);
        painter->restore();

        if (logicalIndex == 0) {
            painter->save();
            painter->setRenderHint(QPainter::Antialiasing, true);
            const int sz = 14;
            const int x = rect.left() + (rect.width() - sz) / 2;
            const int y = rect.top() + (rect.height() - sz) / 2;
            QRect chkBox(x, y, sz, sz);
            if (m_checked) {
                painter->setPen(QPen(Theme::primary(), 1));
                painter->setBrush(Theme::primary());
                painter->drawRoundedRect(chkBox, 3, 3);

                QPen wpen(Qt::white);
                wpen.setWidthF(1.8);
                wpen.setCapStyle(Qt::RoundCap);
                wpen.setJoinStyle(Qt::RoundJoin);
                painter->setPen(wpen);
                painter->drawLine(QPointF(x + 2.8, y + 6.8), QPointF(x + 5.5, y + 9.8));
                painter->drawLine(QPointF(x + 5.5, y + 9.8), QPointF(x + 11.2, y + 3.8));
            } else {
                painter->setPen(QPen(Theme::border(), 1));
                painter->setBrush(QColor(0x1e, 0x22, 0x2b));
                painter->drawRoundedRect(chkBox, 3, 3);
            }
            painter->restore();
        }
    }

    void mousePressEvent(QMouseEvent *e) override
    {
        if (e->button() == Qt::LeftButton) {
            int idx = logicalIndexAt(e->pos());
            if (idx == 0) {
                m_checked = !m_checked;
                viewport()->update();
                if (onCheckToggled)
                    onCheckToggled(m_checked);
                e->accept();
                return;
            }
        }
        QHeaderView::mousePressEvent(e);
    }

private:
    bool m_checked;
};

} // namespace

// ---------------------------------------------------------------- 构造 / 析构

SyncPanel::SyncPanel(QWidget *parent)
    : QWidget(parent)
{
    buildUi();

    m_runner = new RsyncRunner(this);
    connect(m_runner, SIGNAL(fileStarted(QString)),
            this, SLOT(onFileStarted(QString)));
    connect(m_runner, SIGNAL(fileTransferred(QString,int,int)),
            this, SLOT(onFileTransferred(QString,int,int)));
    connect(m_runner, SIGNAL(fileFailedInGroup(QString)),
            this, SLOT(onFileFailedInGroup(QString)));
    connect(m_runner, SIGNAL(byteProgress(int)),
            this, SLOT(onByteProgress(int)));
    connect(m_runner, SIGNAL(byteStats(qint64,double)),
            this, SLOT(onByteStats(qint64,double)));
    connect(m_runner, SIGNAL(allFinished(bool)),
            this, SLOT(onAllFinished(bool)));
    connect(m_runner, SIGNAL(logText(QString)),
            this, SLOT(onRunLog(QString)));

    m_elapsedTimer = new QTimer(this);
    m_elapsedTimer->setInterval(1000);
    connect(m_elapsedTimer, SIGNAL(timeout()), this, SLOT(tickElapsed()));

    // 防抖用单调时钟。QDateTime::currentMSecsSinceEpoch() 会因为系统时间
    // 被调（现场机器对时、NTP 跳一下）而倒退，倒退的那一次窗口会变成负数、
    // 于是接下来一直点一直被吞。
    m_toolClock.start();

    loadConfig();
    // 原来这里是 setStatusLine(T(kStatusDone), 1) —— 程序一打开状态栏就写
    // 「状态：已完成」，而此时一次同步都没发生过。空界面配一个"已完成"，
    // 等于把"什么都没做"显示成"做完了"，用户会以为文件已经传过去过一次。
    setStatusLine(T(kStatusReady), 0);

    // 这里原来有两行"方便演示"的逻辑，两行都删了：
    //
    //   if ((m_localRoot.isEmpty() || !QDir(m_localRoot).exists())
    //       && QDir("D:/ReplayData").exists()) {
    //       m_localRoot = "D:/ReplayData"; ... }
    //   QTimer::singleShot(30, this, SLOT(refreshScanClicked()));
    //
    // 删的理由不是"看着不专业"，是它会**凭空替用户决定数据目录**。
    // 用户第一次打开、什么都没配，程序却自作主张指向 D:/ReplayData 并
    // 30ms 后自动扫描 —— 于是表格里凭空出现 7 个文件、tab 按后缀分好组，
    // 看着像"程序已经连上设备了"。真实情况是用户一台机器都没配。
    // 这是最坏的一类假象：它把"空的"显示成"就绪的"。
    //
    // 客户要求打开程序时该是空的：没配就是没配，配了才扫。
    // 注意 loadConfig() 仍会读回用户自己存过的目录 —— 那是他自己的配置，
    // 该显示就显示，不在这里替他抹掉。
    //
    // 但"配了才扫"不等于"配了也不扫"。原来这两行删干净之后，只剩下设置面板里
    // 的「保存设置」这一条能触发 rescanNow()，于是老客户重启程序看到的是
    // 一张**空表格**：目录填得好好的、文件一个不显示、标题栏也没有刷新按钮，
    // 唯一能出数据的动作是"打开设置再点一次保存"。实测就是这么卡住的。
    //
    // 这里的取舍：只扫**用户自己存过**的目录，扫不扫得动交给 scanFiles() 去判断。
    //   - 目录为空        -> 什么都不扫，界面保持空，状态栏是"就绪"。
    //     （绝不替用户猜目录，见上面被删掉的那两行的理由。）
    //   - 目录有值        -> 扫。存在就读；读不了 / 不存在 / 指向一个文件，
    //     三种各说各的（见 scanFiles() 里那三条分支）。
    //
    // 原来这里多带一个 QDir(m_localRoot).exists()：目录不存在时**启动阶段完全不扫**，
    // 界面上是**一片空白** —— 连「全部 (N)」这个页签都没有，日志抽屉里一个字没有，
    // 状态栏只写"就绪"。用户看到的是"软件没反应"，而真实原因是**他自己配的那个目录没了**
    // （U 盘拔了、网络盘没挂上、路径打错一个字）。三句解释都写在 scanFiles() 里，
    // 但这道门把它们全挡在外面，只有用户自己去点「开始同步」才会说出来。
    // 而这一类和「这个目录里确实没有匹配的文件」在用户眼里完全一样，
    // 它偏偏又是客户最常遇到的那一种。
    //
    // 客户要求：软件打开时表格保持空白初始状态，不自动执行扫描填充。
    // 但必须显示「全部 (0)」Tab 页签，保持界面结构完整，避免顶部空白。
    rebuildTabs();

    // 自动化进度探针。没设 SRT_PXP_REPORT 时这一句直接返回，产品路径一行不多走。
    startProgressProbe();
}

SyncPanel::~SyncPanel()
{
    // QProcess 析构只警告不断进程。手动拉起的工具不显式收尾就会变成孤儿继续跑。
    const QVector<QProcess *> procs = m_toolProcs;
    for (int i = 0; i < procs.size(); ++i) {
        QProcess *p = procs.at(i);
        if (p->state() != QProcess::NotRunning) {
            p->terminate();
            if (!p->waitForFinished(800))
                p->kill();
        }
    }
    m_toolProcs.clear();
}

// ---------------------------------------------------------------- UI

void SyncPanel::buildUi()
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setContentsMargins(18, 14, 18, 14);
    root->setSpacing(12);

    // ---- 1. 文件清单卡片 ----
    QFrame *card = new QFrame;
    card->setObjectName(QStringLiteral("listCard"));
    QVBoxLayout *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(10, 0, 10, 10);
    cardLay->setSpacing(0);

    QFrame *head = new QFrame;
    head->setObjectName(QStringLiteral("cardHeader"));
    head->setFixedHeight(32);
    QHBoxLayout *headLay = new QHBoxLayout(head);
    headLay->setContentsMargins(0, 0, 0, 0);

    QWidget *tabHost = new QWidget;
    QHBoxLayout *tabsLay = new QHBoxLayout(tabHost);
    tabsLay->setContentsMargins(0, 0, 0, 0);
    tabsLay->setSpacing(3);
    m_tabs.clear();
    for (int i = 0; i < 12; ++i) {
        QPushButton *b = new QPushButton(tabHost);
        b->setObjectName(QStringLiteral("fileTab"));
        b->setCheckable(true);
        b->setAutoExclusive(true);
        b->setFixedHeight(22);
        b->setCursor(Qt::PointingHandCursor);
        b->setVisible(false);
        connect(b, SIGNAL(clicked()), this, SLOT(onTabClicked()));
        tabsLay->addWidget(b);
        m_tabs.append(b);
    }
    tabsLay->addStretch(1);
    headLay->addWidget(tabHost);

    m_refresh = new QPushButton(T(kRefresh));
    m_refresh->setObjectName(QStringLiteral("refreshScan"));
    m_refresh->setFixedHeight(22);
    m_refresh->setCursor(Qt::PointingHandCursor);
    m_refresh->setIcon(QIcon(Theme::refreshIcon(12, Theme::textSub())));
    m_refresh->setIconSize(QSize(12, 12));
    // 刷新按钮不再显示。
    //
    // 用户的诉求是「保存设置后自动刷新，不用这个刷新按钮」：改完设置点保存，
    // 清单自己就更新了（见 applyFromSettings 末尾），还留一个按钮等于
    // 告诉用户「你得记得点一下才生效」。
    //
    // 控件本身保留而不是删掉：refreshScanClicked() 仍是内部重扫的编程入口，
    // m_refresh 也还挂在 setBusy/setEnabled 的几处状态切换上。
    // 隐藏的控件在 QHBoxLayout 里不占空间。
    m_refresh->setVisible(false);
    headLay->addWidget(m_refresh);
    cardLay->addWidget(head);

    QFrame *hline = new QFrame;
    hline->setObjectName(QStringLiteral("cardHeaderLine"));
    hline->setFixedHeight(1);
    cardLay->addWidget(hline);

    m_table = new QTableView;
    m_model = new FileTableModel(this);
    m_toolDelegate = new ToolColumnDelegate(this);
    m_table->setModel(m_model);
    m_table->setItemDelegateForColumn(FileTableModel::ColTool, m_toolDelegate);
    // 状态列自己画药丸：QSS 画不出「按文字宽度自适应 + 9999 圆角」的胶囊。
    m_table->setItemDelegateForColumn(FileTableModel::ColStatus, new StatusPillDelegate(this));
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setShowGrid(false);
    m_table->setWordWrap(false);
    m_table->setSortingEnabled(false);
    m_table->setCornerButtonEnabled(false);
    // 手册 §2：表格区固定 235px，纵向滚动，**绝不出现横向滚动条**。
    m_table->setFixedHeight(235);
    m_table->horizontalHeader()->setHighlightSections(false);
    m_table->verticalHeader()->setVisible(false);
    // 原型 td padding 6px + 按钮 23px = 行高 35px。
    m_table->verticalHeader()->setDefaultSectionSize(35);
    m_table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    CheckHeaderView *chv = new CheckHeaderView(Qt::Horizontal, m_table);
    chv->onCheckToggled = [this](bool on) {
        onHeaderCheckToggled(on);
    };
    m_checkHeader = chv;
    m_table->setHorizontalHeader(chv);
    QHeaderView *hv = chv;
    hv->setSectionResizeMode(FileTableModel::ColCheck,    QHeaderView::Fixed);
    hv->setSectionResizeMode(FileTableModel::ColName,     QHeaderView::Stretch);
    hv->setSectionResizeMode(FileTableModel::ColSize,     QHeaderView::Fixed);
    hv->setSectionResizeMode(FileTableModel::ColCreated,  QHeaderView::Fixed);
    hv->setSectionResizeMode(FileTableModel::ColModified, QHeaderView::Fixed);
    hv->setSectionResizeMode(FileTableModel::ColStatus,   QHeaderView::Fixed);
    hv->setSectionResizeMode(FileTableModel::ColTool,     QHeaderView::Fixed);
    m_table->setColumnWidth(FileTableModel::ColCheck,    36);
    m_table->setColumnWidth(FileTableModel::ColSize,     75);
    m_table->setColumnWidth(FileTableModel::ColCreated,  90);
    m_table->setColumnWidth(FileTableModel::ColModified, 90);
    m_table->setColumnWidth(FileTableModel::ColStatus,   85);
    // 156px 死锁：不管配几个工具，这一列都不许变宽（手册 §6.3）。
    m_table->setColumnWidth(FileTableModel::ColTool,     156);
    cardLay->addWidget(m_table);
    root->addWidget(card);

    connect(m_refresh, SIGNAL(clicked()), this, SLOT(refreshScanClicked()));
    connect(m_toolDelegate, SIGNAL(toolButtonClicked(int,int)),
            this, SLOT(onToolButtonClicked(int,int)));
    connect(m_toolDelegate, SIGNAL(toolMoreClicked(int,int)),
            this, SLOT(onToolMoreClicked(int,int)));
    connect(m_table, SIGNAL(doubleClicked(QModelIndex)),
            this, SLOT(onRowDoubleClicked(QModelIndex)));

    // ---- 2. 进度区（全局唯一一根物理进度条，手册 §4）----
    QFrame *progCard = new QFrame;
    progCard->setObjectName(QStringLiteral("progressCard"));
    QVBoxLayout *progLay = new QVBoxLayout(progCard);
    progLay->setContentsMargins(14, 12, 14, 12);
    progLay->setSpacing(8);
    QHBoxLayout *info = new QHBoxLayout;
    info->setSpacing(8);
    m_statusLabel = new QLabel(T(kStatusReady));
    m_statusLabel->setObjectName(QStringLiteral("curStatus"));
    m_countLabel = new QLabel;
    m_countLabel->setObjectName(QStringLiteral("speedCount"));
    info->addWidget(m_statusLabel);
    info->addStretch(1);
    info->addWidget(m_countLabel);
    progLay->addLayout(info);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    progLay->addWidget(m_progress);
    root->addWidget(progCard);

    // ---- 3. 底部操作条 ----
    QHBoxLayout *foot = new QHBoxLayout;
    foot->setSpacing(14);
    m_onlyNew = new QCheckBox(T(kOnlyNew));
    // 这里原来是无条件 setChecked(true)：勾选框每次启动都被钉死成勾上，
    // 用户取消它同步一次（确实生效），关掉程序再打开又变回去 ——
    // 「改了文件不重传」于是反复出现，而界面上没有任何地方解释它。
    // 实测（2026-10-18，麒麟 V11 真实点击四轮）：
    //   默认勾选改文件 -> 接收端 sha256 不变
    //   取消勾选       -> 接收端 sha256 变了
    //   再改一次、重启 -> 又不变了（因为没持久化）
    // 老配置里没有这个键时 loadOnlyNewFiles() 返回 true，开箱行为不变。
    m_onlyNew->setChecked(ConfigStore::loadOnlyNewFiles());
    connect(m_onlyNew, &QCheckBox::toggled, this, [](bool on) {
        ConfigStore::saveOnlyNewFiles(on);
    });
    m_onlyNew->setCursor(Qt::PointingHandCursor);
    foot->addWidget(m_onlyNew);
    m_toggleLog = new QPushButton(T(kLogOpen));
    m_toggleLog->setObjectName(QStringLiteral("btnToggleLog"));
    m_toggleLog->setFixedHeight(28);
    m_toggleLog->setCursor(Qt::PointingHandCursor);
    m_toggleLog->setIcon(QIcon(Theme::docIcon(12, Theme::textSub())));
    m_toggleLog->setIconSize(QSize(12, 12));
    foot->addWidget(m_toggleLog);
    foot->addStretch(1);
    m_startBtn = new QPushButton(T(kStartSync));
    m_startBtn->setObjectName(QStringLiteral("btnStartSync"));
    m_startBtn->setFixedHeight(35);
    m_startBtn->setMinimumWidth(108);
    m_startBtn->setCursor(Qt::PointingHandCursor);
    foot->addWidget(m_startBtn);
    root->addLayout(foot);

    connect(m_startBtn, SIGNAL(clicked()), this, SLOT(startOrCancel()));
    connect(m_toggleLog, SIGNAL(clicked()), this, SLOT(toggleLogDrawer()));

    // ---- 4. 日志抽屉（默认收起，展开 175px，500 行 FIFO）----
    m_logDrawer = new QWidget;
    m_logDrawer->setObjectName(QStringLiteral("logDrawer"));
    m_logDrawer->setVisible(false);
    m_logDrawer->setFixedHeight(175);
    QVBoxLayout *logLay = new QVBoxLayout(m_logDrawer);
    logLay->setContentsMargins(0, 0, 0, 0);
    logLay->setSpacing(0);

    QFrame *logHead = new QFrame;
    logHead->setObjectName(QStringLiteral("logHeader"));
    logHead->setFixedHeight(27);
    QHBoxLayout *logHeadLay = new QHBoxLayout(logHead);
    logHeadLay->setContentsMargins(10, 0, 10, 0);
    QLabel *logTitle = new QLabel(T(kLogTitle));
    logTitle->setObjectName(QStringLiteral("logHeaderText"));
    logHeadLay->addWidget(logTitle);
    logHeadLay->addStretch(1);
    QPushButton *copyBtn = new QPushButton(T(kCopyLog));
    copyBtn->setObjectName(QStringLiteral("logAction"));
    copyBtn->setFlat(true);
    copyBtn->setCursor(Qt::PointingHandCursor);
    QPushButton *clearBtn = new QPushButton(T(kClearLog));
    clearBtn->setObjectName(QStringLiteral("logAction"));
    clearBtn->setFlat(true);
    clearBtn->setCursor(Qt::PointingHandCursor);
    logHeadLay->addWidget(copyBtn);
    logHeadLay->addWidget(clearBtn);
    logLay->addWidget(logHead);

    QFrame *logLine = new QFrame;
    logLine->setObjectName(QStringLiteral("logHeaderLine"));
    logLine->setFixedHeight(1);
    logLay->addWidget(logLine);

    m_logView = new QTextEdit;
    m_logView->setObjectName(QStringLiteral("logView"));
    m_logView->setReadOnly(true);
    // 手册 §3：行数 FIFO，上限 2000 行。
    // QTextEdit 没有 setMaximumBlockCount，得挂在 document() 上 ——
    // 挂在控件上会**静默不生效**，日志就一直涨（注释里记着这个坑，别改回去）。
    //
    // 原来这里是 500：一次同步的日志本来就几百行，500 行只够看半轮，
    // 排查"这个文件当时为什么失败"时前面的行已经被淘汰掉了。
    m_logView->document()->setMaximumBlockCount(2000);
    m_logView->setLineWrapMode(QTextEdit::NoWrap);
    logLay->addWidget(m_logView);
    root->addWidget(m_logDrawer);

    connect(copyBtn, SIGNAL(clicked()), this, SLOT(copyLog()));
    connect(clearBtn, SIGNAL(clicked()), this, SLOT(clearLog()));
}

// ---------------------------------------------------------------- Tab / 过滤

void SyncPanel::rebuildTabs()
{
    const QStringList groups = m_model->suffixGroups();
    if (m_currentTab >= groups.size())
        m_currentTab = 0;
    for (int i = 0; i < m_tabs.size(); ++i) {
        QPushButton *b = m_tabs.at(i);
        if (i >= groups.size()) {
            b->setVisible(false);
            continue;
        }
        const QString suf = groups.at(i);
        b->setVisible(true);
        // 计数必须写进按钮文字：原型是「全部 (7)」/「.spe (3)」。
        // 原来只塞进了 tooltip，结果用户一眼数不出这一组有多少个文件，
        // 还要把鼠标悬上去等提示 —— 提示条还是「勾选数 / 总数」两个数字。
        const int total = m_model->groupTotal(suf);
        b->setText(suf.isEmpty()
                       ? QObject::tr("%1 (%2)").arg(QObject::tr(kTabAll)).arg(total)
                       : QObject::tr(".%1 (%2)").arg(suf).arg(total));
        b->setToolTip(QObject::tr("%1 / %2").arg(m_model->groupChecked(suf))
                                             .arg(total));
        b->setChecked(i == m_currentTab);
    }
}

void SyncPanel::applyFilter(const QString &suffixFilter)
{
    // 过滤在视图层做，不动源模型：源模型的行号是传输回包落状态的锚点，
    // 一旦重排，已经传完的那些行就会张冠李戴。
    m_filterSuffix = suffixFilter;
    for (int r = 0; r < m_model->rowCount(); ++r)
        m_table->setRowHidden(r, !m_model->passesFilter(r, suffixFilter));
}

void SyncPanel::onTabClicked()
{
    for (int i = 0; i < m_tabs.size(); ++i) {
        if (m_tabs.at(i)->isChecked()) {
            m_currentTab = i;
            break;
        }
    }
    const QStringList groups = m_model->suffixGroups();
    if (m_currentTab < groups.size())
        applyFilter(groups.at(m_currentTab));
}

void SyncPanel::onHeaderCheckToggled(bool on)
{
    m_model->setAllChecked(on);
    if (m_checkHeader) {
        static_cast<CheckHeaderView*>(m_checkHeader)->setChecked(on);
    }
    rebuildTabs();
}

void SyncPanel::onSelectAllToggled(bool on)
{
    onHeaderCheckToggled(on);
}

void SyncPanel::refreshScanClicked()
{
    scanFiles();
}

// ---------------------------------------------------------------- 工具调用

QString SyncPanel::absolutePathForRow(int row) const
{
    if (!m_model || row < 0 || row >= m_model->rowCount())
        return QString();
    const QString rel = m_model->files().at(row).relativePath;
    if (m_localRoot.isEmpty())
        return rel;
    return QDir::toNativeSeparators(QDir(m_localRoot).absoluteFilePath(rel));
}

ToolRule SyncPanel::toolRuleForRow(int row) const
{
    if (!m_model || row < 0 || row >= m_model->rowCount())
        return ToolRule();
    // 归一化后再比。原来是裸 suffix == suf：
    // 用户在设置里写 ".spe"、文件实际后缀是 "spe" 时，这个分支永远不成立，
    // 表格当次会话全部显示「未配置工具」；重启一次（读回配置时剥了点）又好了。
    // 工具页走的是 ToolRunner::normSuffix，两边口径不同才会出这种事。
    const QString suf = ToolRunner::normSuffix(m_model->files().at(row).suffix);
    for (int i = 0; i < m_toolRules.size(); ++i)
        if (ToolRunner::normSuffix(m_toolRules.at(i).suffix) == suf)
            return m_toolRules.at(i);
    return ToolRule();
}

void SyncPanel::onToolButtonClicked(int row, int toolIndex)
{
    launchToolForRow(row, toolIndex);
}


void SyncPanel::onToolMoreClicked(int row, int firstHidden)
{
    const ToolRule tr = toolRuleForRow(row);
    const int total = tr.toolCount();
    if (total <= firstHidden) {
        appendLogLine(QStringLiteral("WARN"), QObject::tr(kNoTool), 3);
        return;
    }
    // 赵工原话：「同一个后缀配置了多个工具时，点击调用会弹出一个菜单让用户选择用哪一个工具打开。」
    // 必须弹菜单，不许替他挑第一个。
    QMenu menu;
    for (int i = firstHidden; i < total; ++i) {
        QAction *a = menu.addAction(tr.nameAt(i));
        a->setData(i);
    }
    QAction *chosen = menu.exec(QCursor::pos());
    if (!chosen)
        return;
    launchToolForRow(row, chosen->data().toInt());
}

void SyncPanel::onRowDoubleClicked(const QModelIndex &index)
{
    if (!index.isValid() || index.column() == FileTableModel::ColCheck)
        return;
    // 工具列的双击已经由 delegate 在 press 时发过 toolButtonClicked 了，
    // 这里再发一次就是双击一次起两个进程（实测 40 连点起 40 个、双击起 2 个）。
    if (index.column() == FileTableModel::ColTool)
        return;
    const ToolRule tr = toolRuleForRow(index.row());
    if (tr.toolCount() <= 0) {
        appendLogLine(QStringLiteral("WARN"), QObject::tr(kNoTool), 3);
        return;
    }
    launchToolForRow(index.row(), 0);
}

// ---------------------------------------------------------------- 工具启动预检
//
// 判定与文案都放在这一段、且是 SyncPanel 的 static 成员：
// 判据调的是产品这一份实现。照抄一份副本的话，把产品改回原样判据照样全绿
// —— 上一轮的收尾次序判据第一版就是这么写的，变异测试当场把它证伪。

SyncPanel::LaunchVerdict SyncPanel::preflightLaunch(const QString &toolExe,
                                                   const QString &sourceFile,
                                                   bool isExecutable)
{
    // 顺序有讲究：**先**查工具程序，再查源文件。
    // 两个都不存在时，先报工具那一条（设置没配对，是用户能立刻改的），
    // 报源文件会让用户去检查一个根本没被打开过的文件。
    if (toolExe.trimmed().isEmpty())
        return LaunchNoTool;
    if (!QFileInfo(toolExe).exists())
        return LaunchToolMissing;
    // 无执行权限只在非 Windows 上有意义：Qt 5.9 在 Windows 上对任何存在的
    // 文件都返回 true，这里判了也永远不会命中，白白多一条分支。
    #ifdef Q_OS_WIN
    Q_UNUSED(isExecutable);
    #else
    if (!isExecutable)
        return LaunchToolNotExecutable;
    #endif
    // 源文件最后查：走到这里说明工具是好的，如果源文件没了，那就是数据的问题。
    if (!sourceFile.isEmpty() && !QFileInfo(sourceFile).exists())
        return LaunchSourceMissing;
    return LaunchOk;
}

QString SyncPanel::launchVerdictTag(LaunchVerdict v)
{
    switch (v) {
    case LaunchOk:                return QStringLiteral("ok");
    case LaunchNoTool:            return QStringLiteral("no-tool");
    case LaunchSourceMissing:     return QStringLiteral("source-missing");
    case LaunchToolMissing:       return QStringLiteral("tool-missing");
    case LaunchToolNotExecutable: return QStringLiteral("not-executable");
    }
    return QStringLiteral("unknown");
}

QString SyncPanel::preflightMessage(LaunchVerdict v, const QString &toolExe,
                                    const QString &sourceFile, const QString &toolName)
{
    switch (v) {
    case LaunchSourceMissing:
        return QObject::tr("\xE6\xBA\x90\xE6\x96\x87\xE4\xBB\xB6\xE4\xB8\x8D\xE5\xAD\x98\xE5\x9C\xA8\xEF\xBC\x9A%1\xE3\x80\x82")
            .arg(sourceFile);
    case LaunchToolNotExecutable:
        return QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7 [%1] \xE6\xB2\xA1\xE6\x9C\x89\xE6\x89\xA7\xE8\xA1\x8C\xE6\x9D\x83\xE9\x99\x90\xEF\xBC\x8C\xE6\x97\xA0\xE6\xB3\x95\xE5\x90\xAF\xE5\x8A\xA8\xE3\x80\x82\xE8\xAF\xB7\xE5\x9C\xA8\xE7\xBB\x88\xE7\xAB\xAF\xE6\x89\xA7\xE8\xA1\x8C\xE5\x91\xBD\xE4\xBB\xA4\xEF\xBC\x9A" "chmod +x \"%2\"\xEF\xBC\x9B\xE6\x88\x96\xE5\x9C\xA8 \xE8\xAE\xBE\xE7\xBD\xAE \xE2\x86\x92 \xE6\x96\x87\xE4\xBB\xB6\xE6\x89\x93\xE5\xBC\x80\xE5\xB7\xA5\xE5\x85\xB7 \xE2\x86\x92 \xE9\x80\x89\xE7\xA8\x8B\xE5\xBA\x8F\xE8\xB7\xAF\xE5\xBE\x84 \xE9\x87\x8D\xE6\x96\xB0\xE9\x80\x89\xE6\x8B\xA9\xE4\xB8\x80\xE4\xB8\xAA\xE5\x8F\xAF\xE6\x89\xA7\xE8\xA1\x8C\xE7\xA8\x8B\xE5\xBA\x8F\xE3\x80\x82")
            .arg(toolName, toolExe);
    case LaunchToolMissing:
        return QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7\xE4\xB8\x8D\xE5\xAD\x98\xE5\x9C\xA8\xEF\xBC\x9A%1\xE3\x80\x82\xE8\xAF\xB7\xE5\x88\xB0 \xE8\xAE\xBE\xE7\xBD\xAE \xE2\x86\x92 \xE6\x96\x87\xE4\xBB\xB6\xE6\x89\x93\xE5\xBC\x80\xE5\xB7\xA5\xE5\x85\xB7 \xE2\x86\x92 \xE9\x80\x89\xE7\xA8\x8B\xE5\xBA\x8F\xE8\xB7\xAF\xE5\xBE\x84\xEF\xBC\x8C\xE9\x87\x8D\xE6\x96\xB0\xE9\x80\x89\xE6\x8B\xA9\xE8\xAF\xA5\xE5\xB7\xA5\xE5\x85\xB7\xE7\x9A\x84\xE7\xA8\x8B\xE5\xBA\x8F\xE8\xB7\xAF\xE5\xBE\x84\xE3\x80\x82")
            .arg(toolExe);
    case LaunchNoTool:
        return QObject::tr(kNoTool);
    case LaunchOk:
        break;
    }
    return QString();
}

// 启动器型工具的良性警告码。
//
// 实测（2026-10-18，麒麟实机，不是推断）：
//   xdg-open <文件>  →  文件真的打开了；stderr 打印
//   "GLib-GObject-CRITICAL: g_object_get_data: assertion 'G_IS_OBJECT (object)' failed"
//   退出码 3。
// 也就是这类工具 fork 完子进程就带着警告退出，退出码**不代表打开结果**。
// 原实现（exitCode != 0 一律报红 ERR）让用户看到"文件开了、下面写着 ERR"。
//
// 判据要求"不能放宽成一律成功"，所以两个条件都要：
//   1) 退出码非 0（退出码 0 时它本来就走成功分支，这里不该插手）
//   2) stderr 命中启动器特征串
// 少任何一条都还是红色 ERR。
bool SyncPanel::isBenignLauncherExit(int exitCode, const QByteArray &stderrTail)
{
    if (exitCode == 0)
        return false;
    static const char *kBenignMarkers[] = {
        "GLib-GObject-CRITICAL",
        "g_object_get_data",
        // gio open 走的是同一条 GLib 断言路径，措辞略有差别，一起收。
        "GLib-GIO-CRITICAL",
    };
    for (size_t i = 0; i < sizeof(kBenignMarkers) / sizeof(kBenignMarkers[0]); ++i) {
        if (stderrTail.contains(kBenignMarkers[i]))
            return true;
    }
    return false;
}

QString SyncPanel::benignLauncherMessage(const QString &toolName, int exitCode)
{
    return QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7 [%1] \xE8\xBF\x94\xE5\x9B\x9E\xE9\x9D\x9E\xE9\x9B\xB6\xE9\x80\x80\xE5\x87\xBA\xE7\xA0\x81 %2\xE3\x80\x82")
            .arg(toolName).arg(exitCode);
}

SyncPanel::LaunchVerdict SyncPanel::launchPreflightForTest(int row, int toolIndex) const
{
    const ToolRule tr = toolRuleForRow(row);
    const QString exe = tr.exeAt(toolIndex);
    const QString file = absolutePathForRow(row);
    return preflightLaunch(exe, file, exe.isEmpty() || QFileInfo(exe).isExecutable());
}

void SyncPanel::launchToolForRow(int row, int toolIndex)
{
    const ToolRule tr = toolRuleForRow(row);
    const QString exe  = tr.exeAt(toolIndex);
    const QString name = tr.nameAt(toolIndex);
    const QString file = absolutePathForRow(row);

    // ---- 防抖 ----
    //
    // 放在预检**之前**：连点 5 次"打开"时，第一击就已经把行记上了，
    // 后面 4 击连"源文件还在不在"都不用查就被挡掉，省掉 4 次 stat。
    // 放在预检之后的话，"源文件不存在"这种失败路径每次都会重新查一遍，
    // 而且会在日志里连刷 5 条一样的 ERR。
    const qint64 now = m_toolClock.elapsed();
    const qint64 last = m_toolLaunchAtMs.value(row, -1);
    if (last >= 0 && (now - last) < launchDebounceMs()) {
        ++m_toolDebounceSuppressed;
        return;
    }

    // ---- 预检 ----
    //
    // 原来这里只有两条：exe 为空、exe 不存在。缺了源文件这一条，于是
    // "文件在本机已被删掉/移动"这种情况是**静默**的：工具拿着一个不存在的
    // 路径启动，有些工具自己弹个错误框，有些什么都不弹，界面全程"已拉起工具"。
    const LaunchVerdict verdict = preflightLaunch(exe, file,
                                                  exe.isEmpty() || QFileInfo(exe).isExecutable());
    if (verdict != LaunchOk) {
        const QString msg = preflightMessage(verdict, exe, file, name);
        // ERR：用户点了按钮却什么都没发生，不明确报出来会被当成软件坏了。
        appendLogLine(QStringLiteral("ERR"), msg, 4);
        // 用户可见反馈走状态行 + 自动展开日志抽屉，**不弹模态框**：
        // 点一下就弹框会把用户从表格上打断，他还得先点掉才能继续。
        if (!m_logOpen)
            toggleLogDrawer();
        setStatusLine(msg, 2);
        // 记下"这一行点过了"，否则失败路径上的连点会每次都重新判一遍。
        m_toolLaunchAtMs.insert(row, now);
        return;
    }

    m_toolLaunchAtMs.insert(row, now);

    // 参数整条链借给工具页的 buildArgv（= buildArgs + splitCommand）。
    //
    // 这里原来是自己写的 splitArgs：只认成对双引号，不认反斜杠转义。
    // 用户模板写 -f "{file}"、路径落在 C:\Program Files\ 下时，
    // 路径被撕成 ['-f', 'C:\Program', 'Files\data\x.spe'] 三段，
    // 工具拿一个不存在的目录跑完，界面全程显示「执行完成」，没有任何异常提示。
    ToolRule single = tr;
    single.argsTemplate = tr.argsAt(toolIndex);
    single.exePath.clear();
    single.toolNames.clear();
    single.toolExePaths.clear();
    single.toolArgs.clear();
    single.toolNames.append(tr.nameAt(toolIndex));
    single.toolExePaths.append(tr.exeAt(toolIndex));
    single.toolArgs.append(tr.argsAt(toolIndex));
    const QStringList args = ToolRunner::buildArgv(single, file);

    QProcess *p = new QProcess(this);
    p->setProgram(exe);
    p->setArguments(args);
    p->setWorkingDirectory(QFileInfo(exe).absolutePath());
    // 独立拉起：不 waitForFinished，工具之间并行，前一个没退不挡后一个。
    //
    // 这里从 ForwardedChannels 改成了 SeparateChannels：
    // ForwardedChannels 意味着 stderr 直接进了本进程的 stderr，
    // 我们一个字也拿不到 —— 而"这个非 0 退出码是启动器的良性警告还是真失败"
    // **只能**从 stderr 认。改成自己收之后，onToolStderrReady() 会把每一行
    // 原样转发回本进程 stderr（用户该看到的输出一个不少），
    // 同时攒一份尾部用来判退出码的性质。
    p->setProcessChannelMode(QProcess::SeparateChannels);
    m_toolNames.insert(p, name);
    m_toolStderr.insert(p, QByteArray());
    m_toolProcs.append(p);
    ++m_toolLaunchAttempts;

    connect(p, SIGNAL(finished(int,QProcess::ExitStatus)),
            this, SLOT(onToolFinished(int,QProcess::ExitStatus)));
    connect(p, SIGNAL(errorOccurred(QProcess::ProcessError)),
            this, SLOT(onToolError(QProcess::ProcessError)));
    connect(p, SIGNAL(readyReadStandardError()),
            this, SLOT(onToolStderrReady()));

    p->start();
    appendLogLine(QStringLiteral("SYNC"),
                  QObject::tr("\xE5\xB7\xB2\xE6\x8B\x89\xE8\xB5\xB7\xE5\xB7\xA5\xE5\x85\xB7 [%1] %2").arg(name).arg(file), 1);
}

void SyncPanel::onToolStderrReady()
{
    QProcess *p = qobject_cast<QProcess *>(sender());
    if (!p)
        return;
    const QByteArray chunk = p->readAllStandardError();
    if (chunk.isEmpty())
        return;
    // 原样还给用户。SeparateChannels 之后这些字节不会自己出现在控制台上，
    // 不转发就是**吞掉了工具的报错** —— 那比误报一条 ERR 更糟。
    fwrite(chunk.constData(), 1, static_cast<size_t>(chunk.size()), stderr);
    fflush(stderr);

    // 只留尾部。判据只看尾部那几行特征串，整段留着没有用处，
    // 而一个会往 stderr 刷几万行的工具能把内存吃干净。
    QByteArray &tail = m_toolStderr[p];
    tail += chunk;
    const int kKeep = 8192;
    if (tail.size() > kKeep)
        tail = tail.right(kKeep);
}

void SyncPanel::onToolFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *p = qobject_cast<QProcess *>(sender());
    if (!p)
        return;
    const QString name = m_toolNames.value(p);
    // finished 之后管道里可能还剩最后一段没被 readyRead 捞走的字节。
    // 启动器型工具的 GLib 警告往往正好在最后一段里，
    // 不补这一次读就会把良性码判成红色 ERR。
    const QByteArray tailChunk = p->readAllStandardError();
    QByteArray tail = m_toolStderr.value(p);
    tail += tailChunk;
    if (tailChunk.isEmpty() == false) {
        fwrite(tailChunk.constData(), 1, static_cast<size_t>(tailChunk.size()), stderr);
        fflush(stderr);
    }
    m_toolStderr.remove(p);
    m_toolNames.remove(p);
    m_toolProcs.removeAll(p);
    ++m_toolFinishedCount;

    if (status == QProcess::CrashExit) {
        // 崩溃没有退出码可谈，任何降级都不许碰这一支。
        appendLogLine(QStringLiteral("ERR"),
                      QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7 [%1] \xE5\xBC\x82\xE5\xB8\xB8\xE7\xBB\x88\xE6\xAD\xA2\xEF\xBC\x88\xE5\xB4\xA9\xE6\xBA\x83\xE9\x80\x80\xE5\x87\xBA\xEF\xBC\x89").arg(name), 4);
    } else if (exitCode != 0 && isBenignLauncherExit(exitCode, tail)) {
        // 启动器型工具的良性警告码：黄色 WARN，不是红色 ERR。
        appendLogLine(QStringLiteral("WARN"), benignLauncherMessage(name, exitCode), 3);
    } else if (exitCode != 0) {
        // 其余非 0 一律红色 ERR —— 判据要求"不能放宽成一律成功"，
        // 这一支就是那个"不能"。
        appendLogLine(QStringLiteral("ERR"),
                      QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7 [%1] \xE9\x80\x80\xE5\x87\xBA\xE7\xA0\x81 %2")
                          .arg(name).arg(exitCode), 4);
    } else {
        appendLogLine(QStringLiteral("DONE"),
                      QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7 [%1] \xE6\x89\xA7\xE8\xA1\x8C\xE5\xAE\x8C\xE6\xAF\x95").arg(name), 2);
    }
    p->deleteLater();
}

void SyncPanel::onToolError(QProcess::ProcessError)
{
    QProcess *p = qobject_cast<QProcess *>(sender());
    if (!p)
        return;
    const QString name = m_toolNames.value(p);
    appendLogLine(QStringLiteral("ERR"),
                  QObject::tr("\xE5\xB7\xA5\xE5\x85\xB7 [%1] \xE8\xBF\x9B\xE7\xA8\x8B\xE5\x90\xAF\xE5\x8A\xA8\xE6\x88\x96\xE8\xBF\x90\xE8\xA1\x8C\xE6\x97\xB6\xE5\x87\xBA\xE9\x94\x99\xE3\x80\x82").arg(name), 4);
    m_toolStderr.remove(p);
    m_toolNames.remove(p);
    m_toolProcs.removeAll(p);
    p->deleteLater();
}

// ---------------------------------------------------------------- 日志抽屉

void SyncPanel::toggleLogDrawer()
{
    m_logOpen = !m_logOpen;
    m_logDrawer->setVisible(m_logOpen);
    m_toggleLog->setText(m_logOpen ? T(kLogClose) : T(kLogOpen));
    if (window()) {
        window()->setFixedHeight(m_logOpen ? 680 : 480);
    }
}

void SyncPanel::pushLogLine(const QString &line)
{
    if (!m_logView)
        return;
    // 传进来的已经是排好版的 HTML（appendLogLine 负责拼三段式）。
    m_logView->append(line);
    // 自动滚到底部（手册 §3）。
    QScrollBar *sb = m_logView->verticalScrollBar();
    if (sb)
        sb->setValue(sb->maximum());
}

void SyncPanel::appendLogLine(const QString &tag, const QString &msg, int tagKind)
{
    // 三段式：[时间戳] 徽标 正文，对应原型 .log-entry。
    // 徽标配色照抄 .tag-info / .tag-sync / .tag-done / .tag-warn / .tag-err。
    // 正文里的 HTML 元字符必须转义 —— rsync 的输出里带 < > & 是常事，
    // 直接拼进 HTML 会把整行吃掉，而且报错指向完全无关的地方。
    static const char *kTagCss[5] = {
        "background:#1e293b;color:#94a3b8",   // 0 INFO
        "background:#1e3a8a;color:#60a5fa",   // 1 SYNC
        "background:#064e3b;color:#34d399",   // 2 DONE
        "background:#78350f;color:#fbbf24",   // 3 WARN
        "background:#7f1d1d;color:#f87171",   // 4 ERR
    };
    const int kind = (tagKind >= 0 && tagKind <= 4) ? tagKind : 0;

    const QString ts = QTime::currentTime().toString(QStringLiteral("hh:mm:ss.zzz"));
    QString paddedTag = tag.toHtmlEscaped();
    while (paddedTag.length() < 4) {
        paddedTag.append(QLatin1Char(' '));
    }
    paddedTag.replace(QLatin1Char(' '), QStringLiteral("&nbsp;"));
    const QString safeMsg = msg.toHtmlEscaped();

    // 判据要断言"这一行到底是 ERR 还是 WARN"。toPlainText() 把徽标底色剥掉
    // 之后只剩正文，两档看起来一模一样；这里另存一份 TAG + 正文。
    m_logRecords.append(tag + QLatin1Char('\t') + msg);
    while (m_logRecords.size() > 500)
        m_logRecords.removeFirst();

    pushLogLine(QString(
        "<span style=\"color:#64748b;font-family:Consolas,Monaco,monospace;font-size:10px;\">[%1]</span>"
        "&nbsp;"
        "<span style=\"%2;font-family:Consolas,Monaco,monospace;font-size:9px;font-weight:700;border-radius:3px;padding:1px 5px;\">%3</span>"
        "&nbsp;&nbsp;"
        "<span style=\"color:#cbd5e1;\">%4</span>")
                    .arg(ts)
                    .arg(QLatin1String(kTagCss[kind]))
                    .arg(paddedTag, safeMsg));
}

void SyncPanel::copyLog()
{
    if (!m_logView)
        return;
    // 原来只有 selectAll()，看着像"复制"其实一个字符都没进剪贴板 ——
    // 用户点完再去粘贴，粘到的是他自己之前的内容。
    // 复制的是 toPlainText()：去掉徽标底色和 <span>，粘到记事本里是干净的纯文本。
    QApplication::clipboard()->setText(m_logView->toPlainText());
    appendLogLine(QStringLiteral("INFO"),
                  QObject::tr("\xE5\xB7\xB2\xE5\xA4\x8D\xE5\x88\xB6\xE6\x97\xA5\xE5\xBF\x97\xE5\x88\xB0\xE5\x89\xAA\xE8\xB4\xB4\xE6\x9D\xBF"), 0);
}

void SyncPanel::clearLog()
{
    if (m_logView)
        m_logView->clear();
}

// ---------------------------------------------------------------- 状态 / 进度

void SyncPanel::setStatusLine(const QString &text, int kind)
{
    if (!m_statusLabel)
        return;
    m_statusLabel->setText(text);
    // 换 objectName 之后样式不会自动重算，得让 QSS 重新应用。
    const char *name = "curStatus";
    if (kind == 1)      name = "curStatusDone";
    else if (kind == 2) name = "curStatusCancel";
    m_statusLabel->setObjectName(QString::fromLatin1(name));
    m_statusLabel->style()->polish(m_statusLabel);
}

void SyncPanel::setBusy(bool busy)
{
    m_busy = busy;
    // 开始和取消是**同一个按钮**（btnStartSync / btnStartSyncCancel）。
    // 原来这里 m_startBtn->setEnabled(!busy) 是照旧 UI 写的 —— 那时开始和取消
    // 是两个按钮，传输中禁掉"开始"是对的。可合并之后，这个按钮就是"取消"本身：
    // 禁掉它等于把取消功能整个拿掉，用户看到的是一个红底写着「取消同步」、
    // 点下去毫无反应的按钮。
    //
    // 实测（cancel 场景，400MB）：点取消时按钮 enabled=0，click() 是空操作，
    // 日志里连"正在取消…"都没有，传输一路跑满 100%。场景自己的断言还全绿了 ——
    // 因为那些判据量的是"远端没继续涨"，而取消压根没发生过。
    // 一个永远点不动的取消按钮，比没有取消按钮更糟：用户以为是自己没点到。
    m_startBtn->setEnabled(true);
    m_refresh->setEnabled(!busy);
}

void SyncPanel::onByteProgress(int percent)
{
    // rsync 报的是**当前组内**的字节百分比，不是全局的（组间是串行的，
    // 每组起一个 rsync 进程，每组都从 0% 重新开始）。
    // 旧实现把它直接当成全局百分比用：第 1 组一跑完就到 100，
    // 第 2 组从 0% 重新开始又被 onFileTransferred 里那个 max() 挡住不更新，
    // 于是进度条卡在高位不动 —— 那不是第 2 组的真实进度（缺陷 2）。
    m_groupPercent = percent;
    // 一个文件都还没开始传就先收到这一行，那是第 0 组自己的收尾统计。
    // --ignore-existing 全跳过时 rsync 不为任何文件发 fileStarted，
    // 却仍然会报一行「0 字节 100%」的总结，这一行就是那一组的全部信息，
    // 不接住它，那一组在整个全局口径里就等于不存在。
    if (m_groupIndex < 0 && !m_groupSuffixes.isEmpty())
        m_groupIndex = 0;
    setProgressValue(globalProgressPercent(), "byte");
}

// 全局完成度 = 本轮**已处理完**的文件数 / 本轮总文件数。
//
//   已结束组的全部文件数  +  当前组按组内字节比例折算出的文件数
//   ------------------------------------------------------------
//                        本轮总文件数
//
// 为什么进度条报文件数而不是字节数：字节和速度在计数行里显示了
// （「已传 1.9 GB · 470 MB/s」），进度条再报一遍字节就是两个口径打架。
// 更要紧的是「已处理完」不等于「传了字节」—— 勾了「仅同步新增文件」
// 而远端本来就有同名文件时，那一轮一个字节都不传、退出码仍然是 0，
// 但它确实判定完了每一个文件。字节口径在那一轮恒为 0，进度条永远走不动，
// 用户分不清"还在跑"和"跑完了但没东西可传"（缺陷 1）。
//
// 为什么已结束的组要按**整组文件数**一次算满：组里没被传输的那些文件
// 是被判定为无需传输的，它们在这一组结束时同样算"处理完"了。
// 字节比例只用来插值当前组走到哪儿，不参与跨组的累计。
int SyncPanel::globalProgressPercent() const
{
    if (m_runTotal <= 0)
        return 0;
    int doneFiles = 0;
    for (int i = 0; i < m_groupIndex && i < m_groupSizes.size(); ++i)
        doneFiles += m_groupSizes.at(i);
    const int curSize = (m_groupIndex >= 0 && m_groupIndex < m_groupSizes.size())
                        ? m_groupSizes.at(m_groupIndex) : 0;
    const double weighted = doneFiles + (curSize * m_groupPercent) / 100.0;
    int p = static_cast<int>(weighted * 100.0 / m_runTotal);
    // 全部组结束之前最多停在 99：那 1% 是"还没收尾"的留白，
    // 让用户一眼看得出这轮还在跑。真正打满 100 只发生在 onAllFinished
    // 的成功分支里 —— 取消和失败都到不了那里。
    //
    // 旧写法是 `if (... && p >= 99) p = 98;`，两处不对：
    // 一是只把 99 压成 98、100 照旧放行；二是组内百分比在组末天然会到 100，
    // 多组时第 1 组一结束就撞满 100，用户以为整轮搬完了（缺陷 3）。
    if (p > 99)
        p = 99;
    if (p < 0)
        p = 0;
    return p;
}

// 进度条的唯一写入口。顺带把每次变化记进探针序列，
// 保证导出来的就是真实显示值 —— 判据量的必须是这个，不是某个中间变量。
void SyncPanel::setProgressValue(int v, const char *tag)
{
    if (!m_progress)
        return;
    m_progress->setValue(v);
    const QString entry = QString::number(v) + QLatin1Char(':') + QString::fromLatin1(tag);
    if (m_progressTrace.isEmpty() || m_progressTrace.last() != entry)
        m_progressTrace.append(entry);
}

// 开跑前按同一套规则把本轮文件分组。
//
// 必须和 RsyncRunner 里那段逐字一致：同一条后缀规则只吃一次、
// 第一条命中的赢、空组不建（规则重复或没有匹配文件时组数会对不上，
// 分母一错进度条全程都是错的，而且是那种"看着还挺顺"的错）。
// 只能在这里重算一遍而不能去问 RsyncRunner —— m_groups 是它的私有成员，
// 给它加接口不在这次的范围里。
void SyncPanel::rebuildGroupSizes(const QVector<FileItem> &picked)
{
    m_groupSuffixes.clear();
    m_groupSizes.clear();
    QVector<QString> usedSuffixes;
    for (int i = 0; i < m_syncRules.size(); ++i) {
        const QString suf = m_syncRules.at(i).suffix;
        if (usedSuffixes.contains(suf))
            continue;
        usedSuffixes.append(suf);
        int n = 0;
        for (int k = 0; k < picked.size(); ++k) {
            if (picked.at(k).suffix == suf)
                ++n;
        }
        if (n > 0) {
            m_groupSuffixes.append(suf);
            m_groupSizes.append(n);
        }
    }
    m_groupIndex = -1;
    m_groupPercent = 0;
    // 新的一轮从零开始计。基线不清的话上一轮攒下的字节会漏进这一轮的
    // 显示里（单组时第一帧就会显示上一轮的总量），原来 m_lastBytes 也
    // 全程没有重置点 —— 计数栏在两轮之间会一直挂着上一轮的数。
    m_groupBytesBase = 0;
    m_lastBytes = 0;
}

// 某个后缀落在第几组。m_groupSuffixes 建的时候已经按同样的顺序去重、
// 跳过空组了，所以直接 indexOf 就是组号。
int SyncPanel::groupIndexOfSuffix(const QString &suffix) const
{
    return m_groupSuffixes.indexOf(suffix);
}

// 已传字节的人类可读形式。
//
// 单位刻意跟 rsync 自己报的一致（KB/MB/GB/TB），这样界面上写 "1.9 GB"
// 而日志里也是 "1.90G"，用户能对上；差一个倍率会让人以为程序算错了。
static QString formatBytes(qint64 n)
{
    static const char *const units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return QString::number(v, 'f', u == 0 ? 0 : 1)
           + QLatin1Char(' ') + QLatin1String(units[u]);
}

void SyncPanel::onByteStats(qint64 bytes, double bytesPerSec)
{
    // 「已传字节」原来直接 Q_UNUSED 掉了。界面上只有文件数和速度，
    // 用户看到进度条走到 90% 却不知道到底搬了多少数据出去 ——
    // 大目录慢链路上这两个数差着几十倍，光看百分比没法判断还要等多久。
    // 计数栏本来就是放"这一轮战况"的地方，加进去不算改排版。
    //
    // bytes 是 rsync **本组**的字节数（每组都从 0 重新报），所以要加上
    // 前面那些组已经攒下的基线才是这一轮的真实传输量。直接赋值的话，
    // 组一切换数字就从上一组总量掉回本组的小数 —— 实测差 8.7 万倍。
    m_lastBytes = m_groupBytesBase + bytes;
    m_lastBytesPerSec = bytesPerSec;
    if (!m_countLabel)
        return;
    // 同步开始前也要有内容：这时还没有速率，显示 0.0 而不是空白，
    // 空白会让那一栏看起来是"还没加载出来"。
    const QString base = QObject::tr("%1 / %2 \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6 \xC2\xB7 \xE5\xB7\xB2\xE4\xBC\xA0 %3 \xC2\xB7 %4 MB/s")
                             .arg(m_runDone).arg(m_runTotal)
                             .arg(formatBytes(m_lastBytes))
                             .arg(bytesPerSec / (1024.0 * 1024.0), 0, 'f', 1);
    m_countLabel->setText(base);
}

void SyncPanel::tickElapsed()
{
    // 原型 .speed-and-count 是等宽小字，右侧一行显示「已完成 / 总数 · 速度 · 用时」。
    // 用时每秒都要跳：不显示的话用户只能靠进度条猜这轮传了多久。
    if (!m_countLabel || !m_busy || m_runStartMs <= 0)
        return;
    m_elapsedSecs = (QDateTime::currentMSecsSinceEpoch() - m_runStartMs) / 1000;
    renderCountLabel();
}

// 计数行文本。**只管拼，不碰忙态、不碰计时。**
// 拆出来是因为收尾那一次要单独刷：tickElapsed 开头有 !m_busy 守卫，
// 而 onAllFinished 是先补全 m_runDone 再 setBusy(false)，
// 收尾这一次正好会被守卫挡掉，计数就永远停在最后一次 1Hz 刷新的值上。
void SyncPanel::renderCountLabel()
{
    if (!m_countLabel)
        return;
    const qint64 secs = m_elapsedSecs;
    const qint64 h = secs / 3600;
    const qint64 mnt = (secs % 3600) / 60;
    const qint64 s = secs % 60;
    const QString elapsed = h > 0
        ? QObject::tr("%1:%2:%3").arg(h).arg(mnt, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'))
        : QObject::tr("%1:%2").arg(mnt).arg(s, 2, 10, QLatin1Char('0'));
    m_countLabel->setText(QObject::tr("%1 / %2 \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6 \xC2\xB7 \xE5\xB7\xB2\xE4\xBC\xA0 %3 \xC2\xB7 %4 MB/s \xC2\xB7 %5")
                          .arg(m_runDone).arg(m_runTotal)
                          .arg(formatBytes(m_lastBytes))
                          .arg(m_lastBytesPerSec / (1024.0 * 1024.0), 0, 'f', 1)
                          .arg(elapsed));
}

// ---------------------------------------------------------------- 扫描

QString SyncPanel::currentScanKey() const
{
    QStringList sufs;
    for (int i = 0; i < m_syncRules.size(); ++i)
        sufs.append(m_syncRules.at(i).suffix);
    sufs.sort();
    return m_localRoot + QLatin1Char('|') + sufs.join(QLatin1Char(','));
}

void SyncPanel::scanFiles()
{
    if (m_localRoot.isEmpty()) {
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr("\xE6\x9C\xAC\xE6\x9C\xBA\xE6\x96\x87\xE4\xBB\xB6\xE7\x9B\xAE\xE5\xBD\x95\xE4\xB8\xBA\xE7\xA9\xBA\xE3\x80\x82"), 3);
        return;
    }

    // 目录不存在 / 指向一个文件，也必须说清楚是哪一种。
    //
    // FileScanner::scan 对这两种情况一律返回**空清单**（见那里的 exists() 判断），
    // 于是界面上只剩一句「深度遍历目录完成，发现 0 个待同步文件」。
    // 用户看到的是"扫完了、没有文件"，真实原因却是**路径根本不对**：
    // 打错一个字、U 盘拔了、网络盘没挂上、拖进来的是单个文件 —— 现象一模一样。
    // 而 0 个文件之后再点开始同步，只会说「请先勾选至少一个文件」，
    // 用户按这句话去查文件有没有勾上，永远查不到点上。
    //
    // 两条提示分开写，因为补救动作完全不同：不存在 -> 改路径或把盘挂上；
    // 不是文件夹 -> 改选一个目录。合成一句"路径无效"等于什么都没说。
    const QFileInfo localInfo(m_localRoot);
    const bool rootMissing = !localInfo.exists();
    const bool rootNotDir  = !rootMissing && !localInfo.isDir();

    // 第三种情况：目录在、也是文件夹，但当前账号**读不了**。
    //
    // 属主是别人、权限被改成 ---x、或者被 SELinux / 安全中心挡了，都长这样。
    // 这一类原来会一路走进 FileScanner::scan，扫出**空清单**，
    // 界面上只剩一句「深度遍历目录完成，发现 0 个待同步文件」——
    // 和「这个目录里确实没有匹配的文件」在用户眼里一模一样。
    // 于是点「开始同步」什么也不会发生，界面还是「就绪 / 0 个待同步文件」，
    // 客户会以为自己没勾文件、或者软件坏了，而真因是目录权限。
    //
    // 单独一个分支而不是并进上面那条：补救动作不同（改权限，不是改路径），
    // 话必须分开说。下面那 3 行收尾与上面重复，是有意为之 ——
    // 把已有的两串长转义整体重排的风险，比重复 3 行大得多。
    const bool rootUnreadable = !rootMissing && !rootNotDir && !localInfo.isReadable();
    if (rootUnreadable) {
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr("\xE6\x9C\xAC\xE6\x9C\xBA\xE7\x9B\xAE\xE5\xBD\x95\xE6\xB2\xA1\xE6\x9C\x89\xE8\xAF\xBB\xE5\x8F\x96\xE6\x9D\x83\xE9\x99\x90\xEF\xBC\x9A%1")
                          .arg(m_localRoot), 3);
        m_scanKey.clear();
        onScanFinished(QVector<FileItem>());
        return;
    }

    if (rootMissing || rootNotDir) {
        appendLogLine(QStringLiteral("WARN"),
                      (rootMissing
                       ? QObject::tr("\xE6\x9C\xAC\xE6\x9C\xBA\xE7\x9B\xAE\xE5\xBD\x95\xE4\xB8\x8D\xE5\xAD\x98\xE5\x9C\xA8\xEF\xBC\x9A%1")
                       : QObject::tr("\xE6\x9C\xAC\xE6\x9C\xBA\xE8\xB7\xAF\xE5\xBE\x84\xE4\xB8\x8D\xE6\x98\xAF\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xB9\xEF\xBC\x9A%1"))
                          .arg(m_localRoot), 3);
        // **清单必须一起清掉。**
        // 早退而不清，界面上会留着**上一个目录**的整份文件清单，
        // 而配置里写的是另一个（无效的）路径 —— 用户看着 7 个文件，
        // 以为扫的是当前这个目录。他要是接着点开始同步，
        // startSyncInternal 会因为 m_scanKey 对不上再扫一次、再被这里挡下，
        // 于是只得到一句"请先勾选至少一个文件"：清单上有文件、却让去勾选，
        // 两句话互相打脸，而真实原因（路径不对）在上面那行 WARN 里。
        // 清掉之后列表是空的，行为和"这个目录里确实没有文件"完全一致，
        // 用户不会拿着上一份清单去做判断。
        m_scanKey.clear();
        onScanFinished(QVector<FileItem>());
        return;
    }

    QHash<QString, ToolRule> ruleHash;
    for (int i = 0; i < m_toolRules.size(); ++i)
        ruleHash.insert(m_toolRules.at(i).suffix, m_toolRules.at(i));

    m_refresh->setEnabled(false);
    appendLogLine(QStringLiteral("SCAN"),
                  QObject::tr("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xB7\xB1\xE5\xBA\xA6\xE9\x81\x8D\xE5\x8E\x86\xE6\x9C\xAC\xE6\x9C\xBA\xE7\x9B\xAE\xE5\xBD\x95\xEF\xBC\x9A%1").arg(m_localRoot), 0);
    startScanAsync();
}

void SyncPanel::startScanAsync()
{
    // 已经在扫就别再拉一个。理由见 SyncPanel.h 里 m_scanning 的注释：
    // 并发扫描的第二个结果会在传输进行中把清单和进度整个刷掉。
    // 已经在飞的那一次会照样把 m_syncRequested 消费掉，扫完仍然会接着同步，
    // 所以这里直接返回不会把用户的"开始同步"吃掉。
    if (m_scanning)
        return;
    m_scanning = true;

    QStringList sufs;
    for (int i = 0; i < m_syncRules.size(); ++i)
        sufs.append(m_syncRules.at(i).suffix);

    ScannerWorker *w = new ScannerWorker(m_localRoot, sufs, this);
    QThreadPool::globalInstance()->start(w);
}

// 本轮扫描是否触顶。deliverScanResult() 拿得到 truncated，
// onScanFinished() 只收 files —— 改它的签名要动 SyncPanel.h，
// 而本次只允许改 SyncPanel.cpp / ConfigStore.cpp 两个文件。
// 所以用文件级静态递过去。
//
// 用静态而不是成员变量是安全的：SyncPanel 在整个进程里只有**一个**实例
// （MainWindow.cpp: m_syncPanel = new SyncPanel(m_body)），
// 不存在两份面板轮流写这个标志、互相把对方的结果抹掉的窗口；
// 且 onScanFinished() 只有 deliverScanResult() 这一个调用者
// （见 SyncPanel.h: injectFiles() 也是转 deliverScanResult(files, false)），
// 写入和消费之间不存在别的线程 —— 消费发生在同一个 UI 线程的下一行。
bool g_scanTruncated = false;

void SyncPanel::deliverScanResult(const QVector<FileItem> &files, bool truncated)
{
    // FileScanner::wasTruncated() 只有在这一刻问才有意义：
    // FileScanner::scan() 开头会自己把标志清零（FileScanner.cpp: g_truncated.storeRelease(0)），
    // 扫描一结束就再也问不出"这一轮有没有触顶"了。
    // 原来这里 Q_UNUSED(truncated) 直接把它扔了 ——
    // 上层采集了、这里收到了、又原样丢掉，界面上一次提示都没有。
    g_scanTruncated = truncated;
    onScanFinished(files);
}

void SyncPanel::onScanFinished(const QVector<FileItem> &files)
{
    m_scanning = false;
    m_refresh->setEnabled(true);

    // 用户还没配同步规则时，用这次扫出来的**真实后缀**生成规则。
    //
    // 为什么这是"识别"而不是"编造"，和刚删掉的那批假数据有本质区别：
    //   编造的：工具 exe 路径 C:/tools/speViewer.exe —— 我们无从得知用户把
    //           工具装在哪，编一个只会被点穿。
    //   识别的：目录里客观存在 .spe/.iq/.dat/.h5 —— 扫一遍就知道，是事实。
    // 所以规则可以自动长出来，工具路径必须用户自己填。
    //
    // 这也顺带解掉了"同步规则跟着工具规则走"造成的死结：
    // 用户一个工具都没配时同步规则恒为空，表格连 tab 都没有，
    // 界面空得像坏了。而"传哪些后缀"这件事本来就不依赖"用什么打开"。
    //
    // 不落盘：识别出的规则先在本次会话生效，用户点开始同步就会用它们；
    // 是否持久化由设置里的保存动作决定，不在背后替他写配置。
    if (m_syncRules.isEmpty() && !files.isEmpty()) {
        QVector<SyncRule> nr;
        QSet<QString> seen;
        for (int i = 0; i < files.size(); ++i) {
            const QString suf = files.at(i).suffix.trimmed();
            // 无后缀文件（README 之类）不生成规则 —— 用户要传的是数据文件，
            // 而把一堆无后缀的杂项自动纳入同步范围，是另一种替他做决定。
            if (suf.isEmpty() || seen.contains(suf))
                continue;
            seen.insert(suf);
            SyncRule r;
            r.suffix = suf;
            nr.append(r);
        }
        if (!nr.isEmpty()) {
            m_syncRules = nr;
            // 标记成"程序自己认出来的"。saveConfig() 见到它就不落盘 ——
            // 见 saveConfig 里那段"用户留空 = 传所有文件"的说明。
            m_rulesAutoGenerated = true;
            QStringList names;
            for (int i = 0; i < nr.size(); ++i)
                names.append(nr.at(i).suffix);
            appendLogLine(QStringLiteral("INFO"),
                          QString::fromUtf8(u8"\u5DF2\u6309\u76EE\u5F55\u5185\u5BB9"
                                           u8"\u81EA\u52A8\u8BC6\u522B %1 \u79CD\u540E\u7F00\uFF1A%2")
                              .arg(nr.size()).arg(names.join(QString::fromUtf8(u8"\u3001"))), 1);
        }
    }

    m_scanKey = currentScanKey();

    QVector<FileItem> displayFiles = files;

    // 工具规则要单独喂给模型，操作列才有按钮可画。
    //
    // 这里原来是无差别地把 m_toolRules 全塞进去，于是**只要配过就画按钮**，
    // 不管那个 exe 在这台机器上到底存不存在。用户看到一排能点的按钮就会去点，
    // 点完才告诉他「工具不存在: C:/tools/speViewer.exe」——
    // 按钮本身就是在骗人：它看起来像一个可用的操作，实际点了必然失败。
    //
    // 配置文件里残留的演示路径（编出来的 C:/tools/*.exe）尤其致命：
    // 它们一路从配置读到界面，用户根本不知道这些不是自己配的。
    //
    // 现在只把**真实存在**的 exe 喂给模型，其余的当没配过 ——
    // 界面显示灰色「未配置」提示，用户就去设置里填真路径。
    // 这不是掩盖错误：文件不存在时按钮本来也执行不了，
    // 而"未配置"比"点了报错"更早、更准确地告诉用户缺什么。
    QHash<QString, ToolRule> ruleHash;
    int missingExe = 0;
    for (int i = 0; i < m_toolRules.size(); ++i) {
        const ToolRule &r = m_toolRules.at(i);
        const QString exe = r.exeAt(0);
        if (exe.isEmpty() || !QFileInfo(exe).exists()) {
            ++missingExe;
            continue;
        }
        ruleHash.insert(r.suffix, r);
    }
    if (missingExe > 0) {
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr(u8"\u6709 %1 \u6761\u5DE5\u5177\u89C4\u5219\u7684\u7A0B\u5E8F\u5728\u672C\u673A\u4E0D\u5B58\u5728\uFF0C\u5DF2\u6682\u4E0D\u663E\u793A\u5DE5\u5177\u6309\u94AE\u3002")
                          .arg(missingExe), 3);
    }

    // 大小写只差的重名文件：必须报出来。
    //
    // 发送端在 Windows 上时，A.spe 和 a.spe 在磁盘上本来就是同一个文件 ——
    // 后写的把先写的覆盖了，Windows 不会提示任何东西。接收端是 Linux
    // （大小写敏感），用户以为传过去的是两个文件，实际远端可能只有一份内容，
    // 或者两份内容不同却与他在 Windows 上看到的对不上。
    //
    // 程序替用户决定传哪一个都有风险，所以只报事实。
    //
    // 事实必须**按平台说得准**。原来第二句写的是「在 Windows 上这些是同一个
    // 文件，后写的会覆盖先写的。」—— 发送端是麒麟时这一句就成了噪音：
    // 麒麟上 A.spe 和 a.spe 就是两个不同的文件，两个都会正常传过去，
    // 谁也覆盖不了谁。用户看到"后写的会覆盖先写的"，会去排查一个
    // 根本不存在的问题。
    //
    // 所以改成条件句：把"会不会覆盖"交给磁盘的实际特性去决定，
    // 两边都不说死。
    const QStringList collisions = FileScanner::caseCollisionPaths(displayFiles);
    if (!collisions.isEmpty()) {
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr(u8"有 %1 个目录里存在只差大小写的重名文件。")
                          .arg(collisions.size()), 3);
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr(u8"如果本机磁盘不区分大小写（例如 Windows），这些名字指向同一个文件，后写的会覆盖先写的。"), 3);
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr(u8"接收端如果也用不区分大小写的磁盘，同样只会保留其中一个。"), 3);
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr(u8"涉及的文件：%1").arg(collisions.join(QStringLiteral(" / "))), 3);
    }
    m_model->setFiles(displayFiles, ruleHash, m_localRoot);

    rebuildTabs();
    const QStringList groups = m_model->suffixGroups();
    if (m_currentTab < groups.size())
        applyFilter(groups.at(m_currentTab));

    // 「深度遍历目录完成」必须走在 0 个文件那条早退**之前**。
    //
    // 下面那段完成日志原来在 if (displayFiles.isEmpty()) 的 return 之后，
    // 于是扫到一个文件都没有时，日志最后一行永远停在「正在深度遍历本机目录：…」
    // —— 看着像还在跑，其实早就结束了。用户既不知道扫完了，更不知道是 0 个。
    //
    // 同一个 return 还顺手吞掉了 m_syncRequested：点开始同步排的那一次请求
    // 留在标志里没消费掉，下次扫描成功时会被当成"用户又点了一次"自动开同步。
    //
    // 实测（麒麟 xcb，目录存在但无匹配文件）：日志停在「正在深度遍历」一行，
    // 后面白板一片，状态却写着「就绪」。
    const bool noFiles = displayFiles.isEmpty();
    m_runDone = 0;
    m_runTotal = displayFiles.size();
    if (!noFiles) {
        for (int r = 0; r < displayFiles.size(); ++r) {
            m_model->setFileState(r, FileTableModel::StateWait);
        }
    }
    m_progress->setValue(0);
    setStatusLine(T(kStatusReady), 0);

    if (noFiles) {
        m_countLabel->setText(QObject::tr("0 个待同步文件"));
    } else {
        qint64 totalBytes = 0;
        for (int i = 0; i < files.size(); ++i) {
            totalBytes += files.at(i).size;
        }
        double totalMb = totalBytes / (1024.0 * 1024.0);
        m_countLabel->setText(QObject::tr("0 / %1 个文件 · %2 MB")
                              .arg(files.size())
                              .arg(totalMb, 0, 'f', 1));
    }

    if (g_scanTruncated) {
        // 触顶时不能停在上面那行写的「已完成」上：目录只扫了一部分，
        // 剩下的文件压根没进清单、这一轮也不会被传走，
        // 报「已完成」等于告诉用户"全传完了"。
        // 状态改回 2（警示配色），文字仍是「就绪」——
        // 扫描本身确实结束了、界面也确实可继续操作，只有"完整"这件事不成立。
        // 真正的原因由下面这行 WARN 说清楚，不靠状态文案承担。
        setStatusLine(T(kStatusReady), 2);
        appendNotice(QObject::tr("\xE6\x89\xAB\xE6\x8F\x8F\xE8\xA7\xA6\xE9\xA1\xB6\xEF\xBC\x9A\xE7\x9B\xAE\xE5\xBD\x95\xE6\x9D\xA1\xE7\x9B\xAE\xE8\xB6\x85\xE8\xBF\x87\xE6\x89\xAB\xE6\x8F\x8F\xE4\xB8\x8A\xE9\x99\x90\xEF\xBC\x8C\xE6\x9C\xAC\xE6\xAC\xA1\xE5\x8F\xAA\xE6\x89\xAB\xE5\x88\xB0 %1 \xE4\xB8\xAA\xE5\xBE\x85\xE5\x90\x8C\xE6\xAD\xA5\xE6\x96\x87\xE4\xBB\xB6\xEF\xBC\x8C\xE5\x85\xB6\xE4\xBD\x99\xE6\x96\x87\xE4\xBB\xB6\xE6\x9C\xAA\xE7\xBA\xB3\xE5\x85\xA5\xE6\xB8\x85\xE5\x8D\x95\xE3\x80\x81\xE4\xB8\x8D\xE4\xBC\x9A\xE8\xA2\xAB\xE4\xBC\xA0\xE8\xBE\x93")
                       .arg(files.size()));
    } else {
        appendLogLine(QStringLiteral("SCAN"),
                      QObject::tr("\xE6\xB7\xB1\xE5\xBA\xA6\xE9\x81\x8D\xE5\x8E\x86\xE7\x9B\xAE\xE5\xBD\x95\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C\xE5\x8F\x91\xE7\x8E\xB0 %1 \xE4\xB8\xAA\xE5\xBE\x85\xE5\x90\x8C\xE6\xAD\xA5\xE6\x96\x87\xE4\xBB\xB6")
                          .arg(files.size()), 0);
    }

    // 扫描是「开始同步」的前置步骤时，扫完要自动接着同步，
    // 不该让用户为了传个文件先点一次刷新再点一次开始。
    if (m_syncRequested) {
        m_syncRequested = false;
        if (noFiles) {
            // 一个文件都没有，表格里根本没有行可勾。
            // 让 startSyncInternal 去说「请先勾选至少一个文件」，
            // 用户会翻遍一张空表去找复选框，永远找不到。
            appendLogLine(QStringLiteral("WARN"),
                          QObject::tr("\xE6\x9C\xAC\xE6\x9C\xBA\xE7\x9B\xAE\xE5\xBD\x95\xE4\xB8\x8B\xE6\xB2\xA1\xE6\x9C\x89\xE5\x8C\xB9\xE9\x85\x8D\xE5\xBE\x85\xE5\x90\x8C\xE6\xAD\xA5\xE7\x9A\x84\xE6\x96\x87\xE4\xBB\xB6\xEF\xBC\x8C\xE6\x9C\xAC\xE6\xAC\xA1\xE5\x90\x8C\xE6\xAD\xA5\xE5\xB7\xB2\xE5\x8F\x96\xE6\xB6\x88\xE3\x80\x82"), 3);
        } else {
            startSyncInternal();
        }
    }
}

// ---------------------------------------------------------------- 同步

void SyncPanel::startSyncInternal()
{
    if (!m_target.isValid()) {
        appendLogLine(QStringLiteral("ERR"),
                      QObject::tr("\xE7\x9B\xAE\xE6\xA0\x87\xE7\x94\xB5\xE8\x84\x91 IP \xE6\x88\x96\xE7\x99\xBB\xE5\xBD\x95\xE8\xB4\xA6\xE5\x8F\xB7\xE4\xB8\xBA\xE7\xA9\xBA\xE3\x80\x82"), 4);
        emit settingsRequested();
        return;
    }

    // 清单对不上当前配置（配置改过、或者压根没扫过）就先重扫。
    // 使用手册 §2.4：「点开始同步会自动重新扫描一次」，所以扫描不是前置仪式。
    if (m_scanKey != currentScanKey()) {
        m_syncRequested = true;
        scanFiles();
        return;
    }

    QVector<int> rows = m_model->checkedRows();
    if (rows.isEmpty()) {
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr("\xE5\xB7\xB2\xE9\x80\x89\xE6\x96\x87\xE4\xBB\xB6\xE6\x95\xB0\xE4\xB8\xBA 0\xE3\x80\x82"), 3);
        return;
    }

    QVector<FileItem> picked;
    const QVector<FileItem> all = m_model->files();
    for (int i = 0; i < rows.size(); ++i)
        picked.append(all.at(rows.at(i)));
    m_runTotal = picked.size();
    m_runDone = 0;
    // 分组必须在开跑前算好：全局进度要拿每组的文件数当权重。
    rebuildGroupSizes(picked);
    m_progressTrace.clear();

    setProgressValue(0, "start");
    setStatusLine(T(kStatusRun), 0);
    setBusy(true);
    m_startBtn->setText(T(kCancelSync));
    m_startBtn->setObjectName(QStringLiteral("btnStartSyncCancel"));
    m_startBtn->style()->polish(m_startBtn);
    m_runStartMs = QDateTime::currentMSecsSinceEpoch();
    m_elapsedTimer->start();

    // 排队：所有选中行记录在队列中，真实开始传输某个文件时由 onFileStarted 精确点亮
    m_pendingRows.clear();
    for (int i = 0; i < picked.size(); ++i) {
        const int row = m_model->rowForRelativePath(picked.at(i).relativePath);
        if (row >= 0)
            m_pendingRows.append(row);
    }

    appendLogLine(QStringLiteral("INFO"),
                  QObject::tr("\xE7\xB3\xBB\xE7\xBB\x9F\xE5\x88\x9D\xE5\x8C\x96\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C\xE7\x9B\xAE\xE6\xA0\x87\xEF\xBC\x9A%1:%2")
                      .arg(m_target.host).arg(m_target.port), 0);

    // 勾选项第一次真正被读：这个值决定 rsync 走 --ignore-existing
    // 还是 --checksum。以前它只被创建、没人读，是纯摆设。
    m_runner->start(m_target, m_localRoot, m_syncRules, picked,
                     m_onlyNew && m_onlyNew->isChecked());
}

// 把主按钮复位成"开始同步"。**objectName 必须一起改回来**。
//
// 它是"运行中"的唯一开关：startSyncInternal 里改成 btnStartSyncCancel，
// 而 Theme 里 #btnStartSyncCancel 是红底（#ef4444）。原来复位时只 setText、
// 不改 objectName，也不重新 polish —— 于是同步一结束，主按钮就永远停在红色，
// 摆着一个写着「开始同步」的红按钮。用户第二次点的时候看到的就是它。
// 而且不会报任何错：文字是对的、行为是对的，只有颜色不对。
void SyncPanel::setStartButtonIdle()
{
    m_startBtn->setText(T(kStartSync));
    m_startBtn->setObjectName(QStringLiteral("btnStartSync"));
    m_startBtn->style()->polish(m_startBtn);
}

void SyncPanel::startOrCancel()
{
    // 只要处于忙态，这一按就是取消 —— 不再回头看进程在不在。
    //
    // 原来写成 `m_busy && m_runner->isRunning()`：组与组之间 m_proc 会短暂为空，
    // 那一瞬间点一下就会**再启动一次同步**，而界面上正显示着上一次在跑。
    // 按钮现在传输中一直可点（它就是取消按钮），这个窗口用户完全够得着。
    if (m_busy) {
        if (m_runner->isRunning())
            m_runner->cancel();
        setStatusLine(T(kStatusStop), 2);
        m_startBtn->setText(T(kCancelSync));
        appendLogLine(QStringLiteral("WARN"),
                      QObject::tr("\xE7\x94\xA8\xE6\x88\xB7\xE5\x8F\x96\xE6\xB6\x88\xEF\xBC\x8C\xE5\xB7\xB2\xE4\xBC\xA0\xE8\xBF\x87\xE7\x9A\x84\xE4\xB8\x8D\xE4\xBC\x9A\xE9\x87\x8D\xE4\xBC\xA0"), 3);
        return;
    }
    startSyncInternal();
}

void SyncPanel::onFileStarted(const QString &relativePath)
{
    const int row = m_model->rowForRelativePath(relativePath);
    if (row >= 0) {
        // 将该真实开始传输的行设为「同步中」
        m_model->setFileState(row, FileTableModel::StateSyncing);
        // 这个文件属于哪一组，就把"当前组"推进到那一组。
        // 组内百分比是每组从 0 重新开始的，不在这里换算回去，
        // 上一组留下的高位就会一直挂在进度条上（缺陷 2）。
        // 整组被跳过时不会走到这里，下一组第一个文件开传时一步跨过去，
        // 中间那些"整组没碰过"的组按整组文件数一次算满 —— 那一组确实处理完了。
        const QVector<FileItem> &fs = m_model->files();
        if (row < fs.size()) {
            const int gi = groupIndexOfSuffix(fs.at(row).suffix);
            if (gi >= 0 && gi > m_groupIndex) {
                m_groupIndex = gi;
                m_groupPercent = 0;
                // 新的一组 rsync 的字节数从 0 重新报。此刻 onByteStats 还没
                // 为新组发过任何字节，m_lastBytes 就是上一轮显示的全局值，
                // 正好拿来当基线 —— 不记的话计数栏会掉回本组的小数字。
                m_groupBytesBase = m_lastBytes;
            }
        }
    }
}

void SyncPanel::onFileTransferred(const QString &relativePath, int done, int total)
{
    Q_UNUSED(total);
    const int row = m_model->rowForRelativePath(relativePath);
    if (row >= 0) {
        m_model->setFileState(row, FileTableModel::StateOk);
        m_pendingRows.removeAll(row);
    }
    // 分子是「已处理完」而不是「已传输」，全跳过那一轮在 onAllFinished 里补齐。
    m_runDone = done;
    // 这里原来还有一段"文件数换算成百分比、只涨不跌"的保底，现在删掉了。
    //
    // 它当时是给组内百分比兜底的，可那个组内百分比本身就是错的全局值（缺陷 2）；
    // 而 max() 只增不减，又正好让第 2 组从 0% 重新开始时被挡住不更新，
    // 进度条卡在高位不动 —— 兜底本身就是缺陷的一部分，不是补救。
    //
    // 现在进度条只有一个口径：globalProgressPercent()。
    // 文件计数回到计数行报「已处理 / 总数」，不再去动进度条。
}

// 把一个已经被 onFileTransferred 标成「已完成」的行改回「失败」。
//
// 为什么会走到这一步：--out-format=%n 是「开始传输」时打印的，
// 传了一半就失败的文件**照样会被打印**，于是 onStdout 里
// 「下一个开始 == 上一个结束」的推断把它标成了「已完成」。
// 以前只有最后一个文件靠退出码兜住，中间的失败文件一律误标 ——
// 界面上是绿的，远端是半截文件，下游照单全收就是静默消费损坏数据。
//
// 这里只改显示状态，不动计数：m_runDone 是「已处理完」的分子，
// 一个处理过（哪怕失败）的文件确实已经处理完了，计数没错。
void SyncPanel::onFileFailedInGroup(const QString &relativePath)
{
    const int row = m_model->rowForRelativePath(relativePath);
    if (row < 0)
        return;
    // 只降级「已完成」和「同步中」两种：已经是失败/跳过的行不用再动，
    // 而「待传输」的行不能动 —— 它还没轮到，rsync 报它失败是因为
    // 上一组传它时失败过，本组还没开始，不该提前判死。
    const FileTableModel::FileState st = m_model->fileState(row);
    if (st != FileTableModel::StateOk && st != FileTableModel::StateSyncing)
        return;
    m_model->setFileState(row, FileTableModel::StateFailed);
    m_pendingRows.removeAll(row);
    renderCountLabel();
}

void SyncPanel::onAllFinished(bool ok)
{
    ++m_syncFinishedCount;
    m_syncLastOk = ok;
    // **停表之前先补最后一次刷新。**
    //
    // tickElapsed() 是 1Hz 的，而"最后一个文件传完"到 allFinished 之间
    // 往往不到一秒：两万个文件那一轮的收尾整个落在同一个 tick 窗口里。
    // 原来这里先 m_elapsedTimer->stop() 再 setBusy(false)，而 tickElapsed 开头
    // 就有 !m_busy 就返回的守卫 —— 计数器停在最后一次定时器刷新时的值，
    // 之后再也不会有人去补。
    //
    // 实测（A4，20,000 个 1 字节文件）：界面停在 "19427 / 20000 个文件"，
    // 而产品自己的日志写的是「共 20000 个文件」、接收端也确实是 20000 个。
    // 同一份数据在日志和界面上给出两个数，B1 那次又恰好追上了 50000/50000，
    // 说明这是**非确定性竞态**，不是必然错 —— 越正常反而越难发现。
    tickElapsed();
    m_elapsedTimer->stop();
    setBusy(false);

    // 队列里剩下的行必须有归宿。之前它们会永远停在「同步中」——
    // 而在此之前更糟：这些状态根本没人设，所有行一路停在「待传输」。
    //
    // 取消 -> 已跳过；失败 -> 失败；成功却没收到回包 -> 按已完成收掉
    // （回包可能因为进程提前退出而丢，但远端文件是真的在的，
    //   标成"失败"会让用户去查一个其实没问题的文件）。
    const bool canceled = m_runner->wasCanceled();
    // 「仅同步新增文件」= --ignore-existing：远端已有同名文件时 rsync
    // **一个字节都不传、也不为它打印任何行**，退出码仍然是 0。
    // 所以还留在 m_pendingRows 里的行 = 远端本来就有、本次完全没碰过的文件。
    // 这种行原来落进 ok ? StateOk，被标成「已完成」——
    // 实测（A5）：finalProgress=100、行标「已完成」，而产品日志自己写的是
    // 「共 0 个文件」。同一个界面上两处自相矛盾，用户只能信更醒目的那一处。
    //
    // 不开这个勾选时不能这么判：那种情况下留在队列里的是"成功却没收到回包"
    // 的文件（回包可能因为进程提前退出而丢，但远端文件是真的在的），
    // 按已完成收掉才对 —— 见上面那段注释。
    const bool onlyNew = m_onlyNew && m_onlyNew->isChecked();
    const bool leftoverUntransferred = ok && !canceled && onlyNew && !m_pendingRows.isEmpty();
    const FileTableModel::FileState rest =
        FileTableModel::resolveRestState(canceled, leftoverUntransferred, ok);
    for (int i = 0; i < m_pendingRows.size(); ++i)
        m_model->setFileState(m_pendingRows.at(i), rest);
    m_pendingRows.clear();

    // 全部组都结束 = 本轮每个文件都"处理完"了：传过去的算完成，
    // 判定无需传输的也算完成。这一刻把分子从「已传输」改成「已处理完」。
    //
    // 原来只在真传了东西的时候才推 100（`if (m_runDone > 0)`），
    // 于是「仅同步新增文件」+ 远端已有同名文件那一轮：
    //   进度条停在 0%、计数行停在「0 / 3 个文件」、状态却写「已完成」，
    //   同一屏日志还写着「共 3 个文件，分 1 组同步」。
    // 那一轮**确实做了真实工作**（连接、比对远端、逐个判定），
    // 只是没有字节要搬 —— 进度条必须走完，分子必须算它（缺陷 1）。
    //
    // 只在 ok 时补：取消和失败下没处理完的文件不该被算进去。
    if (ok && m_runTotal > 0)
        m_runDone = m_runTotal;
    // tickElapsed() 在上面就跑完了，刷的是旧分子，这里必须补一次。
    renderCountLabel();

    if (canceled) {
        setStatusLine(T(kStatusStop), 2);
        setStartButtonIdle();
        // 半途取消：进度条停在 99 或更低，绝不打满 ——
        // 那一轮搬不完，打成 100 等于替用户宣布"全传完了"。
        writeProgressProbeReport();
        return;
    }
    if (ok) {
        setStatusLine(T(kStatusDone), 1);
        // 全部组都正常结束才允许打满 100。这一行是全程序**唯一**写 100 的地方，
        // 取消和失败两条分支都到不了 —— 所以"取消不打满""失败不打满"
        // 不是靠另外两处记得别写，而是靠根本走不到这里。
        if (m_runTotal > 0)
            setProgressValue(100, "finished");
        setStartButtonIdle();
        if (leftoverUntransferred)
            // 单独一句把"完成但没传"说清楚。RsyncRunner 那条"勾了仅同步新增"
            // 的日志讲的是覆盖语义、方向不同；这里讲的是这一轮的实际结果。
            appendLogLine(QStringLiteral("DONE"),
                          QObject::tr("\xE5\x90\x8C\xE6\xAD\xA5\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C"
                                      "\xE8\xBF\x9C\xE7\xAB\xAF\xE6\x96\x87\xE4\xBB\xB6\xE5\xB7\xB2\xE6\x98\xAF"
                                      "\xE6\x9C\x80\xE6\x96\xB0\xEF\xBC\x8C\xE6\x9C\xAC\xE6\xAC\xA1\xE6\xB2\xA1"
                                      "\xE6\x9C\x89\xE9\x9C\x80\xE8\xA6\x81\xE4\xBC\xA0\xE8\xBE\x93"
                                      "\xE7\x9A\x84\xE6\x96\x87\xE4\xBB\xB6"), 2);
        else
            appendLogLine(QStringLiteral("DONE"),
                          QObject::tr("\xE5\x90\x8C\xE6\xAD\xA5\xE5\xAE\x8C\xE6\x88\x90\xEF\xBC\x8C\xE5\x85\xB1 %1 \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6").arg(m_runDone), 2);
        writeProgressProbeReport();
    }
    else {
        // 失败不能只改颜色不说原因：用户必须能在日志里看到真实报错。
        setStatusLine(T(kStatusStop), 2);
        setStartButtonIdle();
        appendLogLine(QStringLiteral("ERR"),
                      QObject::tr("\xE5\x90\x8C\xE6\xAD\xA5\xE5\xA4\xB1\xE8\xB4\xA5\xEF\xBC\x8C\xE8\xAF\xA6\xE8\xA7\x81\xE4\xB8\x8A\xE6\x96\xB9\xE6\x97\xA5\xE5\xBF\x97\xE6\x9D\xA1"), 4);
        // 失败同样不打满 100：进度条停在走到的地方，如实反映这轮没搬完。
        writeProgressProbeReport();
    }
}

void SyncPanel::onRunLog(const QString &text)
{
    // RsyncRunner 吐的是纯文本行，这里按行拆成徽标 + 正文。
    const QStringList lines = text.split(QLatin1Char('\n'), QString::SkipEmptyParts);
    for (int i = 0; i < lines.size(); ++i) {
        const QString l = lines.at(i).trimmed();
        if (l.isEmpty())
            continue;
        int kind = 0;
        QString tag = QStringLiteral("INFO");
        const QString lower = l.toLower();
        if (lower.contains("failed") || lower.contains("error") || lower.contains("denied")) {
            kind = 4; tag = QStringLiteral("ERR");
        }
        else if (lower.contains("skip") || lower.contains("unchanged")) {
            kind = 3; tag = QStringLiteral("SKIP");
        }
        else if (lower.contains("total size") || lower.contains("sent ")) {
            kind = 2; tag = QStringLiteral("DONE");
        }
        appendLogLine(tag, l, kind);
    }
}

void SyncPanel::loadConfig()
{
    m_target = ConfigStore::loadTarget();
    m_syncRules = ConfigStore::loadSyncRules();
    m_toolRules = ConfigStore::loadToolRules();
    m_localRoot = m_target.localRoot;

    // 这里原来有两段"方便演示"的逻辑，两段都删了。
    //
    // 段一：目录为空/不存在/路径含 "setup/local" 时，自动改指向硬编码的
    //       D:/ReplayData。除了凭空替用户决定数据源，还有两个更坏的地方：
    //       用户配的目录（移动硬盘、网络盘）**暂时没挂载**也会被悄悄换掉，
    //       挂载恢复后还切不回来；而 contains("setup/local") 是把**测试沙箱
    //       路径写进了产品逻辑** —— 真实路径 D:\data\setup\local_files
    //       同样命中，于是用户自己的目录被替换且零提示。
    //
    // 段二：工具规则为空时凭空造 3 条（spe/iq/h5），exe 路径指向并不存在的
    //       C:/tools/speViewer.exe 等。这一段比段一更严重，因为它**会落盘**：
    //       析构和 closeEvent 都调 saveConfig()，于是用户只要打开再关一次程序，
    //       假工具规则就进了配置文件，之后与他自己配的规则无法区分；
    //       更糟的是用户下次在设置里点一次"保存设置"，applyFromSettings
    //       会把这几个后缀**转成正式的同步规则**，这些文件开始被真的纳入传输。
    //
    // 客户要求打开程序时该是空的：没配就是没配。目录和工具都由用户自己填。
    // 要自动生成，用 FileScanner::detectSuffixes() 扫出后缀再让用户确认，
    // 而不是替他先编一套。
}

void SyncPanel::saveConfig()
{
    // 存之前把内存里的本机目录回填进 target：localRoot 在界面上是独立字段，
    // 只写进 m_localRoot 的话 ConfigStore 拿不到，落盘又会漏。
    m_target.localRoot = m_localRoot;
    ConfigStore::saveTarget(m_target);
    // 自动识别出来的后缀**不许落盘**。
    //
    // 造规则的地方（onScanFinished）注释里写着"不落盘：识别出的规则先在本次
    // 会话生效，是否持久化由设置里的保存动作决定"，但 saveConfig() 原来无条件
    // 把 m_syncRules 写回去 —— 而析构和 closeEvent 都调它。于是：
    //   1. 用户把同步规则**留空**（= 传所有文件，符合他的本意）；
    //   2. 跑一次同步，扫到目录里只有 .spe，于是自动生成了一条 suffix=spe；
    //   3. 退出程序 → "传所有文件"被永久改写成"只传 .spe"；
    //   4. 下次开机那条 spe 规则已经在配置里，用户界面看着也正常，
    //      于是 .iq/.h5 从此**静默不再传输**，界面上一个字的提示都没有。
    //
    // 逗号分隔的多后缀同理：填 "spe,iq,h5"，跑一次之后只剩扫到的第一个。
    //
    // 用户要保存这些规则，只有一条路：自己在设置里点保存。程序识别出来的
    // 东西替他保存，等于替他做了"以后只传这些后缀"的决定。
    ConfigStore::saveSyncRules(m_rulesAutoGenerated ? QVector<SyncRule>() : m_syncRules);
    ConfigStore::saveToolRules(m_toolRules);
}

void SyncPanel::applyFromSettings(const TargetConfig &t, const QString &localRoot,
                                  const QVector<ToolRule> &tools, const QVector<SyncRule> &rules){
    m_target = t;
    m_localRoot = localRoot;
    m_toolRules = tools;

    // 后缀规则跟着工具规则走：配了哪些后缀就同步哪些后缀。
    // 同一后缀配多条工具时只建一条落地规则（取第一条工具的），避免重复传同一批文件。
    //
    // remoteSubDir 必须**从旧规则继承**，不能留空。
    //
    // 为什么：定稿 UI 里没有这个输入框，用户只能从旧配置继承它 ——
    // 而"继承"过去从来没发生过。这里原来是 `SyncRule r; r.suffix = suf;`，
    // 后面 m_syncRules = nr 一覆盖，继承来的值当场被清成空串，
    // 用户只要点一次「保存设置」就丢了。
    //
    // 后果不是"少个配置项"：RsyncRunner 按 remoteSubDir 决定远端落地目录，
    // vendor/spe 变成空之后，文件从 <远端根>/vendor/spe/ 改落到 <远端根>/，
    // 客户的远端目录结构被改掉，全程零提示。
    //
    // 存整条旧规则（不只是 remoteSubDir），因为下面还要拿它**保命**：
    // 定稿 UI 里能配的只有工具规则，用户可能只配了"按后缀分目录"而没配工具，
    // 这时旧规则里的那个后缀是唯一的信息来源，丢掉就等于丢掉用户的配置。
    QHash<QString, SyncRule> oldRules;   // 后缀(小写) -> 旧规则
    for (int i = 0; i < m_syncRules.size(); ++i) {
        const SyncRule &old = m_syncRules.at(i);
        const QString key = ToolRunner::normSuffix(old.suffix);
        if (key.isEmpty())
            continue;
        if (!oldRules.contains(key))
            oldRules.insert(key, old);
    }

    QVector<SyncRule> nr;
    QSet<QString> seen;
    for (int i = 0; i < tools.size(); ++i) {
        const QString suf = tools.at(i).suffix;
        const QString key = ToolRunner::normSuffix(suf);
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        SyncRule r;
        r.suffix = key;
        r.remoteSubDir = oldRules.value(key).remoteSubDir;
        nr.append(r);
    }
    // 旧规则里**没有对应工具**的那些后缀，必须原样保留。
    //
    // 以前这段循环依赖调用方把旧规则传进来，而 MainWindow 传的是空的
    // QVector<SyncRule>() —— 于是这个循环一次都不执行，用户配的
    // "按后缀分目录"规则点一次「保存设置」就没了。
    //
    // 后果比"少个配置项"严重得多：m_syncRules 变空之后，
    // FileScanner 的 wanted 列表也变空，而空列表意味着**不过滤** ——
    // 目录下 .txt / .exe / .dll 全部进入同步范围。
    // 用户只想传 .spe，结果整个目录都被推到了接收端，全程零提示。
    for (int i = 0; i < m_syncRules.size(); ++i) {
        const SyncRule &old = m_syncRules.at(i);
        const QString key = ToolRunner::normSuffix(old.suffix);
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        nr.append(old);
    }
    // 调用方明确传进来的规则优先（将来有 UI 能直接编辑同步规则时用得上）。
    for (int i = 0; i < rules.size(); ++i) {
        const QString key = ToolRunner::normSuffix(rules.at(i).suffix);
        if (key.isEmpty() || seen.contains(key))
            continue;
        seen.insert(key);
        SyncRule r = rules.at(i);
        r.suffix = key;
        nr.append(r);
    }
    m_syncRules = nr;
    // 设置里的「保存设置」是**用户明确的动作**：从这一刻起这批规则是用户选的，
    // 该落盘。必须清掉自动识别标记，否则用户亲手点保存仍然存不进去
    // （那会变成"保存按钮失灵"，比原来的缺陷更难解释）。
    m_rulesAutoGenerated = false;

    // 配置变了，手上的清单就作废：下一次同步前必须重扫，
    // 否则会拿旧目录的清单去传新配置。
    m_scanKey.clear();
    m_model->setFiles(QVector<FileItem>(), QHash<QString, ToolRule>(), m_localRoot);
    rebuildTabs();

    // 这里**故意不调 scanFiles()**。
    //
    // 「保存设置后自动刷新」这条能力已经由 MainWindow::onSettingsAccepted()
    // 末尾那一行 rescanNow() 承担了（它就是 scanFiles() 的公开入口）。
    // 在这里再加一次就是背靠背扫两遍：第一次的结果刚要送到，
    // 第二次又起来了，m_model 被清空重建、m_runTotal 翻倍。
    // rescanrace 场景量的正是"保存设置后扫描只起一次"，
    // 多这一次会把那条判据直接打红。
    //
    // 留这一行注释，是因为"保存后不刷新"和"保存后刷新两次"两种情况
    // 看起来一样（表都是满的），光看界面分不出来。
}

// ---------------------------------------------------------------- 配置提示

void SyncPanel::appendNotice(const QString &text)
{
    // 配置层的提示（规则被截断、口令解不开）一律进日志抽屉。
    // 走 QMessageBox 会在关窗路径上卡住嵌套事件循环，表现为"关不掉"。
    if (!m_logOpen)
        toggleLogDrawer();
    appendLogLine(QStringLiteral("WARN"), text, 3);
}

void SyncPanel::testConnectionWithTarget(const TargetConfig &t, QWidget *sink)
{
    Q_UNUSED(sink);
    if (!t.isValid()) {
        emit testConnectionResult(QObject::tr("\xE8\xAF\xB7\xE5\x85\x88\xE5\xA1\xAB\xE4\xB8\x8A\xE7\x9B\xAE\xE6\xA0\x87\xE7\x94\xb5\xE8\x84\x91\xE5\x92\x8C\xE7\x99\xBB\xE5\xBD\x95\xE8\xB4\xA6\xE5\x8F\xB7"), false, false);
        return;
    }

    // 「正在测试…」先回填，用户才知道按钮点下去了。
    emit testConnectionResult(QObject::tr("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xB5\x8B\xE8\xAF\x95\xE2\x80\xA6"), false, true);
    appendLogLine(QStringLiteral("INFO"),
                  QObject::tr("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xB5\x8B\xE8\xAF\x95\xE8\xBF\x9E\xE6\x8E\xA5 %1:%2 \xE2\x80\xA6").arg(t.host).arg(t.port), 0);

    QThreadPool::globalInstance()->start(new ConnTestWorker(t, m_runner, this));
}

void SyncPanel::testConnectionAsync(QWidget *sink)
{
    testConnectionWithTarget(m_target, sink);
}

void SyncPanel::deliverConnResult(const QString &detail, bool ok)
{
    // QStringLiteral 要的是编译期字面量，不能塞三目运算符。
    appendLogLine(ok ? QStringLiteral("DONE") : QStringLiteral("ERR"), detail, ok ? 2 : 4);
    emit testConnectionResult(detail, ok, false);
}

// ---------------------------------------------------------------- 测试接口

bool SyncPanel::isSyncBusyForTest() const
{
    return m_runner && m_runner->isRunning();
}

int SyncPanel::fileCountForTest() const
{    return m_model ? m_model->rowCount() : 0;
}

// 日志上限：判据要能"灌进去再读回来"，只断言那行常量是 2000 等于没测。
int SyncPanel::logBlockCountForTest() const
{
    return m_logView ? m_logView->document()->blockCount() : -1;
}

int SyncPanel::logCapForTest() const
{
    return m_logView ? m_logView->document()->maximumBlockCount() : -1;
}

void SyncPanel::appendLogLinesForTest(int n)
{
    for (int i = 0; i < n; ++i) {
        // 纯 ASCII 的行正文：判据只按块数断言，不去比中文。
        appendLogLine(QStringLiteral("INFO"),
                      QStringLiteral("probe-line-%1").arg(i + 1), 0);
    }
}

void SyncPanel::resetToolLaunchStateForTest()
{
    m_toolLaunchAtMs.clear();
    m_toolLaunchAttempts = 0;
    m_toolDebounceSuppressed = 0;
    // m_toolFinishedCount 故意**不**清：判据要跨多次 reset 累计它来等进程结束，
    // 清了的话等待条件会从一个已经越过的数重新开始，直接空转。
}

// 见 SyncPanel.h 里的说明：自检必须能看见产品自己说了什么，
// 否则"没跑"和"跑了没残留"在指标上完全一样。
QStringList SyncPanel::logTailForTest(int maxLines) const
{
    QStringList lines;
    if (!m_logView)
        return lines;
    const QString text = m_logView->toPlainText();
    const QStringList all = text.split(QLatin1Char('\n'));
    for (int i = all.size() - 1; i >= 0 && lines.size() < maxLines; --i) {
        const QString t = all.at(i).trimmed();
        if (!t.isEmpty())
            lines.prepend(t);
    }
    return lines;
}

void SyncPanel::setLocalRootForTest(const QString &v)
{
    m_localRoot = v;
    // 顺带镜像到 m_target。产品里这两份本来由 applyFromSettings / saveConfig 分别维护，
    // 但测试钩子只设一份的话，诊断输出读到的 localRoot 会是空的，
    // 让人以为没设进去 —— 而"以为没设进去"和"真的没设进去"必须能分开。
    m_target.localRoot = v;
    m_scanKey.clear();
}
void SyncPanel::setHostForTest(const QString &v)      { m_target.host = v; }
void SyncPanel::setUserForTest(const QString &v)      { m_target.user = v; }
void SyncPanel::setKeyPathForTest(const QString &v)   { m_target.keyPath = v; }
void SyncPanel::setRemoteRootForTest(const QString &v) { m_target.remoteRoot = v; }

void SyncPanel::setOnlyNewForTest(bool v)
{
    if (m_onlyNew) m_onlyNew->setChecked(v);
}
bool SyncPanel::onlyNewForTest() const
{
    return m_onlyNew && m_onlyNew->isChecked();
}

void SyncPanel::setRulesForTest(const QStringList &suffixes)
{
    m_syncRules.clear();
    for (int i = 0; i < suffixes.size(); ++i) {
        SyncRule r;
        r.suffix = suffixes.at(i).toLower();
        m_syncRules.append(r);
    }
    m_scanKey.clear();
}

void SyncPanel::setToolRulesForTest(const QVector<ToolRule> &rules)
{
    m_toolRules = rules;
    if (!m_model)
        return;
    QHash<QString, ToolRule> ruleHash;
    for (int i = 0; i < m_toolRules.size(); ++i)
        ruleHash.insert(m_toolRules.at(i).suffix, m_toolRules.at(i));
    // 操作列的按钮是 delegate 在 paint() 里现算的（toolsForRow 查的就是这份表），
    // 不把新规则重新喂给模型，按钮一个都画不出来。走 setFiles 重灌会清掉行状态，
    // 但这个钩子只在扫描之后、行状态还空着的时候调，没有东西可丢。
    m_model->setFiles(m_model->files(), ruleHash, m_localRoot);
}

// ---------------------------------------------------------------- 进度序列探针
//
// 设了 SRT_PXP_REPORT 就自己跑一轮同步，把整轮进度条的真实取值序列、
// 终值、计数分子、状态行和每个文件的最终状态写成一份文本，然后退出进程。
//
// 驱动的必须是「开始同步」这条产品路径（startSyncForTest -> startOrCancel），
// 不是单独调几个回调模拟出来的：组怎么分、跳过时 rsync 报不报行、
// 全部结束发不发 allFinished，这些都只在真实跑一遍时才是真的。
//
// 环境变量（照 --selfcheck-secrets 那套 SRT_SC_* 的约定）：
//   SRT_PXP_REPORT   报告输出路径（设了才启用探针）
//   SRT_PXP_HOST / _PORT / _USER / _KEY / _REMOTE   接收端
//   SRT_PXP_LOCAL    本机待传目录
//   SRT_PXP_RULES    逗号分隔的后缀，决定分成几组
//   SRT_PXP_ONLYNEW  1 = 勾上「仅同步新增文件」
//   SRT_PXP_CANCEL_MS  >0 = 起跑后多少毫秒点一次「取消」

void SyncPanel::startProgressProbe()
{
    m_probeReportPath = QString::fromLocal8Bit(qgetenv("SRT_PXP_REPORT"));
    if (m_probeReportPath.isEmpty())
        return;

    setHostForTest(QString::fromLocal8Bit(qgetenv("SRT_PXP_HOST")));
    setPortForTest(QString::fromLocal8Bit(qgetenv("SRT_PXP_PORT")).toInt());
    setUserForTest(QString::fromLocal8Bit(qgetenv("SRT_PXP_USER")));
    setKeyPathForTest(QString::fromLocal8Bit(qgetenv("SRT_PXP_KEY")));
    setRemoteRootForTest(QString::fromLocal8Bit(qgetenv("SRT_PXP_REMOTE")));
    setLocalRootForTest(QString::fromLocal8Bit(qgetenv("SRT_PXP_LOCAL")));
    const QStringList suffixes = QString::fromLocal8Bit(qgetenv("SRT_PXP_RULES"))
                                     .split(QLatin1Char(','), QString::SkipEmptyParts);
    setRulesForTest(suffixes);
    setOnlyNewForTest(!QString::fromLocal8Bit(qgetenv("SRT_PXP_ONLYNEW")).isEmpty());

    // 推迟到事件循环起来之后再设：构造函数里 loadConfig() 刚把用户配置读进来，
    // 在它之前设会被覆盖掉，那样跑出来的就是**用户配置**而不是探针给的这一套，
    // 而两者长得一模一样（都是"能连上"或"连不上"），报错都定位不到自己头上。
    QTimer::singleShot(0, this, SLOT(beginProgressProbeRun()));
}

void SyncPanel::beginProgressProbeRun()
{
    startSyncForTest();
    const int cancelMs = QString::fromLocal8Bit(qgetenv("SRT_PXP_CANCEL_MS")).toInt();
    if (cancelMs > 0)
        QTimer::singleShot(cancelMs, this, SLOT(startOrCancel()));
}

int SyncPanel::progressValueForTest() const
{
    return m_progress ? m_progress->value() : -1;
}

QString SyncPanel::statusTextForTest() const
{
    return m_statusLabel ? m_statusLabel->text() : QString();
}

QString SyncPanel::countTextForTest() const
{
    return m_countLabel ? m_countLabel->text() : QString();
}

QStringList SyncPanel::fileStatesForTest() const
{
    QStringList out;
    if (!m_model)
        return out;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        switch (m_model->fileState(r)) {
        case FileTableModel::StateWait:    out.append(QStringLiteral("wait"));    break;
        case FileTableModel::StateSyncing: out.append(QStringLiteral("syncing")); break;
        case FileTableModel::StateOk:      out.append(QStringLiteral("ok"));      break;
        case FileTableModel::StateFailed:  out.append(QStringLiteral("failed"));  break;
        case FileTableModel::StateSkipped: out.append(QStringLiteral("skipped")); break;
        case FileTableModel::StateCanceled: out.append(QStringLiteral("canceled")); break;
        }
    }
    return out;
}

void SyncPanel::writeProgressProbeReport()
{
    if (m_probeReportPath.isEmpty())
        return;
    // 同一个报告路径只写一次。onAllFinished 正常只跑一次，
    // 但探测"同一轮里重复调用会不会写坏报告"时不能写两遍。
    const QString path = m_probeReportPath;
    m_probeReportPath.clear();

    // 用循环拼而不是 QVector::join：这套 Qt 5.9 + VS2015 下
    // QVector<T> 上取不到 QList::join，编不过。
    QStringList groupSuffixes;
    QStringList groupSizes;
    for (int i = 0; i < m_groupSizes.size(); ++i) {
        groupSuffixes.append(m_groupSuffixes.value(i));
        groupSizes.append(QString::number(m_groupSizes.at(i)));
    }

    QString out;
    out += QStringLiteral("runTotal=%1\n").arg(m_runTotal);
    out += QStringLiteral("runDone=%1\n").arg(m_runDone);
    out += QStringLiteral("finalProgress=%1\n").arg(progressValueForTest());
    out += QStringLiteral("ok=%1\n").arg(m_syncLastOk ? 1 : 0);
    out += QStringLiteral("canceled=%1\n").arg((m_runner && m_runner->wasCanceled()) ? 1 : 0);
    out += QStringLiteral("groupSuffixes=%1\n").arg(groupSuffixes.join(QLatin1Char(',')));
    out += QStringLiteral("groupSizes=%1\n").arg(groupSizes.join(QLatin1Char(',')));
    out += QStringLiteral("status=%1\n").arg(statusTextForTest());
    out += QStringLiteral("count=%1\n").arg(countTextForTest());
    out += QStringLiteral("states=%1\n").arg(fileStatesForTest().join(QLatin1Char(',')));
    out += QStringLiteral("seq=%1\n").arg(m_progressTrace.join(QLatin1Char(',')));
    out += QStringLiteral("log=%1\n").arg(logTailForTest(12).join(QStringLiteral(" | ")));

    QFile f(path);
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        f.write(out.toUtf8());
    f.close();

    QApplication::exit(0);
}