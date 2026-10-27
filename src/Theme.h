#pragma once

#include <QApplication>
#include <QColor>
#include <QFontDatabase>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QString>
#include <QDir>

// 设计令牌（AP500BJ 深色工业风 · 严格对齐 ui_preview.html）
//
// 这一版把整套外观从"明亮浅色"翻成"AP500BJ 深色"：
// 窗口底 #0e1424、卡片底 #151d32、主蓝 #1f6fe0、文字 #f8fafc。
// 直角外框（2px）、发丝级分隔线（1px #24324d）、无大色块、无重阴影。
//
// 只有一套外观：不存在"浅色分支"，客户机的系统主题不会把界面翻回浅色。
// 改配色只改这一个文件。
class Theme
{
public:
    // ---- 主题模式：AP500BJ 定稿只有一套外观（深色工控）----
    //
    // 客户（Jira Tools-60）要求界面必须是 AP500BJ 深色工业风。
    // 客户机的系统主题不可控，所以这里**不再按浅色/深色分两套配色**：
    // ThemeMode 这个枚举保留（公开 API 不变，别处还在引用 DarkMode/LightMode），
    // 但实际生效的模式被钉死在 DarkMode —— 谁调用 buildQss(LightMode)，
    // 拿到的仍然是同一份 AP500BJ 深色样式表。
    //
    // 为什么必须这么钉死：只要还留一条"浅色分支"，
    // 客户在浅色主题的 Windows 上装上软件，看到的就是浅色界面 —— 需求直接没达成。
    // 顺带的后果：标题栏那颗外观按钮变成了空操作（点了不会有任何视觉变化），
    // 这是刻意的取舍，不是 bug。
    enum ThemeMode { DarkMode = 0, LightMode = 1 };

    static ThemeMode& modeRef()
    {
        static ThemeMode s_mode = DarkMode;
        return s_mode;
    }
    // 恒为深色：即使有人直接写 Theme::modeRef() = LightMode，
    // isDark() 也仍然是 true —— 颜色入口只有一条，不给"变回浅色"留缝。
    static bool isDark() { return true; }
    static ThemeMode currentMode() { return DarkMode; }
    static void setMode(ThemeMode) { modeRef() = DarkMode; }
    static void toggleMode() { modeRef() = DarkMode; }

    // ---- AP500BJ 色板（与 ui_preview.html 的 :root 完全一致）----
    //
    // 窗口底 #0e1424 / 卡片底 #151d32 / 次级面板 #101726、#0c1220 / 输入框底 #0a0f1d
    // 边框 #24324d / 悬停边框 #3a4b6e
    // 主蓝 #1f6fe0 / hover #2b7cf0
    // 主文字 #f8fafc / 次文字 #8fa0c0 / 弱文字 #5c6c87
    // 绿 #10b981 / 红 #ef4444 / 红底 #4c161d
    static QColor primary()      { return QColor(0x1f, 0x6f, 0xe0); }  // 主蓝
    static QColor primaryHover() { return QColor(0x2b, 0x7c, 0xf0); }  // 主蓝悬停
    static QColor success()      { return QColor(0x10, 0xb9, 0x81); }  // 绿
    static QColor danger()       { return QColor(0xef, 0x44, 0x44); }  // 红
    static QColor warn()         { return QColor(0xf5, 0xa5, 0x24); }  // 琥珀（已取消用）
    static QColor textMain()     { return QColor(0xf8, 0xfa, 0xfc); }  // 主文字
    static QColor textSub()      { return QColor(0x8f, 0xa0, 0xc0); }  // 次文字
    static QColor textMuted()    { return QColor(0x5c, 0x6c, 0x87); }  // 弱注释
    static QColor border()       { return QColor(0x24, 0x32, 0x4d); }  // 发丝级边框
    static QColor divider()      { return QColor(0x24, 0x32, 0x4d); }  // 分割线
    static QColor canvas()       { return QColor(0x0e, 0x14, 0x24); }  // 窗口底
    static QColor panelBg()      { return QColor(0x15, 0x1d, 0x32); }  // 卡片底

    // ---- 徽章（同步状态）与操作按钮的语义配色 ----
    //
    // 这一组就是 AP500BJ 定稿里状态标签的五档色（ui_preview.html 的 .tag-*）：
    // 底 / 字 / 边 三件套，深色底 + 亮字 + 同色系描边。
    //
    // 为什么单独开一组，而不是让各 delegate 自己调上面的基础色：
    // 状态标签要在深色面板上保持同样的视觉重量，就得是「深底 + 亮字」。
    // 这不是调一下 primary() 能得到的 —— 同一个 primary() 在这套配色里只有一个值。
    //
    // 原来 StatusPillDelegate::pillOf() 与 ToolColumnDelegate::skinOf()
    // 各自把自己的值写死在函数里，既不问当前主题、也没有第二套值，
    // 于是浅色模式下界面整体变白，这两处仍是深底亮字：
    // 一片白里压着几块深灰深蓝的方块（用户 2026-10-16 报的"与环境不符合"）。
    //
    // 现在 AP500BJ 只有一套外观，这一组色是**唯一色源**：
    // 改配色只改 Theme.h，delegate 只负责"这个状态用哪一档"。
    struct Chip { QColor fg, bg, border; };

    // 待同步：底 #1a2438 / 字 #8fa0c0 / 边 #2b3a57
    static Chip chipWait()
    {
        Chip c;
        c.fg = QColor(0x8f, 0xa0, 0xc0); c.bg = QColor(0x1a, 0x24, 0x38); c.border = QColor(0x2b, 0x3a, 0x57);
        return c;
    }
    // 同步中：底 #133465 / 字 #6db0ff / 边 #1f6fe0
    static Chip chipSyncing()
    {
        Chip c;
        c.fg = QColor(0x6d, 0xb0, 0xff); c.bg = QColor(0x13, 0x34, 0x65); c.border = QColor(0x1f, 0x6f, 0xe0);
        return c;
    }
    // 已同步：底 #0b3625 / 字 #4ade80 / 边 #059669
    static Chip chipOk()
    {
        Chip c;
        c.fg = QColor(0x4a, 0xde, 0x80); c.bg = QColor(0x0b, 0x36, 0x25); c.border = QColor(0x05, 0x96, 0x69);
        return c;
    }
    // 失败：底 #5a171d / 字 #fca5a5 / 边 #dc2626
    static Chip chipFailed()
    {
        Chip c;
        c.fg = QColor(0xfc, 0xa5, 0xa5); c.bg = QColor(0x5a, 0x17, 0x1d); c.border = QColor(0xdc, 0x26, 0x26);
        return c;
    }
    // 已跳过：底 #10393b / 字 #6fd6de / 边 #1f6b70
    static Chip chipSkipped()
    {
        Chip c;
        c.fg = QColor(0x6f, 0xd6, 0xde); c.bg = QColor(0x10, 0x39, 0x3b); c.border = QColor(0x1f, 0x6b, 0x70);
        return c;
    }
    // 已取消：琥珀色。必须和 chipSkipped 的青、chipFailed 的红都拉得开 ——
    // 用户取消之后扫一眼颜色就该知道"这些没传"，而不是被青色骗成"远端已最新"。
    static Chip chipCanceled()
    {
        Chip c;
        c.fg = QColor(0xfc, 0xd3, 0x4d); c.bg = QColor(0x3a, 0x2a, 0x12); c.border = QColor(0xb4, 0x53, 0x09);
        return c;
    }

    // 表格行选中底色。原来两个 delegate 都写死深蓝 #1e3a8a，
    // 浅色模式下一选中就是一条深蓝带，和浅色行底完全不搭；
    // AP500BJ 深色下那条深蓝带也太重，改成比行底亮一档的深蓝灰 #22304d。
    static QColor rowSelectedBg()
    {
        return QColor(0x22, 0x30, 0x4d);
    }

    // 失败行整行红底（定稿图 .row-alert，--red-bg #4c161d）。
    // 失败是这一轮最需要被一眼看到的状态，光靠一列里的红标签不够 ——
    // 列表一长，那一列就淹没在其它行里。
    static QColor rowAlertBg()
    {
        return QColor(0x4c, 0x16, 0x1d);
    }

    // 标题栏系统按钮（设置 / 最小化 / 最大化 / 关闭）图标的静态色。
    // AP500BJ 标题栏是深底，图标用蓝灰 #9cb1d4（与定稿图一致）。
    // 关闭键是**红的**（定稿图 .ico-close #ff3b30），和另外两键区分开。
    static QColor chromeIcon()  { return QColor(0x9c, 0xb1, 0xd4); }
    static QColor chromeIconOn(){ return QColor(0x6d, 0xb0, 0xff); }
    static QColor chromeIconClose() { return QColor(0xff, 0x3b, 0x30); }

    // 跨平台字体栈（手册 §10.3）。Windows 与麒麟的可用字族完全不同，
    // 写死单一字族在另一端会掉回 Qt 默认字体，中文立刻变成方框。
    //
    // 两个坑（都实测踩过）：
    //  1) setFamily("A, B") 会把整串当成**一个字族名**，那个名字不存在，
    //     字体引擎报 `FT_New_Face failed with index 0 : 2` 然后掉回一个
    //     不保证有中文字形的兜底字体。
    //  2) Qt 5.9.6 的 QFont **还没有 setFamilies()**（那是 5.13 才有的 API），
    //     所以只能自己从 QFontDatabase 里挑第一个真实存在的字族。
    static QStringList fontStack()
    {
#ifdef Q_OS_WIN
        return QStringList()
            << QStringLiteral("Microsoft YaHei UI")
            << QStringLiteral("Microsoft YaHei")
            << QStringLiteral("Segoe UI")
            << QStringLiteral("SimSun");
#else
        return QStringList()
            << QStringLiteral("Noto Sans CJK SC")
            << QStringLiteral("Source Han Sans SC")
            << QStringLiteral("WenQuanYi Micro Hei")
            << QStringLiteral("Droid Sans Fallback")
            << QStringLiteral("Noto Sans CJK");
#endif
    }

    static QStringList monoStack()
    {
#ifdef Q_OS_WIN
        return QStringList() << QStringLiteral("Consolas") << QStringLiteral("Courier New");
#else
        return QStringList()
            << QStringLiteral("Noto Sans Mono CJK SC")
            << QStringLiteral("DejaVu Sans Mono");
#endif
    }

    static QString pickFamily(const QStringList &wanted)
    {
        // Qt5 里 QFontDatabase::families() 是**非静态**成员，必须先有实例。
        QFontDatabase db;
        const QStringList have = db.families();
        for (int i = 0; i < wanted.size(); ++i) {
            for (int j = 0; j < have.size(); ++j) {
                if (have.at(j).compare(wanted.at(i), Qt::CaseInsensitive) == 0)
                    return have.at(j);   // 返回系统里的原名，别回 wanted 的写法
            }
        }
        // 栈里一个都没有 = 这台机器没有中文字体。返回空串让 Qt 自己兜底，
        // 但这个状态必须能被 dump 看出来，所以不静默塞一个西文字体。
        return QString();
    }

    static QString resolvedFamily()  { return pickFamily(fontStack()); }
    static QString resolvedMono()    { return pickFamily(monoStack()); }

    // 给 QSS 用的写法：QSS 里带空格的字族名必须加引号，否则「Microsoft YaHei」
    // 会被解析成两个字族。
    static QString qssName(const QString &f)
    {
        return f.isEmpty() ? QString() : QStringLiteral("\"%1\"").arg(f);
    }

    static QString fontFamily()  { return qssName(resolvedFamily()); }
    static QString monoFamily()  { return qssName(resolvedMono()); }

    // 下拉箭头图标：QSS 里 QComboBox::down-arrow 用 image: url(...)。
    // 不给图的话 Qt 会画系统原生箭头（浅色箭头压在深色底上会糊成一团），
    // 所以这里现画一张，和 checkIconPath() 同一个套路。
    static QString comboArrowPath()
    {
        static QString path;
        if (!path.isEmpty())
            return path;
        const QString tmp = QDir::tempPath() + QStringLiteral("/srt_combo_arrow.png");
        QPixmap pm(9, 6);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(QColor(0x8f, 0xa0, 0xc0));
        pen.setWidthF(1.4);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(QColor(0x8f, 0xa0, 0xc0));
        QPainterPath tri;
        tri.moveTo(0.8, 0.9);
        tri.lineTo(8.2, 0.9);
        tri.lineTo(4.5, 5.3);
        tri.closeSubpath();
        p.drawPath(tri);
        p.end();
        pm.save(tmp, "PNG");
        path = QDir::fromNativeSeparators(tmp);
        return path;
    }

    static QString checkIconPath()
    {
        static QString path;
        if (!path.isEmpty())
            return path;
        const QString tmp = QDir::tempPath() + QStringLiteral("/srt_check_white.png");
        QPixmap pm(14, 14);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        QPen pen(Qt::white);
        pen.setWidthF(1.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.drawLine(QPointF(2.8, 6.8), QPointF(5.5, 9.8));
        p.drawLine(QPointF(5.5, 9.8), QPointF(11.2, 3.8));
        p.end();
        pm.save(tmp, "PNG");
        path = QDir::fromNativeSeparators(tmp);
        return path;
    }

    static void apply(QApplication *app)
    {
        if (!app)
            return;

        QFont f = app->font();
        const QString fam = resolvedFamily();
        if (!fam.isEmpty())
            f.setFamily(fam);
        f.setPixelSize(12);
        app->setFont(f);

        app->setStyleSheet(buildQss());
    }

    // 一台**没装中文字体**的机器（麒麟精简版就是这个状态）会让 pickFamily 返回空串。
    //
    // 原来直接把空串替换进 `font-family: %FAMILY%;`，QSS 里就留下一条
    // 值为空的声明 `font-family: ;`。这既不是合法 CSS，也没测过 Qt 5.9 的解析器
    // 遇到它会怎么处理（不报错？丢掉整条规则？还是把后面的声明一起吃掉？）。
    // 与其赌一个没验证过的解析行为，不如**根本别写这条声明**：
    // 没有 font-family 就等于"不覆盖"，控件继承 QApplication 的默认字体 ——
    // 这正是找不到中文字体时唯一说得通的行为。
    //
    // 所以占位符换成的不是字族名，而是**整条声明**。
    // 占位符的用法契约：**整条替换**，不能当成属性值拼在别的属性后面。
    //
    // QLineEdit#setting* 那条规则原来写的是
    //     font-size: 12px; font-family: %MONODECL%; color: #0f172a;
    // 展开后是
    //     font-size: 12px; font-family: font-family: "Consolas";; color: #0f172a;
    // 一条非法声明。Windows 上 Qt 把它**静默丢掉**（界面看着完全正常），
    // Linux 上 Qt 直接报 `Could not parse application stylesheet`，
    // 于是**整份样式表被丢弃** —— 麒麟上界面会变成没做过样式的原生控件外观。
    // 也就是说这行错误在两个平台上表现完全不同，只在目标平台上炸。
    //
    // 这里现在**不写任何 font-family**，让输入框继承 QApplication 的字体：
    // 这正是 Windows 一直以来实际渲染出来的结果（实测 Microsoft YaHei UI），
    // 也就是说这一改不改变已定稿界面的任何一个像素。
    static QString fontDecl()
    {
        const QString f = fontFamily();
        return f.isEmpty() ? QStringLiteral("/* no-CJK-font */")
                          : QStringLiteral("font-family: %1;").arg(f);
    }

    static QString monoDecl()
    {
        const QString f = monoFamily();
        return f.isEmpty() ? QStringLiteral("/* no-mono-font */")
                          : QStringLiteral("font-family: %1;").arg(f);
    }

    static QString buildDarkQss()
    {
        return QStringLiteral(R"QSS(
QWidget {
    color: #f8fafc;
    %FAMILYDECL%
    font-size: 12px;
}

/* ---- AP500BJ 深色工控窗体：直角外框（2px）、发丝级分隔线（1px #24324d） ---- */
QWidget#rootFrame {
    background: #0e1424;
    border: 1px solid #24324d;
    border-radius: 2px;
}
QFrame#splashFrame {
    background: #151d32;
    border: 1px solid #24324d;
    border-radius: 2px;
}

QWidget#pageBg {
    background: #0e1424;
    border: none;
}

/* ---- 标题栏 ---- */
QFrame#titleBar {
    background: #090d17;
    border: none;
    border-bottom: 1px solid #24324d;
}
QFrame#titleBarLine { background: #24324d; border: none; }
QLabel#appTitle { font-size: 14px; font-weight: 700; color: #ffffff; }

/* 标题栏右侧的系统按钮：最小化 / 最大化 / 关闭。
   悬停只给一层极淡的浅底（定稿图 .win-ctrl:hover rgba(255,255,255,.08)），
   原来这里给关闭键加了整块红底，和定稿图对不上 —— 关闭键的红是**叉本身**的颜色。 */
QToolButton#sysBtn, QToolButton#sysBtnClose {
    background: transparent; border: 1px solid transparent;
    border-radius: 2px; color: #9cb1d4;
}
QToolButton#sysBtn:hover, QToolButton#sysBtnClose:hover {
    background: #1a2438; color: #ffffff; border-color: transparent;
}

/* 标题栏右侧的「设置」文字按钮（定稿图 .btn-settings-head）。
   原来这里放的是一颗齿轮图标，和定稿图的文字按钮对不上。 */
QPushButton#settingsHeadBtn {
    background: #162035; border: 1px solid #24324d;
    border-radius: 2px; color: #9cb1d4;
    padding: 3px 12px; font-size: 12px;
}
QPushButton#settingsHeadBtn:hover { background: #1f6fe0; color: #ffffff; border-color: #1f6fe0; }

/* ---- 文件清单卡片 ---- */
QFrame#listCard {
    background: #151d32; border: 1px solid #24324d; border-radius: 2px;
}
QFrame#cardHeader { background: #151d32; border: none; }
QFrame#cardHeaderLine { background: #24324d; border: none; }

QPushButton#fileTab {
    background: #192237; border: 1px solid #24324d;
    border-radius: 2px; padding: 4px 12px;
    font-size: 12px; font-weight: 600; color: #8fa0c0;
}
QPushButton#fileTab:hover   { background: #23314f; color: #ffffff; }
QPushButton#fileTab:checked { background: #1f6fe0; border-color: #1f6fe0;
                               color: #ffffff; font-weight: 600; }

QPushButton#refreshScan {
    background: #1a253c; border: 1px solid #24324d; border-radius: 2px;
    padding: 4px 12px; font-size: 12px; font-weight: 500; color: #b8cced;
}
QPushButton#refreshScan:hover    { background: #23314f; color: #ffffff; }
QPushButton#refreshScan:disabled { color: #5c6c87; border-color: #24324d; background: #101726; }

/* ---- 表格：工控深色数据网格 ---- */
QTableView {
    background-color: #151d32;
    color: #e2e8f0;
    border: none;
    gridline-color: #1b2540;
    selection-background-color: #22304d;
    selection-color: #ffffff;
    outline: none;
}
QTableView::item { border: none; padding: 0px; }
QHeaderView { background: #111726; border: none; }
QHeaderView::section {
    background-color: #111726;
    color: #9cb1d4;
    font-weight: 600;
    font-size: 11px;
    padding: 7px 8px;
    border: none;
    border-bottom: 1px solid #24324d;
    border-right: 1px solid #1b2540;
}
QTableCornerButton::section { background: #111726; border: none; }

/* ---- 进度区 ---- */
QFrame#progressCard {
    background: #12192a; border: 1px solid #24324d; border-radius: 2px;
}
QLabel#curStatus  { font-size: 12px; font-weight: 600; color: #f8fafc; }
QLabel#curStatusDone { font-size: 12px; font-weight: 600; color: #10b981; }
QLabel#curStatusCancel { font-size: 12px; font-weight: 600; color: #ef4444; }
QLabel#speedCount {
    font-size: 11.5px; color: #8fa0c0; %MONODECL%
}

/* 唯一一根物理进度条：科技双色电光渐变。
   进度条刻意做细 —— 客户明确要求「不要太明显太粗」。
   8px 在深色底上是一根抢眼的横杠，把注意力从文件清单上拉走；
   4px 既看得出进度位置，又不喧宾夺主。圆角同样收到 2px。 */
QProgressBar {
    background: #0a0e1a; border: 1px solid #192336; border-radius: 2px;
    height: 4px; min-height: 4px; max-height: 4px;
    text-align: center; color: transparent;
}
QProgressBar::chunk {
    border-radius: 2px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #1f6fe0, stop:1 #10b981);
}

/* ---- 状态徽章（AP500BJ 直角标签） ---- */
QLabel#statusWait {
    background-color: #1a2438; color: #8fa0c0;
    border: 1px solid #2b3a57; border-radius: 2px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
}
QLabel#statusSyncing {
    background-color: #133465; color: #6db0ff;
    border: 1px solid #1f6fe0; border-radius: 2px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
    %MONODECL%
}
QLabel#statusOk {
    background-color: #0b3625; color: #4ade80;
    border: 1px solid #059669; border-radius: 2px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
}

/* ---- 底部操作条与表格复选框 ---- */
QCheckBox { color: #c4d3eb; spacing: 6px; font-size: 12.5px; }
QCheckBox::indicator, QTableView::indicator {
    width: 14px; height: 14px; border: 1px solid #3a4b6e;
    border-radius: 2px; background: #0a0f1d;
}
QCheckBox::indicator:hover, QTableView::indicator:hover  { border-color: #1f6fe0; }
QCheckBox::indicator:checked, QTableView::indicator:checked {
    background: #1f6fe0; border: 1px solid #2b7cf0;
    image: url("%CHECK_ICON%");
}

QRadioButton { color: #c4d3eb; spacing: 6px; font-size: 12px; }
QRadioButton::indicator {
    width: 14px; height: 14px; border: 1px solid #3a4b6e;
    border-radius: 7px; background: #0a0f1d;
}
QRadioButton::indicator:hover { border-color: #1f6fe0; }
QRadioButton::indicator:checked { background: #1f6fe0; border: 1px solid #2b7cf0; }

QPushButton#btnStartSync {
    background: #1f6fe0; color: #ffffff; border: none;
    border-radius: 2px; padding: 7px 24px;
    font-size: 13px; font-weight: 700;
}
QPushButton#btnStartSync:hover    { background: #2b7cf0; }
QPushButton#btnStartSync:disabled { background: #1f2c49; color: #5c6c87; }
QPushButton#btnStartSyncCancel {
    background: #b43a3a; color: #ffffff; border: none;
    border-radius: 2px; padding: 7px 24px;
    font-size: 13px; font-weight: 700;
}
QPushButton#btnStartSyncCancel:hover { background: #c94242; }

QPushButton#btnToggleLog {
    background: #162036; color: #9cb1d4;
    border: 1px solid #24324d; border-radius: 2px;
    padding: 6px 12px; font-size: 12px; font-weight: 600;
}
QPushButton#btnToggleLog:hover {
    background: #1f2c49; color: #ffffff; border-color: #3a4b6e;
}

/* 底栏中间的控制带（定稿图 .bottom-center-ctrls）：
   「仅同步新增文件 | 带宽限制」收在一个带边框的窄盒子里，
   竖线是 .split-line（1px × 14px 发丝线）。 */
QFrame#bottomCtrlBand {
    background: #111727; border: 1px solid #24324d; border-radius: 2px;
}
QFrame#splitLine { background: #24324d; border: none; }

/* ---- 日志抽屉：深色工业终端 ---- */
QWidget#logDrawer {
    background: #0c1220; border: 1px solid #24324d; border-radius: 2px;
}
QFrame#logHeader { background: #101726; border: none; }
QFrame#logHeaderLine { background: #24324d; border: none; }
QLabel#logHeaderText { font-size: 12px; font-weight: 600; color: #8fa0c0; }
QPushButton#logAction {
    background: transparent; border: none; color: #5c6c87;
    font-size: 11px; padding: 2px 4px;
}
QPushButton#logAction:hover { color: #6db0ff; }
QPlainTextEdit#logView, QTextEdit#logView {
    background: #0c1220; color: #dbe4f3; border: none;
    padding: 4px 10px;
    %MONODECL% font-size: 12px;
    selection-background-color: #1f6fe0; selection-color: #ffffff;
}

/* ---- 设置弹窗（无原生白顶栏，直角卡片） ---- */
QDialog#settingsDialog {
    background: transparent;
}
QFrame#settingsCard {
    background: #151d32;
    border: 1px solid #24324d;
    border-radius: 2px;
}
QFrame#dialogHeader {
    background: #090d17;
    border-bottom: 1px solid #24324d;
}
QLabel#dialogTitleText {
    font-size: 13.5px;
    font-weight: 700;
    color: #f8fafc;
}
QToolButton#dialogCloseBtn, QPushButton#dialogCloseBtn {
    background: transparent;
    border: none;
    border-radius: 2px;
    color: #8fa0c0;
    font-size: 14px;
}
QToolButton#dialogCloseBtn:hover, QPushButton#dialogCloseBtn:hover {
    background: #ef4444;
    color: #ffffff;
}
QToolButton#dialogCloseBtn:pressed, QPushButton#dialogCloseBtn:pressed {
    background: #dc2626;
    color: #ffffff;
}
)QSS"
                    R"QSS(
QFrame#dialogFooter {
    background: #090d17;
    border-top: 1px solid #24324d;
}
QLabel#settingLabel { font-size: 11.5px; color: #9eb2d1; font-weight: 500; }
QLabel#settingHint  { font-size: 11px; color: #5c6c87; }

/* ---- 设置弹窗的分组结构（定稿图 .grp-box / .grp-title / .switch-board）---- */
QFrame#settingsGroup {
    background: #151d32; border: 1px solid #24324d; border-radius: 2px;
}
QLabel#groupTitle {
    font-size: 12px; font-weight: 700; color: #c4d3eb;
}
QFrame#switchBoard {
    background: #0c1220; border: 1px solid #24324d; border-radius: 2px;
}
/* 同步方式二选一：连在一起的分段按钮，选中的蓝底白字。 */
QPushButton#chkAutoSyncModeInterval, QPushButton#chkAutoSyncModeDaily {
    background: #162035; color: #9cb1d4;
    border: 1px solid #24324d; border-radius: 2px;
    padding: 5px 14px; font-size: 12px; font-weight: 600;
}
QPushButton#chkAutoSyncModeInterval:hover, QPushButton#chkAutoSyncModeDaily:hover {
    background: #23314f; color: #ffffff;
}
QPushButton#chkAutoSyncModeInterval:checked, QPushButton#chkAutoSyncModeDaily:checked {
    background: #1f6fe0; color: #ffffff; border-color: #1f6fe0;
}
QLineEdit#settingInput,
QLineEdit#settingSourceDir, QLineEdit#settingHost, QLineEdit#settingUser,
QLineEdit#settingPassword, QLineEdit#settingKeyPath, QLineEdit#settingRemoteRoot,
QLineEdit#settingPort, QLineEdit#settingAutoSyncInterval, QTimeEdit#settingAutoSyncDailyTime, QComboBox#settingAuthMode {
    background: #0a0f1d; border: 1px solid #283750; border-radius: 2px;
    padding: 0 8px; min-height: 28px; max-height: 28px;
    font-size: 12px; color: #ffffff;
    selection-background-color: #1f6fe0; selection-color: #ffffff;
}
QLineEdit#settingInput:focus, QLineEdit#settingSourceDir:focus,
QLineEdit#settingHost:focus, QLineEdit#settingUser:focus,
QLineEdit#settingPassword:focus, QLineEdit#settingKeyPath:focus,
QLineEdit#settingRemoteRoot:focus, QLineEdit#settingPort:focus,
QLineEdit#settingAutoSyncInterval:focus, QTimeEdit#settingAutoSyncDailyTime:focus,
QLineEdit:focus, QComboBox#settingAuthMode:focus {
    border: 1px solid #1f6fe0; background: #0a0f1d;
}
QLineEdit#settingInput:disabled, QLineEdit#settingSourceDir:disabled,
QLineEdit#settingHost:disabled, QLineEdit#settingUser:disabled,
QLineEdit#settingPassword:disabled, QLineEdit#settingKeyPath:disabled,
QLineEdit#settingRemoteRoot:disabled, QLineEdit#settingPort:disabled,
QLineEdit#settingAutoSyncInterval:disabled, QTimeEdit#settingAutoSyncDailyTime:disabled,
QComboBox#settingAuthMode:disabled { background: #0c1220; color: #5c6c87; border-color: #1b2540; }

QPushButton#btnBrowse, QPushButton#btnBrowseKey {
    background: #1a253c; color: #b8cced;
    border: 1px solid #24324d; border-radius: 2px;
    padding: 0 10px; min-height: 28px; max-height: 28px;
    font-size: 12px; font-weight: 500; white-space: nowrap;
}
QPushButton#btnBrowse:hover, QPushButton#btnBrowseKey:hover { background: #23314f; color: #ffffff; border-color: #3a4b6e; }
QPushButton#btnBrowse:disabled, QPushButton#btnBrowseKey:disabled {
    background: #101726; color: #5c6c87; border-color: #1b2540;
}
QPushButton#btnAddRule {
    background: #1a253c; color: #b8cced;
    border: 1px solid #24324d; border-radius: 2px;
    padding: 4px 10px; font-size: 12px;
}
QPushButton#btnAddRule:hover { background: #23314f; color: #ffffff; }
QPushButton#btnTestConn {
    background: transparent; color: #6db0ff;
    border: 1px solid #1f6fe0; border-radius: 2px;
    padding: 6px 14px; font-size: 12px;
}
QPushButton#btnTestConn:hover    { background: #1f6fe0; color: #ffffff; }
QPushButton#btnTestConn:disabled { color: #5c6c87; border-color: #24324d; }
QPushButton#btnSave {
    background: #1f6fe0; color: #ffffff; border: none;
    border-radius: 2px; padding: 6px 20px; font-size: 12.5px; font-weight: 600;
}
QPushButton#btnSave:hover { background: #2b7cf0; }
QLabel#testLabel  { font-size: 11.5px; font-weight: 500; color: #8fa0c0; }
QLabel#testLabelOk   { font-size: 11.5px; font-weight: 500; color: #4ade80; }
QLabel#testLabelFail { font-size: 11.5px; font-weight: 500; color: #fca5a5; }

/* ---- 规则表（设置弹窗内）---- */
QTableView#ruleTable {
    background: #151d32; border: 1px solid #24324d; gridline-color: #1b2540; outline: none; color: #e2e8f0;
}
QHeaderView#ruleHeader::section {
    background-color: #111726; color: #9cb1d4;
    font-weight: 600; font-size: 11px; padding: 6px 10px;
    border: none; border-bottom: 1px solid #24324d;
}
QPushButton#ruleRemove {
    background: transparent; border: 1px solid transparent;
    border-radius: 2px; color: #ef4444; font-size: 13px;
}
QPushButton#ruleRemove:hover { background: #5a171d; color: #fca5a5; }

/* ---- 系统弹窗与原生对话框 ----
   坑（2026-10-24 用户截图）：全局 `QWidget { color: #f8fafc }` 会把
   QMessageBox 里的文字也刷成近白，而 QMessageBox 自己没有被样式表接管，
   底色走的是系统默认浅色 —— 结果「白底白字」，提示内容完全看不见，
   按钮上的字同样看不见。所以这里必须显式把弹窗接管过来。

   同一个坑也影响 QFileDialog（设置里的「浏览…」）、QInputDialog：
   白底列表框配全局浅色字同样读不了，下面一并接管。
   AP500BJ 版里这些弹窗全部是深底浅字，不再出现浅色系统弹窗。 */
QMessageBox {
    background-color: #151d32;
    color: #f8fafc;
}
QMessageBox QLabel {
    background: transparent;
    color: #f8fafc;
}
QMessageBox QPushButton {
    background: #162036; color: #9cb1d4;
    border: 1px solid #24324d; border-radius: 2px;
    padding: 5px 18px; min-width: 76px; min-height: 26px;
    font-size: 12px;
}
QMessageBox QPushButton:hover   { background: #1f2c49; border-color: #3a4b6e; color: #ffffff; }
QMessageBox QPushButton:pressed { background: #23314f; color: #ffffff; }
QMessageBox QPushButton:default { border-color: #1f6fe0; color: #ffffff; }

QDialog {
    background-color: #0e1424;
    color: #f8fafc;
}
QDialog QLabel { color: #f8fafc; }
QDialog QTreeView, QDialog QListView, QDialog QTableView {
    background-color: #151d32; color: #f8fafc;
    border: 1px solid #24324d;
    selection-background-color: #22304d; selection-color: #ffffff;
}
QDialog QHeaderView::section {
    background-color: #111726; color: #9cb1d4;
    border: none; border-bottom: 1px solid #24324d;
}
QDialog QPushButton {
    background: #162036; color: #9cb1d4;
    border: 1px solid #24324d; border-radius: 2px;
    padding: 5px 14px;
}
QDialog QPushButton:hover { background: #1f2c49; border-color: #3a4b6e; color: #ffffff; }

QLineEdit {
    background: #0a0f1d; color: #f8fafc;
    border: 1px solid #283750; border-radius: 2px;
    padding: 3px 7px;
}
QComboBox {
    background: #0a0f1d; color: #f8fafc;
    border: 1px solid #283750; border-radius: 2px;
    padding: 3px 7px;
}
QComboBox::drop-down {
    subcontrol-origin: padding; subcontrol-position: top right;
    width: 18px; border: none; background: transparent;
}
QComboBox::down-arrow { image: url("%COMBO_ARROW%"); width: 9px; height: 6px; }
QComboBox QAbstractItemView {
    background-color: #151d32; color: #f8fafc;
    border: 1px solid #24324d;
    selection-background-color: #1f6fe0; selection-color: #ffffff;
}
QComboBox QAbstractItemView::item { color: #e2e8f0; padding: 4px 8px; }
QComboBox QAbstractItemView::item:selected { background: #1f6fe0; color: #ffffff; }

/* 带宽限制选择器：外观和下拉框完全一致，只是它是按钮 + 菜单
   （选项超过 3 个时要折叠成「两项 + 更多…」，QComboBox 做不到）。
   箭头复用同一张 %COMBO_ARROW%，不然两个控件的下拉感会不一样。 */
QToolButton#cmbBandwidthLimit {
    background: #0a0f1d; color: #f8fafc;
    border: 1px solid #283750; border-radius: 2px;
    padding: 0 20px 0 8px; font-size: 12px; text-align: left;
}
QToolButton#cmbBandwidthLimit:hover { border-color: #3a4b6e; background: #0d1424; }
QToolButton#cmbBandwidthLimit::menu-indicator {
    image: url("%COMBO_ARROW%"); width: 9px; height: 6px;
    subcontrol-position: center right; right: 6px;
}

QAbstractSpinBox {
    background: #0a0f1d; color: #f8fafc;
    border: 1px solid #283750; border-radius: 2px;
    padding: 0 6px; min-height: 28px; max-height: 28px;
    selection-background-color: #1f6fe0; selection-color: #ffffff;
}
QAbstractSpinBox:focus { border: 1px solid #1f6fe0; }
QAbstractSpinBox::up-button, QAbstractSpinBox::down-button { width: 0; border: none; }

/* ---- 旧版多页外壳（ToolPanel：当前不显示，但保留一套深色，避免误开后是浅色） ---- */
QFrame#card { background: #151d32; border: 1px solid #24324d; border-radius: 2px; }
QLabel#cardTitle { font-size: 12px; font-weight: 700; color: #8fa6cb; }
QLabel#pageTitle { font-size: 14px; font-weight: 700; color: #f8fafc; }
QLabel#hint { font-size: 11px; color: #5c6c87; }
QWidget#pageScroll { background: transparent; border: none; }
QPushButton#primaryAction {
    background: #1f6fe0; color: #ffffff; border: none; border-radius: 2px;
    padding: 6px 16px; font-size: 12px; font-weight: 600;
}
QPushButton#primaryAction:hover { background: #2b7cf0; }
QPushButton#dangerAction {
    background: #b43a3a; color: #ffffff; border: none; border-radius: 2px;
    padding: 6px 16px; font-size: 12px; font-weight: 600;
}
QPushButton#dangerAction:hover { background: #c94242; }

/* ---- 通用 ---- */
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QMenu {
    background: #151d32; border: 1px solid #24324d; border-radius: 2px;
    padding: 4px; color: #f8fafc;
}
QMenu::item { padding: 5px 12px; border-radius: 2px; font-size: 11.5px; color: #e2e8f0; }
QMenu::item:selected { background: #1f6fe0; color: #ffffff; }
QMenu::separator { height: 1px; background: #24324d; margin: 3px 6px; }

QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #24324d; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #3a4b6e; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 2px; }
QScrollBar::handle:horizontal { background: #24324d; border-radius: 4px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: #3a4b6e; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QToolTip {
    background: #0c1220; color: #ffffff; border: 1px solid #24324d;
    padding: 5px 9px; font-size: 11px;
}
)QSS")
            .replace(QStringLiteral("%FAMILYDECL%"), fontDecl())
            .replace(QStringLiteral("%MONODECL%"), monoDecl())
            .replace(QStringLiteral("%CHECK_ICON%"), checkIconPath())
            .replace(QStringLiteral("%COMBO_ARROW%"), comboArrowPath());
    }

    // AP500BJ 定稿只有一套外观：这里的"浅色分支"**不再是一套浅色样式表**，
    // 而是直接返回同一份深色样式表。
    //
    // 为什么留着这个函数：它是公开 API，别处（以及历史探针）还可能调用
    // Theme::buildLightQss()。留着并让它返回深色，
    // 就不存在"某条路径调了浅色分支 ⇒ 客户机器上界面变白"的情况。
    static QString buildLightQss()
    {
        return buildDarkQss();
    }

    // AP500BJ：无论调用方传什么模式，返回的都是同一份 AP500BJ 深色样式表。
    //
    // 客户（Jira Tools-60）要求界面必须是深色工业风，而客户机的系统主题不可控，
    // 所以这里**不按系统主题分叉** —— 这是"客户机器是浅色主题 ⇒ 界面变回浅色"
    // 唯一可能被堵死的地方。
    static QString buildQss(ThemeMode mode = currentMode())
    {
        Q_UNUSED(mode);
        return buildDarkQss();
    }

    // Logo：QPainter 现画，不引 .qrc / 不引 QtSvg（Qt 5.9 没有 QtSvg 模块）。
    // 图形含义：波形（录制的原始数据）流向播放三角（回放）。
    static QPixmap logo(int size)
    {
        static QPixmap cache;
        static int cached = 0;
        if (!cache.isNull() && cached == size)
            return cache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 32.0, size / 32.0);

        p.setPen(Qt::NoPen);
        // AP500BJ 主蓝 #1f6fe0。旧值 #2563eb 是上一版"明亮浅色"方案的主色，
        // 和定稿的 #1f6fe0 不是同一个蓝，放到顶栏小图标上会显得发飘。
        // 外框圆角从 6 收到 4：AP500BJ 的语言是直角外框。
        p.setBrush(QColor(0x1f, 0x6f, 0xe0));
        p.drawRoundedRect(QRectF(0, 0, 32, 32), 4, 4);

        QPainterPath wave;
        wave.moveTo(6.5, 20.5);
        wave.cubicTo(8.6, 20.5, 8.6, 11.5, 10.7, 11.5);
        wave.cubicTo(12.8, 11.5, 12.8, 20.5, 14.9, 20.5);
        // 必须写成 QPen pen((QColor(Qt::white)))，不能写 QPen pen(QColor(Qt::white))：
        // 后者会被 C++ 解析成"声明一个名叫 pen 的函数，它的参数叫 Qt::white、类型是 QColor"
        // （most vexing parse），MSVC 报 C2751 / C2228 / C2664。
        QPen pen((QColor(Qt::white)));
        pen.setWidthF(2.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawPath(wave);

        QPainterPath tri;
        tri.moveTo(19.0, 11.2);
        tri.lineTo(26.2, 16.0);
        tri.lineTo(19.0, 20.8);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(Qt::white));
        p.drawPath(tri);
        p.end();

        cache = pm;
        cached = size;
        return cache;
    }

    // ---- 线性小图标（原型里都是 24x24 viewBox 的 stroke SVG）----
    //
    // 为什么现画不用字符：原型用的是 SVG，替身字符（☀ ⚙ ↻）依赖具体字体有没有
    // 这个字形，Windows 上有、麒麟上大概率没有 —— 实测设置钮就渲染成了一个太阳。
    // 而且就算有，字形粗细也对不上 2px stroke。统一用 QPainter 画，两端一致。
    //
    // 统一约定：24x24 设计坐标，stroke 宽 2，圆头圆角，与原型 SVG 同参数。

    static QPixmap gearIcon(int size, const QColor &c)
    {
        static QPixmap cache;
        static QColor cached;
        static int cachedSize = 0;
        if (!cache.isNull() && cachedSize == size && cached == c)
            return cache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 24.0, size / 24.0);

        // 这里原来**根本没有 setPen**。QPainter 新建时持有的是默认画笔
        // （Qt::black、1px 实线），于是 c 这个参数从头到尾没被用过，
        // 齿轮不管在深色还是浅色标题栏上都画成一根几乎看不清的黑细线 ——
        // 深色栏上像一团糊掉的污点（用户 2026-10-16 报的"太黑了"）。
        // 传色的一方以为改好了颜色，实际生效的从来是 Qt 的默认黑。
        // 参数宽度/端点/连接与同文件其它图标保持同一套（2px / 圆头 / 圆角接）。
        QPen pen(c);
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        // 图二 Feather 标准圆润 8 瓣设置齿轮
        const double pi = 3.14159265358979323846;
        const int lobes = 8;
        const double rBase = 7.6;
        const double rAmp = 1.9;
        QPainterPath path;
        const int steps = 64;
        for (int i = 0; i <= steps; ++i) {
            double angle = i * (2.0 * pi / steps);
            double r = rBase + rAmp * std::cos(lobes * angle);
            double x = 12.0 + r * std::cos(angle);
            double y = 12.0 + r * std::sin(angle);
            if (i == 0) path.moveTo(x, y);
            else path.lineTo(x, y);
        }
        path.closeSubpath();
        p.drawPath(path);
        // 中心圆孔
        p.drawEllipse(QPointF(12.0, 12.0), 3.0, 3.0);
        p.end();

        cache = pm; cached = c; cachedSize = size;
        return cache;
    }

    static QPixmap themeToggleIcon(int size, const QColor &c, bool dark)
    {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 24.0, size / 24.0);

        QPen pen(c);
        pen.setWidthF(1.8);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        if (dark) {
            // 图二标准 Feather 线性弯月（一模一样像素级对齐）：
            // M21 12.79A9 9 0 1 1 11.21 3 7 7 0 0 0 21 12.79z
            QPainterPath moon;
            moon.moveTo(20.3, 13.0);
            moon.cubicTo(19.8, 17.2, 16.3, 20.5, 12.0, 20.5);
            moon.cubicTo(7.3, 20.5, 3.5, 16.7, 3.5, 12.0);
            moon.cubicTo(3.5, 7.3, 7.3, 3.7, 11.2, 3.7);
            moon.cubicTo(11.2, 7.5, 13.0, 10.5, 15.5, 12.2);
            moon.cubicTo(17.2, 13.2, 19.0, 13.3, 20.3, 13.0);
            moon.closeSubpath();
            p.drawPath(moon);
        } else {
            // 当前明亮：清爽太阳
            p.drawEllipse(QPointF(12, 12), 4.5, 4.5);
            for (int i = 0; i < 8; ++i) {
                p.save();
                p.translate(12, 12);
                p.rotate(i * 45.0);
                p.drawLine(QPointF(0, -7.5), QPointF(0, -10.0));
                p.restore();
            }
        }
        p.end();
        return pm;
    }

    static QPixmap closeIcon(int size, const QColor &c)
    {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 16.0, size / 16.0);

        QPen pen(c);
        pen.setWidthF(1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(3.8, 3.8), QPointF(12.2, 12.2));
        p.drawLine(QPointF(12.2, 3.8), QPointF(3.8, 12.2));
        p.end();
        return pm;
    }

    static QPixmap minIcon(int size, const QColor &c)
    {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 16.0, size / 16.0);

        QPen pen(c);
        pen.setWidthF(1.8);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(2.5, 8.0), QPointF(13.5, 8.0));
        p.end();
        return pm;
    }

    // 最大化：方框，上边框粗一档（定稿图 .ico-max-box）。
    static QPixmap maxIcon(int size, const QColor &c)
    {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 16.0, size / 16.0);

        QPen pen(c);
        pen.setWidthF(1.4);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(3.0, 3.0, 10.0, 10.0));
        p.setPen(QPen(c, 2.4));
        p.drawLine(QPointF(3.0, 3.0), QPointF(13.0, 3.0));
        p.end();
        return pm;
    }

    // 还原：两个错开的方框（定稿图 .ico-restore）。
    // 后层方框被前层盖住的那一截，用**标题栏底色**擦掉 ——
    // 否则两层框线交叠，画出来是一团看不清的网格。
    static QPixmap restoreIcon(int size, const QColor &c)
    {
        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 16.0, size / 16.0);

        p.setPen(QPen(c, 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(1.5, 5.0, 8.0, 8.0));         // 后层
        p.fillRect(QRectF(5.5, 1.5, 9.0, 9.0), QColor(0x09, 0x0d, 0x17));
        p.setPen(QPen(c, 1.4));
        p.drawRect(QRectF(5.5, 1.5, 9.0, 9.0));         // 前层
        p.setPen(QPen(c, 2.4));
        p.drawLine(QPointF(5.5, 1.5), QPointF(14.5, 1.5));
        p.end();
        return pm;
    }

    static QPixmap refreshIcon(int size, const QColor &c)
    {
        static QPixmap cache;
        static QColor cached;
        static int cachedSize = 0;
        if (!cache.isNull() && cachedSize == size && cached == c)
            return cache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 24.0, size / 24.0);

        QPen pen(c);
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        // 现代轻盈顺时针圆环刷新：圆心(12,12)，半径 7.5
        // 扫 290 度，不重叠不拥挤
        QPainterPath arc;
        arc.arcMoveTo(QRectF(4.5, 4.5, 15.0, 15.0), 320);
        arc.arcTo(QRectF(4.5, 4.5, 15.0, 15.0), 320, -280);
        p.drawPath(arc);

        // 箭头顺时针切线朝下
        QPainterPath arrow;
        arrow.moveTo(14.5, 3.5);
        arrow.lineTo(19.2, 5.8);
        arrow.lineTo(17.2, 10.5);
        p.drawPath(arrow);

        p.end();

        cache = pm; cached = c; cachedSize = size;
        return cache;
    }

    static QPixmap docIcon(int size, const QColor &c)
    {
        static QPixmap cache;
        static QColor cached;
        static int cachedSize = 0;
        if (!cache.isNull() && cachedSize == size && cached == c)
            return cache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.scale(size / 24.0, size / 24.0);

        QPen pen(c);
        pen.setWidthF(2.0);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        // 纸张外形 + 右上折角
        QPainterPath page;
        page.moveTo(14, 2);
        page.lineTo(6, 2);
        page.cubicTo(4.9, 2, 4, 2.9, 4, 4);
        page.lineTo(4, 20);
        page.cubicTo(4, 21.1, 4.9, 22, 6, 22);
        page.lineTo(18, 22);
        page.cubicTo(19.1, 22, 20, 21.1, 20, 20);
        page.lineTo(20, 8);
        page.closeSubpath();
        p.drawPath(page);
        p.drawPolyline(QPolygonF() << QPointF(14, 2) << QPointF(14, 8) << QPointF(20, 8));
        p.drawLine(QPointF(16, 13), QPointF(8, 13));
        p.drawLine(QPointF(16, 17), QPointF(8, 17));
        p.end();

        cache = pm; cached = c; cachedSize = size;
        return cache;
    }

    // 导航图标：传输 = 两条反向箭头；回放 = 播放三角。
    // 定稿 UI 已经没有左侧导航（工具调用并进了表格操作列），但旧测试探针
    // 仍可能引用它，保留以免连带改一堆探针；新界面不调用。
    static QPixmap navIcon(int size, bool playback)
    {
        static QPixmap syncCache;
        static QPixmap toolCache;
        static int syncCached = 0;
        static int toolCached = 0;
        if (playback && !toolCache.isNull() && toolCached == size)
            return toolCache;
        if (!playback && !syncCache.isNull() && syncCached == size)
            return syncCache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = size / 16.0;
        p.scale(s, s);

        QPen pen(QColor(playback ? Theme::textMuted() : Theme::primary()));
        pen.setWidthF(1.4);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);

        if (playback) {
            QPainterPath tri;
            tri.moveTo(5.5, 3.2);
            tri.lineTo(12.2, 8.0);
            tri.lineTo(5.5, 12.8);
            tri.closeSubpath();
            p.setBrush(pen.color());
            p.drawPath(tri);
        } else {
            p.drawLine(QPointF(2.5, 5.6), QPointF(11.5, 5.6));
            p.drawLine(QPointF(9.0, 3.1), QPointF(11.6, 5.6));
            p.drawLine(QPointF(9.0, 8.1), QPointF(11.6, 5.6));
            p.drawLine(QPointF(13.5, 10.6), QPointF(4.5, 10.6));
            p.drawLine(QPointF(7.0, 8.1), QPointF(4.4, 10.6));
            p.drawLine(QPointF(7.0, 13.1), QPointF(4.4, 10.6));
        }
        p.end();

        if (playback) {
            toolCached = size;
            toolCache = pm;
            return pm;
        }
        syncCached = size;
        syncCache = pm;
        return pm;
    }

    // 启动页形态 1：蓝底 AP 字母徽标（原型 splash 的第一形态）。
    // 与 logo() 分开缓存：两者的圆角不同（AP500BJ 版 64px 上是 rx=8），
    // 共用一个缓存会在同尺寸请求下拿到上一种形状。
    static QPixmap splashAp(int size)
    {
        static QPixmap cache;
        static int cached = 0;
        if (!cache.isNull() && cached == size)
            return cache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = size / 64.0;
        p.scale(s, s);

        QLinearGradient g(0, 0, 64, 64);
        g.setColorAt(0.0, QColor(0x2b, 0x7c, 0xf0));
        g.setColorAt(1.0, QColor(0x1f, 0x6f, 0xe0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(0, 0, 64, 64), 8, 8);

        QFont f = QApplication::font();
        f.setPixelSize(static_cast<int>(25 * s));
        f.setWeight(QFont::Black);
        p.setFont(f);
        p.setPen(QPen(QColor(Qt::white)));
        p.drawText(QRectF(0, 0, 64, 64), Qt::AlignCenter, QStringLiteral("AP"));
        p.end();

        cache = pm;
        cached = size;
        return cache;
    }

    // 启动页形态 2：波形 + 回放三角（原型 logo-state-sync）。
    // 与 splashAp 同一个渐变、同一个 rx=8，两个形态之间只换中间的字/图形，
    // 外框完全不变 —— 这样演变才像"同一个徽标在变"，而不是两张图在切。
    static QPixmap splashSync(int size)
    {
        static QPixmap cache;
        static int cached = 0;
        if (!cache.isNull() && cached == size)
            return cache;

        QPixmap pm(size, size);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        const qreal s = size / 64.0;
        p.scale(s, s);

        QLinearGradient g(0, 0, 64, 64);
        g.setColorAt(0.0, QColor(0x2b, 0x7c, 0xf0));
        g.setColorAt(1.0, QColor(0x1f, 0x6f, 0xe0));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(0, 0, 64, 64), 8, 8);

        QPainterPath wave;
        wave.moveTo(13, 41);
        wave.cubicTo(18, 41, 18, 23, 23, 23);
        wave.cubicTo(28, 23, 28, 41, 33, 41);
        QPen pen((QColor(Qt::white)));
        pen.setWidthF(3.2);
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(wave);

        QPainterPath tri;
        tri.moveTo(39, 22);
        tri.lineTo(53, 32);
        tri.lineTo(39, 42);
        tri.closeSubpath();
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(Qt::white));
        p.drawPath(tri);
        p.end();

        cache = pm;
        cached = size;
        return cache;
    }
};
