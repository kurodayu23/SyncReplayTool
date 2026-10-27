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
enum Variant { VarBlue = 0, VarSlate, VarGreen, VarMore, VarFail, VarNone };

struct Skin {
    QColor bg, border, fg, hoverBg, hoverFg, hoverBorder;
};

// AP500BJ 深色工控配色，直接照 ui_preview.html 的 .btn-tool 取值。
//
// 这里原来分浅/深两套值：深色一套是 #1e3a8a/#2563eb 那种高饱和蓝，
// 浅色一套是翻面的浅底深字。AP500BJ 定稿只有一套外观，
// 所以这里**不再分模式** —— 免得任何一条分支漏改就把浅色值漏回界面。
//
// 工具按钮是卡片内的次级控件：底 #1a253e / 边 #24324d / 字 #9cc2f7，
// 悬停才是主蓝实底（.btn-tool:hover）。
Skin skinOf(int v)
{
    Skin s;
    switch (v) {
    case VarSlate:
        s.bg = QColor(0x1a, 0x25, 0x3e); s.border = QColor(0x24, 0x32, 0x4d);
        s.fg = QColor(0xc4, 0xd3, 0xeb);
        s.hoverBg = QColor(0x23, 0x31, 0x4f); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x3a, 0x4b, 0x6e);
        break;
    case VarGreen:
        s.bg = QColor(0x0b, 0x36, 0x25); s.border = QColor(0x05, 0x96, 0x69);
        s.fg = QColor(0x4a, 0xde, 0x80);
        s.hoverBg = QColor(0x05, 0x96, 0x69); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x10, 0xb9, 0x81);
        break;
    case VarMore:
        // 「更多」：中性折叠入口，和 .btn-log 同族（底 #162036 / 字 #9cb1d4）
        s.bg = QColor(0x16, 0x20, 0x36); s.border = QColor(0x24, 0x32, 0x4d);
        s.fg = QColor(0x9c, 0xb1, 0xd4);
        s.hoverBg = QColor(0x1f, 0x2c, 0x49); s.hoverFg = QColor(0xff, 0xff, 0xff); s.hoverBorder = QColor(0x3a, 0x4b, 0x6e);
        break;
    case VarFail:
        // 失败行的按钮（定稿图 .row-alert 里那行的 .btn-tool 是 #5a171d）。
        s.bg = QColor(0x5a, 0x17, 0x1d); s.border = QColor(0x8b, 0x26, 0x30);
        s.fg = QColor(0xfc, 0xa5, 0xa5);
        s.hoverBg = QColor(0x6b, 0x1d, 0x24); s.hoverFg = Qt::white; s.hoverBorder = QColor(0xef, 0x44, 0x44);
        break;
    case VarBlue:
    default:
        s.bg = QColor(0x1a, 0x25, 0x3e); s.border = QColor(0x24, 0x32, 0x4d);
        s.fg = QColor(0x9c, 0xc2, 0xf7);
        s.hoverBg = QColor(0x1f, 0x6f, 0xe0); s.hoverFg = Qt::white; s.hoverBorder = QColor(0x1f, 0x6f, 0xe0);
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
    if (toolTotal <= 3)
        return toolTotal;   // 3 个以内：全部直接画出来，一个都不藏
    // 超过 3 个：前两个是真按钮，第 3 格是折叠入口「…」。
    return 3;
}

int ToolColumnDelegate::foldSlotIndex(int toolTotal) const
{
    // 折叠格固定在第 3 格（index 2），且**只有**工具超过 3 个时才存在。
    // 3 个工具时第 3 格是货真价实的第 3 个工具按钮，不许变成折叠格 ——
    // 那会把"3 个以内全显示"这条规则悄悄破掉。
    return (toolTotal > 3) ? 2 : -1;
}

// 这一行要画的按钮文字。
//
// 原来固定是「打开」+「更多」，从不显示用户填的工具名。
// 只有一个工具的时候这个选择是**多余**的：这一列整列只有这一个工具，
// 「打开」对用户没有任何信息量 —— 用户配的是「记事本」还是「回放工具」，
// 界面上一个字都看不出来，还得多去设置里对一遍才知道点下去会发生什么。
// 所以：**1 个工具时显示配置里的工具名**。
//
// 多个工具时（2 个 / 3 个）也直接写工具名：
//   - 每个可见按钮对应**一个确定**的工具，点哪个就是哪个，不存在歧义；
//   - 原来 3 个及以上一律写成「打开」+「更多」，等于把客户配的工具名藏起来，
//     用户看一列下来全是「打开」，根本不知道自己配了几个、哪几个。
//
// 超过 3 个时：前两个写工具名，第 3 格是折叠入口「…」，点开 QMenu 选其余的。
// 折叠与否按**个数**判断，不按像素宽度 —— 宽度会随名字变化，
// 同一份配置换个名字就变了行为。
//
// 名字兜底（ToolRule::nameAt 从 exe 文件名兜底）这一路也照常显示：
// 用户没填名字时，给「speViewer」总比给「打开」多一条信息，
// 而这一栏本来就是"这台机器上真实存在的工具"。
//
// names 由调用方**先省略号截断**再传进来（见 fitButtonLabel）：
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
    if (toolTotal <= 3) {
        // 2 个 / 3 个工具：每个都直接写工具名，点哪个调哪个。
        for (int i = 0; i < toolTotal; ++i) {
            const QString n = names.value(i).trimmed();
            labels << (n.isEmpty() ? QObject::tr("\xE6\x89\x93\xE5\xBC\x80") : n);
        }
        return labels;
    }
    // 4 个及以上：前两个写工具名，第 3 格是折叠入口「…」。
    for (int i = 0; i < 2; ++i) {
        const QString n = names.value(i).trimmed();
        labels << (n.isEmpty() ? QObject::tr("\xE6\x89\x93\xE5\xBC\x80") : n);
    }
    labels << QObject::tr("\xE2\x80\xA6");       // …（U+2026 水平省略号）
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

// 折叠入口「…」本身的文字宽度（和按钮同一个字体：11px DemiBold）。
static int foldTextWidth()
{
    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    const QFontMetrics fm(f);
    return fm.width(QString::fromUtf8("\xE2\x80\xA6"));
}

// 按钮文字的**自然宽度**（和 buttonRects 用的是同一个字体，量出来才能对得上）。
static int buttonTextWidth(const QString &s)
{
    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    return QFontMetrics(f).width(s);
}

// 一行里摆 n 个真按钮（总槽位 slotCount，其中可能含一个折叠格「…」）时，
// 每个真按钮分到多宽的文字。
//
// 分配规则是**按自然宽度**，不是平均分：
//   - 短名字（"T1"、两字中文）本来就窄，不该被同行的长名字连累成「…」；
//   - 只有总宽真的装不下时才开始裁，而且先裁最长的那个。
//
// 原来这里是「可用宽 ÷ 名字个数」的平均分，而且还**无条件**扣掉折叠格「…」的宽度：
//   3 个工具那一档根本没有折叠格（第 3 格是货真价实的第 3 个工具），
//   却照样被扣掉一个「…」的宽度 ⇒ 每个按钮少 3~4px ⇒ 两字工具名被裁成「回…」
//   ⇒ 界面上三个按钮全变成省略号（v2.3 自检 tool-button-label 就是这么红的）。
//
// 宽度算错的后果不是"字被截短"，而是**按钮整个消失**：buttonRects 排不下就返回
// 空矩形，空矩形在 paint 里被跳过、在 editorEvent 里被当成"没点中" ——
// 用户看到的是这一格少一个按钮，而且点不动。
static QVector<int> allocateButtonTextWidths(int cellWidth, const QStringList &raw,
                                             int slotCount, int foldW)
{
    const int n = raw.size();
    QVector<int> out;
    if (n <= 0)
        return out;
    if (slotCount <= 1) {
        out << maxSingleButtonTextWidth(cellWidth);
        return out;
    }
    // 可用于文字的总宽：扣掉左右内边距、每个按钮的左右 padding、按钮间距，
    // 以及（只有真的存在折叠格时）折叠格自己占的那一份。
    const int usable = cellWidth - 24;
    int total = usable - 16 * slotCount - 5 * (slotCount - 1) - 1;
    if (slotCount > n)
        total -= foldW;              // 有折叠格才扣它的宽度

    QVector<int> natural;
    int sum = 0;
    for (int i = 0; i < n; ++i) {
        natural << buttonTextWidth(raw.at(i));
        sum += natural.at(i);
    }
    if (sum <= total)
        return natural;              // 全部按自然宽度摆得下 ⇒ 一个都不裁

    // 装不下：先给每个按钮一个平均份额，剩下的额度再补给"还没拿到自然宽度"的那几个。
    const int fair = qMax(12, total / n);
    QVector<int> w;
    int used = 0;
    for (int i = 0; i < n; ++i) {
        w << qMin(natural.at(i), fair);
        used += w.at(i);
    }
    int left = total - used;
    bool progress = true;
    while (left > 0 && progress) {
        progress = false;
        for (int i = 0; i < n && left > 0; ++i) {
            if (w.at(i) < natural.at(i)) {
                // 用 operator[] 而不是 at()：Qt 的 at() 返回 const 引用，
                // 在它上面 ++ 直接编译不过（C3892：不能给常量赋值）。
                ++w[i];
                --left;
                progress = true;
            }
        }
    }
    return w;
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

    // 整组按钮**居中**。
    //
    // 定稿图实测：工具按钮左右余量相等（截图 1349..1407，列 1303..1453），
    // 原来一律从列左边 +6px 起排，贴着左边，和定稿图对不上。
    //
    // 居中是**排完之后整体平移**：「装不装得下」的判断一个字节都不动
    //（排不下仍然留空 = 按钮不画、点不到），只把排好的整组挪到列中间，
    // 这样"判据说装得下、画出去却越列"这类错位不会因为改居中而冒出来。
    int groupL = -1;
    int groupR = -1;
    for (int i = 0; i < out.size(); ++i) {
        if (out.at(i).isNull())
            continue;
        if (groupL < 0)
            groupL = out.at(i).left();
        groupR = out.at(i).right();
    }
    if (groupL >= 0) {
        const int groupW = groupR - groupL + 1;
        // 居中范围用左右各 6px 的对称边距（视觉居中），不是原来 6/18 的不对称留白。
        int nx = cell.left() + 6 + ((cell.width() - 12) - groupW) / 2;
        // 平移后不许越出这一列 —— 更不许压到「同步状态」列上去。
        if (nx + groupW - 1 > right)
            nx = right - groupW + 1;
        if (nx < left)
            nx = left;
        const int shift = nx - groupL;
        if (shift != 0) {
            for (int i = 0; i < out.size(); ++i) {
                if (!out.at(i).isNull())
                    out[i].translate(shift, 0);
            }
        }
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

    // 需要写出工具名的槽位：1 / 2 / 3 个工具时是全部；
    // 4 个及以上时只有前 2 个（第 3 格是折叠入口「…」，没有名字可写）。
    const int named = (total > 3) ? 2 : total;
    // 变量名不能叫 slots —— 那是 Qt 的关键字（Q_SLOTS），展开后这行就成了
    // "const int = visibleButtonCount(total);"，编译期直接报 C2513。
    const int slotCount = visibleButtonCount(total);

    // 每个名字能有多宽：**先**算准，再截断。
    //
    // 裁在 buttonRects 之前是关键：buttonRects 按文字宽排布，
    // 不裁的话超长名字算出超宽按钮，然后因为装不下被整条留空 ——
    // 按钮直接消失，而不是"名字被截断"。用户看到的是"这一格什么都没有"。
    //
    // 宽度按**自然宽度**分配（allocateButtonTextWidths）：
    // 短名字一个都不裁，只有总宽真的装不下才裁最长的那个。
    QStringList raw;
    for (int i = 0; i < named; ++i)
        raw << model->toolNameAt(row, i);
    const QVector<int> widths = allocateButtonTextWidths(cell.width(), raw, slotCount, foldTextWidth());
    QStringList names;
    for (int i = 0; i < raw.size(); ++i)
        names << fitButtonLabel(raw.at(i), widths.value(i, 12));
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
    // AP500BJ：卡片内的按钮是直角（.btn-tool 是 2px），不是 4px 圆角。
    p->drawRoundedRect(r, 2, 2);

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
    // paint() 是 const 的，index.model() 给的是 const 指针，
    // qobject_cast 必须转成 const 目标类型，否则 C2440。
    const FileTableModel *model = qobject_cast<const FileTableModel *>(index.model());
    if (!model)
        model = m_model;
    if (!model) {
        painter->restore();
        return;
    }

    // 行底色：选中 / 失败红底 / 普通。三种要在每一列上一致，
    // 否则失败行的红底被这一列冲掉，红底断成一截一截。
    QColor rowBg = Theme::panelBg();
    if (option.state & QStyle::State_Selected)
        rowBg = Theme::rowSelectedBg();
    else if (model->fileState(index.row()) == FileTableModel::StateFailed)
        rowBg = Theme::rowAlertBg();
    painter->fillRect(option.rect, rowBg);

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

    const int bc   = visibleButtonCount(total);
    const int fold = foldSlotIndex(total);
    // 折叠入口「…」不带 ▾ 字形 —— 原型的下拉感来自 QMenu 的弹出位置，
    // 字体里找一个三角字符反而不统一。
    const QStringList labels = labelsForCell(option.rect, index.row(), model);
    const QVector<QRect> rects = buttonRects(option.rect, labels);

    // 变体分配：真按钮是主操作（蓝），折叠入口「…」是中性入口（灰），
    // 失败行的按钮跟着整行一起变红（定稿图 .row-alert）。
    //
    // 原来还有第三档绿色（"h5 的绿是按后缀给的一档绿"），触发条件是
    // 后缀名等于 h5 或者按钮文字里含「查看数据」—— 后者是**在猜**用户配了什么名字。
    // 按钮既然已经固定成「打开」，按后缀挑颜色更没有任何依据，去掉这一档。
    const bool failedRow = (model->fileState(index.row()) == FileTableModel::StateFailed);
    for (int i = 0; i < bc; ++i) {
        const QRect r = rects.at(i);
        if (r.isNull())
            continue;
        if (i == fold)
            drawButton(painter, r, labels.at(i), VarMore);
        else
            drawButton(painter, r, labels.at(i), failedRow ? VarFail : VarBlue);
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

    const int bc   = visibleButtonCount(total);
    const int fold = foldSlotIndex(total);
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
            if (i == fold)
                // 折叠入口「…」：按原始需求「同一个后缀配置了多个工具时，点击调用会弹出一个菜单
                // 让用户选择用哪一个工具打开」。必须弹菜单，不许偷偷挑第一个。
                //
                // firstHidden = fold = 2：前两个工具已经是各自独立的按钮了，
                // 菜单里只列第 3 个及以后。
                emit toolMoreClicked(index.row(), fold);
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

