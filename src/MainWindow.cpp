#include <cstdio>
#include "MainWindow.h"

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTimer>
#include <QPixmap>
#include <QPainter>
#include <QScreen>
#include <QDesktopWidget>
#include <QMouseEvent>
#include <QCloseEvent>
#include <QGraphicsDropShadowEffect>
#include <QMenu>
#include <QApplication>
#include <QScrollBar>
#include <QAbstractScrollArea>

#include "Theme.h"
#include "SyncPanel.h"
#include "ToolPanel.h"
#include "SettingsDialog.h"
#include "ScreenFit.h"
#include "ConfigStore.h"

namespace {
QString T(const char *s) { return QString::fromUtf8(s); }

const char *kAppName     = "\xE6\x95\xB0\xE6\x8D\xAE\xE5\x90\x8C\xE6\xAD\xA5\xE5\xB7\xA5\xE5\x85\xB7";  // 数据同步工具
const char *kSplashReady = "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE5\x90\x8C\xE6\xAD\xA5\xE5\xBC\x95\xE6\x93\x8E...";  // 正在初始化同步引擎...

// 主界面的名义尺寸**不在本文件另存一份** —— 真源是 ScreenFit.h
// （kMainWidth / kMainHeight / kMainHeightLogOpen）。
//
// 原来这里另写了一份 980/460/759，与 ScreenFit.h 里的同一组数字各改各的：
// 改了一边另一边就过时，正是"屏幕自适应"做成两份规则的根源。
// 2026-10-28 起只留一份（缺陷 4 的同源问题）。
//
// 保留一下当初的来历：
//   · 宽度 980 = HTML 定稿原型 .window-frame width: 980px
//     （更早一版原型是 830，交付包却停在 830 —— 同一份设计稿，
//      HTML 预览和真程序对不上，客户一眼就能看出"不是同一个东西"）。
//     表格 7 列加起来 596px 固定，多出来的 150px 全给「文件名」列。
//   · 高度 460 = 用户定稿截图 1477×691，标题栏 38px、行高 34px
//     都是 1.5 倍缩放 ⇒ 逻辑高度 = 691 / 1.5 ≈ 460。
//   · 展开日志 759 = 设置弹窗整高显示实测 758px（640×758），
//     于是展开态与设置弹窗是同一套尺寸体系。


int g_splashMs = 2000;
bool g_skipSplash = false;

// 齿轮图标统一放在 Theme::gearIcon()。这里原来有一份本地实现，只画了 8 根
// 放射短线、没有外圈也没有中心孔，渲染出来是个"太阳"（原型是齿轮），
// 而且两份实现会各改各的。现在只留 Theme 那一份。

// 启动页底部那条流光线。原型是一条 38% 宽、1px 高、从左扫到右的渐变亮线。
QFrame *makeFlowBar()
{
    QFrame *bar = new QFrame;
    bar->setFixedHeight(2);
    bar->setStyleSheet(QStringLiteral("background: #1f6fe0; border-radius: 1px;"));
    return bar;
}
} // namespace

void MainWindow::setSplashDuration(int ms) { g_splashMs = ms; }
void MainWindow::setSkipSplash(bool skip) { g_skipSplash = skip; }

// ---------------------------------------------------------------- 构造

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(T(kAppName));
    m_startDpr = devicePixelRatioF();

    applyWindowFrame();

    m_root = new QWidget;
    m_root->setObjectName(QStringLiteral("rootFrame"));
    QVBoxLayout *rootLay = new QVBoxLayout(m_root);
    rootLay->setContentsMargins(0, 0, 0, 0);
    rootLay->setSpacing(0);

    buildTitleBar();
    rootLay->addWidget(m_titleBar);

    buildBody();
    rootLay->addWidget(m_body, 1);

    // 直接呈现主界面，无任何等待界面与分层
    m_titleBar->setVisible(true);
    m_body->setVisible(true);

    setCentralWidget(m_root);
    refitToScreen();

    // ConfigStore 是静态 API，只能通过唯一实例发信号，所以由主窗口当中转。
    connect(ConfigStore::instance(), SIGNAL(configNotice(QString)), this, SLOT(onConfigNotice(QString)));
    // 连接探测跑完回填设置弹窗的结果栏
    connect(m_syncPanel, SIGNAL(testConnectionResult(QString,bool,bool)),
            this, SLOT(onTestConnectionResult(QString,bool,bool)));
}

void MainWindow::tickSplashPulse()
{
    if (!m_splashDot)
        return;
    m_splashPulseOn = !m_splashPulseOn;
    m_splashDot->setFixedSize(m_splashPulseOn ? 10 : 6, m_splashPulseOn ? 10 : 6);
    m_splashDot->setStyleSheet(m_splashPulseOn
        ? QStringLiteral("background: #2b7cf0; border-radius: 5px;")
        : QStringLiteral("background: #1f6fe0; border-radius: 3px;"));
}

MainWindow::~MainWindow()
{
    if (m_syncPanel)
        m_syncPanel->saveConfig();
}

void MainWindow::applyWindowFrame()
{
    // 手册 §10.1：禁用原生标题栏，改自定义扁平标题栏。
    // 原生标题栏在 Windows 10/11 上是灰白的，在麒麟上又完全是另一套，
    // 两边对不上，手册里的"微晶工业感"根本无从谈起。
    Qt::WindowFlags flags = Qt::Window;
    flags |= Qt::FramelessWindowHint;
    flags |= Qt::WindowMinimizeButtonHint;
    setWindowFlags(flags);
    setAttribute(Qt::WA_TranslucentBackground, true);
    // 宽度锁在这里，真正的**尺寸**由 refitToScreen() 按窗口所在那块屏统一收敛。
    // 数值取自 ScreenFit.h（唯一真源），本文件不再另存一份。
    setFixedWidth(ScreenFit::kMainWidth);
    setMinimumWidth(ScreenFit::kMainWidth);
    setMaximumWidth(ScreenFit::kMainWidth);
}

// ---------------------------------------------------------------- 启动页

void MainWindow::buildSplash()
{
    m_splash = new QFrame;
    m_splash->setObjectName(QStringLiteral("splashFrame"));
    // 原型 .splash-card 是 410x245。铺满整窗（原来 640x480）会让启动页变成
    // 一张空白大卡片，logo 孤零零飘在中间，和定稿图一眼就不一样。
    m_splash->setFixedSize(410, 245);
    QVBoxLayout *lay = new QVBoxLayout(m_splash);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);
    lay->addStretch(1);

    m_splashLogo = new QLabel;
    m_splashLogo->setPixmap(Theme::splashAp(64));
    m_splashLogo->setFixedSize(64, 64);
    m_splashLogo->setAlignment(Qt::AlignCenter);
    lay->addWidget(m_splashLogo, 0, Qt::AlignHCenter);

    m_splashName = new QLabel(T(kAppName));
    QFont nf = QApplication::font();
    nf.setPixelSize(17);                 // 原型 16.5px，整数像素取 17
    nf.setWeight(QFont::Bold);
    m_splashName->setFont(nf);
    m_splashName->setStyleSheet(QStringLiteral("color: #f8fafc;"));
    m_splashName->setAlignment(Qt::AlignCenter);
    lay->addSpacing(14);
    lay->addWidget(m_splashName);

    // 副文本左侧那颗呼吸的蓝点（原型 .splash-pulse-dot，6px、1.6s 一涨一落）。
    // 没有它，"正在初始化..." 是一行死文字，整个启动页就没了生气。
    QWidget *subRow = new QWidget;
    QHBoxLayout *subLay = new QHBoxLayout(subRow);
    subLay->setContentsMargins(0, 0, 0, 0);
    subLay->setSpacing(6);
    m_splashDot = new QFrame;
    m_splashDot->setFixedSize(6, 6);
    m_splashDot->setStyleSheet(QStringLiteral("background: #1f6fe0; border-radius: 3px;"));
    subLay->addWidget(m_splashDot, 0, Qt::AlignVCenter);

    m_splashStatus = new QLabel(T(kSplashReady));
    QFont sf = QApplication::font();
    sf.setPixelSize(11);
    m_splashStatus->setFont(sf);
    m_splashStatus->setStyleSheet(QStringLiteral("color: #8fa0c0;"));  // AP500BJ 次文字色
    subLay->addWidget(m_splashStatus);
    subLay->addStretch(1);
    lay->addSpacing(5);
    lay->addWidget(subRow, 0, Qt::AlignHCenter);

    // 流光线贴在卡片**底边**（原型 .splash-bottom-bar 是 position:absolute; bottom:0），
    // 所以它不进 VBox —— 一进 VBox 就跟着内容流走，会浮在文字下方而不是边上。
    // m_splash 是固定 410x245 的，直接给绝对几何不会因 resize 跑位。
    QFrame *bar = makeFlowBar();
    bar->setParent(m_splash);
    bar->setFixedWidth(410 / 3);
    bar->setFixedHeight(2);
    bar->move((410 - bar->width()) / 2, 245 - 2);
    bar->raise();
    m_splashBarFill = bar;

    // 上下各留一份 stretch，内容才真的落在卡片正中（原来一顶一底是 1:2，偏高）
    lay->addStretch(1);
}

void MainWindow::tickSplashMorph()
{
    m_splashPhase = 1;
    // 形态 2：波形 + 回放三角。原型是两个形态在同一张卡上交叉淡入淡出
    // （morphOut / morphIn，2.2s）；Qt 里没有 CSS keyframes，这里在切点换图，
    // 换成整张 splashSync —— 原来这个槽里把 label 找到之后就 Q_UNUSED 掉了，
    // 启动页从头到尾只有一个形态，"平滑演变"是写在注释里而没做的。
    if (m_splashLogo)
        m_splashLogo->setPixmap(Theme::splashSync(64));
    if (m_splashBarFill)
        m_splashBarFill->setStyleSheet(QStringLiteral("background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #2b7cf0,stop:1 #1f6fe0); border-radius: 1px;"));
}

void MainWindow::finishSplash()
{
    if (m_splashTimer)
        m_splashTimer->stop();
    if (m_splashMorph)
        m_splashMorph->stop();
    // 呼吸点必须一起停：它是**周期性**的，不 stop 的话启动页藏起来之后
    // 这个定时器还会一直转到窗口销毁，每 800ms 醒一次去改一个看不见的控件。
    if (m_splashPulse)
        m_splashPulse->stop();
    if (m_splash)
        m_splash->setVisible(false);
    if (m_titleBar)
        m_titleBar->setVisible(true);
    if (m_body)
        m_body->setVisible(true);
    refitToScreen();
}

// ---------------------------------------------------------------- 标题栏

void MainWindow::buildTitleBar()
{
    m_titleBar = new QFrame;
    m_titleBar->setObjectName(QStringLiteral("titleBar"));
    m_titleBar->setFixedHeight(44);
    QHBoxLayout *lay = new QHBoxLayout(m_titleBar);
    lay->setContentsMargins(14, 0, 8, 0);
    lay->setSpacing(6);

    // 标题栏左侧**只有文字**，不放 logo。
    // 定稿图（ui_preview.html 的 .window-titlebar）就是这么画的：一个蓝底圆角
    // 图形杵在「数据同步工具」左边，和 AP500「纯净工控、无装饰」的语言相反。
    // 用户 2026-10-27 明确要求去掉，程序图标（任务栏那个）保留。
    QLabel *title = new QLabel(T(kAppName));
    title->setObjectName(QStringLiteral("appTitle"));
    lay->addWidget(title);
    lay->addStretch(1);

    // 右侧顺序与定稿图一致：设置 → 最小化 → 最大化 → 关闭。
    m_settingsHead = new QPushButton;
    m_settingsHead->setObjectName(QStringLiteral("settingsHeadBtn"));
    m_settingsHead->setText(QString::fromUtf8(u8"设置"));
    m_settingsHead->setFixedHeight(24);
    m_settingsHead->setCursor(Qt::PointingHandCursor);
    m_settingsHead->setToolTip(QString::fromUtf8(u8"传输与工具设置"));
    lay->addWidget(m_settingsHead);

    QToolButton *minBtn = new QToolButton;
    minBtn->setObjectName(QStringLiteral("sysBtn"));
    minBtn->setIcon(QIcon(Theme::minIcon(11, Theme::chromeIcon())));
    minBtn->setIconSize(QSize(11, 11));
    minBtn->setFixedSize(34, 26);
    minBtn->setCursor(Qt::PointingHandCursor);
    minBtn->setToolTip(QString::fromUtf8(u8"最小化"));
    connect(minBtn, SIGNAL(clicked()), this, SLOT(showMinimized()));
    lay->addWidget(minBtn);

    // 最大化 / 还原。定稿图标题栏右上角是三键：最小、最大化、关闭，
    // 原来 Qt 这边只有最小和关闭，少了中间那一个 —— 界面和定稿对不上。
    m_maxBtn = new QToolButton;
    m_maxBtn->setObjectName(QStringLiteral("sysBtn"));
    m_maxBtn->setIcon(QIcon(Theme::maxIcon(11, Theme::chromeIcon())));
    m_maxBtn->setIconSize(QSize(11, 11));
    m_maxBtn->setFixedSize(34, 26);
    m_maxBtn->setCursor(Qt::PointingHandCursor);
    m_maxBtn->setToolTip(QString::fromUtf8(u8"最大化"));
    connect(m_maxBtn, SIGNAL(clicked()), this, SLOT(onToggleMaximize()));
    lay->addWidget(m_maxBtn);

    QToolButton *closeBtn = new QToolButton;
    closeBtn->setObjectName(QStringLiteral("sysBtnClose"));
    // 关闭键是红的（定稿图 .ico-close #ff3b30），最小/最大化才是蓝灰。
    closeBtn->setIcon(QIcon(Theme::closeIcon(11, Theme::chromeIconClose())));
    closeBtn->setIconSize(QSize(11, 11));
    closeBtn->setFixedSize(34, 26);
    closeBtn->setCursor(Qt::PointingHandCursor);
    closeBtn->setToolTip(QString::fromUtf8(u8"关闭"));
    connect(closeBtn, SIGNAL(clicked()), this, SLOT(close()));
    lay->addWidget(closeBtn);

    connect(m_settingsHead, SIGNAL(clicked()), this, SLOT(onOpenSettings()));
    // 标题栏空白处用来拖窗：手写实现，不依赖 FramelessWindowHint 的系统行为 ——
    // 那在不同平台/不同 Qt 版本上表现并不一致。
    m_titleBar->installEventFilter(this);
}

// ---------------------------------------------------------------- 主体

void MainWindow::buildBody()
{
    m_body = new QWidget;
    m_body->setObjectName(QStringLiteral("pageBg"));
    QVBoxLayout *lay = new QVBoxLayout(m_body);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    m_syncPanel = new SyncPanel(m_body);
    // 定稿 UI 是单栏：工具调用并进了表格的操作列，不再有独立页签。
    // ToolPanel 保留在树上（测试探针仍通过 toolPanel() 取它），但不显示。
    m_toolPanel = new ToolPanel(m_body);
    m_toolPanel->setVisible(false);

    lay->addWidget(m_syncPanel);

    connect(m_syncPanel, SIGNAL(settingsRequested()), this, SLOT(onOpenSettings()));
    // 日志抽屉的高度决定权交给 MainWindow（SyncPanel 不再自己设窗口高度）。
    connect(m_syncPanel, SIGNAL(logDrawerToggled(bool)), this, SLOT(onLogDrawerToggled(bool)));
}

// ---------------------------------------------------------------- 设置

void MainWindow::onOpenSettings()
{
    if (!m_settings) {
        m_settings = new SettingsDialog(this);
        connect(m_settings, SIGNAL(settingsTestConnectionRequested()), this, SLOT(onTestConnectionRequested()));
        connect(m_settings, SIGNAL(settingsAccepted()), this, SLOT(onSettingsAccepted()));
    }
    m_settings->loadConfig(m_syncPanel->targetConfig(),
                           m_syncPanel->localRoot(),
                           m_syncPanel->toolRules());
    // 探针截图模式下设置面板也不上屏（WA_DontShowOnScreen 必须在 show() 之前设）。
    if (m_probeNoScreen)
        m_settings->setAttribute(Qt::WA_DontShowOnScreen, true);
    m_settings->show();
}

void MainWindow::onLogDrawerToggled(bool open)
{
    // 最大化状态下**不动窗口尺寸**（2026-10-28 缺陷 3）。
    //
    // 原来这里无条件 setFixedHeight(...)：setFixedSize 会把 min/max 一起写死，
    // 而最大化几何由窗口管理器管 —— 结果 isMaximized() 还是 true、
    // 几何却变成 1280×728，成了"最大化但尺寸不对"的怪状态，谁都修不回来。
    // 最大化时日志抽屉在布局内部，175px 高够放，不需要动窗口。
    if (isMaximized())
        return;

    // 高度按**窗口当前所在那块屏**的可用高收敛。
    //
    // 原来用 primaryScreen()：主窗口拖到更矮的副屏仍按主屏算高度，
    // 展开日志直接掉出屏幕（2026-10-28 缺陷 2）。
    // 现在与设置弹窗共用 ScreenFit::screenForWidget() —— 同一块屏、同一套数。
    QScreen *scr = ScreenFit::screenForWidget(this);
    if (!scr)
        return;
    const QRect avail = scr->availableGeometry();

    // 收敛规则只有一份（ScreenFit.h），主界面和设置弹窗共用。
    const QSize sz = ScreenFit::mainWindow(avail.width(), avail.height(), open);

    // ★**位置也要跟着收敛**（2026-10-28 缺陷 1）：
    // 只 setFixedHeight 不挪位置，窗口加高后**底边会掉出屏幕**。
    // 实测（1280×764 可用区）：460 → 759 后底边到 909，超出 146px，
    // 175px 高的日志抽屉只剩 29px 可见 —— 用户点了"展开日志"，
    // 看到的是几乎空的抽屉。窗口拖到屏幕下半屏时超出更多（实测 264px）。
    //
    // 规则：保住原来的顶边/左边，越界才往里挪（ScreenFit::fitInside）。
    const QRect target = ScreenFit::fitInside(sz, geometry(), avail);
    setFixedSize(sz);
    move(target.topLeft());
}

void MainWindow::showSettingsForProbe()
{
    m_probeNoScreen = true;
    onOpenSettings();
}

void MainWindow::saveSettingsForProbe()
{
    onSettingsAccepted();
}

void MainWindow::startSyncForProbe()
{
    if (m_syncPanel)
        m_syncPanel->startSyncForTest();
}

void MainWindow::switchPageForTest(int index)
{
    // 见 MainWindow.h 的注释：旧探针的 setCurrentIndex(0/1) 在单栏 UI 上
    // 用「日志抽屉关 / 开」等价替代。只在当前状态与目标不一致时才动作，
    // 否则探针连着调两次 0 会把抽屉反复开关，白白多出重绘。
    if (!m_syncPanel)
        return;
    const bool wantOpen = (index != 0);
    if (m_syncPanel->isLogOpenForTest() == wantOpen)
        return;
    m_syncPanel->toggleLogDrawer();
}

void MainWindow::onSettingsAccepted(){
    // 「保存设置」过去只调了 QDialog::accept() 关闭弹窗，applyConfig 根本没被调用 ——
    // 用户把 IP、账号、密码、本机目录全改完点保存，界面关掉，配置一个字没变，
    // 下次打开还是旧值。这里才是真正落盘的地方。
    if (!m_settings || !m_syncPanel)
        return;
    TargetConfig t = m_syncPanel->targetConfig();
    QString localRoot;
    QVector<ToolRule> tools;
    m_settings->applyConfig(&t, &localRoot, &tools);
    m_syncPanel->applyFromSettings(t, localRoot, tools, QVector<SyncRule>());
    m_syncPanel->saveConfig();
    m_syncPanel->rescanNow();
}

void MainWindow::onTestConnectionRequested()
{
    // 连接测试由设置弹窗发起，结果回填。真正发起的是 SyncPanel 那条已测通的路径。
    if (!m_settings || !m_syncPanel)
        return;
    m_settings->showTestResult(T("\xE6\xAD\xA3\xE5\x9C\xA8\xE6\xB5\x8B\xE8\xAF\x95..."), false, true);   // 正在测试...
    const TargetConfig curTarget = m_settings->currentTargetConfig();
    m_syncPanel->testConnectionWithTarget(curTarget, m_settings);
}

void MainWindow::onTestConnectionResult(const QString &text, bool ok, bool testing)
{
    if (m_settings)
        m_settings->showTestResult(text, ok, testing);
}

void MainWindow::onConfigNotice(const QString &message)
{
    m_configNotices.append(message);
    if (m_noticeScheduled)
        return;
    // 关窗那一刻不做模态弹窗：closeEvent 会卡在嵌套事件循环里，
    // 用户看到的就是"关不掉"。提示改由日志承接。
    QTimer::singleShot(50, this, SLOT(flushConfigNotices()));
    m_noticeScheduled = true;
}

void MainWindow::flushConfigNotices()
{
    m_noticeScheduled = false;
    if (m_configNotices.isEmpty() || !m_syncPanel)
        return;
    for (int i = 0; i < m_configNotices.size(); ++i)
        m_syncPanel->appendNotice(m_configNotices.at(i));
    m_configNotices.clear();
}

// ---------------------------------------------------------------- 窗口行为

void MainWindow::refitToScreen()
{
    // 最大化状态下不重设尺寸：setFixedSize 会把死锁重新装回去，
    // 最大化窗口当场被压回固定尺寸。
    if (isMaximized())
        return;
    // 按**窗口当前所在那块屏**算，不一定是主屏（2026-10-28 缺陷 2）。
    // 与设置弹窗同一份规则（ScreenFit::screenForWidget）。
    QScreen *scr = ScreenFit::screenForWidget(this);
    if (!scr)
        return;
    const QRect avail = scr->availableGeometry();
    // **自适应屏幕**：笔记本 1366×768、1080p、4K 缩放各不相同。
    // 写死 980×460 在窄屏上会被裁掉，展开日志的 759 在 768p 上更是放不下。
    // 收敛规则只有一份（ScreenFit.h），主界面和设置弹窗共用。
    const bool logOpen = (m_syncPanel && m_syncPanel->isLogOpenForTest());
    const QSize sz = ScreenFit::mainWindow(avail.width(), avail.height(), logOpen);
    // 第一次上屏：居中。之后：保住用户拖出来的位置，只在越界时往里挪 ——
    // 换屏 / DPR 变化时把窗口拽回屏幕正中会打断正在进行的拖拽。
    const QRect target = m_placed ? ScreenFit::fitInside(sz, geometry(), avail)
                                   : ScreenFit::centeredIn(sz, avail);
    setFixedSize(sz);
    move(target.topLeft());
    m_placed = true;
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::ScreenChangeInternal
        || event->type() == QEvent::WindowStateChange) {
        const qreal now = devicePixelRatioF();
        if (!qFuzzyCompare(now, m_startDpr)) {
            m_startDpr = now;
            refitToScreen();
        } else if (event->type() == QEvent::ScreenChangeInternal) {
            // 换屏了（拖到另一块显示器 / 拔掉外接屏）：按**新那块屏**的可用区
            // 重新收敛一次。位置保留，只在越界时往里挪（refitToScreen 内部处理）——
            // 不在这里把窗口拽回屏幕正中：那会打断正在进行的拖拽。
            refitToScreen();
        }
    }
}

void MainWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton && event->pos().y() <= m_titleBar->height()) {
        m_dragging = true;
        m_dragOffset = event->globalPos() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QMainWindow::mousePressEvent(event);
}

void MainWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        move(event->globalPos() - m_dragOffset);
        event->accept();
        return;
    }
    QMainWindow::mouseMoveEvent(event);
}

void MainWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging) {
        m_dragging = false;
        event->accept();
        return;
    }
    QMainWindow::mouseReleaseEvent(event);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_titleBar && event->type() == QEvent::MouseButtonPress) {
        QMouseEvent *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragOffset = me->globalPos() - frameGeometry().topLeft();
            return true;
        }
    }
    else if (event->type() == QEvent::MouseButtonRelease) {
        m_dragging = false;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_settings) {
        m_settings->close();
        m_settings->deleteLater();
        m_settings = nullptr;
    }
    if (m_syncPanel)
        m_syncPanel->saveConfig();
    event->accept();
}

// 最大化 / 还原。
//
// 定稿图标题栏右上角是三键（最小 / 最大化 / 关闭），原来 Qt 这边只有两键。
//
// 窗口平时是**死锁宽度** 830px（手册 §2 的黄金矩形），所以最大化时必须先
// 把这个死锁解开 —— 否则按了最大化、窗口纹丝不动，比没有这个键更糟；
// 还原时再把死锁装回去，位置也回到屏幕中间。
void MainWindow::onToggleMaximize()
{
    if (!m_maxBtn)
        return;
    if (isMaximized()) {
        m_maxBtn->setIcon(QIcon(Theme::maxIcon(11, Theme::chromeIcon())));
        m_maxBtn->setToolTip(QString::fromUtf8(u8"最大化"));
        showNormal();
        refitToScreen();
    } else {
        m_maxBtn->setIcon(QIcon(Theme::restoreIcon(11, Theme::chromeIcon())));
        m_maxBtn->setToolTip(QString::fromUtf8(u8"还原"));
        // 解开宽高的死锁，才真的能铺满可用区域。
        setMinimumSize(0, 0);
        setMaximumSize(QWIDGETSIZE_MAX, QWIDGETSIZE_MAX);
        showMaximized();
    }
}
