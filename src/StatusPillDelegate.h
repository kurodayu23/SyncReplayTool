#pragma once

#include <QStyledItemDelegate>

// 「同步状态」列的绘制：药丸徽章。
//
// 为什么不能靠 QSS：QSS 只能给 item 画矩形背景和边框，做不出
// 「按文字宽度自适应 + 9999px 圆角」的胶囊。原型 .file-status-text 就是这个形状
// （padding 1px 7px / radius 9999px / 11px / 600），套 QSS 只能得到一个
// 铺满整列的方块，85px 宽的列里会像一块补丁。
//
// 只装在 ColStatus 一列（setItemDelegateForColumn），其余列不受影响。
class StatusPillDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit StatusPillDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const Q_DECL_OVERRIDE;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const Q_DECL_OVERRIDE;
};
