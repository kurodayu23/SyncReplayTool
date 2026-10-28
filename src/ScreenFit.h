#pragma once

// 屏幕自适应：**唯一**的尺寸收敛算法。
//
// 为什么要有这个文件：主界面和设置弹窗都要按「屏幕可用区」收敛尺寸，
// 而收敛规则必须**只有一份** —— 两处各写一份的话，改了一边另一边就过时了，
// 于是出现"大屏上弹窗小气、矮屏上弹窗被裁"这种两边都怪的情况。
//
// 为什么规则写死成「先按屏幕收敛、再卡上下界」：
//   · 上界 = 名义尺寸（980×460 / 640×560）—— 大屏上不显得小气；
//   · 下界 = 能用的最小尺寸 —— 再小就"没法看"了（表格列挤没、输入框放不下）；
//   · 中间 = 按屏幕可用区线性收敛 —— 笔记本 1366×768、1080p、4K 缩放都能用。
//
// 这一份里现在有**三类**东西：
//   ① 纯函数（只吃数字）：mainWindow() / settingsDialog() /
//      fitInside() / centeredIn() —— 可以被自检拿一整张屏幕矩阵跑一遍，
//      不需要在真机上逐个改分辨率。
//   ② 找屏：screenForWidget() —— 「这个窗口该按哪块屏算」。
//   ③ 常量：所有尺寸上下界 —— 其它文件一律引用这里，不再各存一份。
//
// ② 单独拎出来是因为它是**修 bug 的关键**：原来主窗口按 primaryScreen()
// 算高度、设置弹窗按"父窗口所在屏"算 —— 同一屏两套规则，
// 主窗口拖到更矮的副屏就会按主屏的高度展开日志，底边直接掉出屏幕。
// 现在两边都走 screenForWidget()，同一块屏、同一套数。
//
// 屏幕矩阵见 main.cpp 的 selfcheck：1366×768 笔记本、1920×1080、1024×768 老本、
// 800×600 极小、2560×1600@200%（=1280×800 逻辑）、1280×720@150%（=853×480 逻辑）。

#include <QSize>
#include <QRect>
#include <QPoint>
#include <QList>
#include <QtGlobal>
#include <QWidget>
#include <QGuiApplication>
#include <QScreen>
#include <QWindow>

namespace ScreenFit {

// 主界面名义尺寸。
const int kMainWidth         = 980;
const int kMainHeight        = 460;
const int kMainHeightLogOpen = 759;
// 主界面宽度下界：表格 7 列里 6 列定宽（共 596px），剩下的给「文件名」列。
// 再窄那列就没了 —— 那是整个表格里最该看的一列。
const int kMainWidthMin  = 720;
const int kMainHeightMin = 320;

// 设置弹窗名义尺寸。
const int kSettingsWidth  = 640;
const int kSettingsHeight = 560;
const int kSettingsWidthMin  = 360;
const int kSettingsHeightMin = 320;

// 屏幕四周留的余量（任务栏/边框）。
const int kMargin = 40;

// 主界面尺寸。logOpen = 日志抽屉是否展开。
inline QSize mainWindow(int availW, int availH, bool logOpen)
{
    const int wantH = logOpen ? kMainHeightLogOpen : kMainHeight;
    const int w = qMax(kMainWidthMin,  qMin(kMainWidth,  availW));
    const int h = qMax(kMainHeightMin, qMin(wantH,       availH));
    return QSize(w, h);
}

// 设置弹窗尺寸。
inline QSize settingsDialog(int availW, int availH)
{
    const int w = qMax(kSettingsWidthMin,  qMin(kSettingsWidth,  availW - kMargin));
    const int h = qMax(kSettingsHeightMin, qMin(kSettingsHeight, availH - kMargin));
    return QSize(w, h);
}

// 这个窗口该按**哪块屏**算。
//
// 优先级：窗口当前所在屏 → 几何中心落在哪块屏 → 主屏。
//
// 为什么不能用 primaryScreen()：多显示器下主窗口拖到副屏（哪怕分辨率一样）
// 也不该按主屏算高度；拔掉外接屏之后主屏也会变。
//
// 注意：Qt 5.9 没有 QGuiApplication::screenAt()（5.10 才有），
// 这里用 QWindow::screen()（5.0 就有）+ 自己遍历 screens() 兜底。
inline QScreen *screenForWidget(QWidget *w)
{
    if (!w)
        return QGuiApplication::primaryScreen();
    if (w->windowHandle()) {
        QScreen *s = w->windowHandle()->screen();
        if (s)
            return s;
    }
    // 还没上屏（构造期）时 windowHandle() 为空：按几何中心落在哪块屏判断。
    const QPoint c = w->mapToGlobal(w->rect().center());
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        if (screens.at(i)->geometry().contains(c))
            return screens.at(i);
    }
    return QGuiApplication::primaryScreen();
}

// 把「目标尺寸 + 当前位置」收敛成「一定整块落在 avail 内」的矩形。
//
// 为什么必须有这个：只 setFixedHeight 而不挪位置，窗口加高后**底边会掉出屏幕**。
// 实测（1280×764 可用区）：460 → 759 后底边到 909，超出 146px，
// 175px 高的日志抽屉只剩 29px 可见 —— 用户点了"展开日志"，看到的是几乎空的抽屉。
// 窗口拖到屏幕下半屏时超出更多（实测 264px）。
//
// 规则：尺寸先按 avail 收；位置**优先保留原来的顶边/左边**，越界才往里挪。
// 挪的顺序是"先保住顶边、挪不动才贴下边"：窗口在上半屏时点开日志不会乱跳，
// 窗口在下半屏时则被拉回屏幕内。
inline QRect fitInside(QSize want, const QRect &cur, const QRect &avail)
{
    const int w = qMin(want.width(),  avail.width());
    const int h = qMin(want.height(), avail.height());
    int x = cur.x();
    int y = cur.y();
    if (x + w > avail.right() + 1)  x = avail.right() + 1 - w;
    if (x < avail.x())               x = avail.x();
    if (y + h > avail.bottom() + 1) y = avail.bottom() + 1 - h;
    if (y < avail.y())               y = avail.y();
    return QRect(x, y, w, h);
}

// 居中并夹紧（弹窗、首次上屏用）。
inline QRect centeredIn(QSize want, const QRect &avail)
{
    const int w = qMin(want.width(),  avail.width());
    const int h = qMin(want.height(), avail.height());
    const QRect c(avail.x() + (avail.width() - w) / 2,
                  avail.y() + (avail.height() - h) / 2, w, h);
    return fitInside(QSize(w, h), c, avail);
}

} // namespace ScreenFit
