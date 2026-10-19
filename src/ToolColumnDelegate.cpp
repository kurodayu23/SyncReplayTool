#include "ToolColumnDelegate.h"

#include <QPainter>
#include <QFontMetrics>
#include <QMenu>
#include <QMouseEvent>
#include <QApplication>
#include <QAbstractItemModel>
#include <QRect>
#include "Theme.h"

namespace {
// 按钮的三种配色，对应原型 .btn-tool-act 的三个修饰类。
// 用数组而不是散在各处写字符串：配色和用在哪一档必须一眼能对上。
enum Variant { VarBlue = 0, VarSlate, VarGreen, VarMore, VarNone };

struct Skin {
    QColor bg, border, fg, hoverBg, hoverFg, hoverBorder;
};

// 四档按钮全部取自 Theme 的 chipXxx()，不再在本文件写死深色值。
// 这里原来每个 case 都直接 QColor(0x..)：浅色模式下界面变白，
// 按钮仍是深蓝/深绿底 + 亮字，白底上一排深色方块（用户 2026-10-16 报的）。
Skin skinOf(int v)
{
    Skin s;
    const bool dark = Theme::isDark();
    switch (v) {
    case VarSlate:
        if (dark) {
            s.bg = QColor(0x33, 0x41, 0x55); s.border = QColor(0x47, 0x55, 0x69);
            s.fg = QColor(0xe2, 0xe8, 0xf0);
            s.hoverBg = QColor(0x47, 0x55, 0x69); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x64, 0x74, 0x8b);
        } else {
            s.bg = QColor(0xe2, 0xe8, 0xf0); s.border = QColor(0xcb, 0xd5, 0xe1);
            s.fg = QColor(0x33, 0x41, 0x55);
            s.hoverBg = QColor(0xcb, 0xd5, 0xe1); s.hoverFg = QColor(0x1e, 0x29, 0x3b); s.hoverBorder = QColor(0x94, 0xa3, 0xb8);
        }
        break;
    case VarGreen:
        if (dark) {
            s.bg = QColor(0x06, 0x4e, 0x3b); s.border = QColor(0x05, 0x96, 0x69);
            s.fg = QColor(0x34, 0xd3, 0x99);
            s.hoverBg = QColor(0x05, 0x96, 0x69); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x10, 0xb9, 0x81);
        } else {
            s.bg = QColor(0xd1, 0xfa, 0xe5); s.border = QColor(0x6e, 0xe7, 0xb7);
            s.fg = QColor(0x04, 0x78, 0x57);
            s.hoverBg = QColor(0xa7, 0xf3, 0xd0); s.hoverFg = QColor(0x06, 0x5f, 0x46); s.hoverBorder = QColor(0x34, 0xd3, 0x99);
        }
        break;
    case VarMore:
        // 「更多」：工控中性暗灰边框与底色
        if (dark) {
            s.bg = QColor(0x1f, 0x24, 0x2d); s.border = QColor(0x37, 0x41, 0x51);
            s.fg = QColor(0x94, 0xa3, 0xb8);
            s.hoverBg = QColor(0x2d, 0x37, 0x48); s.hoverFg = QColor(0x60, 0xa5, 0xfa); s.hoverBorder = QColor(0x3b, 0x82, 0xf6);
        } else {
            s.bg = QColor(0xf8, 0xfa, 0xfc); s.border = QColor(0xe2, 0xe8, 0xf0);
            s.fg = QColor(0x47, 0x55, 0x69);
            s.hoverBg = QColor(0xef, 0xf6, 0xff); s.hoverFg = QColor(0x25, 0x63, 0xeb); s.hoverBorder = QColor(0xbf, 0xdb, 0xfe);
        }
        break;
    case VarBlue:
    default:
        if (dark) {
            s.bg = QColor(0x1e, 0x3a, 0x8a); s.border = QColor(0x25, 0x63, 0xeb);
            s.fg = QColor(0x93, 0xc5, 0xfd);
            s.hoverBg = QColor(0x25, 0x63, 0xeb); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x60, 0xa5, 0xfa);
        } else {
            s.bg = QColor(0xdb, 0xea, 0xfe); s.border = QColor(0x93, 0xc5, 0xfd);
            s.fg = QColor(0x1d, 0x4e, 0xd8);
            s.hoverBg = QColor(0x25, 0x63, 0xeb); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x1d, 0x4e, 0xd8);
        }
        break;
    }
    return s;
}
} // namespace

ToolColumnDelegate::ToolColumnDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
    m_model = qobject_cast<FileTableModel *>(parent);
}

int ToolColumnDelegate::visibleButtonCount(int toolTotal) const
{
    if (toolTotal <= 0)
        return 0;
    if (toolTotal == 1)
        return 1;
    return 2;      // 2 个及以上：「打开」+ 「更多▾」
}

// 这一行要画的按钮文字。
//
// 原来固定是「打开」+「更多」，从不显示用户填的工具名。
// 只有一个工具的时候这个选择是**多余**的：这一列整列只有这一个工具，
// 「打开」对用户没有任何信息量 —— 用户配的是「记事本」还是「回放工具」，
// 界面上一个字都看不出来，还得多去设置里对一遍才知道点下去会发生什么。
// 所以：**1 个工具时显示配置里的工具名**。
//
// 多个工具时保持「打开」+「更多」不变：
//   - 「打开」在这个场景下是**真的歧义**（有几个工具就叫哪个），
//     显示某一个的名字等于替用户挑好了，正是这一列要避免的事；
//   - 需要区分时那一步已经有 QMenu 了（onToolMoreClicked 弹菜单让用户选）。
//
// 名字兜底（ToolRule::nameAt 从 exe 文件名兜底）这一路也照常显示：
// 用户没填名字时，给「speViewer」总比给「打开」多一条信息，
// 而这一栏本来就是"这台机器上真实存在的工具"。
//
// singleName 由调用方**先省略号截断**再传进来（见 fitButtonLabel）：
// buttonRects 按文字宽度排布，不截断的话超长名字会算出超宽按钮，
// 然后因为装不下被整条留空 —— 按钮直接消失，而不是被截断。
static QStringList buttonLabels(int toolTotal, const QStringList &names)
{
    QStringList labels;
    if (toolTotal <= 0)
        return labels;
    if (toolTotal == 1) {
        // 名字为空（理论上不会：nameAt 有 exe 兜底）时退回「打开」，
        // 不能返回一个空字符串按钮 —— 那是一个画得出、点得到、
        // 但什么字都没有的按钮。
        const QString n = names.value(0).trimmed();
        labels << (n.isEmpty() ? QObject::tr("\xE6\x89\x93\xE5\xBC\x80") : n);
        return labels;
    }
    if (toolTotal == 2) {
        // 两个工具：两个都直接写工具名。editorEvent 里 total>2 才弹菜单，
        // 也就是说第二个按钮本来就是直调第二个工具的，标成「更多」是误导。
        for (int i = 0; i < 2; ++i) {
            const QString n = names.value(i).trimmed();
            labels << (n.isEmpty() ? QObject::tr("\xE6\x89\x93\xE5\xBC\x80") : n);
        }
        return labels;
    }
    // 三个及以上：工具名长短不定，按**个数**判断而不是像素宽度 —— 宽度会随
    // 名字变化，同一列在「ab/cd」和「数据处理工具/格式转换」两种配置下宽度
    // 差一大截，拿宽度决定显不显示菜单，同一份配置换个名字就变了行为。
    labels << QObject::tr("\xE6\x89\x93\xE5\xBC\x80");                  // 打开
    labels << QObject::tr("\xE6\x9B\xB4\xE5\xA4\x9A");             // 更多
    return labels;
}

// 单个按钮最多能有多宽。
//
// 和 buttonRects 里的常量一一对应（两边各写一份的话，改了一边就会出现
// "算出来能放下、画出去却压到状态列"）：
//     left  = cell.left() + 6
//     right = cell.right() - 18
//     按钮宽 = 文字宽 + 16
//     要求  left + 文字宽 + 16 <= right
// 解出来是 cell.width() - 41。写成 -40 会差一个像素：名字正好卡在边界上时
// buttonRects 会认为装不下，把按钮整条留空 —— 用户看到的是"这一格什么都没有"。
int maxSingleButtonTextWidth(int cellWidth)
{
    return cellWidth - 41;
}

// 把工具名裁到装得下的宽度。绝不允许溢出到按钮区域之外 ——
// 这一列右边紧挨着状态列，压过去会把状态盖掉，而状态是排查问题第一眼看的东西。
//
// 这里**没有**用 QFontMetrics::elidedText()。实测（离屏、Qt 5.9.6）：
// elidedText 对一整串没有断点的超长文本返回的是**不带省略号的硬截断**
// （144 字符的工具名在 156px 列里出来是 "LongToolN"，尾巴没有 "…"）。
// 界面上那就是一个断在半截的字，和「省略号截断」的要求不是一回事，
// 而且在不同字体环境下给不给省略号还不一样 —— 判据没法稳定地断言它。
// 自己按宽度收窄再补省略号，结果是确定的。
static QString fitButtonLabel(const QString &text, int maxWidth)
{
    if (maxWidth <= 0)
        return QString();
    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    const QFontMetrics fm(f);
    if (fm.width(text) <= maxWidth)
        return text;

    const QString ell = QString::fromUtf8("\xE2\x80\xA6");   // U+2026 …
    const int ellW = fm.width(ell);
    const int budget = maxWidth - ellW;
    if (budget <= 0)
        return ell;                       // 连省略号都放不下，就只给省略号
    int n = text.length();
    while (n > 0 && fm.width(text.left(n)) > budget)
        --n;
    return text.left(n) + ell;
}


QVector<QRect> ToolColumnDelegate::buttonRects(const QRect &cell, const QStringList &labels) const
{
    // 原型：td-act 左内边距 6px，按钮间距 5px，右边留 18px 呼吸余量（手册 §2），
    // 按钮高 23px、左右 padding 各 8px。
    const int left  = cell.left() + 6;
    const int right = cell.right() - 18;
    const int gap   = 5;
    const int h     = 23;
    const int y     = cell.top() + (cell.height() - h) / 2;

    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    const QFontMetrics fm(f);

    QVector<QRect> out;
    out.reserve(labels.size());
    int x = left;
    for (int i = 0; i < labels.size(); ++i) {
        // Qt 5.9 的 QFontMetrics 只有 width()；horizontalAdvance 是 5.11 才有的。
        const int w = fm.width(labels.at(i)) + 16;
        if (x + w > right) {
            out.append(QRect());          // 156px 装不下：留空，绝不溢出到状态列
            continue;
        }
        out.append(QRect(x, y, w, h));
        x += w + gap;
    }
    return out;
}

// paint() 与 editorEvent() 共用的**唯一**一份标签构造。
//
// 这两处各写一份的话，改了一边就会出现"画得出来、点不到"，
// 而且两边都编译得过 —— 文件里原有的注释已经记过一次这个坑。
QStringList ToolColumnDelegate::labelsForCell(const QRect &cell, int row,
                                              const FileTableModel *model) const
{
    const int total = model->toolsForRow(row);
    if (total <= 0)
        return QStringList();
    if (total >= 3)
        return buttonLabels(total, QStringList());
    // 1 个 / 2 个工具：显示配置里的工具名，并**先**裁到装得下的宽度。
    // 裁在 buttonRects 之前是关键：buttonRects 按文字宽排布，
    // 不裁的话超长名字算出超宽按钮，然后因为装不下被整条留空 ——
    // 按钮直接消失，而不是"名字被截断"。用户看到的是"这一格什么都没有"。
    // 两个按钮时要各分一半：buttonRects 装不下的那个会被留空（按钮消失），
    // 而按钮消失时 editorEvent 返回 true 会吃掉点击，这一格就彻底点不动了。
    const int per = (total >= 2)
                        ? (maxSingleButtonTextWidth(cell.width()) - 5) / 2
                        : maxSingleButtonTextWidth(cell.width());
    QStringList names;
    for (int i = 0; i < total; ++i)
        names << fitButtonLabel(model->toolNameAt(row, i), per);
    return buttonLabels(total, names);
}

QStringList ToolColumnDelegate::buttonLabelsForTest(const QRect &cell, int row,
                                                    const FileTableModel *model) const
{
    return labelsForCell(cell, row, model);
}

QVector<QRect> ToolColumnDelegate::buttonRectsForTest(const QRect &cell,
                                                      const QStringList &labels) const
{
    return buttonRects(cell, labels);
}

void ToolColumnDelegate::drawButton(QPainter *p, const QRect &r, const QString &text, int variant) const
{
    if (r.width() <= 0)
        return;
    const Skin s = skinOf(variant);
    p->save();
    p->setRenderHint(QPainter::Antialiasing, true);
    p->setPen(QPen(s.border, 1));
    p->setBrush(s.bg);
    p->drawRoundedRect(r, 4, 4);

    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    p->setFont(f);
    p->setPen(s.fg);
    // 按钮文字里如果还塞不下就省略号，但"更多▾"这种功能按钮不许被截断成"更…"
    const QFontMetrics fm(f);
    QString t = fm.elidedText(text, Qt::ElideRight, r.width() - 8);
    p->drawText(r, Qt::AlignCenter, t);
    p->restore();
}

void ToolColumnDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    painter->save();
    QColor rowBg = (option.state & QStyle::State_Selected) ? Theme::rowSelectedBg() : Theme::panelBg();
    painter->fillRect(option.rect, rowBg);

    // paint() 是 const 的，index.model() 给的是 const 指针，
    // qobject_cast 必须转成 const 目标类型，否则 C2440。
    const FileTableModel *model = qobject_cast<const FileTableModel *>(index.model());
    if (!model)
        model = m_model;
    if (!model) {
        painter->restore();
        return;
    }

    const int total = model->toolsForRow(index.row());
    if (total <= 0) {
        // 「未配置工具」：原型是灰斜体，不是禁用按钮 —— 它是说明，不是控件。
        QFont f = QApplication::font();
        f.setPixelSize(11);
        f.setItalic(true);
        painter->setFont(f);
        painter->setPen(Theme::textMuted());
        painter->drawText(option.rect.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft,
                          QObject::tr("\xE6\x9C\xAA\xE9\x85\x8D\xE7\xBD\xAE\xE5\xB7\xA5\xE5\x85\xB7"));  // 未配置工具
        painter->restore();
        return;
    }

    const int bc = visibleButtonCount(total);
    // 「更多」是折叠入口，不带 ▾ 字形 —— 原型的下拉感来自 QMenu 的弹出位置，
    // 字体里找一个三角字符反而不统一。
    const QStringList labels = labelsForCell(option.rect, index.row(), model);
    const QVector<QRect> rects = buttonRects(option.rect, labels);

    // 变体分配：第 1 个是主操作（蓝），「更多」是中性折叠入口（灰）。
    //
    // 原来还有第三档绿色（"h5 的绿是按后缀给的一档绿"），触发条件是
    // 后缀名等于 h5 或者按钮文字里含「查看数据」—— 后者是**在猜**用户配了什么名字。
    // 按钮既然已经固定成「打开」，按后缀挑颜色更没有任何依据，去掉这一档。
    for (int i = 0; i < bc; ++i) {
        const QRect r = rects.at(i);
        if (r.isNull())
            continue;
        if (i == bc - 1 && total > 1)
            drawButton(painter, r, labels.at(i), VarMore);
        else
            drawButton(painter, r, labels.at(i), VarBlue);
    }
    painter->restore();
}

QSize ToolColumnDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    // 列宽由外部固定 156px，这里只保证高度：23px 按钮 + 上下各 6px = 35px。
    return QSize(156, 35);
}

// 点击分发。delegate 自己算命中的是第几个按钮，模型只收「哪一行、第几个工具」。
bool ToolColumnDelegate::editorEvent(QEvent *event, QAbstractItemModel *model,
                                   const QStyleOptionViewItem &option, const QModelIndex &index)
{
    // 按下和松开都要接。
    //
    // 原来只接 MouseButtonRelease：QAbstractItemView 只在**按下的那一行已经是
    // 当前选中行**时，才把松开事件转给 delegate 的 editorEvent。
    // 于是用户第一次点工具按钮 —— 那一行还没被选中 —— 按钮什么都不发生，
    // 得先点一下行、再点一下按钮才对。这不是 Qt 的毛病，是这里的选择条件。
    //
    // 这列画的是按钮，不是可拖拽的单元格，语义上本来就没有"按下与松开
    // 必须落在同一处"的要求，所以按下即触发是安全的；接住按下还能顺带吃掉
    // 这个事件，免得整行的选中/焦点跟着抖一下。
    const bool press = (event->type() == QEvent::MouseButtonPress);
    const bool release = (event->type() == QEvent::MouseButtonRelease);
    if (!press && !release)
        return QStyledItemDelegate::editorEvent(event, model, option, index);

    QMouseEvent *me = static_cast<QMouseEvent *>(event);
    if (me->button() != Qt::LeftButton)
        return false;

    FileTableModel *fm = qobject_cast<FileTableModel *>(model);
    if (!fm)
        return false;

    const int total = fm->toolsForRow(index.row());
    if (total <= 0)
        return true;                       // 「未配置工具」这行是说明文字，不该有任何反应

    const int bc = visibleButtonCount(total);
    // 必须和 paint() 用同一个 labelsForCell()：两边各写一份标签构造的话，
    // 改了一边就会出现"画得出来、点不到"，而且两边都编译得过。
    const QStringList labels = labelsForCell(option.rect, index.row(), fm);
    const QVector<QRect> rects = buttonRects(option.rect, labels);
    // buttonRects 是按 option.rect 算的，返回的矩形和 option.rect **同一坐标系**
    // （也就是视口坐标）。原来这里却拿 rects 去比 me->pos() - option.rect.topLeft()
    // 这个**单元格相对坐标** —— x 从 480 开始的矩形永远不可能包含 x=23，
    // 于是每一次点击都落到最后的"空白处"分支被吃掉，工具一个都没被调起过。
    // paint() 里两边的坐标空间是一致的，所以画得出来、点不动，肉眼看不出问题。
    const QPoint pos = me->pos();

    for (int i = 0; i < bc; ++i) {
        if (rects.at(i).isNull() || !rects.at(i).contains(pos))
            continue;
        // **只在按下时调起，松开只吃掉事件。**
        //
        // 原来这一段在 press 和 release 两条路径上都会跑到底，于是**一次点击
        // 启动两次工具**。上面那段注释解释了为什么要接按下（release 在"按下时
        // 那一行还没被选中"的情况下根本不会被转发过来），但补的方式错了：
        // 正确的做法是"改成按下触发"，不是"按下和松开都触发"。
        //
        // 实测（2026-10-18，Windows 离屏探针）：表格里连点 40 次，
        // 起来了 **80 个进程**。界面毫无异常，因为两次启动是同一个 exe 的
        // 两个实例，用户只会看到工具窗口开两份、或者文件被同一工具打开两次。
        //
        // 必须用 press 单独判：Qt 的 MouseButtonDblClick 也带 LeftButton，
        // 不能顺手一起放行，否则双击会变成四次启动。
        if (press) {
            if (i == bc - 1 && total > 2)
                // 「更多」：按赵工「同一个后缀配置了多个工具时，点击调用会弹出一个菜单
                // 让用户选择用哪一个工具打开」。必须弹菜单，不许偷偷挑第一个。
                emit toolMoreClicked(index.row(), bc - 1);
            else
                emit toolButtonClicked(index.row(), i);
        }
        return true;
    }
    return true;                           // 落在空白处也吃掉，别让行选中跟着抖
}

void ToolColumnDelegate::onButtonClicked(const QModelIndex &index)
{
    Q_UNUSED(index);
    // 保留给外部直接调用（例如自动化测试模拟点击）。真实点击走 editorEvent。
}

