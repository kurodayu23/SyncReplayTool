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
// 折叠规则（用户 2026-10-27 定；列宽 156px 死锁，绝不撑宽）：
//   1 个工具      -> 一个按钮，舒展占满
//   2 个工具      -> 两个按钮并排
//   3 个工具      -> 三个按钮并排（**不**提前折叠）
//   4 个及以上    -> 前两个按钮 + 第 3 格「…」，点开 QMenu 选其余的
//
// 为什么「3 个以内全显示、超过 3 个才折叠」：
// 同一个后缀配多个工具是现场常见做法（回放 + 转换 + 导出）。
// 早早就把第 3 个藏进菜单，等于让用户每次都要多点一次才能看见自己配了什么，
// 而 3 个按钮在 156px 里是排得下的 —— 没排下再折。
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
    // 点中了折叠格「…」。view 必须弹 QMenu 让用户选，不许在 delegate 里替他挑第一个 ——
    // 原始需求描述：「点击调用会弹出一个菜单让用户选择用哪一个工具打开」。
    void toolMoreClicked(int row, int firstHiddenIndex);

private slots:
    void onButtonClicked(const QModelIndex &index);

private:
    // 返回该行实际要画几个**槽位**（0 / 1 / 2 / 3）。
    // 槽位 ≠ 工具按钮：4 个及以上时，第 3 个槽位是折叠格「…」，
    // 点它弹菜单，而不是直接调用第 3 个工具。
    int visibleButtonCount(int toolTotal) const;
    // 折叠格「…」在第几个槽位（0 起）；没有折叠格时返回 -1。
    // paint() 和 editorEvent() 都必须用这一份判断"这个格是不是折叠格"，
    // 各写一份的话就会出现"画的是「…」、点的是第 3 个工具"。
    int foldSlotIndex(int toolTotal) const;
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
