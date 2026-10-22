#pragma once

#include <QStyledItemDelegate>
#include "FileTableModel.h"

// 「工具调用」列的绘制与点击。
//
// 为什么必须自己画而不是摆真 QPushButton：
// 摆一个真按钮就是 setCellWidget，那会为每一行创建一个 QWidget 子控件。
// 一万个文件就是一万个控件，滚动直接卡死，内存直接爆。
// 这里全部用 painter 画，点击区域自己算。
//
// 三档降级（手册 §6.3，列宽 156px 死锁，绝不撑宽）：
//   1 个工具    -> 一个按钮，舒展占满
//   2 个工具    -> 两个按钮并排
//   3 个及以上  -> 第一个按钮 + 「更多」，点开 QMenu 选其余的
class ToolColumnDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit ToolColumnDelegate(QObject *parent = nullptr);

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override;

    // 基类签名是 const 引用；写成值传递会连不上 override，
    // 编译器报 C3668「没有重写任何基类方法」，而重载解析还会静默挑走。
    bool editorEvent(QEvent *event, QAbstractItemModel *model,
                      const QStyleOptionViewItem &option, const QModelIndex &index) override;

    // ---- 自动化判据用 ----
    //
    // 判据要能拿到**画与点共用的那一份**标签和矩形，而不是照抄一份。
    // 照抄的话，把产品改回「打开」判据照样全绿（自证的判据）。
    QStringList   buttonLabelsForTest(const QRect &cell, int row,
                                      const FileTableModel *model) const;
    QVector<QRect> buttonRectsForTest(const QRect &cell, const QStringList &labels) const;

signals:
    // 点中了该行第 toolIndex 个工具按钮（toolIndex 从 0 起，按实际配置序）。
    void toolButtonClicked(int row, int toolIndex);
    // 点中了「更多」。view 必须弹 QMenu 让用户选，不许在 delegate 里替他挑第一个 ——
    // 原始需求描述：「点击调用会弹出一个菜单让用户选择用哪一个工具打开」。
    void toolMoreClicked(int row, int firstHiddenIndex);

private slots:
    void onButtonClicked(const QModelIndex &index);

private:
    // 返回该行实际要画几个按钮（0 / 1 / 2，其中第 2 个可能是「更多」）。
    int visibleButtonCount(int toolTotal) const;
    // paint() 与 editorEvent() 共用的标签构造。**必须**只有这一份：
    // 两边各写一份的话，改了一边就会出现"画得出来、点不到"。
    QStringList labelsForCell(const QRect &cell, int row, const FileTableModel *model) const;
    // 按钮矩形按**内容宽度**排布，不是均分。
    // 原型 .btn-tool-act 是 inline-flex + padding 0 8px，按钮宽度 = 文字宽 + 16，
    // 所以「IQ解析」(49px) 和「更多」(38px) 宽度并不相等；均分会把「更多」
    // 拉成一个和长名字一样宽的空壳按钮，一眼就假。放不下的按钮返回空矩形。
    QVector<QRect> buttonRects(const QRect &cell, const QStringList &labels) const;
    void drawButton(QPainter *p, const QRect &r, const QString &text, int variant) const;

    FileTableModel *m_model = nullptr;
};
