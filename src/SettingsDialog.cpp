#include "SettingsDialog.h"

#include <cstdio>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QFrame>
#include <QScrollArea>
#include <QScrollBar>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QStyledItemDelegate>
#include <QFontMetrics>
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
#include <QToolButton>
#include <QTimer>
#include <QScreen>
#include <QWindow>
#include <QGuiApplication>
#include <QGraphicsDropShadowEffect>
#include "Theme.h"
#include "ScreenFit.h"
#include "ConfigStore.h"
#include "AutoSyncScheduler.h"

namespace {

// 规则表的第 4 列：每行的「删除」按钮。
//
// 为什么需要这一列：产品里**从来没有**过删除规则的入口 ——
// removeRuleRow() 只被自动化测试钩子调用过，用户点「+ 添加规则」
// 加出来的行就再也去不掉（2026-10-28 用户报障：「添加规则之后无法进行删除」）。
const int kRuleDelCol = 3;

// 「关联程序路径」列的绘制委托。
//
// 为什么不能直接用默认绘制：QTableWidget 默认 wordWrap = true，
// 长路径会在 28px 高的行里换行成两行，**第二行被裁掉** ——
// 用户看到的是"路径断在半截、还有一截看不见"（2026-10-28 报障「路径太挤了」）。
//
// 也不能直接靠 QFontMetrics::elidedText：本工程在 ToolColumnDelegate.cpp 里
// 实测过，Qt 5.9.6 对"一整串没有断点的超长文本"返回的是**不带省略号的硬截断**
// （144 字符的工具名在 156px 列里出来是 "LongToolN"，尾巴没有 "…"）。
//
// 这里自己裁：**中间省略**，头尾都留 —— 路径最有用的信息是开头几级目录
// 和最后的 exe 文件名，两头都在才认得出是哪一条规则。
// 完整路径放进单元格的 tooltip，鼠标悬停即见全称。
class RulePathDelegate : public QStyledItemDelegate
{
public:
    // 派生类**不会**继承基类的构造函数，所以这里必须自己写一个：
    // 不写的话 new RulePathDelegate(table) 会被当成拷贝构造，编译报错。
    explicit RulePathDelegate(QObject *parent = nullptr)
        : QStyledItemDelegate(parent) {}

    // 按宽度裁文本：头尾各留一部分，中间放一个省略号。
    static QString elideMiddle(const QString &text, int maxWidth, const QFontMetrics &fm)
    {
        if (maxWidth <= 0)
            return QString();
        if (fm.width(text) <= maxWidth)
            return text;
        const QString ell = QString::fromUtf8("\xE2\x80\xA6");   // U+2026 …
        const int ellW = fm.width(ell);
        if (ellW >= maxWidth)
            return ell;                       // 连省略号都放不下
        const int budget = maxWidth - ellW;
        // 头略长于尾：目录层级比文件名更耐看。
        const int headW = budget / 2 + (budget % 2);
        const int tailW = budget - headW;
        int head = 0;
        while (head < text.length() && fm.width(text.left(head + 1)) <= headW)
            ++head;
        int tail = 0;
        while (tail < text.length() && fm.width(text.right(tail + 1)) <= tailW)
            ++tail;
        if (head + tail >= text.length())
            return text;
        return text.left(head) + ell + text.right(tail);
    }

    void initStyleOption(QStyleOptionViewItem *option, const QModelIndex &index) const override
    {
        QStyledItemDelegate::initStyleOption(option, index);
        // 只换**画出来**的文字，模型里的原文一个字都不动
        // （applyConfig 要拿完整路径落盘，ruleCellForTest 也读原文）。
        // 颜色/背景仍然由样式表接管（QTableView#ruleTable 的 color: #e2e8f0）——
        // 所以这里改的是 option->text，而不是自己重画一遍。
        if (option->text.isEmpty())
            return;
        // 单元格左右各留 6px 内边距（默认绘制就是这么留的），再多留 4px 余量，
        // 免得裁出来的字刚好顶到格线、被样式表二次裁一次。
        const int w = option->rect.width() - 16;
        if (w <= 0)
            return;                           // sizeHint 阶段 rect 无效，原样返回
        option->text = elideMiddle(option->text, w, QFontMetrics(option->font));
    }
};

// 规则表的表头。定稿图的表头行**有列竖线**，而 QTableWidget 的 gridline
// 只画在数据区，表头一条竖线都没有 —— 于是表头是三段没有分隔的大字，
// 跟定稿图对不上。
//
// 这里自绘：每段右边缘画一条 #1c2740 的竖线（与数据区的列竖线同色同位），
// 表头底色由 QSS 给（#111726，比数据行 #151d32 深）。
class RuleHeaderView : public QHeaderView
{
public:
    explicit RuleHeaderView(QWidget *parent = nullptr)
        : QHeaderView(Qt::Horizontal, parent)
    {
        setSectionsClickable(false);
        setHighlightSections(false);
        // 定稿图实测：表头高 29px（表头 + 2 行 = 85px，行高 28）。
        setFixedHeight(29);
    }

    // 注意：**这里不自己画表头**。
    //
    // Qt 5 的 QHeaderView::paintEvent 会走 `style()->drawControl(CE_Header)`；
    // app 级 QSS 里一旦出现 `QHeaderView::section` 规则，样式表样式就接管，
    // 它自己画每个 section 的背景 / 文字 / 边框。
    // 于是：
    //   ① 重写 paintSection() 不生效（样式表样式不调它）—— 实测像素扫描确认表头无竖线；
    //   ② 重写 paintEvent() 也不生效（QHeaderView 自己的 paintEvent 被样式表路径接管）——
    //      同样实测确认。
    // 正确做法是**顺着样式表样式这条路走**：竖线写成 section 的 border-right，
    // 由样式表样式自己画。详见 Theme.h 里那条 QSS 规则的注释。
};

const char *kTxtHost       = "\xE7\x9B\xAE\xE6\xA0\x87\xE7\x94\xb5\xE8\x84\x91 IP";      // 目标电脑 IP
const char *kTxtPort       = "SSH \xE7\xAB\xAF\xE5\x8F\xA3";      // SSH 端口
const char *kTxtKeyFile    = "SSH \xE7\xA7\x81\xE9\x92\xA5\xE6\x96\x87\xE4\xBB\xB6";      // SSH 私钥文件
const char *kTxtBrowseFile = "\xE9\x80\x89\xE6\x8B\xA9\xE6\x96\x87\xE4\xBB\xB6";      // 选择文件
const char *kTxtUser       = "\xE7\x99\xBB\xE5\xBD\x95\xE8\xB4\xA6\xE5\x8F\xB7";      // 登录账号
const char *kTxtPassword   = "\xE7\x99\xBB\xE5\xBD\x95\xE5\xAF\x86\xE7\xA0\x81";      // 登录密码
const char *kTxtRemoteRoot = "\xE8\xBF\x9C\xE7\xAB\xAF\xE4\xBF\x9D\xE5\xAD\x98\xE7\x9B\xAE\xE5\xBD\x95";  // 远端保存目录
const char *kTxtSourceDir  = "\xE6\x9C\xAC\xE6\x9C\xBA\xE5\x90\x8C\xE6\xAD\xA5\xE7\x9B\xAE\xE5\xBD\x95";  // 本机同步目录
const char *kTxtBrowse     = "\xE9\x80\x89\xE6\x8B\xA9\xE7\x9B\xAE\xE5\xBD\x95";      // 选择目录
const char *kTxtTools      = "\xE6\x96\x87\xE4\xBB\xB6\xE6\x89\x93\xE5\xBC\x80\xE5\xB7\xA5\xE5\x85\xB7";  // 文件打开工具
const char *kTxtAddRule    = "+ \xE6\xB7\xBB\xE5\x8A\xA0\xE8\xA7\x84\xE5\x88\x99";    // + 添加规则
const char *kTxtTestConn   = "\xE6\xB5\x8B\xE8\xAF\x95\xE7\xBD\x91\xE7\xBB\x9C\xE8\xBF\x9E\xE6\x8E\xA5";  // 测试网络连接
const char *kTxtSave       = "\xE4\xBF\x9D\xE5\xAD\x98\xE8\xAE\xBE\xE7\xBD\xAE";      // 保存设置
const char *kTxtDialogTitle= "\xE8\xAE\xBE\xE7\xBD\xAE";  // 设置
const char *kTxtColSuffix  = "\xE6\x96\x87\xE4\xBB\xB6\xE7\xB1\xBB\xE5\x9E\x8B";      // 文件类型
const char *kTxtColTool    = "\xE5\xB7\xA5\xE5\x85\xB7\xE5\x90\x8D\xE7\xA7\xB0";      // 工具名称
const char *kTxtColPath    = "\xE5\x85\xB3\xE8\x81\x94\xE7\xA8\x8B\xE5\xBA\x8F\xE8\xB7\xAF\xE5\xBE\x84";  // 关联程序路径
// 带上范围：客户看得出可以填多少。
// 原来这里只有"自动同步间隔"，而输入框挂了 QIntValidator(10, 86400)
// —— 客户想填 5 秒会被**静默拒绝**（输入框不反应），以为软件坏了。
const char *kTxtAutoInterval = "自动同步间隔";  // 自动同步间隔
// 取值范围（10 ~ 86400 秒）写在**输入框 tooltip** 里，不写在标签上：
// 定稿图的标签只有「自动同步间隔」六个字，范围后缀是程序自己加的。
const char *kTxtSec          = "\xE7\xA7\x92";  // 秒
// 每天定点的时刻标签。
const char *kTxtDailyTime    = "每天定时时刻";  // 每天定时时刻

// 设置弹窗的固定尺寸**不在这里写死**：真源是 ScreenFit.h
// （kSettingsWidth / kSettingsHeight / kSettingsWidthMin / kSettingsHeightMin）。
//
// 原来这里另存了一份 640/560，与 ScreenFit.h 里的同一组数字各改各的 ——
// 迟早分叉（2026-10-28 缺陷 4）。现在只留一份。
//
// 顺带把「为什么尺寸要固定」记在这儿，免得以后有人"优化"回去：
// 弹窗高度原来跟着「规则条数」长（每加一条规则 +28px），加到第 5、6 条时
// 被顶到 805px，**底栏（保存设置）被挤出屏幕**，用户点不到保存
//（2026-10-28 用户报障：「添加规则后，尺寸都看不到了」）。
// 固定之后：多出来的内容在中间**滚动**（QScrollArea + 深色滚动条），
// 弹窗尺寸永远不变，底栏永远在。


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

// 已经是 QString 的文案（比如运行时拼出来的组标题）。
QLabel *mkLabelQ(const QString &text, const char *objName)
{
    QLabel *l = new QLabel(text);
    l->setObjectName(QString::fromLatin1(objName));
    return l;
}

// 一个带标题的分组卡片（定稿图 .grp-box：卡片 + 左上角一行组标题）。
//
// 设置窗口原来把所有字段**平铺**在一列里，一屏看下去全是输入框，
// 不知道哪几个是一回事。定稿图分成三组：路径与网络 / 同步选项 / 文件打开工具。
QFrame *mkGroupBox(const QString &title, QVBoxLayout **layOut)
{
    QFrame *box = new QFrame;
    box->setObjectName(QStringLiteral("settingsGroup"));
    QVBoxLayout *lay = new QVBoxLayout(box);
    lay->setContentsMargins(14, 10, 14, 12);
    lay->setSpacing(8);
    lay->addWidget(mkLabelQ(title, "groupTitle"));
    *layOut = lay;
    return box;
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
    // 定稿图（用户 2026-10-27 给的设置弹窗截图）实测宽度 960px，缩放比 1.5
    // （标题栏 38px、弹窗边框 1px 都对得上）⇒ 逻辑宽度 = 960 / 1.5 = 640px。
    // 原来的 570px 是照更早一版原型抄的，字段挤在一起（选择目录/选择文件按钮
    // 直接被输入框顶掉）。HTML 定稿 .dialog-card 也是 640px。
    setFixedWidth(ScreenFit::kSettingsWidth);
    // SettingsDialog.h 一直承诺"整体 max-height 80vh"，但构造函数里从来没实现：
    // 只有宽度上限，高度被 sizeHint 顶到 ~690px，而 X11 分配的窗口只有 533px，
    // 无边框 + 透明背景下没有 WM 兜底，超出部分直接被裁 —— 实测"本机文件目录 /
    // 目标电脑 IP / SSH 端口"三个最关键的字段在屏幕上看不见。
    // 中间那块是 QScrollArea，限高之后自然出滚动条，字段一条都不会丢。
    // 按**父窗口所在那块屏**算（多显示器下主窗口在副屏时弹窗跟着走），
    // 与 ScreenFit::screenForWidget() 是同一份规则。
    QScreen *scr = ScreenFit::screenForWidget(parentWidget());
    const QRect avail = scr ? scr->availableGeometry() : QRect(0, 0, 1280, 764);
    setMinimumHeight(ScreenFit::kSettingsHeightMin);
    // 尺寸**钉死**，并且**跟着屏幕收敛**。
    //
    // 为什么要收敛：笔记本 1366×768、1080p、4K 缩放各不相同 ——
    // 写死 640×560，在矮屏上会顶出屏幕，在大屏上又显得小气。
    // 这里统一按「屏幕可用区 - 40px」收敛（availableGeometry 已经排除了任务栏）。
    // 收敛规则只有一份（ScreenFit.h），主界面和设置弹窗共用。
    {
        const QSize sz = ScreenFit::settingsDialog(avail.width(), avail.height());
        setFixedSize(sz);
    }

    QVBoxLayout *outerLayout = new QVBoxLayout(this);
    // 定稿图实测：标题栏顶到 y=0（弹窗本身只有 1px 外框），
    // 原来留 10px 会让整块内容往下沉 10px、三组卡片比定稿图窄一圈。
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setObjectName(QStringLiteral("settingsCard"));
    outerLayout->addWidget(m_card);

    QVBoxLayout *cardLayout = new QVBoxLayout(m_card);
    cardLayout->setContentsMargins(0, 0, 0, 0);
    cardLayout->setSpacing(0);

    // 顶栏：左边只有「设置」两个字，右边一个细叉关闭，可拖动。
    //
    // 原来标题前面还有一颗齿轮 —— 定稿图的设置弹窗标题栏**没有**这颗齿轮，
    // 顶部栏只有标题和叉。齿轮反而和主窗口那颗撞车，容易被当成两套设置入口。
    m_header = new QFrame;
    m_header->setObjectName(QStringLiteral("dialogHeader"));
    // 定稿图实测标题栏高 38px（原来 44）。
    m_header->setFixedHeight(38);

    QHBoxLayout *hl = new QHBoxLayout(m_header);
    hl->setContentsMargins(12, 0, 12, 0);
    hl->setSpacing(8);

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
    m_scrollArea = scroll;
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame);
    // 尺寸策略：Minimum（只有 GrowFlag、没有 ShrinkFlag）。
    //
    // 为什么：QWidgetItem::minimumSize() 在策略带 ShrinkFlag 时取
    // minimumSizeHint()，而 QScrollArea 的 minimumSizeHint() 就是
    // **内容容器的 minimumSizeHint**（内容多高，它就要求多高）——
    // 于是「加几条规则」就能把弹窗顶高（实测 805px，底栏被挤出屏幕）。
    // 改成 Minimum 之后，外层布局取的是滚动区的**显式最小高**（下面设成 0），
    // 弹窗高度不再被内容牵着走：内容超出就在中间滚动。
    //
    // 内容容器**必须保留自己的 minimumSizeHint**（= 布局最小高）：
    // QScrollArea::resizeEvent 会把内容 resize 成 max(视口高, 内容最小高)，
    // 如果连内容最小高也钉成 0，内容就会被压到视口高度，
    // 布局放不下 ⇒ 控件互相重叠（实测截图就是这样）。
    scroll->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    QWidget *body = new QWidget;
    QVBoxLayout *bl = new QVBoxLayout(body);
    // 定稿图实测：三组卡片左右各留 15.33px（卡片左边框 x=23/1.5），
    // 距标题栏 14.7px、距底栏 14.7px，组间距约 11px。
    bl->setContentsMargins(14, 14, 14, 14);
    bl->setSpacing(8);
    scroll->setWidget(body);
    cardLayout->addWidget(scroll, 1);

    // ==== 组 1：路径与网络 ====
    //
    // 定稿图把设置窗口分成三组卡片：路径与网络 / 同步选项 / 文件打开工具。
    // 原来所有字段**平铺**在一列里，一屏看下去全是输入框，
    // 哪几个是一回事、哪个属于哪块，完全看不出来。
    QVBoxLayout *netLay = nullptr;
    QFrame *gNet = mkGroupBox(QString::fromUtf8(u8"路径与网络"), &netLay);
    // 定稿图「路径与网络」这一组实测高 267px（截图 960×1129，1.5 倍缩放），
    // 默认间距 8 会把它撑到 299px。四行字段各挤 4px，整体正好回到定稿图。
    netLay->setSpacing(4);
    bl->addWidget(gNet);

    // 1.1 本机同步目录 + 选择目录
    netLay->addWidget(mkLabel(kTxtSourceDir, "settingLabel"));
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
    netLay->addLayout(srcRow);
    connect(browse, SIGNAL(clicked()), this, SLOT(browseSourceDir()));

    // 1.2 目标电脑 IP + SSH 端口（两列并排）
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
    netLay->addLayout(g1);

    // 1.3 远端保存目录 + 登录账号（两列并排）
    //
    // 字段顺序照定稿图：远端保存目录在登录账号**左边**、同一行。
    // 原来远端保存目录被排到私钥那一行下面，一眼看过去顺序是乱的。
    QGridLayout *g3 = new QGridLayout;
    g3->setSpacing(10);
    g3->addWidget(mkLabel(kTxtRemoteRoot, "settingLabel"), 0, 0);
    g3->addWidget(mkLabel(kTxtUser, "settingLabel"), 0, 1);
    m_remoteRoot = mkInput("settingRemoteRoot", QString(),
                           QString::fromUtf8("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\x44:/Data"));  // 例如: D:/Data
    m_user = mkInput("settingUser", QString(),
                     QString::fromUtf8("\xE4\xBE\x8B\xE5\xA6\x82\xEF\xBC\x9A\x41\x64ministrator"));  // 例如: Administrator
    g3->addWidget(m_remoteRoot, 1, 0);
    g3->addWidget(m_user, 1, 1);
    g3->setColumnStretch(0, 1);
    g3->setColumnStretch(1, 1);
    netLay->addLayout(g3);

    // 1.4 登录密码 + SSH 私钥文件（+ 选择文件）
    //
    // 私钥这一栏（选填）：填了就用密钥认证，不填就回落到密码认证 ——
    // RsyncRunner 一直按这个语义在跑（`useKey()` 为真时走 `ssh -i`，
    // 并额外用 `-o` 关掉密码交互；为假且密码非空时走 askpass 脚本喂密码）。
    // 同样地：ConfigStore 存 target/keyPath，后端全支持，**只有界面没接上**。
    // 为什么要补：企业客户基本都改成密钥认证了，没有这一栏他们只能改配置文件。
    QGridLayout *g4 = new QGridLayout;
    g4->setSpacing(10);
    g4->addWidget(mkLabel(kTxtPassword, "settingLabel"), 0, 0);
    g4->addWidget(mkLabel(kTxtKeyFile, "settingLabel"), 0, 1);
    m_password = mkInput("settingPassword", QString(),
                         QString::fromUtf8("\xE7\x99\xBB\xE5\xBD\x95\xE5\xAF\x86\xE7\xA0\x81"));  // 登录密码
    m_password->setEchoMode(QLineEdit::Password);
    m_keyPath = mkInput("settingKeyPath", QString(),
                        QString::fromUtf8("\xE9\x80\x89\xE6\x8B\xA9\xE7\xA7\x81\xE9\x92\xA5\xE6\x96\x87\xE4\xBB\xB6"));  // 选择私钥文件
    QPushButton *keyBrowse = new QPushButton(QString::fromUtf8(kTxtBrowseFile));
    keyBrowse->setObjectName(QStringLiteral("btnBrowseKey"));
    keyBrowse->setFixedHeight(28);
    keyBrowse->setCursor(Qt::PointingHandCursor);
    QHBoxLayout *keyRow = new QHBoxLayout;
    keyRow->setSpacing(8);
    keyRow->addWidget(m_keyPath, 1);
    keyRow->addWidget(keyBrowse);
    g4->addWidget(m_password, 1, 0);
    g4->addLayout(keyRow, 1, 1);
    g4->setColumnStretch(0, 1);
    g4->setColumnStretch(1, 2);
    netLay->addLayout(g4);
    connect(keyBrowse, SIGNAL(clicked()), this, SLOT(browseKeyFile()));

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
    // 「自动同步」总开关。
    //
    // 它原来摆在主界面底栏，Jira 上的方案评审把它放进了设置窗口：
    // 底栏那一排只留「同步范围 / 带宽限制 / 开始同步」这种随手可调的，
    // 需要配置的东西（开关、周期、时刻）归设置窗口 —— 同一个开关在两处
    // 都能改，客户会不知道到底听谁的。
    //
    // 勾上 = 按下面选的周期自动跑；不勾 = 只在手动点「开始同步」时才跑。
    // ==== 组 2：同步选项 ====
    //
    // 自动同步：**两种互斥模式**（需求 10 的"类似 cron"）
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
    QVBoxLayout *optLay = nullptr;
    QFrame *gOpt = mkGroupBox(QString::fromUtf8(u8"同步选项"), &optLay);
    bl->addWidget(gOpt);

    // 内嵌的控制板（定稿图 .switch-board）：比组卡片再深一档。
    QFrame *board = new QFrame;
    board->setObjectName(QStringLiteral("switchBoard"));
    QVBoxLayout *boardLay = new QVBoxLayout(board);
    boardLay->setContentsMargins(12, 10, 12, 10);
    // 加了分隔线之后，两个方向的间距各让 2px，总高度不变。
    boardLay->setSpacing(7);
    optLay->addWidget(board);

    // 2.1 第一行：左侧「自动同步」总开关，右侧「同步方式」分段按钮。
    //
    // 「自动同步」总开关原来摆在主界面底栏，Jira 上的方案评审把它放进了设置窗口：
    // 底栏那一排只留「同步范围 / 带宽限制 / 开始同步」这种随手可调的，
    // 需要配置的东西（开关、周期、时刻）归设置窗口 —— 同一个开关在两处
    // 都能改，客户会不知道到底听谁的。
    //
    // 勾上 = 按下面选的周期自动跑；不勾 = 只在手动点「开始同步」时才跑。
    QHBoxLayout *topRow = new QHBoxLayout;
    topRow->setSpacing(8);
    m_autoSyncBox = new QCheckBox(QString::fromUtf8(u8"自动同步"));
    m_autoSyncBox->setObjectName(QStringLiteral("chkAutoSyncEnabled"));
    m_autoSyncBox->setCursor(Qt::PointingHandCursor);
    m_autoSyncBox->setChecked(ConfigStore::loadAutoSyncEnabled());
    m_autoSyncBox->setToolTip(QString::fromUtf8(
        u8"勾上：按下面选的周期自动同步。\n"
        u8"不勾：只在手动点「开始同步」时同步。"));
    topRow->addWidget(m_autoSyncBox);
    topRow->addStretch(1);
    // 定稿图里两个分段按钮左边**没有任何文字标签**。原来这里挂了一个
    // 「同步方式」的小灰字 —— 程序有、图上没有，删掉；说明移到按钮 tooltip。

    // 模式二选一。定稿图上是**分段按钮**（选中的蓝底白字），不是单选圆点 ——
    // 两颗单选圆点挂在设置窗口里太像表单控件，和整套 AP500BJ 的直角按钮语言不一致。
    m_modeInterval = new QPushButton(QString::fromUtf8(u8"按间隔重复"));
    m_modeInterval->setObjectName(QStringLiteral("chkAutoSyncModeInterval"));
    m_modeInterval->setCheckable(true);
    m_modeInterval->setAutoExclusive(true);
    m_modeInterval->setCursor(Qt::PointingHandCursor);
    m_modeDaily = new QPushButton(QString::fromUtf8(u8"每天定时"));
    m_modeDaily->setObjectName(QStringLiteral("chkAutoSyncModeDaily"));
    m_modeDaily->setCheckable(true);
    m_modeDaily->setAutoExclusive(true);
    m_modeDaily->setCursor(Qt::PointingHandCursor);
    m_modeInterval->setToolTip(QString::fromUtf8(
        u8"同步方式：按间隔重复。\n每隔下面设置的秒数自动同步一次。"));
    m_modeDaily->setToolTip(QString::fromUtf8(
        u8"同步方式：每天定时。\n每天在下面设置的时刻自动同步一次。"));
    topRow->addWidget(m_modeInterval);
    topRow->addWidget(m_modeDaily);
    boardLay->addLayout(topRow);

    // 2.2 间隔输入（只有「按间隔重复」时可见可用）
    // 定稿图里「自动同步间隔 [30] 秒」这一行**独占一个描边小框**，
    // 和上面「自动同步」那一行分开。原来是直接贴在 board 里，两个控件糊在一起。
    QFrame *intervalBox = new QFrame;
    intervalBox->setObjectName(QStringLiteral("intervalBox"));
    QHBoxLayout *intervalRow = new QHBoxLayout(intervalBox);
    intervalRow->setContentsMargins(10, 6, 10, 6);
    intervalRow->setSpacing(8);
    QLabel *intervalLabel = mkLabel(kTxtAutoInterval, "settingLabel");
    m_autoSyncInterval = mkInput("settingAutoSyncInterval",
                                 QString::number(AutoSyncScheduler::DefaultIntervalSec),
                                 QString::number(AutoSyncScheduler::DefaultIntervalSec));
    m_autoSyncInterval->setValidator(new QIntValidator(AutoSyncScheduler::MinIntervalSec,
                                                       AutoSyncScheduler::MaxIntervalSec,
                                                       m_autoSyncInterval));
    // 定稿图实测：间隔输入框 ≈60 逻辑 px（原来 96，把「秒」字推得太远）。
    m_autoSyncInterval->setFixedWidth(60);
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
    // 定稿图里「自动同步间隔 [30] 秒」是**一组**贴左排的，
    // 输入框紧跟在标签后面；原来中间插了 stretch，把输入框顶到了右边。
    intervalRow->addWidget(intervalLabel);
    intervalRow->addWidget(m_autoSyncInterval);
    intervalRow->addWidget(secLabel);
    intervalRow->addStretch(1);
    boardLay->addWidget(intervalBox);
    m_intervalRowBox = intervalBox;

    // 2.3 定点时刻选择（只有「每天定时」时可见可用）
    QWidget *dailyBox = new QWidget;
    QHBoxLayout *dailyRow = new QHBoxLayout(dailyBox);
    dailyRow->setContentsMargins(0, 0, 0, 0);
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
    boardLay->addWidget(dailyBox);
    m_dailyRowBox = dailyBox;

    // 2.4 开机自动启动（定稿图里靠右对齐）
    //
    // **只负责启动程序**，不擅自开启自动同步：程序被自启拉起来后，
    // 沿用用户上次勾选的「自动同步」状态（sync/autoSyncEnabled）。
    // 这一点必须写在界面上 —— 否则客户会以为"开了开机自启就等于每天自动传"。
    QHBoxLayout *autoStartRow = new QHBoxLayout;
    autoStartRow->addStretch(1);
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
    autoStartRow->addWidget(m_autoStartBox);
    // 定稿图：控制板里「开机自动启动」这一行**上面**有一条整行分隔线，
    // 把它和上面的间隔设置区分开。没有这条线，两块内容糊在一起。
    QFrame *boardDivider = new QFrame;
    boardDivider->setObjectName(QStringLiteral("boardDivider"));
    boardDivider->setFixedHeight(1);
    boardLay->addWidget(boardDivider);
    boardLay->addLayout(autoStartRow);

    // 模式的初始状态来自配置（老配置没有模式键 → 按间隔重复 = 老版本行为）。
    //
    // 顺序很重要：先 setChecked 再 connect。
    // 反过来的话，构造时的 setChecked 会触发 toggled →
    // onAutoSyncModeChanged() 把"读到的状态"当成"用户刚点的"处理。
    // （这里不会落盘 —— 模式只在点「保存设置」时才写。）
    {
        const bool dailyMode = (ConfigStore::loadAutoSyncMode() == AutoSyncScheduler::ModeDaily);
        m_modeDaily->setChecked(dailyMode);
        m_modeInterval->setChecked(!dailyMode);
    }
    connect(m_modeInterval, &QPushButton::toggled, this, &SettingsDialog::onAutoSyncModeChanged);
    connect(m_modeDaily, &QPushButton::toggled, this, &SettingsDialog::onAutoSyncModeChanged);
    onAutoSyncModeChanged();

    // ==== 组 3：文件打开工具 ====
    QVBoxLayout *toolsLay = nullptr;
    QFrame *gTools = mkGroupBox(QString::fromUtf8(kTxtTools), &toolsLay);
    bl->addWidget(gTools);

    // 第 4 列是「删除」按钮列（每行一个 ×）。
    // 原来只有 3 列、也从来没有任何删除入口 —— 用户加了规则就删不掉。
    m_ruleTable = new QTableWidget(0, 4);
    m_ruleTable->setObjectName(QStringLiteral("ruleTable"));
    // 表头先换掉再设列名：setHorizontalHeaderLabels() 是给**当前表头**写标签，
    // 顺序反了的话新表头上一个标签都没有（表头会是一片空白）。
    m_ruleTable->setHorizontalHeader(new RuleHeaderView(m_ruleTable));
    m_ruleTable->setHorizontalHeaderLabels(QStringList()
                                           << QString::fromUtf8(kTxtColSuffix)
                                           << QString::fromUtf8(kTxtColTool)
                                           << QString::fromUtf8(kTxtColPath)
                                           << QString());   // 第 4 列：删除按钮列（表头留空）
    m_ruleTable->horizontalHeader()->setHighlightSections(false);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    // 定稿图实测：规则表**有横竖分隔线**（#1c2740），原来整个关掉了。
    m_ruleTable->setShowGrid(true);
    // 行高照定稿图实测值（表头 + 2 行 = 85px）。
    m_ruleTable->verticalHeader()->setDefaultSectionSize(28);
    // 定稿图「文件打开工具」这一组实测高 171px：组标题 + 规则表（表头 + 2 行）
    // + 「+ 添加规则」按钮。规则表原来死锁 160px（能塞 4 行），整组被撑到 244px，
    // 面板因此比定稿图高出 70 多 px、第三组差点被挤出屏幕。
    m_ruleTable->setFixedHeight(96);
    // 列宽照定稿图实测：文件类型列 110px、工具名称列 130px
    //（定稿图表头列竖线逻辑位置 28 / 138 / 268 / 612.7，差值即列宽）；
    // 「关联程序路径」列吃掉剩余宽度，最后是固定 44px 的删除按钮列。
    //
    // 为什么不能继续用 setStretchLastSection(true)：加了第 4 列之后
    // "最后一列"变成了删除按钮列 —— 路径列被压成固定宽，按钮列反被拉满整行。
    m_ruleTable->horizontalHeader()->setStretchLastSection(false);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_ruleTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    m_ruleTable->setColumnWidth(0, 110);
    m_ruleTable->setColumnWidth(1, 130);
    m_ruleTable->setColumnWidth(3, 44);
    // 「关联程序路径」列**单行显示**。
    //
    // 默认 wordWrap = true：长路径在 28px 高的行里换行成两行，
    // 第二行被裁掉一半 —— 用户看到的是「路径断在半截」（2026-10-28 报障
    // 「路径太挤了」）。单行 + 中间省略（RulePathDelegate）才是干净的做法：
    // 头尾都在、中间一个省略号，完整路径在 tooltip 里。
    m_ruleTable->setWordWrap(false);
    m_rulePathDelegate = new RulePathDelegate(m_ruleTable);
    m_ruleTable->setItemDelegateForColumn(2, m_rulePathDelegate);
    toolsLay->addWidget(m_ruleTable);

    // 「+ 添加规则」放在表格**下面**（定稿图 .btn-add 就是这么排的），
    // 原来它挂在标题行右边，和组标题挤在一起。
    m_addRule = new QPushButton(QString::fromUtf8(kTxtAddRule));
    m_addRule->setObjectName(QStringLiteral("btnAddRule"));
    m_addRule->setCursor(Qt::PointingHandCursor);
    toolsLay->addWidget(m_addRule, 0, Qt::AlignLeft);

    // 定稿图里「文件打开工具」这一组是：标题 + 规则表 + 「+ 添加规则」，
    // **下面没有提示文字**。原来这里多挂了一行「提示：在列表中点击对应的工具按钮…」。
    // 用户要的是"跟定稿图完全一模一样"，这行字去掉。
    // （行为本身没变：点工具按钮照样直接打开选中文件，写在手册里。）
    connect(m_addRule, SIGNAL(clicked()), this, SLOT(addRuleRow()));

    // ---- 底栏（图四风格：左侧[测试网络连接]，右侧[保存设置]）----
    QFrame *footer = new QFrame;
    footer->setObjectName(QStringLiteral("dialogFooter"));
    // 定稿图实测底栏高 43px（原来 52）。
    footer->setFixedHeight(44);
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

    // 「记住设置」勾选框已按客户意见从界面上移除（Jira Tools-60）。
    //
    // 它的真实语义是「目标连接信息要不要写进本机配置文件」—— 界面上只写
    // 「记住设置」四个字，客户看不懂它管什么，评审时明确要求去掉。
    //
    // 底下的闸门还在（ConfigStore::saveTarget / loadTarget 会看这个键），
    // 而界面上已经没有任何地方能改它了 —— 这个键必须被定死成「保存」。
    //
    // ★ 钉死的位置在 **main() 的启动段**（早于任何 target 配置被读或被写），
    // 不是这里。原来写在这个构造函数里，于是老配置里存着 false 的用户：
    // 启动时 loadTarget() 读不到自己的连接信息，saveTarget() 还会把 ini 里的
    // target/* 删掉 —— 界面又没地方能改回来，连接信息就这么没了。
    fl->addSpacing(10);

    m_btnSave = new QPushButton(QString::fromUtf8(kTxtSave));
    m_btnSave->setObjectName(QStringLiteral("btnSave"));
    m_btnSave->setCursor(Qt::PointingHandCursor);
    fl->addWidget(m_btnSave);

    cardLayout->addWidget(footer);

    // ---- 面板高度：按内容整高显示，三组卡片一屏全露 ----
    //
    // 定稿图里「路径与网络 / 同步选项 / 文件打开工具」三组**一次全看得见**，没有滚动条。
    //
    // 原来窗口只长到 640×526：QScrollArea::sizeHint() 在内容还没真正上屏时算不准
    // （Qt 5.9 会把它截到 viewport 的尺寸上限），于是窗口只够放下一组卡片，
    // 第三组「文件打开工具」整组被裁掉 —— 用户根本看不到工具规则表。
    //
    // 这里用**内容布局自己算出来的**高度兜底：内容放得下就整高显示（不滚），
    // 屏幕确实不够高（麒麟 768p 那种）才退回复制滚动，一个字段都不会丢。
    bl->activate();
    const int contentH = bl->sizeHint().height();
    // setFixedHeight 会同时写死 minimumHeight，这里读它比读 sizeHint 准
    // （sizeHint 对 QFrame 来说只是内部布局的建议值，不是实际高度）。
    const int chromeH  = m_header->minimumHeight() + footer->minimumHeight()
                       + outerLayout->contentsMargins().top() + outerLayout->contentsMargins().bottom();
    // 限高按**屏幕实际可用高度**算，不按 80vh：80vh 是给麒麟 768p 那种矮屏留的，
    // 在 1080p 上会白白砍掉 100 多 px，面板被迫滚动、三组卡片露不全。
    // 留 40px 余量给窗口边框/任务栏，矮屏上仍然自动退回滚动。
    // 限高与 ScreenFit::settingsDialog() 用**同一个余量**（kMargin = 40）。
    // 原来这里写的是 avail.height()-2，比真实约束大 38px ——
    // 调试打印出来的 limit 比屏幕实际允许的大，排障时会被这个数带偏
    //（2026-10-28 缺陷 5）。
    const int limitH   = qMax(ScreenFit::kSettingsHeightMin,
                              avail.height() - ScreenFit::kMargin);
    // 分层高度诊断（设 SRT_DEBUG_LAYOUT=1 才打印）：三组卡片各占多高、
    // 内容一共多高、屏幕允许多高 —— 对不上定稿图时按这三个数定位。
    if (!qgetenv("SRT_DEBUG_LAYOUT").isEmpty()) {
        printf("[layout] net=%d opt=%d tools=%d content=%d chrome=%d limit=%d screen=%dx%d\n",
               gNet->sizeHint().height(), gOpt->sizeHint().height(), gTools->sizeHint().height(),
               contentH, chromeH, limitH, avail.width(), avail.height());
        fflush(stdout);
    }
    // 面板高度：**固定**，不跟内容走（构造时就已经 setFixedHeight 钉死了）。
    //
    // 这里只保留诊断输出，不再按内容重设高度。
    //
    // 原来这里是「内容整高显示」：算一遍内容高度，放得下就 setFixedHeight(内容高)。
    // 两个问题都出在这套逻辑上：
    //   ① 内容放不下时**没有 else 分支** ⇒ QScrollArea 的 sizeHint 塌成 339
    //      ⇒ 弹窗被压扁成半屏（2026-10-28 现场截图 640×339）。
    //   ② 内容高度跟着「规则条数」变 ⇒ 每加一条规则弹窗长 28px，
    //      加到第 5、6 条时弹窗 805px，底栏被挤出屏幕。
    //
    // 现在：高度固定（名义 560，按屏幕收敛），内容多出来就在中间滚动。
    // 滚动区的最小高**不给**：它一旦被撑大，外层布局就会把弹窗顶高
    // （ScrollBody 的 minimumSizeHint 已经钉成 0，内容超高时自动出滚动条）。
    scroll->setMinimumHeight(0);

    connect(m_btnSave, SIGNAL(clicked()), this, SLOT(onSaveClicked()));
    connect(test, SIGNAL(clicked()), this, SIGNAL(settingsTestConnectionRequested()));
}

// 上屏时把面板**钉在屏幕可用区内**。
//
// 无边框窗口：Qt 在 show() 时按父窗口居中，而这个面板可能高达 758px
// （几乎占满 764 的可用高度）—— 一旦父窗口位置偏一点，弹窗就会有一截
// 落到任务栏上被裁掉。垂直居中，超出就贴上边。
void SettingsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);

    // 用**父窗口所在的那块屏**，不一定是主屏。
    // 多显示器下主窗口拖到副屏时，设置弹窗必须跟着走 ——
    // 否则弹窗会出现在另一块屏上（用户会以为程序卡住）。
    // 拔掉外接显示器之后同理。
    //
    // 找屏规则与主窗口**共用一份**（ScreenFit::screenForWidget）：
    // 优先用「窗口当前所在屏」，构造期没有 windowHandle 时按几何中心判断，
    // 最后才退到主屏。
    //
    // 注意：Qt 5.9.6 没有 QGuiApplication::screenAt()（5.10 才有），
    // 本工程用 Qt 5.9.6 ⇒ 自己遍历 screens() 判断。
    QScreen *scr = ScreenFit::screenForWidget(parentWidget() ? parentWidget() : this);
    if (!scr)
        return;
    const QRect avail = scr->availableGeometry();

    // 尺寸也按**这块屏**重新收敛一次：构造时算的是主屏，
    // 窗口被拖到另一块屏（分辨率/缩放不同）之后就过时了。
    const QSize want = ScreenFit::settingsDialog(avail.width(), avail.height());
    if (size() != want)
        setFixedSize(want);

    const QSize sz = size();
    if (sz.width() <= 0 || sz.height() <= 0)
        return;
    // 居中并保证整块落在可用区内（与主窗口同一份规则）。
    move(ScreenFit::centeredIn(sz, avail).topLeft());
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
    // 「自动同步」总开关也在这一刻落盘。
    //
    // 顺序同样重要：必须在 emit settingsAccepted() **之前** ——
    // 那条信号会走到 SyncPanel::applyFromSettings()，它读的就是这个键
    // （读到新的勾选状态才会重新排程）。顺序反了的话，
    // 本轮排程仍按旧的开关状态走，要等下次打开设置才对。
    if (m_autoSyncBox)
        ConfigStore::saveAutoSyncEnabled(m_autoSyncBox->isChecked());
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
            // √ (U+221A) 而不是 ✓ (U+2713)：后者在 Microsoft YaHei UI 里没有字形，
            // 精简版 Windows / 容器上会变成豆腐块；√ 在 GB2312 就收录，任何中文字体都有。
            m_saveTip->setText(QString::fromUtf8(u8"\u221A \u8BBE\u7F6E\u5DF2\u4FDD\u5B58") // √ 设置已保存
                               + effectiveNote);
        } else {
            m_saveTip->setText(QString::fromUtf8(u8"\u00D7 \u8BBE\u7F6E\u672A\u4FDD\u5B58")
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
    // 整行一起藏：只藏输入框的话，"自动同步间隔"和"秒"两个标签会变成孤儿 ——
    // 界面上剩一行"自动同步间隔　　秒"，中间是空的。
    //
    // 输入框仍然**单独**藏一次：自检判据 auto-sync-mode-exclusive-ui 判的是
    // isHidden()（控件自己被 setVisible(false) 过），父容器藏起来不算。
    if (m_intervalRowBox)
        m_intervalRowBox->setVisible(!daily);
    if (m_dailyRowBox)
        m_dailyRowBox->setVisible(daily);
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
            m_saveTip->setText(QString::fromUtf8(u8"\u00D7 ") + msg);   // ×
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
    // 第 4 列：加出来的行同样得能删。
    attachRuleDeleteButton(r);
    // 表格高度必须**跟着行数走**，否则新行落在可视区之外 ——
    // 用户点一下看着什么都没发生（2026-10-27 报障的现象）。
    syncRuleTableHeight();
    // 再滚到底：新行必须在可见区里，光"存在"不算数。
    m_ruleTable->scrollToBottom();
    // 弹窗尺寸固定 ⇒ 外层滚动区可能停在中间，新行还在折叠线以下。
    // 顺手把**外层**也滚到底，让用户一眼看到刚加的那一行。
    if (m_scrollArea && m_scrollArea->verticalScrollBar()) {
        QScrollBar *sb = m_scrollArea->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
    m_ruleTable->setCurrentCell(r, 0);
    m_ruleTable->editItem(m_ruleTable->item(r, 0));
}

// 给规则表的第 row 行装上「删除」按钮（第 4 列）。
//
// 为什么要有：产品里**从来没有**过删除规则的入口 —— removeRuleRow()
// 只被自动化测试钩子调用过。用户点「+ 添加规则」加出来的行就再也去不掉
//（2026-10-28 用户报障：「添加规则之后无法进行删除」）。
//
// 按钮放在单元格里的一个小 holder 上（不是直接铺满单元格）：铺满的话
// hover 的高亮会糊满整格，看着不像一个按钮。
//
// ★**行号不写死**：删掉中间一行之后，后面的行会往上挪一位 ——
// 写死行号的按钮会删掉**别人的**规则。所以点击时现查"这一格里的按钮是谁"。
void SettingsDialog::attachRuleDeleteButton(int row)
{
    if (!m_ruleTable || row < 0 || row >= m_ruleTable->rowCount())
        return;

    QWidget *holder = new QWidget(m_ruleTable);
    QHBoxLayout *lay = new QHBoxLayout(holder);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addStretch(1);

    QToolButton *btn = new QToolButton(holder);
    btn->setObjectName(QStringLiteral("ruleDelBtn"));
    btn->setText(QString::fromUtf8(u8"\u00D7"));            // ×
    btn->setToolTip(QString::fromUtf8(u8"删除这条规则"));
    btn->setCursor(Qt::PointingHandCursor);
    btn->setFocusPolicy(Qt::NoFocus);
    btn->setFixedSize(20, 20);
    lay->addWidget(btn, 0, Qt::AlignVCenter);
    lay->addStretch(1);

    m_ruleTable->setCellWidget(row, kRuleDelCol, holder);

    connect(btn, &QToolButton::clicked, this, [this, holder]() {
        if (!m_ruleTable)
            return;
        for (int i = 0; i < m_ruleTable->rowCount(); ++i) {
            if (m_ruleTable->cellWidget(i, kRuleDelCol) == holder) {
                removeRuleRow(i);
                return;
            }
        }
    });
}

void SettingsDialog::removeRuleRow(int row)
{
    if (row < 0 || row >= m_ruleTable->rowCount())
        return;
    // 单元格里的删除按钮要**先**摘掉：removeRow 之后那一行的 index 全部失效，
    // 挂在上面的 widget 由 Qt 回收 —— 不摘干净的话，残留按钮的点击
    // 会顺着 lambda 里的 holder 去找"当前行号"，删错行。
    if (QWidget *w = m_ruleTable->cellWidget(row, kRuleDelCol)) {
        m_ruleTable->removeCellWidget(row, kRuleDelCol);
        w->deleteLater();
    }
    m_ruleTable->removeRow(row);
    if (row < m_rowExists.size())
        m_rowExists.removeAt(row);
    // 少了一行，表格也得矮回去 —— 否则删完下面留一块空白。
    syncRuleTableHeight();
}

void SettingsDialog::onRuleCellChanged(int row, int col)
{
    if (row >= 0 && row < m_rowExists.size())
        m_rowExists[row] = true;
    // 「关联程序路径」列是单行 + 中间省略显示的，
    // 完整路径必须能一眼看到 —— 鼠标悬停显示全称。
    if (col == 2 && m_ruleTable && row >= 0 && row < m_ruleTable->rowCount()) {
        QTableWidgetItem *it = m_ruleTable->item(row, 2);
        if (it)
            it->setToolTip(it->text());
    }
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
    // 「自动同步」总开关：每次打开弹窗都按已存的值回填。
    if (m_autoSyncBox)
        m_autoSyncBox->setChecked(ConfigStore::loadAutoSyncEnabled());

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
            // 从配置读出来的行同样要有删除按钮。
            attachRuleDeleteButton(r);
        }
    }

    // 规则表高度随规则数走（实现见 syncRuleTableHeight）。
    syncRuleTableHeight();
}

// 规则表高度随规则数走。
//
// 原来死锁 96px（表头 + 2 行）：配到第 3 条规则时第 3 行被裁掉，
// 右边还冒出一条滚动条 —— 用户以为规则没保存上。
// 定稿图是 2 条规则（表头 + 2 行 = 85px），所以 2 条时高度一个像素都不动。
//
// 为什么必须抽成函数：**点「+ 添加规则」之后也要重算**。
// 原来只有 loadConfig 会算 —— 点完按钮表格高度不变，新行落在可视区之外，
// 用户点一下看着什么都没发生（2026-10-27 用户报障）。
void SettingsDialog::syncRuleTableHeight()
{
    const int rows = m_ruleTable->rowCount();
    // 表头 29 + 每行 28（定稿图实测：表头 + 2 行 = 85px）。
    const int hdrH = 29;
    const int rowH = 28;
    // **必须加上上下边框各 1px**：少算这 2px，内容就比视口高 1px，
    // Qt 判定溢出 ⇒ 表格右侧冒出一条垂直滚动条（定稿图没有）。
    // 实测踩过：公式写成 +1 时规则表右侧一直挂着一条滚动条。
    int want = hdrH + rows * rowH + 2 * m_ruleTable->frameWidth();
    if (want > 260)
        want = 260;
    const int delta = want - m_ruleTable->height();
    if (delta != 0) {
        m_ruleTable->setFixedHeight(want);
        // ★ 这里**不能**再改弹窗高度，也不能再撑滚动区的最小高。
        //
        // 原来这两行（m_scrollArea->setMinimumHeight(+delta) +
        // setFixedHeight(height() + delta)）让「加一条规则」就把弹窗顶高 28px ——
        // 加到第 5、6 条时弹窗 805px，底栏（保存设置）被挤出屏幕，
        // 用户点不到保存（2026-10-28 用户报障：「添加规则后，尺寸都看不到了」）。
        //
        // 现在：弹窗尺寸固定，多出来的内容在中间滚动（ScrollBody + QScrollArea）。
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

// ------------------------------------------------------------------
// 自动化探针（--selfcheck-gui）用的接口。声明在 SettingsDialog.h 的 public 区。
// ------------------------------------------------------------------

int SettingsDialog::ruleRowCountForTest() const
{
    return m_ruleTable ? m_ruleTable->rowCount() : -1;
}

int SettingsDialog::ruleTableHeightForTest() const
{
    return m_ruleTable ? m_ruleTable->height() : -1;
}

bool SettingsDialog::ruleLastRowVisibleForTest() const
{
    if (!m_ruleTable || m_ruleTable->rowCount() <= 0)
        return false;
    const int rows = m_ruleTable->rowCount();
    const QRect last = m_ruleTable->visualItemRect(m_ruleTable->item(rows - 1, 0));
    const QRect vp = m_ruleTable->viewport()->rect();
    // 必须**完整落在可视区里**：只露半行也算看不见。
    return vp.contains(last);
}

void SettingsDialog::clickAddRuleForTest()
{
    if (m_addRule)
        m_addRule->click();
}

void SettingsDialog::clickSaveForTest()
{
    if (m_btnSave)
        m_btnSave->click();
}

void SettingsDialog::removeRuleRowForTest(int row)
{
    removeRuleRow(row);
}

// 第 row 行有没有「删除」按钮。
bool SettingsDialog::ruleRowHasDeleteButtonForTest(int row) const
{
    if (!m_ruleTable || row < 0 || row >= m_ruleTable->rowCount())
        return false;
    QWidget *holder = m_ruleTable->cellWidget(row, kRuleDelCol);
    return holder && holder->findChild<QToolButton *>() != nullptr;
}

// 点第 row 行的「删除」按钮（走按钮自己的 clicked 信号，和用户手点同一条路）。
void SettingsDialog::clickRuleDeleteForTest(int row)
{
    if (!m_ruleTable || row < 0 || row >= m_ruleTable->rowCount())
        return;
    QWidget *holder = m_ruleTable->cellWidget(row, kRuleDelCol);
    if (!holder)
        return;
    QToolButton *btn = holder->findChild<QToolButton *>();
    if (btn)
        btn->click();
}

void SettingsDialog::setRuleTableHeightForTest(int h)
{
    if (m_ruleTable)
        m_ruleTable->setFixedHeight(h);
}

void SettingsDialog::setRuleCellForTest(int row, int col, const QString &text)
{
    if (!m_ruleTable)
        return;
    if (!m_ruleTable->item(row, col))
        m_ruleTable->setItem(row, col, new QTableWidgetItem);
    m_ruleTable->item(row, col)->setText(text);
}

QString SettingsDialog::ruleCellForTest(int row, int col) const
{
    if (!m_ruleTable)
        return QString();
    QTableWidgetItem *it = m_ruleTable->item(row, col);
    return it ? it->text() : QString();
}

void SettingsDialog::setAutoSyncIntervalForTest(const QString &s)
{
    if (m_autoSyncInterval)
        m_autoSyncInterval->setText(s);
}

QString SettingsDialog::autoSyncIntervalForTest() const
{
    return m_autoSyncInterval ? m_autoSyncInterval->text() : QString();
}

void SettingsDialog::setAutoSyncEnabledForTest(bool on)
{
    if (m_autoSyncBox)
        m_autoSyncBox->setChecked(on);
}

bool SettingsDialog::autoSyncEnabledForTest() const
{
    return m_autoSyncBox ? m_autoSyncBox->isChecked() : false;
}

void SettingsDialog::showTestResult(const QString &text, bool ok, bool testing)
{
    if (!m_testLabel)
        return;
    if (testing) {
        m_testLabel->setText(text);
        m_testLabel->setStyleSheet(QStringLiteral("color: #6db0ff; font-weight: 500; font-size: 11px;"));
        m_testLabel->setVisible(true);
        return;
    }
    m_testLabel->setText(text);
    m_testLabel->setStyleSheet(ok ? QStringLiteral("color: #4ade80; font-weight: 600; font-size: 11px;")
                                  : QStringLiteral("color: #fca5a5; font-weight: 600; font-size: 11px;"));
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

