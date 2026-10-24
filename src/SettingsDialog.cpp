#include "SettingsDialog.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QScrollArea>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QCheckBox>
#include <QRadioButton>
#include <QTimeEdit>
#include <QIntValidator>
#include <QStyle>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QIcon>
#include <QTimer>
#include <QScreen>
#include <QGuiApplication>
#include <QGraphicsDropShadowEffect>
#include "Theme.h"
#include "ConfigStore.h"
#include "AutoSyncScheduler.h"

namespace {
const char *kTxtHost       = "\xE7\x9B\xAE\xE6\xA0\x87\xE7\x94\xb5\xE8\x84\x91 IP";      // 目标电脑 IP
const char *kTxtPort       = "SSH \xE7\xAB\xAF\xE5\x8F\xA3";      // SSH 端口
const char *kTxtKeyFile    = "\xE7\xA7\x81\xE9\x92\xA5\xE6\x96\x87\xE4\xBB\xB6";      // 私钥文件
const char *kTxtBrowseFile = "\xE9\x80\x89\xE6\x8B\xA9\xE6\x96\x87\xE4\xBB\xB6";      // 选择文件
const char *kTxtUser       = "\xE7\x99\xBB\xE5\xBD\x95\xE8\xB4\xA6\xE5\x8F\xB7";      // 登录账号
const char *kTxtPassword   = "\xE7\x99\xBB\xE5\xBD\x95\xE5\xAF\x86\xE7\xA0\x81";      // 登录密码
const char *kTxtRemoteRoot = "\xE7\x9B\xAE\xE6\xA0\x87\xE8\x90\xBD\xE7\x9B\x98\xE8\xB7\xAF\xE5\xBE\x84";  // 目标落盘路径
const char *kTxtSourceDir  = "\xE6\x9C\xAC\xE6\x9C\xBA\xE6\x96\x87\xE4\xBB\xB6\xE7\x9B\xAE\xE5\xBD\x95";  // 本机文件目录
const char *kTxtBrowse     = "\xE9\x80\x89\xE6\x8B\xA9\xE7\x9B\xAE\xE5\xBD\x95";      // 选择目录
const char *kTxtTools      = "\xE6\x96\x87\xE4\xBB\xB6\xE6\x89\x93\xE5\xBC\x80\xE5\xB7\xA5\xE5\x85\xB7";  // 文件打开工具
const char *kTxtAddRule    = "+ \xE6\xB7\xBB\xE5\x8A\xA0\xE8\xA7\x84\xE5\x88\x99";    // + 添加规则
const char *kTxtTestConn   = "\xE6\xB5\x8B\xE8\xAF\x95\xE7\xBD\x91\xE7\xBB\x9C\xE8\xBF\x9E\xE6\x8E\xA5";  // 测试网络连接
const char *kTxtSave       = "\xE4\xBF\x9D\xE5\xAD\x98\xE8\xAE\xBE\xE7\xBD\xAE";      // 保存设置
const char *kTxtDialogTitle= "\xE4\xBC\xA0\xE8\xBE\x93\xE4\xB8\x8E\xE5\xB7\xA5\xE5\x85\xB7\xE8\xAE\xBE\xE7\xBD\xAE";  // 传输与工具设置
const char *kTxtColSuffix  = "\xE6\x96\x87\xE4\xBB\xB6\xE7\xB1\xBB\xE5\x9E\x8B";      // 文件类型
const char *kTxtColTool    = "\xE5\xB7\xA5\xE5\x85\xB7\xE5\x90\x8D\xE7\xA7\xB0";      // 工具名称
const char *kTxtColPath    = "\xE5\x85\xB3\xE8\x81\x94\xE7\xA8\x8B\xE5\xBA\x8F\xE8\xB7\xAF\xE5\xBE\x84";  // 关联程序路径
const char *kTxtHint       = "\xE6\x8F\x90\xE7\xA4\xBA\xEF\xBC\x9A\xE5\x88\x97\xE8\xA1\xA8\xE4\xB8\xAD\xE7\x9A\x84\xE5\xB7\xA5\xE5\x85\xB7\xE6\x8C\x89\xE9\x92\xAE\xE7\x9B\xB4\xE6\x8E\xA5\xE4\xBD\x9C\xE7\x94\xA8\xE4\xBA\x8E\xE5\xBD\x93\xE5\x89\x8D\xE9\x80\x89\xE4\xB8\xAD\xE7\x9A\x84\xE6\x96\x87\xE4\xBB\xB6\xE3\x80\x82";  // 提示：在列表中点击对应的工具按钮，即可直接打开选中文件。
// 带上范围：客户看得出可以填多少。
// 原来这里只有"自动同步间隔"，而输入框挂了 QIntValidator(10, 86400)
// —— 客户想填 5 秒会被**静默拒绝**（输入框不反应），以为软件坏了。
const char *kTxtAutoInterval = "自动同步间隔（10 ~ 86400 秒）";  // 自动同步间隔（10~86400 秒）
const char *kTxtSec          = "\xE7\xA7\x92";  // 秒
// 每天定点的时刻标签。
const char *kTxtDailyTime    = "每天定时时刻";  // 每天定时时刻

QLabel *mkLabel(const char *text, const char *objName)
{
    QLabel *l = new QLabel(QString::fromUtf8(text));
    l->setObjectName(QString::fromLatin1(objName));
    return l;
}

QLineEdit *mkInput(const char *objName, const QString &value, const QString &ph)
{
    QLineEdit *e = new QLineEdit(value);
    e->setObjectName(QString::fromLatin1(objName));
    e->setPlaceholderText(ph);
    e->setMinimumHeight(28);
    e->setMaximumHeight(28);
    return e;
}
} // namespace

SettingsDialog::SettingsDialog(QWidget *parent)
    : QDialog(parent)
{
    setObjectName(QStringLiteral("settingsDialog"));
    setWindowTitle(QString::fromUtf8(kTxtDialogTitle));
    // 采用无边框 + 背景半透明，由内部 settingsCard 呈现 14px 大圆角与深色工控质感
    //
    // 这里刻意用 Qt::Window 而不是 Qt::Dialog。
    // Qt::Dialog 在 X11 上会同时发出 _NET_WM_WINDOW_TYPE_DIALOG + WM_TRANSIENT_FOR
    // + 无边框 _MOTIF_WM_HINTS（还会附带 _KDE_NET_WM_WINDOW_TYPE_OVERRIDE），
    // 这套组合在 Xwayland / wlroots 内置 XWM 上会被误判成 popup 并挂到一个
    // 不可见的父 surface 上 —— 症状是"窗口在 X 侧存在、位置尺寸都对，
    // 但 Map State 永远是 IsUnMapped，屏幕上什么都没有"。
    // 实测（银河麒麟 V11，wlroots XWM）：程序启动后第一次点标题栏齿轮，
    // 什么都没出来；第二次点才正常。因为它是模态的，客户此时已经被挡住，
    // 看起来就像软件卡死，连"设置目标电脑"这一步都过不去。
    //
    // 面板本来就有自己的标题栏和关闭按钮，不依赖 WM 的模态/父窗口行为，
    // 改成 Qt::Window 没有功能损失。
    setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setFixedWidth(570);
    // SettingsDialog.h 一直承诺"整体 max-height 80vh"，但构造函数里从来没实现：
    // 只有宽度上限，高度被 sizeHint 顶到 ~690px，而 X11 分配的窗口只有 533px，
    // 无边框 + 透明背景下没有 WM 兜底，超出部分直接被裁 —— 实测"本机文件目录 /
    // 目标电脑 IP / SSH 端口"三个最关键的字段在屏幕上看不见。
    // 中间那块是 QScrollArea，限高之后自然出滚动条，字段一条都不会丢。
    const QRect avail = QGuiApplication::primaryScreen()->availableGeometry();
    setMinimumHeight(320);
    setMaximumHeight(qMax(320, avail.height() * 4 / 5));

    QVBoxLayout *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(10, 10, 10, 10);
    outerLayout->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("settingsCard"));
    outerLayout->addWidget(m_card);

    QVBoxLayout *cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // ---- 顶栏（图三风格：左侧科技蓝齿轮 + 标题，右侧细叉关闭，可拖动）----
    m_header = new QFrame;
    m_header->setObjectName(QStringLiteral("dialogHeader"));
    m_header->setFixedHeight(44);

    QHBoxLayout *hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(18, 0, 12, 0);
    hl->setSpacing(8);

    QLabel *gearLabel = new QLabel;
    // 深色下原来取 #60a5fa。gearIcon() 漏了 setPen、画出来一直是 Qt 默认黑，
    // 这个色其实一直没生效过（见 Theme.h gearIcon 注释）。现在 pen 补上了，
    // 统一走 Theme::chromeIcon()，两套主题各一份，不再在这里另挑一个蓝。
    gearLabel->setPixmap(Theme::gearIcon(16, Theme::chromeIcon()));
    gearLabel->setFixedSize(16, 16);
    hl->addWidget(gearLabel);

    QLabel *titleLabel = mkLabel(kTxtDialogTitle, "dialogTitle");
    QFont tf = titleLabel->font();
    tf.setBold(true);
    tf.setPixelSize(13);
    titleLabel->setFont(tf);
    hl->addWidget(titleLabel);
    hl->addStretch(1);

    m_btnClose = new QPushButton;
    m_btnClose->setObjectName(QStringLiteral("dialogCloseBtn"));
    m_btnClose->setFixedSize(26, 26);
    m_btnClose->setCursor(Qt::PointingHandCursor);
    m_btnClose->setIcon(QIcon(Theme::closeIcon(11, Theme::textSub())));
    m_btnClose->setIconSize(QSize(11, 11));
    m_btnClose->setToolTip(QString::fromUtf8("\xE5\x85\xB3\xE9\x97\xAD (ESC)")); // 关闭 (ESC)
    hl->addWidget(m_btnClose);
    connect(m_btnClose, SIGNAL(clicked()), this, SLOT(reject()));

    cardLayout->addWidget(m_header);

    // ---- 中间滚动区（图四表单内容）----
    QScrollArea *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);

    QWidget *body = new QWidget;
    QVBoxLayout *bl = new QVBoxLayout(body);
    bl->setContentsMargins(18, 14, 18, 14);
    bl->setSpacing(11);
    scroll->setWidget(body);
    cardLayout->addWidget(scroll, 1);

    // 1. 本机文件目录 + 选择目录
    bl->addWidget(mkLabel(kTxtSourceDir, "settingLabel"));
    QHBoxLayout *srcRow = new QHBoxLayout;
    srcRow->setSpacing(8);
    m_sourceDir = mkInput("settingSourceDir", QString(),
                          QString::fromUtf8("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A/home/\xE7\x94\xA8\xE6\x88\xB7\xE5\x90\x8D/\xE6\x95\xB0\xE6\x8D\xAE \xE6\x88\x96 ~/data"));  // 例如: /home/用户名/数据 或 ~/data
    QPushButton *browse = new QPushButton(QString::fromUtf8(kTxtBrowse));
    browse->setObjectName(QStringLiteral("btnBrowse"));
    browse->setFixedHeight(28);
    browse->setCursor(Qt::PointingHandCursor);
    srcRow->addWidget(m_sourceDir, 1);
    srcRow->addWidget(browse);
    bl->addLayout(srcRow);
    connect(browse, SIGNAL(clicked()), this, SLOT(browseSourceDir()));

    // 2. 目标电脑 IP + SSH 端口（两列并排）
    //
    // 端口这一栏以前不存在，于是 ssh 一律连 22。可后端从一开始就是支持的：
    // ConfigStore 存 target/port（默认 22）、RsyncRunner 用 `ssh -p <端口>`。
    // 也就是说这是**只有界面没接上**，不是功能没做。
    // 客户 sshd 跑在非 22 端口（企业里封 22 很常见）时，
    // 界面上无处可填 = 用户根本没法连，只能改配置文件 —— 那是产品缺陷。
    QGridLayout *g1 = new QGridLayout;
    g1->setSpacing(10);
    g1->addWidget(mkLabel(kTxtHost, "settingLabel"), 0, 0);
    g1->addWidget(mkLabel(kTxtPort, "settingLabel"), 0, 1);
    m_host = mkInput("settingHost", QString(),
                     QString::fromUtf8("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\x31\x39\x32.168.1.100"));  // 例如: 192.168.1.100
    m_port = mkInput("settingPort", QStringLiteral("22"),
                     QString::fromUtf8("\xE9\xBB\x98\xE8\xAE\xA4 22"));  // 默认 22
    m_port->setValidator(new QIntValidator(1, 65535, m_port));
    m_port->setMaximumWidth(96);
    g1->addWidget(m_host, 1, 0);
    g1->addWidget(m_port, 1, 1);
    g1->setColumnStretch(0, 1);
    g1->setColumnStretch(1, 0);
    bl->addLayout(g1);

    // 3. 登录账号 + 登录密码（两列并排）
    QGridLayout *g2 = new QGridLayout;
    g2->setSpacing(10);
    g2->addWidget(mkLabel(kTxtUser, "settingLabel"), 0, 0);
    g2->addWidget(mkLabel(kTxtPassword, "settingLabel"), 0, 1);
    m_user = mkInput("settingUser", QString(),
                     QString::fromUtf8("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\x41\x64ministrator"));  // 例如: Administrator
    m_password = mkInput("settingPassword", QString(),
                         QString::fromUtf8("\xE8\xBE\x93\xE5\x85\xA5\xE8\xBF\x9C\xE7\xA8\x8B\xE7\x99\xBB\xE5\xBD\x95\xE5\xAF\x86\xE7\xA0\x81"));  // 输入远程登录密码
    m_password->setEchoMode(QLineEdit::Password);
    g2->addWidget(m_user, 1, 0);
    g2->addWidget(m_password, 1, 1);
    g2->setColumnStretch(0, 1);
    g2->setColumnStretch(1, 1);
    bl->addLayout(g2);

    // 3b. 私钥文件（选填）
    //
    // 填了就用密钥认证，不填就回落到密码认证 —— RsyncRunner 一直按这个语义在跑
    // （RsyncRunner.cpp: `useKey()` 为真时走 `ssh -i`，并额外用 `-o` 关掉密码交互；
    //  为假且密码非空时走 askpass 脚本喂密码）。
    // 同样地：ConfigStore 存 target/keyPath，后端全支持，**只有界面没接上**。
    //
    // 为什么要补：企业客户基本都改成密钥认证了，没有这一栏他们只能改配置文件。
    bl->addWidget(mkLabel(kTxtKeyFile, "settingLabel"));
    QHBoxLayout *keyRow = new QHBoxLayout;
    keyRow->setSpacing(8);
    m_keyPath = mkInput("settingKeyPath", QString(),
                        QString::fromUtf8("\xE7\x95\x99\xE7\xA9\xBA\xE5\x88\x99\xE7\x94\xA8\xE5\xAF\x86\xE7\xA0\x81\xE7\x99\xBB\xE5\xBD\x95"));  // 留空则用密码登录
    QPushButton *keyBrowse = new QPushButton(QString::fromUtf8(kTxtBrowseFile));
    keyBrowse->setObjectName(QStringLiteral("btnBrowseKey"));
    keyBrowse->setFixedHeight(28);
    keyBrowse->setCursor(Qt::PointingHandCursor);
    keyRow->addWidget(m_keyPath, 1);
    keyRow->addWidget(keyBrowse);
    bl->addLayout(keyRow);
    connect(keyBrowse, SIGNAL(clicked()), this, SLOT(browseKeyFile()));

    // 4. 目标落盘路径
    bl->addWidget(mkLabel(kTxtRemoteRoot, "settingLabel"));
    m_remoteRoot = mkInput("settingRemoteRoot", QString(),
                           QString::fromUtf8("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\x44:/Data"));  // 例如: D:/Data
    bl->addWidget(m_remoteRoot);

    // 4b. 自动同步：**两种互斥模式**（需求 10 的"类似 cron"）
    //
    //   「按间隔重复」—— 老版本的行为，每 N 秒一次（默认 30 秒）
    //   「每天定时」  —— 每天 HH:MM 一次（默认 03:00）
    //
    // 为什么不给 cron 表达式：需求 10 原文「不要让用户手填 cron」。
    // 表达式会带出"看起来对但永远不触发"的组合，客户填错一个字段就静默停摆。
    // 为什么默认 03:00：客户机多半晚上不干活、凌晨带宽空闲 ——
    // 但这**只是默认值**，客户可以改（不同客户的上传/维护窗口不一样）。
    //
    // 两套控件**互斥**：当前模式之外的那一套被**藏起来并禁用**，
    // 免得用户填了一个不生效的值还以为软件坏了。
    QHBoxLayout *modeRow = new QHBoxLayout;
    modeRow->setSpacing(16);
    m_modeInterval = new QRadioButton(QString::fromUtf8(u8"按间隔重复"));
    m_modeInterval->setObjectName(QStringLiteral("chkAutoSyncModeInterval"));
    m_modeDaily = new QRadioButton(QString::fromUtf8(u8"每天定时"));
    m_modeDaily->setObjectName(QStringLiteral("chkAutoSyncModeDaily"));
    modeRow->addWidget(m_modeInterval);
    modeRow->addWidget(m_modeDaily);
    modeRow->addStretch(1);
    bl->addLayout(modeRow);

    // 4b-1. 间隔输入（只有「按间隔重复」时可见可用）
    QHBoxLayout *intervalRow = new QHBoxLayout;
    intervalRow->setSpacing(8);
    QLabel *intervalLabel = mkLabel(kTxtAutoInterval, "settingLabel");
    m_autoSyncInterval = mkInput("settingAutoSyncInterval",
                                 QString::number(AutoSyncScheduler::DefaultIntervalSec),
                                 QString::number(AutoSyncScheduler::DefaultIntervalSec));
    m_autoSyncInterval->setValidator(new QIntValidator(AutoSyncScheduler::MinIntervalSec,
                                                       AutoSyncScheduler::MaxIntervalSec,
                                                       m_autoSyncInterval));
    m_autoSyncInterval->setMaximumWidth(96);
    // tooltip 必须和**实测行为**一致，不能给客户一个做不到的承诺。
    //
    // 实测（Qt 5.9.6 的 QIntValidator(10, 86400)）：
    //   · 它只卡**上限**，不卡下限 —— 5、0 都能输进去（低于下限属于"还可能补成合法值"的中间态）
    //   · 超上限不是"输不进去"，而是**把多出来的位数丢掉**：键入 999999，框里会剩 9999
    //   · 字母输不进去（键入 abc，框里是空的）
    //   · 越界/非法值最终会被 autoSyncInterval() 归一：<10 → 10，空/非数字 → 30
    // 旧文案写的是"超出范围的数字输入框不会接受"，实测**不成立** ——
    // 照着它操作的人（填 5、填 999999）会得到与承诺相反的结果。
    m_autoSyncInterval->setToolTip(QString::fromUtf8(
        u8"可填 10 ~ 86400 秒（约 10 秒 ~ 24 小时）。\n"
        u8"留空用默认 30 秒；填 1~9 按 10 秒算。\n"
        u8"只能填数字，超上限的数字会少几位（999999 会变成 9999）。\n"
        u8"保存时输入框会被回填成真正生效的秒数。"));
    m_autoSyncInterval->setAlignment(Qt::AlignCenter);
    QLabel *secLabel = mkLabel(kTxtSec, "settingLabel");
    intervalRow->addWidget(intervalLabel);
    intervalRow->addStretch(1);
    intervalRow->addWidget(m_autoSyncInterval);
    intervalRow->addWidget(secLabel);
    bl->addLayout(intervalRow);

    // 4b-2. 定点时刻选择（只有「每天定时」时可见可用）
    QHBoxLayout *dailyRow = new QHBoxLayout;
    dailyRow->setSpacing(8);
    QLabel *dailyLabel = mkLabel(kTxtDailyTime, "settingLabel");
    m_autoSyncDailyTime = new QTimeEdit(AutoSyncScheduler::defaultDailyTime());
    m_autoSyncDailyTime->setObjectName(QStringLiteral("settingAutoSyncDailyTime"));
    m_autoSyncDailyTime->setDisplayFormat(QStringLiteral("HH:mm"));
    m_autoSyncDailyTime->setMaximumWidth(96);
    m_autoSyncDailyTime->setToolTip(QString::fromUtf8(
        u8"每天在这个时刻自动同步一次（按本机本地时间）。\n"
        u8"关机期间错过的那一次，开机后会自动补跑一轮。"));
    dailyRow->addWidget(dailyLabel);
    dailyRow->addStretch(1);
    dailyRow->addWidget(m_autoSyncDailyTime);
    bl->addLayout(dailyRow);

    // 模式的初始状态来自配置（老配置没有模式键 → 按间隔重复 = 老版本行为）。
    //
    // 顺序很重要：先 setChecked 再 connect。
    // 反过来的话，构造时的 setChecked 会触发 toggled →
    // onAutoSyncModeChanged() 把"读到的状态"当成"用户刚点的"处理。
    // （这里不会落盘 —— 模式只在点「保存设置」时才写。）
    m_modeInterval->setChecked(ConfigStore::loadAutoSyncMode() != AutoSyncScheduler::ModeDaily);
    connect(m_modeInterval, &QRadioButton::toggled, this, &SettingsDialog::onAutoSyncModeChanged);
    connect(m_modeDaily, &QRadioButton::toggled, this, &SettingsDialog::onAutoSyncModeChanged);
    onAutoSyncModeChanged();

    // 4c. 开机自动启动
    //
    // **只负责启动程序**，不擅自开启自动同步：程序被自启拉起来后，
    // 沿用用户上次勾选的「自动同步」状态（sync/autoSyncEnabled）。
    // 这一点必须写在界面上 —— 否则客户会以为"开了开机自启就等于每天自动传"。
    m_autoStartBox = new QCheckBox(QString::fromUtf8(u8"开机自动启动"));
    m_autoStartBox->setObjectName(QStringLiteral("chkAutoStart"));
    m_autoStartBox->setCursor(Qt::PointingHandCursor);
    m_autoStartBox->setToolTip(QString::fromUtf8(
        u8"勾上以后，每次开机都会自动打开本程序。\n"
        u8"注意：这**只是把程序打开**，是否自动同步仍按你在同步页上的勾选状态走。\n"
        u8"取消勾选会把这个开机项删掉，不留残留。"));
    // 初始状态来自配置（用户意图）。老配置里没有 app/autoStartEnabled → false。
    //
    // setChecked 必须在 connect **之前**：否则构造时就触发 toggled，
    // 把"读到的状态"当成"用户点的"写回配置 —— 老配置会被写脏。
    m_autoStartBox->setChecked(ConfigStore::loadAutoStartEnabled());
    connect(m_autoStartBox, &QCheckBox::toggled, this, &SettingsDialog::onAutoStartToggled);
    bl->addWidget(m_autoStartBox);

    // 5. 文件打开工具规则表（图四）
    QHBoxLayout *toolsHead = new QHBoxLayout;
    QLabel *toolsLabel = mkLabel(kTxtTools, "dialogTitle");
    QFont rtf = toolsLabel->font();
    rtf.setBold(true);
    rtf.setPixelSize(12);
    toolsLabel->setFont(rtf);
    toolsHead->addWidget(toolsLabel);
    toolsHead->addStretch(1);

    m_addRule = new QPushButton(QString::fromUtf8(kTxtAddRule));
    m_addRule->setObjectName(QStringLiteral("btnAddRule"));
    m_addRule->setCursor(Qt::PointingHandCursor);
    toolsHead->addWidget(m_addRule);
    bl->addLayout(toolsHead);

    m_ruleTable = new QTableWidget(0, 3);
    m_ruleTable->setObjectName(QStringLiteral("ruleTable"));
    m_ruleTable->setHorizontalHeaderLabels(QStringList()
                                           << QString::fromUtf8(kTxtColSuffix)
                                           << QString::fromUtf8(kTxtColTool)
                                           << QString::fromUtf8(kTxtColPath));
    m_ruleTable->horizontalHeader()->setHighlightSections(false);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_ruleTable->setShowGrid(false);
    m_ruleTable->setFixedHeight(160);
    m_ruleTable->horizontalHeader()->setStretchLastSection(true);
    m_ruleTable->setColumnWidth(0, 95);
    m_ruleTable->setColumnWidth(1, 140);
    bl->addWidget(m_ruleTable);

    QLabel *hint = mkLabel(kTxtHint, "settingHint");
    hint->setWordWrap(true);
    bl->addWidget(hint);

    connect(m_addRule, SIGNAL(clicked()), this, SLOT(addRuleRow()));

    // ---- 底栏（图四风格：左侧[测试网络连接]，右侧[保存设置]）----
    QFrame *footer = new QFrame;
    footer->setObjectName(QStringLiteral("dialogFooter"));
    footer->setFixedHeight(52);
    QHBoxLayout *fl = new QHBoxLayout(footer);
    fl->setContentsMargins(18, 0, 18, 0);

    QPushButton *test = new QPushButton(QString::fromUtf8(kTxtTestConn));
    test->setObjectName(QStringLiteral("btnTestConn"));
    test->setCursor(Qt::PointingHandCursor);

    m_testLabel = mkLabel("", "testLabel");
    m_testLabel->setVisible(false);

    QHBoxLayout *testRow = new QHBoxLayout;
    testRow->setSpacing(8);
    testRow->addWidget(test);
    testRow->addWidget(m_testLabel);
    testRow->addStretch(1);
    fl->addLayout(testRow);

    m_saveTip = mkLabel("", "saveTip");
    m_saveTip->setVisible(false);
    fl->addWidget(m_saveTip);

    m_rememberBox = new QCheckBox(QString::fromUtf8(u8"记住设置"));
    m_rememberBox->setObjectName(QStringLiteral("chkRememberSettings"));
    m_rememberBox->setCursor(Qt::PointingHandCursor);
    m_rememberBox->setChecked(ConfigStore::loadRememberSettings());
    connect(m_rememberBox, &QCheckBox::toggled, this, [](bool on) {
        ConfigStore::saveRememberSettings(on);
    });
    // 「记住设置」的实际语义（2026-10-20 全文查证 ConfigStore 后确认，
    // 2026-10-21 已让它真正生效）：
    //
    // 勾上   —— 目标连接信息（IP、端口、账号、密码、私钥、本机目录、远端路径）
    //           写进本机配置文件，下次启动自动填回；
    // 不勾   —— 这些信息不写进配置文件，并且配置文件里原有的那一份会被清除
    //           （清除时会在日志抽屉里说一声）。
    //
    // 工具规则与同步规则不受它影响：那两组在各自的面板里，不在这个弹窗里。
    //
    // 原来的实现里这个勾选框只记录自己被勾没被勾，实际的落盘与恢复走的是
    // 另一条路（saveTarget 无条件写、loadTarget 无条件读），所以它等于摆设。
    // 现在 ConfigStore::saveTarget / loadTarget 两侧都加了闸门。
    //
    // 上面这段说明同时挂成 tooltip，让用户在界面上就能看到真实行为。
    m_rememberBox->setToolTip(QString::fromUtf8(
        u8"勾上：点「保存设置」后，目标电脑 IP、SSH 端口、登录账号、登录密码、私钥文件、"
        u8"本机文件目录、目标落盘路径会写入本机配置文件，下次启动自动填回。\n"
        u8"不勾：这些信息不写入本机配置文件；配置文件里原有的那一份会被清除。\n"
        u8"工具规则与同步规则不受这个勾选框影响。"));
    fl->addWidget(m_rememberBox);
    fl->addSpacing(10);

    m_btnSave = new QPushButton(QString::fromUtf8(kTxtSave));
    m_btnSave->setObjectName(QStringLiteral("btnSave"));
    m_btnSave->setCursor(Qt::PointingHandCursor);
    fl->addWidget(m_btnSave);

    cardLayout->addWidget(footer);

    connect(m_btnSave, SIGNAL(clicked()), this, SLOT(onSaveClicked()));
    connect(test, SIGNAL(clicked()), this, SIGNAL(settingsTestConnectionRequested()));
}

void SettingsDialog::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragging = true;
        m_dragOffset = event->globalPos() - frameGeometry().topLeft();
        event->accept();
        return;
    }
    QDialog::mousePressEvent(event);
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        move(event->globalPos() - m_dragOffset);
        event->accept();
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void SettingsDialog::mouseReleaseEvent(QMouseEvent *event)
{
    m_dragging = false;
    QDialog::mouseReleaseEvent(event);
}

void SettingsDialog::onSaveClicked()
{
    // 生效值必须**看得见**。
    //
    // 原来这一段是"用户填什么就悄悄按什么算"：填 5 实际按 10 跑、
    // 填 abc / 0 实际按 30 跑，界面上除了「✓ 设置已保存」之外一个字都不说。
    // 用户以为自己填的就是生效的，下一轮的节奏和心里想的对不上，
    // 而且要等到界面上的「下次同步 Ns 后」和预期不一样才会发现。
    //
    // 这里做两件事，都不新增控件、不动布局：
    //   1) 把真正生效的秒数**回填进输入框**（用户当场就看得见）；
    //   2) 保存提示里带上"按 N 秒生效"，把差异说清楚。
    const int effective = autoSyncInterval();
    const QString typed = m_autoSyncInterval ? m_autoSyncInterval->text().trimmed() : QString();
    const bool corrected = (typed != QString::number(effective));
    if (corrected && m_autoSyncInterval)
        m_autoSyncInterval->setText(QString::number(effective));

    ConfigStore::saveAutoSyncInterval(effective);
    // 自动同步模式与定点时刻：也在「保存设置」这一刻落盘。
    //
    // 必须在 emit settingsAccepted() **之前** ——
    // 那条信号会走到 SyncPanel::applyFromSettings()，它会重新排程
    // （autoSyncScheduleNext(true)），而排程要读的就是这两个键。
    // 顺序反了的话，这一轮排程用的还是旧模式，要等下次开设置才对。
    ConfigStore::saveAutoSyncMode(autoSyncMode());
    ConfigStore::saveAutoSyncDailyTime(autoSyncDailyTime());
    emit settingsAccepted();

    // 落盘结果必须**如实**反映。
    //
    // 原来这里无条件显示「✓ 设置已保存」，而 ConfigStore 里从头到尾没有一处
    // 检查 QSettings::status() —— 写失败（盘满、目录只读、配置文件被锁住）
    // 时错误被整个吞掉。用户看到「已保存」，关掉重开，
    // IP / 端口 / 密码 / 规则全回到旧值，界面上却什么异常都没有。
    //
    // QSettings 的错误是**延迟**报的：setValue 全部返回 void，
    // 只有 sync() 之后 status() 才说得出真话。所以那边补了 flushSettings()，
    // 结果记在 ConfigStore::lastWriteError() 里。
    const QString err = ConfigStore::lastWriteError();

    // 满足用户要求：显示已保存提示，但不自动关闭设置框
    if (m_saveTip) {
        // 生效值必须看得见。两种模式各说一句：
        //   按间隔 → 间隔被规整过才提示（保持原口径）
        //   每天定时 → 一定提示"每天几点生效"，否则客户不知道自己改没改成
        QString effectiveNote;
        if (autoSyncMode() == AutoSyncScheduler::ModeDaily) {
            effectiveNote = QString::fromUtf8(u8"（每天定时 %1 生效）")
                               .arg(autoSyncDailyTime().toString(QStringLiteral("HH:mm")));
        } else if (corrected) {
            effectiveNote = QString::fromUtf8(u8"（自动同步间隔按 %1 秒生效）").arg(effective);
        }
        if (err.isEmpty()) {
            m_saveTip->setText(QString::fromUtf8(u8"\u2713 \u8BBE\u7F6E\u5DF2\u4FDD\u5B58") // ✓ 设置已保存
                               + effectiveNote);
        } else {
            m_saveTip->setText(QString::fromUtf8(u8"\u2717 \u8BBE\u7F6E\u672A\u4FDD\u5B58")
                               + QString::fromUtf8(u8"\uFF1A") + err);
        }
        m_saveTip->setVisible(true);
        QTimer::singleShot(3000, m_saveTip, &QLabel::hide);
    }
    if (m_btnSave) {
        m_btnSave->setText(err.isEmpty() ? QString::fromUtf8(u8"\u5DF2\u4FDD\u5B58")   // 已保存
                                         : QString::fromUtf8(u8"\u672A\u4FDD\u5B58")); // 未保存
        QTimer::singleShot(2000, this, [this]() {
            if (m_btnSave)
                m_btnSave->setText(QString::fromUtf8(kTxtSave));
        });
    }
}

// 两种模式互斥：当前模式之外的那一套控件**藏起来并禁用**。
//
// 为什么不是"只置灰"：两个可填的框同时摆在那里，客户填了不生效的那一个，
// 会以为软件坏了（"我明明设了每天 3 点，怎么还是每 30 秒一次"）。
// 藏起来 + 禁用，两个保险。
void SettingsDialog::onAutoSyncModeChanged()
{
    const bool daily = (m_modeDaily && m_modeDaily->isChecked());
    if (m_autoSyncInterval) {
        m_autoSyncInterval->setVisible(!daily);
        m_autoSyncInterval->setEnabled(!daily);
    }
    if (m_autoSyncDailyTime) {
        m_autoSyncDailyTime->setVisible(daily);
        m_autoSyncDailyTime->setEnabled(daily);
    }
}

AutoSyncScheduler::Mode SettingsDialog::autoSyncMode() const
{
    if (m_modeDaily && m_modeDaily->isChecked())
        return AutoSyncScheduler::ModeDaily;
    return AutoSyncScheduler::ModeInterval;
}

QTime SettingsDialog::autoSyncDailyTime() const
{
    if (m_autoSyncDailyTime)
        return m_autoSyncDailyTime->time();
    return AutoSyncScheduler::defaultDailyTime();
}

bool SettingsDialog::autoStartEnabled() const
{
    return m_autoStartBox && m_autoStartBox->isChecked();
}

// 「开机自动启动」：勾上立刻写系统，取消立刻删，不等「保存设置」。
//
// 写/删失败必须**看得见**：
//   · 界面上的勾选状态回滚成"真实状态"（不能勾着而系统里没有）；
//   · 弹一句人话，写清楚落到哪儿、为什么失败 —— 部署人员要能据此排查；
//   · 同时进日志抽屉（ConfigStore::notice）。
void SettingsDialog::onAutoStartToggled(bool on)
{
    QString err;
    const bool ok = ConfigStore::applyAutoStart(on, &err);
    if (!ok) {
        // 回滚勾选状态：界面上的"当前状态"必须等于系统里的真实状态。
        if (m_autoStartBox) {
            m_autoStartBox->blockSignals(true);
            m_autoStartBox->setChecked(!on);
            m_autoStartBox->blockSignals(false);
        }
        const QString where = ConfigStore::autoStartLocation();
        const QString reason = err.isEmpty()
            ? (on ? QString::fromUtf8(u8"写入失败") : QString::fromUtf8(u8"删除失败"))
            : err;
        const QString msg = (on ? QString::fromUtf8(u8"开机自动启动设置失败：%1")
                               : QString::fromUtf8(u8"取消开机自动启动失败：%1"))
                            .arg(reason);
        const QString full = msg + QString::fromUtf8(u8"\n位置：%1").arg(where);
        if (m_saveTip) {
            m_saveTip->setText(QString::fromUtf8(u8"\u2717 ") + msg);   // ✗
            m_saveTip->setVisible(true);
            QTimer::singleShot(6000, m_saveTip, &QLabel::hide);
        }
        ConfigStore::notice(full);
        QMessageBox::warning(this, QString::fromUtf8(u8"开机自动启动"), full);
        return;
    }
    // 只有真的落到系统里了，才把"用户意图"记进配置。
    // 反过来的话，配置说"已开"而系统里没有 → 界面显示已开、开机不启动。
    ConfigStore::saveAutoStartEnabled(on);
}

void SettingsDialog::browseSourceDir()
{
    const QString cur = m_sourceDir->text();
    const QString d = QFileDialog::getExistingDirectory(this, QString(), cur);
    if (!d.isEmpty())
        m_sourceDir->setText(d);
}

void SettingsDialog::browseKeyFile()
{
    const QString cur = m_keyPath->text();
    const QString f = QFileDialog::getOpenFileName(this, QString(), cur);
    if (!f.isEmpty())
        m_keyPath->setText(f);
}

void SettingsDialog::addRuleRow()
{
    const int r = m_ruleTable->rowCount();
    m_ruleTable->insertRow(r);
    while (m_rowExists.size() <= r)
        m_rowExists.append(false);
    m_rowExists[r] = false;
    for (int c = 0; c < 3; ++c) {
        QTableWidgetItem *it = new QTableWidgetItem;
        const QString mono = Theme::resolvedMono();
        it->setFont(mono.isEmpty() ? it->font() : QFont(mono));
        if (c == 0)
            it->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        m_ruleTable->setItem(r, c, it);
    }
    m_ruleTable->setCurrentCell(r, 0);
    m_ruleTable->editItem(m_ruleTable->item(r, 0));
}

void SettingsDialog::removeRuleRow(int row)
{
    if (row < 0 || row >= m_ruleTable->rowCount())
        return;
    m_ruleTable->removeRow(row);
    if (row < m_rowExists.size())
        m_rowExists.removeAt(row);
}

void SettingsDialog::onRuleCellChanged(int row, int col)
{
    if (row >= 0 && row < m_rowExists.size())
        m_rowExists[row] = true;
    Q_UNUSED(col);
}

void SettingsDialog::loadConfig(const TargetConfig &target,
                                const QString &localRoot,
                                const QVector<ToolRule> &toolRules)
{
    m_sourceDir->setText(localRoot);
    m_host->setText(target.host);
    m_internalPort = target.port > 0 ? target.port : 22;
    m_port->setText(QString::number(m_internalPort));
    m_user->setText(target.user);
    m_password->setText(target.password);
    m_internalKeyPath = target.keyPath;
    m_keyPath->setText(m_internalKeyPath);
    m_remoteRoot->setText(target.remoteRoot);
    if (m_autoSyncInterval)
        m_autoSyncInterval->setText(QString::number(ConfigStore::loadAutoSyncInterval()));
    // 自动同步模式与定点时刻：每次打开弹窗都按**已存的值**回填。
    // 老配置没有这两个键 → 按间隔重复 / 03:00（= 老版本行为，一个字都不写回去）。
    if (m_modeInterval && m_modeDaily) {
        const bool daily = (ConfigStore::loadAutoSyncMode() == AutoSyncScheduler::ModeDaily);
        m_modeInterval->setChecked(!daily);
        m_modeDaily->setChecked(daily);
    }
    if (m_autoSyncDailyTime)
        m_autoSyncDailyTime->setTime(ConfigStore::loadAutoSyncDailyTime());
    onAutoSyncModeChanged();
    if (m_rememberBox)
        m_rememberBox->setChecked(ConfigStore::loadRememberSettings());

    m_ruleTable->setRowCount(0);
    m_rowExists.clear();
    m_toolArgsByExe.clear();
    for (int i = 0; i < toolRules.size(); ++i) {
        const ToolRule &tr = toolRules.at(i);
        const int n = tr.toolCount();
        for (int k = 0; k < n; ++k) {
            const QString exe = tr.exeAt(k).trimmed();
            // 记下「这个后缀的这个 exe 原先带什么参数」。规则表只有 3 列、没有参数列，
            // 所以 applyConfig 采不到 args —— 但**不能因为采不到就抹掉**：
            // 参数是在工具页配的，用户点一次「保存设置」就全没了。
            // applyConfig 是 const 的，所以快照必须存在这里、这里是非 const。
            // 键带后缀：同一个 exe 配给 .spe 和 .raw 时两边的参数是各自独立的，
            // 只用 exe 做键会让后写的覆盖先写的（见 SettingsDialog.h 里的说明）。
            const QString args = tr.argsAt(k);
            if (!exe.isEmpty() && !args.isEmpty())
                m_toolArgsByExe.insert(argKey(tr.suffix, exe), args);
            const int r = m_ruleTable->rowCount();
            m_ruleTable->insertRow(r);
            m_rowExists.append(true);
            QTableWidgetItem *a = new QTableWidgetItem(QStringLiteral(".") + tr.suffix);
            QTableWidgetItem *b = new QTableWidgetItem(tr.nameAt(k));
            QTableWidgetItem *c = new QTableWidgetItem(tr.exeAt(k));
            a->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            b->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            c->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
            c->setToolTip(tr.exeAt(k));
            m_ruleTable->setItem(r, 0, a);
            m_ruleTable->setItem(r, 1, b);
            m_ruleTable->setItem(r, 2, c);
        }
    }
}

// 参数快照的键：(归一化后缀, 小写 exe)。
// 后缀要剥掉前导点、并统一小写 —— 表格里可能写成 ".spe"、"spe" 或 ".SPE"，
// 三者在 applyConfig 里都会被剥成同一个 "spe"，键必须跟着用同一套归一化，
// 否则存的时候是 "spe"、取的时候是 ".SPE"，取不到就退化成空串（= 又清空了）。
QString SettingsDialog::argKey(const QString &suffix, const QString &exe)
{
    QString s = suffix.trimmed();
    while (s.startsWith(QLatin1Char('.')))
        s.remove(0, 1);
    return s.toLower() + QLatin1Char('\x1f') + exe.trimmed().toLower();
}

void SettingsDialog::applyConfig(TargetConfig *target,
                                 QString *localRoot,
                                 QVector<ToolRule> *toolRules) const
{
    if (target) {
        const QString rawHost = m_host->text().trimmed();

        // 端口有两个入口，这里定死优先级，避免"两个框都能设端口"时互相盖：
        //   1. IP 框里写 "主机:端口"（scp 习惯写法）—— 显式写了端口，以它为准
        //   2. 否则用独立的「SSH 端口」框
        //   3. 端口框空 / 非法 -> 22
        //
        // 端口框是给不懂 scp 语法的现场人员用的，所以它有标签、有范围校验；
        // IP 框保持干净（一个纯 IP），不必先知道"端口要写在后面"这件事。
        //
        // 用局部变量而不是写成员：本函数是 const 的，成员是"最近一次生效值"，
        // 在这里改它会让"保存过的配置"和"内存里的值"对不上。

        // 端口框的值先算出来，被 host:port 写法覆盖时再改。
        //
        // 静默退回 22 这一支以前**零提示**，于是填了全角数字、带空格、
        // 超范围的现场人员点完保存，看到的只会是同步时的
        // 「网络层：没有连到目标电脑…可能是 IP 填错、网络或交换机不通」——
        // 真因就在他刚敲进去的那一栏，却被指去了防火墙。
        // 这里当场说一句人话，提示仍走日志抽屉（见 ConfigStore::notice 的约定）。
        //
        // 这里无条件调 noticePortProblem()，判断在它里面 ——
        // 端口**留空**也要说。上一版写的是
        // `if (!portText.isEmpty() && !(portOk && pv > 0 && pv <= 65535))`，
        // 空值被那个 isEmpty 守卫放过去了，而"端口栏空着直接点保存"正是
        // 现场最常见的走法（默认值不填、或者想清空重填）。
        // 结果和全角端口一样：静默存成 22，同步时全失败，原因被指去网络层。
        //
        // 判据也换成 ConfigStore::isPlainPortText，和读配置那一侧共用同一条，
        // 免得"保存时判一种、读回来判另一种"。
        const QString portText = m_port->text().trimmed();
        int port = ConfigStore::isPlainPortText(portText) ? portText.toInt() : 22;
        ConfigStore::noticePortProblem(portText);

        if (rawHost.startsWith(QLatin1Char('['))) {
            // [::1]:2226 —— 带方括号的 IPv6（rsync/scp 的标准写法）。
            // 原来这一支只是被跳过、整串塞进 host，出来的是个坏地址。
            const int close = rawHost.indexOf(QLatin1Char(']'));
            if (close > 1) {
                target->host = rawHost.mid(1, close - 1);
                const QString tail = rawHost.mid(close + 1).trimmed();
                if (tail.startsWith(QLatin1Char(':'))) {
                    bool ok2 = false;
                    const int p2 = tail.mid(1).trimmed().toInt(&ok2);
                    if (ok2 && p2 > 0 && p2 <= 65535)
                        port = p2;
                }
            } else {
                target->host = rawHost;
            }
            target->port = port;
        } else if (rawHost.contains(QLatin1Char(':'))) {
            const QStringList parts = rawHost.split(QLatin1Char(':'));
            if (parts.size() == 2) {
                // 只在这一支才改写 host：裸 IPv6 含多个冒号，会落到下面
                // 的 else，保持原样（用默认端口）。
                target->host = parts.at(0).trimmed();
                bool ok = false;
                const int p = parts.at(1).trimmed().toInt(&ok);
                if (ok && p > 0 && p <= 65535)
                    port = p;
                target->port = port;
            } else {
                target->host = rawHost;
                target->port = port;
            }
        } else {
            target->host = rawHost;
            target->port = port;
        }
        target->user = m_user->text().trimmed();
        target->password = m_password->text();
        target->remoteRoot = m_remoteRoot->text().trimmed();
        target->keyPath = m_keyPath->text().trimmed();
    }
    if (localRoot)
        *localRoot = m_sourceDir->text().trimmed();
    if (!toolRules)
        return;

    toolRules->clear();
    for (int r = 0; r < m_ruleTable->rowCount(); ++r) {
        QTableWidgetItem *a = m_ruleTable->item(r, 0);
        QTableWidgetItem *b = m_ruleTable->item(r, 1);
        QTableWidgetItem *c = m_ruleTable->item(r, 2);
        if (!a || !b || !c)
            continue;
        QString suf = a->text().trimmed();
        while (suf.startsWith(QLatin1Char('.')))
            suf.remove(0, 1);
        QString name = b->text().trimmed();
        const QString exe  = c->text().trimmed();

        // 智能纠错：若第一列误填了表头文本"文件类型"，自动将第二列的英文字符纠正为后缀
        if (suf == QString::fromUtf8(u8"\u6587\u4EF6\u7C7B\u578B") && !name.isEmpty()) {
            suf = name;
            name = QString::fromUtf8(u8"\u67E5\u770B\u5DE5\u5177");
        }
        if (suf.isEmpty() || (name.isEmpty() && exe.isEmpty()))
            continue;

        int idx = -1;
        for (int k = 0; k < toolRules->size(); ++k) {
            if (toolRules->at(k).suffix.compare(suf, Qt::CaseInsensitive) == 0) {
                idx = k;
                break;
            }
        }
        if (idx < 0) {
            ToolRule nr;
            nr.suffix = suf;
            nr.toolNames.append(name);
            nr.toolExePaths.append(exe);
            // 原来这里是写死的 QString()：表格没有参数列，于是把工具页
            // 配好的 --play {file} 之类参数一律清成空串。参数在这一栏
            // 是"看不见但存在"的，采不到就该**沿用原值**，而不是抹掉。
            nr.toolArgs.append(m_toolArgsByExe.value(argKey(suf, exe)));
            toolRules->append(nr);
        } else {
            (*toolRules)[idx].toolNames.append(name);
            (*toolRules)[idx].toolExePaths.append(exe);
            (*toolRules)[idx].toolArgs.append(m_toolArgsByExe.value(argKey(suf, exe)));
        }
    }
}

void SettingsDialog::showTestResult(const QString &text, bool ok, bool testing)
{
    if (!m_testLabel)
        return;
    if (testing) {
        m_testLabel->setText(text);
        m_testLabel->setStyleSheet(QStringLiteral("color: #60a5fa; font-weight: 500; font-size: 11px;"));
        m_testLabel->setVisible(true);
        return;
    }
    m_testLabel->setText(text);
    m_testLabel->setStyleSheet(ok ? QStringLiteral("color: #34d399; font-weight: 600; font-size: 11px;")
                                  : QStringLiteral("color: #f87171; font-weight: 600; font-size: 11px;"));
    m_testLabel->setVisible(true);
}

TargetConfig SettingsDialog::currentTargetConfig() const
{
    TargetConfig t;
    applyConfig(&t, nullptr, nullptr);
    return t;
}

int SettingsDialog::autoSyncInterval() const
{
    if (!m_autoSyncInterval)
        return AutoSyncScheduler::DefaultIntervalSec;
    // 空/非法输入 → 回默认值，**不能**落到下限。
    //
    // toInt() 对空串返回 0（且 ok=false），clampIntervalSec(0) 会截成下限 10。
    // 于是"把输入框清空想重填"再点保存，自动同步就变成**每 10 秒**一轮
    // ——全目录深度扫描 + SSH + rsync 背靠背连跑，是最激进的一档，
    // 而输入框里是空的，客户完全不知道自己改了什么（重开设置才看得到 10）。
    //
    // QIntValidator 的 Intermediate 态允许空串，所以这不是理论问题。
    bool ok = false;
    const int v = m_autoSyncInterval->text().trimmed().toInt(&ok);
    if (!ok || v <= 0)
        return AutoSyncScheduler::DefaultIntervalSec;
    // 范围**必须**引用共享常量，不能在这里另写一份。
    // 这里原来写的是 [5, 3600]，而输入框的校验器已经改成
    // AutoSyncScheduler::MinIntervalSec..MaxIntervalSec（10~86400）——
    // 两处口径不一致的后果是：输入框只让填 10~86400，
    // 但这个函数把读到的值截到 5~3600，于是 3601~86400 的合法输入会被截成 3600
    // ——客户填"每天一次"(86400)，实际存成"每小时一次"(3600)，
    // 而且界面上还显示着他填的 86400。**完全察觉不到的静默改写。**
    return AutoSyncScheduler::clampIntervalSec(v);
}

