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

// 设计令牌（定稿版 · 严格对齐 ui_proposal_A.html 与 使用手册.md 第 8/10 章）
//
// 这一版把主强调色从旧的 #4b6bfb 换成了 #2563eb：旧色偏紫、饱和度高，
// 在白底上大面积铺开会显"塑料"；#2563eb 是深空电光蓝，配 #f8fafc 页底
// 才会有手册里说的"陶瓷白 + 微晶工业感"。改配色只改这一个文件。
class Theme
{
public:
    // ---- 主题模式定义与自适应色板（AP500BJ 官方工控深色标准为默认）----
    enum ThemeMode { DarkMode = 0, LightMode = 1 };
    static ThemeMode& modeRef()
    {
        static ThemeMode s_mode = DarkMode;
        return s_mode;
    }
    static bool isDark() { return modeRef() == DarkMode; }
    static ThemeMode currentMode() { return modeRef(); }
    static void setMode(ThemeMode m) { modeRef() = m; }
    static void toggleMode() { modeRef() = (modeRef() == DarkMode ? LightMode : DarkMode); }

    static QColor primary()      { return isDark() ? QColor(0x3b, 0x82, 0xf6) : QColor(0x25, 0x63, 0xeb); }  // 强调蓝
    static QColor primaryHover() { return isDark() ? QColor(0x60, 0xa5, 0xfa) : QColor(0x1d, 0x4e, 0xd8); }
    static QColor success()      { return isDark() ? QColor(0x10, 0xb9, 0x81) : QColor(0x16, 0xa3, 0x4a); }  // 荧光绿
    static QColor danger()       { return isDark() ? QColor(0xef, 0x44, 0x44) : QColor(0xdc, 0x26, 0x26); }
    static QColor warn()         { return isDark() ? QColor(0xf5, 0x9e, 0x0b) : QColor(0xd9, 0x77, 0x06); }
    static QColor textMain()     { return isDark() ? QColor(0xf1, 0xf5, 0xf9) : QColor(0x1e, 0x29, 0x3b); }  // 主文字
    static QColor textSub()      { return isDark() ? QColor(0x94, 0xa3, 0xb8) : QColor(0x64, 0x74, 0x8b); }  // 次文字
    static QColor textMuted()    { return isDark() ? QColor(0x64, 0x74, 0x8b) : QColor(0x94, 0xa3, 0xb8); }  // 弱注释
    static QColor border()       { return isDark() ? QColor(0x37, 0x41, 0x51) : QColor(0xe2, 0xe8, 0xf0); }  // 边框
    static QColor divider()      { return isDark() ? QColor(0x2d, 0x37, 0x48) : QColor(0xf1, 0xf5, 0xf9); }  // 分割线
    static QColor canvas()       { return isDark() ? QColor(0x1e, 0x22, 0x2b) : QColor(0xf8, 0xfa, 0xfc); }  // 底色
    static QColor panelBg()      { return isDark() ? QColor(0x25, 0x2a, 0x34) : QColor(0xff, 0xff, 0xff); }  // 面板色

    // ---- 徽章（同步状态）与操作按钮的语义配色 ----
    //
    // 为什么单独开一组，而不是让各 delegate 自己调上面的基础色：
    // 状态药丸是「深底 + 亮字」，要在浅色面板上保持同样的视觉重量，
    // 就得翻成「浅底 + 深字」。这不是调一下 primary() 能得到的 ——
    // 同一个 primary() 在两套主题里只有一个值。
    //
    // 原来 StatusPillDelegate::pillOf() 与 ToolColumnDelegate::skinOf()
    // 各自把自己的深色值写死在函数里，既不问当前主题、也没有第二套值，
    // 于是浅色模式下界面整体变白，这两处仍是深底亮字：
    // 一片白里压着几块深灰深蓝的方块（用户 2026-10-16 报的"与环境不符合"）。
    //
    // 浅色档取同色相的浅底/深字（Tailwind 100 / 700 两档），
    // 于是两套主题是明度镜像关系，语义色不变、只有明暗翻面。
    struct Chip { QColor fg, bg, border; };

    static Chip chipWait()
    {
        Chip c;
        if (isDark()) { c.fg = QColor(0x94, 0xa3, 0xb8); c.bg = QColor(0x2a, 0x30, 0x3c); c.border = QColor(0x37, 0x41, 0x51); }
        else          { c.fg = QColor(0x47, 0x55, 0x69); c.bg = QColor(0xf1, 0xf5, 0xf9); c.border = QColor(0xcb, 0xd5, 0xe1); }
        return c;
    }
    static Chip chipSyncing()
    {
        Chip c;
        if (isDark()) { c.fg = QColor(0x60, 0xa5, 0xfa); c.bg = QColor(0x1e, 0x3a, 0x8a); c.border = QColor(0x25, 0x63, 0xeb); }
        else          { c.fg = QColor(0x1d, 0x4e, 0xd8); c.bg = QColor(0xdb, 0xea, 0xfe); c.border = QColor(0x93, 0xc5, 0xfd); }
        return c;
    }
    static Chip chipOk()
    {
        Chip c;
        if (isDark()) { c.fg = QColor(0x34, 0xd3, 0x99); c.bg = QColor(0x06, 0x4e, 0x3b); c.border = QColor(0x05, 0x96, 0x69); }
        else          { c.fg = QColor(0x04, 0x78, 0x57); c.bg = QColor(0xd1, 0xfa, 0xe5); c.border = QColor(0x6e, 0xe7, 0xb7); }
        return c;
    }
    static Chip chipFailed()
    {
        Chip c;
        if (isDark()) { c.fg = QColor(0xf8, 0x71, 0x71); c.bg = QColor(0x7f, 0x1d, 0x1d); c.border = QColor(0xdc, 0x26, 0x26); }
        else          { c.fg = QColor(0xb9, 0x1c, 0x1c); c.bg = QColor(0xfe, 0xe2, 0xe2); c.border = QColor(0xfc, 0xa5, 0xa5); }
        return c;
    }
    static Chip chipSkipped()
    {
        Chip c;
        if (isDark()) { c.fg = QColor(0x2d, 0xd4, 0xbf); c.bg = QColor(0x13, 0x4e, 0x4a); c.border = QColor(0x11, 0x5e, 0x59); }
        else          { c.fg = QColor(0x0f, 0x76, 0x6e); c.bg = QColor(0xcc, 0xfb, 0xf1); c.border = QColor(0x5e, 0xea, 0xd4); }
        return c;
    }
    // 已取消：琥珀色。必须和 chipSkipped 的青、chipFailed 的红都拉得开 ——
    // 用户取消之后扫一眼颜色就该知道"这些没传"，而不是被青色骗成"远端已最新"。
    static Chip chipCanceled()
    {
        Chip c;
        if (isDark()) { c.fg = QColor(0xfb, 0xbf, 0x24); c.bg = QColor(0x78, 0x35, 0x0f); c.border = QColor(0xb4, 0x53, 0x09); }
        else          { c.fg = QColor(0xb4, 0x53, 0x09); c.bg = QColor(0xfe, 0xf3, 0xc7); c.border = QColor(0xfb, 0xbf, 0x24); }
        return c;
    }

    // 表格行选中底色。原来两个 delegate 都写死深蓝 #1e3a8a，
    // 浅色模式下一选中就是一条深蓝带，和浅色行底完全不搭。
    static QColor rowSelectedBg()
    {
        return isDark() ? QColor(0x1e, 0x3a, 0x8a) : QColor(0xef, 0xf6, 0xff);
    }

    // 标题栏系统按钮（齿轮 / 最小化 / 关闭）图标的静态色。
    // 浅色下用 primary 深蓝而不是 textSub 灰蓝：白底上灰蓝近乎看不见，
    // 而图标是这套窗口唯一还在的"框架"，不能弱到没有存在感。
    static QColor chromeIcon()  { return isDark() ? QColor(0x94, 0xa3, 0xb8) : QColor(0x1d, 0x4e, 0xd8); }
    static QColor chromeIconOn(){ return isDark() ? QColor(0x93, 0xc5, 0xfd) : QColor(0x25, 0x63, 0xeb); }

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
    color: #f1f5f9;
    %FAMILYDECL%
    font-size: 12px;
}

/* ---- AP500BJ 官方工控深色窗体（14px 柔和曲线感，对标图一） ---- */
QWidget#rootFrame {
    background: #1e222b;
    border: 1px solid #374151;
    border-radius: 14px;
}
QFrame#splashFrame {
    background: #252a34;
    border: 1px solid #374151;
    border-radius: 16px;
}

QWidget#pageBg {
    background: #1e222b;
    border: none;
    border-bottom-left-radius: 14px;
    border-bottom-right-radius: 14px;
}

/* ---- 标题栏 ---- */
QFrame#titleBar {
    background: #1e222b;
    border: none;
    border-top-left-radius: 14px;
    border-top-right-radius: 14px;
}
QFrame#titleBarLine { background: #2d3748; border: none; }
QLabel#appTitle { font-size: 14px; font-weight: 700; color: #ffffff; }

QToolButton#sysBtn, QToolButton#sysBtnClose, QToolButton#themeToggleBtn {
    background: transparent; border: 1px solid transparent;
    border-radius: 5px; color: #94a3b8;
}
QToolButton#sysBtn:hover  { background: #2d3748; color: #ffffff; }
QToolButton#sysBtnClose:hover { background: #ef4444; color: #ffffff; border-color: #ef4444; }
QToolButton#themeToggleBtn:hover { background: #2d3748; color: #60a5fa; border-color: #3b82f6; }
QToolButton#gearBtn {
    background: transparent; border: 1px solid transparent;
    border-radius: 5px; color: #94a3b8;
}
QToolButton#gearBtn:hover   { background: #2d3748; color: #60a5fa; border-color: #3b82f6; }
QToolButton#gearBtn:checked { background: #1e3a8a; color: #93c5fd; border-color: #3b82f6; }

/* ---- 文件清单卡片 ---- */
QFrame#listCard {
    background: #252a34; border: 1px solid #374151; border-radius: 7px;
}
QFrame#cardHeader { background: #1f242d; border: none; }
QFrame#cardHeaderLine { background: #2d3748; border: none; }

QPushButton#fileTab {
    background: transparent; border: 1px solid transparent;
    border-radius: 4px; padding: 2px 7px;
    font-size: 11px; font-weight: 500; color: #94a3b8;
}
QPushButton#fileTab:hover   { background: #2d3748; color: #ffffff; }
QPushButton#fileTab:checked { background: #3b82f6; border-color: #3b82f6;
                               color: #ffffff; font-weight: 600; }

QPushButton#refreshScan {
    background: #1e293b; border: 1px solid #3b82f6; border-radius: 4px;
    padding: 2px 7px; font-size: 11px; font-weight: 500; color: #60a5fa;
}
QPushButton#refreshScan:hover    { background: #1e3a8a; border-color: #60a5fa; color: #ffffff; }
QPushButton#refreshScan:disabled { color: #64748b; border-color: #374151; }

/* ---- 表格：消除灰框与黑线，工控深色数据网格 ---- */
QTableView {
    background-color: #252a34;
    color: #f1f5f9;
    border: none;
    gridline-color: #2d3748;
    selection-background-color: #1e3a8a;
    selection-color: #ffffff;
    outline: none;
}
QTableView::item { border: none; padding: 0px; }
QHeaderView { background: #1f242d; border: none; }
QHeaderView::section {
    background-color: #1f242d;
    color: #94a3b8;
    font-weight: 600;
    font-size: 11px;
    padding: 7px 8px;
    border: none;
    border-bottom: 1px solid #374151;
}
QTableCornerButton::section { background: #1f242d; border: none; }

/* ---- 进度区 ---- */
QFrame#progressCard {
    background: #252a34; border: 1px solid #374151; border-radius: 7px;
}
QLabel#curStatus  { font-size: 12px; font-weight: 600; color: #f1f5f9; }
QLabel#curStatusDone { font-size: 12px; font-weight: 600; color: #10b981; }
QLabel#curStatusCancel { font-size: 12px; font-weight: 600; color: #ef4444; }
QLabel#speedCount {
    font-size: 11.5px; color: #94a3b8; %MONODECL%
}

/* 唯一一根物理进度条：科技双色电光渐变 */
QProgressBar {
    background: #1e222b; border: none; border-radius: 4px;
    height: 6px; min-height: 6px; max-height: 6px;
    text-align: center; color: transparent;
}
QProgressBar::chunk {
    border-radius: 4px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #10b981);
}

/* ---- 状态药丸徽章 ---- */
QLabel#statusWait {
    background-color: #1e293b; color: #94a3b8;
    border: 1px solid #334155; border-radius: 10px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
}
QLabel#statusSyncing {
    background-color: #1e3a8a; color: #93c5fd;
    border: 1px solid #3b82f6; border-radius: 10px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
    %MONODECL%
}
QLabel#statusOk {
    background-color: #064e3b; color: #4ade80;
    border: 1px solid #059669; border-radius: 10px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
}

/* ---- 底部操作条与表格复选框 ---- */
QCheckBox { color: #94a3b8; spacing: 6px; font-size: 12.5px; }
QCheckBox::indicator, QTableView::indicator {
    width: 14px; height: 14px; border: 1px solid #4b5563;
    border-radius: 3px; background: #1e222b;
}
QCheckBox::indicator:hover, QTableView::indicator:hover  { border-color: #3b82f6; }
QCheckBox::indicator:checked, QTableView::indicator:checked {
    background: #2563eb; border: 1px solid #3b82f6;
    image: url("%CHECK_ICON%");
}

QPushButton#btnStartSync {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #3b82f6);
    color: #ffffff; border: none;
    border-radius: 5px; padding: 6px 22px;
    font-size: 13px; font-weight: 600;
}
QPushButton#btnStartSync:hover    { background: #1d4ed8; }
QPushButton#btnStartSync:disabled { background: #475569; color: #94a3b8; }
QPushButton#btnStartSyncCancel {
    background: #ef4444; color: #ffffff; border: none;
    border-radius: 5px; padding: 6px 22px;
    font-size: 13px; font-weight: 600;
}
QPushButton#btnStartSyncCancel:hover { background: #dc2626; }

QPushButton#btnToggleLog {
    background: #252a34; color: #94a3b8;
    border: 1px solid #374151; border-radius: 5px;
    padding: 4px 10px; font-size: 11.5px; font-weight: 500;
}
QPushButton#btnToggleLog:hover {
    background: #1e3a8a; color: #60a5fa; border-color: #3b82f6;
}

/* ---- 日志抽屉：深色工业终端 ---- */
QWidget#logDrawer {
    background: #090d16; border: 1px solid #1e293b; border-radius: 6px;
}
QFrame#logHeader { background: #111827; border: none; }
QFrame#logHeaderLine { background: #1e293b; border: none; }
QLabel#logHeaderText { font-size: 12px; font-weight: 600; color: #cbd5e1; }
QPushButton#logAction {
    background: transparent; border: none; color: #64748b;
    font-size: 11px; padding: 2px 4px;
}
QPushButton#logAction:hover { color: #60a5fa; }
QPlainTextEdit#logView, QTextEdit#logView {
    background: #090d16; color: #cbd5e1; border: none;
    padding: 8px 12px;
    %MONODECL% font-size: 13px;
    selection-background-color: #1e3a8a; selection-color: #ffffff;
}

/* ---- 设置弹窗（无原生白顶栏，14px 大圆角曲线感，对标图三、图四） ---- */
QDialog#settingsDialog {
    background: transparent;
}
QFrame#settingsCard {
    background: #1e222b;
    border: 1px solid #374151;
    border-radius: 14px;
}
QFrame#dialogHeader {
    background: #1f242d;
    border-bottom: 1px solid #374151;
    border-top-left-radius: 14px;
    border-top-right-radius: 14px;
}
QLabel#dialogTitleText {
    font-size: 13.5px;
    font-weight: 700;
    color: #f1f5f9;
}
QToolButton#dialogCloseBtn, QPushButton#dialogCloseBtn {
    background: transparent;
    border: none;
    border-radius: 14px;
    color: #94a3b8;
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
QFrame#dialogFooter {
    background: #1f242d;
    border-top: 1px solid #2d3748;
    border-bottom-left-radius: 14px;
    border-bottom-right-radius: 14px;
}
QLabel#settingLabel { font-size: 11.5px; color: #94a3b8; font-weight: 500; }
QLabel#settingHint  { font-size: 11px; color: #64748b; }
QLineEdit#settingInput,
QLineEdit#settingSourceDir, QLineEdit#settingHost, QLineEdit#settingUser,
QLineEdit#settingPassword, QLineEdit#settingKeyPath, QLineEdit#settingRemoteRoot,
QLineEdit#settingPort, QComboBox#settingAuthMode {
    background: #252a34; border: 1px solid #374151; border-radius: 5px;
    padding: 0 8px; min-height: 28px; max-height: 28px;
    font-size: 12px; color: #ffffff;
    selection-background-color: #2563eb; selection-color: #ffffff;
}
QLineEdit#settingInput:focus, QLineEdit#settingSourceDir:focus,
QLineEdit#settingHost:focus, QLineEdit#settingUser:focus,
QLineEdit#settingPassword:focus, QLineEdit#settingKeyPath:focus,
QLineEdit#settingRemoteRoot:focus, QLineEdit#settingPort:focus,
QLineEdit:focus, QComboBox#settingAuthMode:focus {
    border: 1px solid #3b82f6; background: #1a1f2c;
}
QLineEdit#settingInput:disabled, QLineEdit#settingSourceDir:disabled,
QLineEdit#settingHost:disabled, QLineEdit#settingUser:disabled,
QLineEdit#settingPassword:disabled, QLineEdit#settingKeyPath:disabled,
QLineEdit#settingRemoteRoot:disabled, QLineEdit#settingPort:disabled,
QComboBox#settingAuthMode:disabled { background: #1e222b; color: #64748b; border-color: #2d3748; }

QPushButton#btnBrowse, QPushButton#btnBrowseKey {
    background: #252a34; color: #f1f5f9;
    border: 1px solid #374151; border-radius: 5px;
    padding: 0 12px; min-height: 28px; max-height: 28px;
    font-size: 11.5px; font-weight: 500; white-space: nowrap;
}
QPushButton#btnBrowse:hover, QPushButton#btnBrowseKey:hover { background: #1e3a8a; color: #ffffff; border-color: #3b82f6; }
QPushButton#btnBrowse:disabled, QPushButton#btnBrowseKey:disabled {
    background: #1e222b; color: #64748b; border: 1px solid #2d3748;
}
QPushButton#btnAddRule {
    background: #252a34; color: #60a5fa;
    border: 1px solid #3b82f6; border-radius: 4px;
    padding: 2px 7px; font-size: 11px;
}
QPushButton#btnAddRule:hover { background: #1e3a8a; color: #ffffff; }
QPushButton#btnTestConn {
    background: transparent; color: #60a5fa;
    border: 1px solid #3b82f6; border-radius: 4px;
    padding: 4px 10px; font-size: 11.5px;
}
QPushButton#btnTestConn:hover    { background: #1e3a8a; color: #ffffff; }
QPushButton#btnTestConn:disabled { color: #64748b; border-color: #374151; }
QPushButton#btnSave {
    background: #2563eb; color: #ffffff; border: none;
    border-radius: 4px; padding: 5px 13px; font-size: 11.5px; font-weight: 500;
}
QPushButton#btnSave:hover { background: #1d4ed8; }
QLabel#testLabel  { font-size: 11.5px; font-weight: 500; color: #94a3b8; }
QLabel#testLabelOk   { font-size: 11.5px; font-weight: 500; color: #10b981; }
QLabel#testLabelFail { font-size: 11.5px; font-weight: 500; color: #ef4444; }

/* ---- 规则表（设置弹窗内）---- */
QTableView#ruleTable {
    background: #252a34; border: 1px solid #374151; gridline-color: #2d3748; outline: none; color: #ffffff;
}
QHeaderView#ruleHeader::section {
    background-color: #1f242d; color: #94a3b8;
    font-weight: 600; font-size: 11px; padding: 6px 10px;
    border: none; border-bottom: 1px solid #374151;
}
QPushButton#ruleRemove {
    background: transparent; border: 1px solid transparent;
    border-radius: 3px; color: #ef4444; font-size: 13px;
}
QPushButton#ruleRemove:hover { background: #7f1d1d; color: #fecaca; }

/* ---- 通用 ---- */
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QMenu {
    background: #1f242d; border: 1px solid #374151; border-radius: 5px;
    padding: 4px; color: #f1f5f9;
}
QMenu::item { padding: 5px 12px; border-radius: 3px; font-size: 11.5px; color: #f1f5f9; }
QMenu::item:selected { background: #1e3a8a; color: #ffffff; }
QMenu::separator { height: 1px; background: #2d3748; margin: 3px 6px; }

QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #374151; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #4b5563; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 2px; }
QScrollBar::handle:horizontal { background: #374151; border-radius: 4px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: #4b5563; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QToolTip {
    background: #0f172a; color: #ffffff; border: none;
    padding: 5px 9px; font-size: 11px;
}
)QSS")
            .replace(QStringLiteral("%FAMILYDECL%"), fontDecl())
            .replace(QStringLiteral("%MONODECL%"), monoDecl())
            .replace(QStringLiteral("%CHECK_ICON%"), checkIconPath());
    }

    static QString buildLightQss()
    {
        return QStringLiteral(R"QSS(
QWidget {
    color: #1e293b;
    %FAMILYDECL%
    font-size: 12px;
}

QWidget#rootFrame {
    background: #f8fafc;
    border: 1px solid #cbd5e1;
    border-radius: 14px;
}
QFrame#splashFrame {
    background: #ffffff;
    border: 1px solid #cbd5e1;
    border-radius: 16px;
}

QWidget#pageBg {
    background: #f8fafc;
    border: none;
    border-bottom-left-radius: 14px;
    border-bottom-right-radius: 14px;
}

/* ---- 标题栏 ---- */
QFrame#titleBar {
    background: #ffffff;
    border: none;
    border-bottom: 1px solid #e2e8f0;
    border-top-left-radius: 14px;
    border-top-right-radius: 14px;
}
QFrame#titleBarLine { background: #e2e8f0; border: none; }
QLabel#appTitle { font-size: 14px; font-weight: 700; color: #0f172a; }

QToolButton#sysBtn, QToolButton#sysBtnClose, QToolButton#themeToggleBtn {
    background: transparent; border: 1px solid transparent;
    border-radius: 5px; color: #64748b;
}
QToolButton#sysBtn:hover  { background: #f1f5f9; color: #0f172a; }
QToolButton#sysBtnClose:hover { background: #ef4444; color: #ffffff; border-color: #ef4444; }
QToolButton#themeToggleBtn:hover { background: #eff6ff; color: #2563eb; border-color: #bfdbfe; }
QToolButton#gearBtn {
    background: transparent; border: 1px solid transparent;
    border-radius: 5px; color: #64748b;
}
QToolButton#gearBtn:hover   { background: #eff6ff; color: #2563eb; border-color: #bfdbfe; }
QToolButton#gearBtn:checked { background: #dbeafe; color: #1d4ed8; border-color: #3b82f6; }

/* ---- 文件清单卡片 ---- */
QFrame#listCard {
    background: #ffffff; border: 1px solid #e2e8f0; border-radius: 7px;
}
QFrame#cardHeader { background: #f8fafc; border: none; }
QFrame#cardHeaderLine { background: #e2e8f0; border: none; }

QPushButton#fileTab {
    background: transparent; border: 1px solid transparent;
    border-radius: 4px; padding: 2px 7px;
    font-size: 11px; font-weight: 500; color: #64748b;
}
QPushButton#fileTab:hover   { background: #f1f5f9; color: #0f172a; }
QPushButton#fileTab:checked { background: #2563eb; border-color: #2563eb;
                               color: #ffffff; font-weight: 600; }

QPushButton#refreshScan {
    background: #eff6ff; border: 1px solid #bfdbfe; border-radius: 4px;
    padding: 2px 7px; font-size: 11px; font-weight: 500; color: #2563eb;
}
QPushButton#refreshScan:hover    { background: #dbeafe; border-color: #3b82f6; color: #1d4ed8; }
QPushButton#refreshScan:disabled { color: #94a3b8; border-color: #e2e8f0; background: #f8fafc; }

/* ---- 表格：浅色微晶 ---- */
QTableView {
    background-color: #ffffff;
    color: #1e293b;
    border: none;
    gridline-color: #f1f5f9;
    selection-background-color: #eff6ff;
    selection-color: #1e40af;
    outline: none;
}
QTableView::item { border: none; padding: 0px; }
QHeaderView { background: #f8fafc; border: none; }
QHeaderView::section {
    background-color: #f8fafc;
    color: #64748b;
    font-weight: 600;
    font-size: 11px;
    padding: 7px 8px;
    border: none;
    border-bottom: 1px solid #e2e8f0;
}
QTableCornerButton::section { background: #f8fafc; border: none; }

/* ---- 进度区 ---- */
QFrame#progressCard {
    background: #ffffff; border: 1px solid #e2e8f0; border-radius: 7px;
}
QLabel#curStatus  { font-size: 12px; font-weight: 600; color: #0f172a; }
QLabel#curStatusDone { font-size: 12px; font-weight: 600; color: #16a34a; }
QLabel#curStatusCancel { font-size: 12px; font-weight: 600; color: #ef4444; }
QLabel#speedCount {
    font-size: 11.5px; color: #64748b; %MONODECL%
}

QProgressBar {
    background: #e2e8f0; border: none; border-radius: 4px;
    height: 6px; min-height: 6px; max-height: 6px;
    text-align: center; color: transparent;
}
QProgressBar::chunk {
    border-radius: 4px;
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #10b981);
}

/* ---- 状态徽章 ---- */
QLabel#statusWait {
    background-color: #f1f5f9; color: #64748b;
    border: 1px solid #e2e8f0; border-radius: 10px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
}
QLabel#statusSyncing {
    background-color: #eff6ff; color: #1d4ed8;
    border: 1px solid #bfdbfe; border-radius: 10px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
    %MONODECL%
}
QLabel#statusOk {
    background-color: #ecfdf5; color: #059669;
    border: 1px solid #a7f3d0; border-radius: 10px;
    padding: 1px 7px; font-size: 11px; font-weight: 600;
}

QCheckBox { color: #64748b; spacing: 6px; font-size: 12.5px; }
QCheckBox::indicator, QTableView::indicator {
    width: 14px; height: 14px; border: 1px solid #cbd5e1;
    border-radius: 3px; background: #ffffff;
}
QCheckBox::indicator:hover, QTableView::indicator:hover  { border-color: #2563eb; }
QCheckBox::indicator:checked, QTableView::indicator:checked {
    background: #2563eb; border: 1px solid #2563eb;
    image: url("%CHECK_ICON%");
}

QPushButton#btnStartSync {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #2563eb, stop:1 #3b82f6);
    color: #ffffff; border: none;
    border-radius: 5px; padding: 6px 22px;
    font-size: 13px; font-weight: 600;
}
QPushButton#btnStartSync:hover    { background: #1d4ed8; }
QPushButton#btnStartSync:disabled { background: #94a3b8; color: #f1f5f9; }
QPushButton#btnStartSyncCancel {
    background: #ef4444; color: #ffffff; border: none;
    border-radius: 5px; padding: 6px 22px;
    font-size: 13px; font-weight: 600;
}
QPushButton#btnStartSyncCancel:hover { background: #dc2626; }

QPushButton#btnToggleLog {
    background: #ffffff; color: #64748b;
    border: 1px solid #e2e8f0; border-radius: 5px;
    padding: 4px 10px; font-size: 11.5px; font-weight: 500;
}
QPushButton#btnToggleLog:hover {
    background: #eff6ff; color: #2563eb; border-color: #bfdbfe;
}

/* ---- 日志抽屉 ---- */
QWidget#logDrawer {
    background: #0f172a; border: 1px solid #1e293b; border-radius: 6px;
}
QFrame#logHeader { background: #1e293b; border: none; }
QFrame#logHeaderLine { background: #334155; border: none; }
QLabel#logHeaderText { font-size: 12px; font-weight: 600; color: #cbd5e1; }
QPushButton#logAction {
    background: transparent; border: none; color: #94a3b8;
    font-size: 11px; padding: 2px 4px;
}
QPushButton#logAction:hover { color: #60a5fa; }
QPlainTextEdit#logView, QTextEdit#logView {
    background: #0f172a; color: #cbd5e1; border: none;
    padding: 8px 12px;
    %MONODECL% font-size: 13px;
    selection-background-color: #2563eb; selection-color: #ffffff;
}

/* ---- 设置弹窗 ---- */
QDialog#settingsDialog { background: transparent; }
QFrame#settingsCard {
    background: #ffffff; border: 1px solid #cbd5e1; border-radius: 14px;
}
QFrame#dialogHeader {
    background: #f8fafc; border-bottom: 1px solid #e2e8f0;
    border-top-left-radius: 14px; border-top-right-radius: 14px;
}
QLabel#dialogTitleText { font-size: 13.5px; font-weight: 700; color: #0f172a; }
QToolButton#dialogCloseBtn, QPushButton#dialogCloseBtn {
    background: transparent; border: none; border-radius: 14px;
    color: #64748b; font-size: 14px;
}
QToolButton#dialogCloseBtn:hover, QPushButton#dialogCloseBtn:hover { background: #fee2e2; color: #ef4444; }
QToolButton#dialogCloseBtn:pressed, QPushButton#dialogCloseBtn:pressed { background: #fca5a5; color: #b91c1c; }

QFrame#dialogFooter {
    background: #f8fafc; border-top: 1px solid #e2e8f0;
    border-bottom-left-radius: 14px; border-bottom-right-radius: 14px;
}
QLabel#settingLabel { font-size: 11.5px; color: #475569; font-weight: 500; }
QLabel#settingHint  { font-size: 11px; color: #94a3b8; }
QLineEdit#settingInput,
QLineEdit#settingSourceDir, QLineEdit#settingHost, QLineEdit#settingUser,
QLineEdit#settingPassword, QLineEdit#settingKeyPath, QLineEdit#settingRemoteRoot,
QLineEdit#settingPort {
    background: #ffffff; border: 1px solid #cbd5e1; border-radius: 5px;
    padding: 0 8px; min-height: 28px; max-height: 28px;
    font-size: 12px; color: #0f172a;
    selection-background-color: #2563eb; selection-color: #ffffff;
}
QLineEdit#settingInput:focus, QLineEdit#settingSourceDir:focus,
QLineEdit#settingHost:focus, QLineEdit#settingUser:focus,
QLineEdit#settingPassword:focus, QLineEdit#settingRemoteRoot:focus,
QLineEdit:focus {
    border: 1px solid #2563eb; background: #fafafa;
}

QPushButton#btnBrowse {
    background: #f1f5f9; color: #334155;
    border: 1px solid #cbd5e1; border-radius: 5px;
    padding: 0 12px; min-height: 28px; max-height: 28px;
    font-size: 11.5px; font-weight: 500;
}
QPushButton#btnBrowse:hover { background: #e2e8f0; color: #0f172a; border-color: #94a3b8; }
QPushButton#btnAddRule {
    background: #eff6ff; color: #2563eb;
    border: 1px solid #bfdbfe; border-radius: 4px;
    padding: 2px 7px; font-size: 11px;
}
QPushButton#btnAddRule:hover { background: #dbeafe; color: #1d4ed8; }
QPushButton#btnTestConn {
    background: transparent; color: #2563eb;
    border: 1px solid #2563eb; border-radius: 4px;
    padding: 4px 10px; font-size: 11.5px;
}
QPushButton#btnTestConn:hover { background: #eff6ff; }
QPushButton#btnSave {
    background: #2563eb; color: #ffffff; border: none;
    border-radius: 4px; padding: 5px 13px; font-size: 11.5px; font-weight: 500;
}
QPushButton#btnSave:hover { background: #1d4ed8; }

QTableView#ruleTable {
    background: #ffffff; border: 1px solid #e2e8f0; gridline-color: #f1f5f9; outline: none; color: #0f172a;
}
QHeaderView#ruleHeader::section {
    background-color: #f8fafc; color: #64748b;
    font-weight: 600; font-size: 11px; padding: 6px 10px;
    border: none; border-bottom: 1px solid #e2e8f0;
}

QScrollArea { border: none; background: transparent; }
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px; }
QScrollBar::handle:vertical { background: #cbd5e1; border-radius: 4px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: #94a3b8; }
QScrollBar::horizontal { background: transparent; height: 8px; margin: 2px; }
QScrollBar::handle:horizontal { background: #cbd5e1; border-radius: 4px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: #94a3b8; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QToolTip {
    background: #0f172a; color: #ffffff; border: none;
    padding: 5px 9px; font-size: 11px;
}
)QSS")
            .replace(QStringLiteral("%FAMILYDECL%"), fontDecl())
            .replace(QStringLiteral("%MONODECL%"), monoDecl())
            .replace(QStringLiteral("%CHECK_ICON%"), checkIconPath());
    }

    static QString buildQss(ThemeMode mode = currentMode())
    {
        return (mode == DarkMode) ? buildDarkQss() : buildLightQss();
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
        // 定稿主色 #2563eb。旧值 #4b6bfb 是更亮的靛蓝，放在顶栏小图标上
        // 会显得发飘、和按钮的 #2563eb 不是同一个蓝，一眼就"不高级"。
        p.setBrush(QColor(0x25, 0x63, 0xeb));
        p.drawRoundedRect(QRectF(0, 0, 32, 32), 6, 6);

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
    // 与 logo() 分开缓存：两者的圆角不同（原型 64px 上是 rx=16，即 1/4 边长），
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
        g.setColorAt(0.0, QColor(0x3b, 0x82, 0xf6));
        g.setColorAt(1.0, QColor(0x1d, 0x4e, 0xd8));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(0, 0, 64, 64), 16, 16);

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
    // 与 splashAp 同一个渐变、同一个 rx=16，两个形态之间只换中间的字/图形，
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
        g.setColorAt(0.0, QColor(0x3b, 0x82, 0xf6));
        g.setColorAt(1.0, QColor(0x1d, 0x4e, 0xd8));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRoundedRect(QRectF(0, 0, 64, 64), 16, 16);

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
