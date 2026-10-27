#include "StatusPillDelegate.h"

#include <QPainter>
#include <QApplication>
#include <QFontMetrics>
#include <QAbstractItemModel>
#include "FileTableModel.h"
#include "Theme.h"

namespace {

struct Pill {
    QColor fg, bg, border;
    bool mono;      // 原型只有「同步中」用等宽：它后面跟着实时百分比，要对齐跳动
    bool pulse;     // 同步中带一个 5px 的呼吸点
};

// 配色全部取自 Theme 的 chipXxx()，不在本文件里写死：
// 语义色（哪一档表示什么、底/字/边各是什么色）归 Theme 管，
// 这里只负责"这个状态用哪一档"。
//
// AP500BJ 深色版（ui_preview.html 的 .tag-*）：
//   同步中 底#133465 字#6db0ff 边#1f6fe0
//   已同步 底#0b3625 字#4ade80 边#059669
//   失败   底#5a171d 字#fca5a5 边#dc2626
//   待同步 底#1a2438 字#8fa0c0 边#2b3a57
//   已跳过 底#10393b 字#6fd6de 边#1f6b70
// Theme 那一层是**唯一**色源：改配色只改 Theme.h。
Pill pillOf(FileTableModel::FileState st)
{
    Pill p = { Theme::chipWait().fg, Theme::chipWait().bg, Theme::chipWait().border, false, false };
    switch (st) {
    case FileTableModel::StateOk:
        p = { Theme::chipOk().fg, Theme::chipOk().bg, Theme::chipOk().border, false, false };
        break;
    case FileTableModel::StateSyncing:
        p = { Theme::chipSyncing().fg, Theme::chipSyncing().bg, Theme::chipSyncing().border, true, true };
        break;
    case FileTableModel::StateFailed:
        p = { Theme::chipFailed().fg, Theme::chipFailed().bg, Theme::chipFailed().border, false, false };
        break;
    case FileTableModel::StateSkipped:
        p = { Theme::chipSkipped().fg, Theme::chipSkipped().bg, Theme::chipSkipped().border, false, false };
        break;
    case FileTableModel::StateCanceled:
        p = { Theme::chipCanceled().fg, Theme::chipCanceled().bg, Theme::chipCanceled().border, false, false };
        break;
    case FileTableModel::StateWait:
    default:
        p = { Theme::chipWait().fg, Theme::chipWait().bg, Theme::chipWait().border, false, false };
        break;
    }
    return p;
}

} // namespace

StatusPillDelegate::StatusPillDelegate(QObject *parent)
    : QStyledItemDelegate(parent)
{
}

void StatusPillDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option,
                               const QModelIndex &index) const
{
    painter->save();
    const FileTableModel *model = qobject_cast<const FileTableModel *>(index.model());
    const QString text = index.data(Qt::DisplayRole).toString();

    // 行底色：选中 / 失败红底 / 普通。
    // 三种必须在**每一列**上一致 —— 这一列是自绘的，原来固定刷 panelBg，
    // 于是失败行的红底被这一列和工具列冲掉，红底断成一截一截。
    QColor rowBg = Theme::panelBg();
    if (option.state & QStyle::State_Selected)
        rowBg = Theme::rowSelectedBg();
    else if (model && model->fileState(index.row()) == FileTableModel::StateFailed)
        rowBg = Theme::rowAlertBg();
    painter->fillRect(option.rect, rowBg);

    if (!model || text.isEmpty()) {
        painter->restore();
        return;
    }

    const Pill p = pillOf(model->fileState(index.row()));

    QFont f = QApplication::font();
    f.setPixelSize(11);
    f.setWeight(QFont::DemiBold);
    if (p.mono) {
        const QString mono = Theme::resolvedMono();
        if (!mono.isEmpty())
            f.setFamily(mono);
    }
    const QFontMetrics fm(f);
    const int padH = 7;                       // 原型 padding: 1px 7px
    const int gap  = p.pulse ? 4 : 0;         // 原型 gap: 4px
    const int dot  = p.pulse ? 5 : 0;
    // Qt 5.9 的 QFontMetrics 只有 width()；horizontalAdvance 是 5.11 才有的。
    const int h = fm.height() + 2;            // 原型上下 padding 各 1px

    // 85px 是 SyncPanel 锁死的列宽（手册 §6.3）。文案一变长（"同步中 45%" + 后缀
    // 名很长的工具名），药丸就会顶进隔壁的「工具调用」列 —— 而那一列的按钮
    // 是 delegate 现画的，谁后画谁盖，最后出现的就是"按钮少一块"的怪样子。
    // 所以这里先按列宽裁文本，再算药丸宽度，保证它永远待在自己那一格里。
    //
    // 裁出来的 shown 必须一路用到 drawText。原先这里算完 shown 就扔了，
    // 最后画的是**原文 text**：药丸按省略号后的宽度缩好了，画的却是没缩过的字，
    // 于是 Qt 在窄矩形里硬裁 —— 长文案变成"半个字突然没了"，连省略号都没有，
    // 而上面那段防串列的注释等于白写。
    QString shown = text;
    const int maxText = option.rect.width() - padH * 2 - gap - dot;
    if (maxText > 0 && fm.width(shown) > maxText)
        shown = fm.elidedText(shown, Qt::ElideRight, maxText);
    const int drawW = fm.width(shown) + padH * 2 + gap + dot;

    QRect pill(option.rect.center().x() - drawW / 2, option.rect.center().y() - h / 2, drawW, h);

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setPen(QPen(p.border, 1));
    painter->setBrush(p.bg);
    // AP500BJ 定稿：状态标签是**直角**（ui_preview.html 的 .tag 是 2px），
    // 不是胶囊。这里原来画的是 h/2 全圆角 —— 一排药丸压在深色表格里像一串气泡，
    // 和 AP500BJ「直角外框 + 发丝分隔」的整体语言对不上。
    painter->drawRoundedRect(pill.adjusted(0, 0, -1, -1), 2.0, 2.0);

    QRect textR(pill);
    textR.setLeft(textR.left() + padH + dot + gap);
    textR.setRight(textR.right() - padH);
    painter->setFont(f);
    painter->setPen(p.fg);
    painter->drawText(textR, Qt::AlignVCenter | Qt::AlignLeft, shown);

    if (p.pulse) {
        painter->setPen(Qt::NoPen);
        // AP500BJ：呼吸点用「同步中」徽章的亮字色 #6db0ff。
        // 底色是 #133465（深蓝），原来的 #2563eb 压在上面几乎看不出在动。
        painter->setBrush(QColor(0x6d, 0xb0, 0xff));
        painter->drawEllipse(QPointF(pill.left() + padH + 2.5, pill.center().y() + 0.5), 2.5, 2.5);
    }
    painter->restore();
}

QSize StatusPillDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    Q_UNUSED(option);
    Q_UNUSED(index);
    // 宽度由 SyncPanel 锁死 85px，这里只保证不因为药丸把行撑高。
    return QSize(85, 35);
}
