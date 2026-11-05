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
// 日志抽屉的高度（**唯一真源**：SyncPanel 建抽屉时用同一个值）。
const int kLogDrawerHeight   = 175;
// 日志抽屉的**正下限**（2026-11-05 修）。
//
// 为什么必须有下限：抽屉高度原先是「窗口能长出来的那部分」，
// 在**矮屏**上（可用高 ≤ 471）那一部分是 0 ⇒ 抽屉高度 0px ⇒
// 用户点「同步日志」，按钮文案变了、**但日志一个字都看不见** ——
// 看起来像按钮坏了。
//
// 96 = 27px 标题栏 + 69px 正文区（够看几行）。
// 代价由**表格**让：抽屉开着时表格的最小高度降到 kTableMinWithLog，
// 空间够的话表格照样铺满（它 stretch=1），不够时它先让。
const int kLogDrawerMin      = 96;
// 表格区的最小高度（手册 §2 的 235，收起态）。
const int kTableMinNormal    = 235;
// 表格区的最小高度（**抽屉开着**时）：抽屉占的那部分从表格这里借。
//
// 为什么不能一直是 235：矮屏上窗口长不高，抽屉只能从表格这块借高度 ——
// 表格是可滚动的，少显示几行远比"日志点了看不见"强。
// 60 ≈ 1.7 行，保证表格区域本身还在、用户知道东西还在那儿。
const int kTableMinWithLog   = 60;
// 面板竖向间距（SyncPanel 的 root 布局，**唯一真源**）。
//
// 为什么它要算进窗口高度：日志抽屉默认是**隐藏**的，而 QBoxLayout 不给
// 隐藏控件留位置、也不给它算间距 —— 它变可见的那一刻，布局里**多出一段间距**。
// 窗口不多长这 12px 的话，这段间距就从「清单卡片与进度区之间那段留白」里扣，
// 于是进度区整个往下挪 12px（实测 before=339 after=351）。
const int kPanelSpacing      = 12;
// 日志展开时的主界面高度 = 收起高度 + 抽屉高度 + 多出来的那段间距。
//
// 为什么不是 759（2026-11-04 修）：
// 原来写死 759，而抽屉只有 175 ⇒ 窗口多长出来的 299px 里，
// 只有 175 给了抽屉，**剩下 124px 全被清单卡片与进度区之间那段弹性留白吃掉**。
// 后果有两样，都是客户一眼能看出来的：
//   ① 进度条（以及底部按钮带）整体下移 124px —— 点一下日志，界面像被"抻"了；
//   ② 卡片和进度区之间出现一大片空白。
// 截图实测：展开前进度区紧贴卡片下方，展开后中间空了 280px（物理）。
//
// 改成「收起高度 + 抽屉高度 + 多出的间距」= 647 之后：
//   · 抽屉正好占满多出来的 187px ⇒ 那段留白**一点不变**；
//   · 进度区、底部按钮带的位置**完全不动**，只有窗口底边往下长 187px；
//   · 顺带修好 768p 笔记本：可用高 728 < 759，原来展开日志时底边会掉出屏幕、
//     抽屉被裁；647 ≤ 728，现在整块放得下。
const int kMainHeightLogOpen = kMainHeight + kLogDrawerHeight + kPanelSpacing;
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
// 日志抽屉在给定可用高下能开多高。
//
// 为什么需要它：抽屉原来固定 175px。屏幕可用高不足「460 + 175 + 12」时，
// 窗口长不到那么高，抽屉只能去挤布局 —— 表现就是**进度条整体上移**
// （实测 availH=600 移 47px = 187-140；availH=460 移 148px 且抽屉越出窗口
// 压住进度卡）。这不是"环境限制"，是产品该降级的地方。
// 规则：抽屉最多占「窗口比收起态多出来的那部分」减去间距，其余保持不动。
//
// 2026-11-05 修：加了**正下限** kLogDrawerMin。
// 原来这里是 `qMax(0, ...)` ⇒ 可用高 ≤ 471 时返回 **0** ⇒
// 抽屉 0px ⇒ 用户点「同步日志」**什么都不会发生**。
// 那个区间在自检的屏幕矩阵里就有（853×480@150%，可用高 440）——
// 自检当时只判"窗口不越界"，没判"抽屉是不是看得见"，所以没人发现。
//
// 现在：抽屉至少 kLogDrawerMin 高，多出来的那部分从**表格**那里借
// （见 kTableMinWithLog 与 SyncPanel::setLogDrawerHeight）。
inline int logDrawerHeight(int availH)
{
    const int shut  = qMax(kMainHeightMin, qMin(kMainHeight, availH));
    const int open  = qMax(kMainHeightMin, qMin(kMainHeightLogOpen, availH));
    const int extra = qMax(0, open - shut);          // 窗口能长多高
    return qMax(kLogDrawerMin, qMin(kLogDrawerHeight, extra - kPanelSpacing));
}
inline QSize mainWindow(int availW, int availH, bool logOpen)
{
    // 展开目标 647 > 收起目标 460，两者都先卡下界 320、再按可用高收敛，
    // 所以**展开高度天然 ≥ 收起高度**（展开永不比收起矮）。
    //
    // 这个不变量已由自检锁住（--selfcheck 的 screen-matrix 六档判据里带 noShrink=1），
    // 不在这里再加一层 qMax —— 那层永远不生效，是死代码
    // （2026-11-05 审查实测：新旧实现 3100 万组合 diff=0）。
    //
    // 另：availH < 647 时窗口长不到「460 + 175 + 12」，抽屉必然挤占布局，
    // 此时"点日志进度区不动"物理上做不到 —— 该区间由 G18b 判据 SKIP，
    // 不能对外表述成"小屏也修好了"。
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
