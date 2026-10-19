#include "MainWindow.h"

#include <QFrame>
#include <QLabel>
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
#include "ConfigStore.h"

namespace {
QString T(const char *s) { return QString::fromUtf8(s); }

const char *kAppName     = "\xE6\x95\xB0\xE6\x8D\xAE\xE5\x90\x8C\xE6\xAD\xA5\xE5\xB7\xA5\xE5\x85\xB7";  // 数据同步工具
const char *kSplashReady = "\xE6\xAD\xA3\xE5\x9C\xA8\xE5\x88\x9D\xE5\xA7\x8B\xE5\x8C\x96\xE5\x90\x8C\xE6\xAD\xA5\xE5\xBC\x95\xE6\x93\x8E...";  // 正在初始化同步引擎...

// 窗口尺寸严格死锁 830x480（完全复原 HTML 原型黄金矩形，杜绝正方形失真）。
const int kWindowWidth = 830;
const int kWindowHeight = 480;

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
    bar->setStyleSheet(QStringLiteral("background: #e2e8f0; border-radius: 1px;"));
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
        ? QStringLiteral("background: #3b82f6; border-radius: 5px;")
        : QStringLiteral("background: #3b82f6; border-radius: 3px;"));
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
    setFixedWidth(kWindowWidth);
    setMinimumWidth(kWindowWidth);
    setMaximumWidth(kWindowWidth);
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
    m_splashName->setStyleSheet(QStringLiteral("color: #0f172a;"));
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
    m_splashDot->setStyleSheet(QStringLiteral("background: #3b82f6; border-radius: 3px;"));
    subLay->addWidget(m_splashDot, 0, Qt::AlignVCenter);

    m_splashStatus = new QLabel(T(kSplashReady));
    QFont sf = QApplication::font();
    sf.setPixelSize(11);
    m_splashStatus->setFont(sf);
    m_splashStatus->setStyleSheet(QStringLiteral("color: #64748b;"));  // 原型次文字色
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
        m_splashBarFill->setStyleSheet(QStringLiteral("background: qlineargradient(x1:0,y1:0,x2:1,y2:0,stop:0 #60a5fa,stop:1 #2563eb); border-radius: 1px;"));
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

    QLabel *logo = new QLabel;
    logo->setPixmap(Theme::logo(26));
    logo->setFixedSize(26, 26);
    lay->addWidget(logo);

    QLabel *title = new QLabel(T(kAppName));
    title->setObjectName(QStringLiteral("appTitle"));
    lay->addWidget(title);
    lay->addSpacing(9);
    lay->addStretch(1);

    m_themeToggle = new QToolButton;
    m_themeToggle->setObjectName(QStringLiteral("themeToggleBtn"));
    m_themeToggle->setIcon(QIcon(Theme::themeToggleIcon(16, Theme::chromeIcon(), Theme::isDark())));
    m_themeToggle->setIconSize(QSize(16, 16));
    m_themeToggle->setFixedSize(28, 28);
    m_themeToggle->setCursor(Qt::PointingHandCursor);
    m_themeToggle->setToolTip(QString::fromUtf8("\xE5\x88\x87\xE6\x8D\xA2\xE5\xA4\x96\xE8\xA7\x82\xE9\xA3\x8E\xE6\xA0\xBC (\xE6\x9A\x97\xE9\xBB\x91\xE5\xB7\xA5\xE6\x8E\xA7 / \xE6\x98\x8E\xE4\xBA\xAE\xE6\xB5\x85\xE8\x89\xB2)"));  // 切换外观风格 (暗黑工控 / 明亮浅色)
    lay->addWidget(m_themeToggle);
    connect(m_themeToggle, SIGNAL(clicked()), this, SLOT(onToggleTheme()));

    m_gear = new QToolButton;
    m_gear->setObjectName(QStringLiteral("gearBtn"));
    {
        QIcon gearIco;
        gearIco.addPixmap(Theme::gearIcon(16, Theme::chromeIcon()), QIcon::Normal, QIcon::Off);
        gearIco.addPixmap(Theme::gearIcon(16, Theme::chromeIconOn()), QIcon::Normal, QIcon::On);
        m_gear->setIcon(gearIco);
    }
    m_gear->setIconSize(QSize(16, 16));
    m_gear->setFixedSize(28, 28);
    m_gear->setCheckable(true);
    m_gear->setCursor(Qt::PointingHandCursor);
    m_gear->setToolTip(QString::fromUtf8("\xE4\xBC\xA0\xE8\xBE\x93\xE4\xB8\x8E\xE5\xB7\xA5\xE5\x85\xB7\xE8\xAE\xBE\xE7\xBD\xAE"));  // 传输与工具设置
    lay->addWidget(m_gear);

    QFrame *sep = new QFrame;
    sep->setObjectName(QStringLiteral("titleBarLine"));
    sep->setFrameShape(QFrame::VLine);
    // 这里原来写死 `background: #374151` 且**没有 objectName**。
    // 两个问题叠加：颜色是两套主题共用的深灰，浅色模式下白底上一道黑杠
    // （用户 2026-10-16 报的"齿轮旁边还有黑色的东西"）；
    // 而 Theme.h 里 `QFrame#titleBarLine` 那两条正确的两套配色规则
    // 因为选择器对不上，从来没生效过。
    //
    // 修法是删掉 widget 级的 setStyleSheet、交给 app 级 QSS 按主题给色。
    // 不能只改颜色留在这：widget 级样式表优先级高于 app 级且会阻断继承，
    // 留着它 Theme.h 里那条规则永远轮不到。
    sep->setFixedHeight(16);
    lay->addSpacing(4);
    lay->addWidget(sep);
    lay->addSpacing(4);

    QToolButton *minBtn = new QToolButton;
    minBtn->setObjectName(QStringLiteral("sysBtn"));
    minBtn->setIcon(QIcon(Theme::minIcon(11, Theme::chromeIcon())));
    minBtn->setIconSize(QSize(11, 11));
    minBtn->setFixedSize(26, 26);
    minBtn->setCursor(Qt::PointingHandCursor);
    connect(minBtn, SIGNAL(clicked()), this, SLOT(showMinimized()));
    lay->addWidget(minBtn);

    QToolButton *closeBtn = new QToolButton;
    closeBtn->setObjectName(QStringLiteral("sysBtnClose"));
    closeBtn->setIcon(QIcon(Theme::closeIcon(11, Theme::chromeIcon())));
    closeBtn->setIconSize(QSize(11, 11));
    closeBtn->setFixedSize(26, 26);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, SIGNAL(clicked()), this, SLOT(close()));
    lay->addWidget(closeBtn);

    connect(m_gear, SIGNAL(clicked()), this, SLOT(onOpenSettings()));
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
    m_settings->show();
    if (m_gear)
        m_gear->setChecked(true);
}

void MainWindow::showSettingsForProbe()
{
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
    if (m_gear)
        m_gear->setChecked(false);
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
    QScreen *scr = QGuiApplication::primaryScreen();
    if (!scr)
        return;
    const QRect avail = scr->availableGeometry();
    const int h = (m_syncPanel && m_syncPanel->isLogOpenForTest()) ? 630 : kWindowHeight;
    setFixedSize(kWindowWidth, h);
    move(avail.center().x() - kWindowWidth / 2,
         avail.center().y() - h / 2);
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

void MainWindow::onToggleTheme()
{
    Theme::toggleMode();
    qApp->setStyleSheet(Theme::buildQss(Theme::currentMode()));
    if (m_themeToggle) {
        m_themeToggle->setIcon(QIcon(Theme::themeToggleIcon(16, Theme::chromeIcon(), Theme::isDark())));
        m_themeToggle->setToolTip(Theme::isDark()
            ? QString::fromUtf8("\xE5\x88\x87\xE6\x8D\xA2\xE5\x88\xB0\xE6\xB5\x85\xE8\x89\xB2\xE6\xA8\xA1\xE5\xBC\x8F")
            : QString::fromUtf8("\xE5\x88\x87\xE6\x8D\xA2\xE5\x88\xB0\xE6\x9A\x97\xE9\xBB\x91\xE5\xB7\xA5\xE6\x8E\xA7\xE6\xA8\xA1\xE5\xBC\x8F"));
    }
    if (m_gear) {
        QIcon gearIco;
        gearIco.addPixmap(Theme::gearIcon(16, Theme::chromeIcon()), QIcon::Normal, QIcon::Off);
        gearIco.addPixmap(Theme::gearIcon(16, Theme::chromeIconOn()), QIcon::Normal, QIcon::On);
        m_gear->setIcon(gearIco);
    }
    update();

    // 状态徽章和工具按钮是 delegate 里用 QPainter 现画的，颜色每次 paint 都现读
    // Theme。qApp->setStyleSheet() 只会让 QSS 那一半重刷，delegate 自绘的那一半
    // 要显式 update 才会重新取色 —— 不补这一句，切换主题后界面主体变了、
    // 表格里那两列还停在上一个主题的颜色上。
    const QWidgetList all = QApplication::allWidgets();
    for (int i = 0; i < all.size(); ++i) {
        QAbstractScrollArea *sa = qobject_cast<QAbstractScrollArea *>(all.at(i));
        if (sa && sa->viewport())
            sa->viewport()->update();
    }
}
