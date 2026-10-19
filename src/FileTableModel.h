#pragma once

#include <QAbstractTableModel>
#include <QHash>
#include <QVector>
#include <QMenu>
#include "Model.h"

class QToolButton;

// 待传输文件清单的数据模型。列宽与原型严格一致（手册 §2）：
//
//   1 选择框 36px | 2 文件名 auto | 3 大小 75px | 4 创建时间 90px
//   | 5 修改时间 90px | 6 同步状态 85px | 7 工具调用 156px
//
// 三个必须守住的东西：
//
// 1) **不许 setCellWidget**。那会给每一行真的建一个 QWidget，
//    一万个文件就是一万个子控件，滚动直接卡死、内存直接爆。
//    操作列必须用 delegate 画。
// 2) **表格里不许出现第二条进度条**（手册 §4）。同步状态只用文字药丸徽章。
// 3) **操作列 156px 死锁**。1 个工具显示单按钮，2 个并排，
//    3 个及以上折叠成「首选 + 更多▾」，无论几个工具都不许撑宽这一列。
class FileTableModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        ColCheck = 0,   // 选择框
        ColName,         // 文件名
        ColSize,         // 文件大小
        ColCreated,      // 创建时间
        ColModified,     // 修改时间
        ColStatus,       // 同步状态（文字徽章，不画进度条）
        ColTool,         // 工具调用
        ColumnCount
    };

    // 单个文件的传输状态。枚举值和显示文案必须一一对应，
    // 散开写字符串的话"跑完了但行上还写着同步中"就再也没有一处能查了。
    //
    // StateSkipped 与 StateCanceled 必须是两个状态，不能合并：
    // 前者是「远端本来就有、这次一个字节没碰」（--ignore-existing 的正常结果），
    // 后者是「用户点了取消、根本没传」。合成一个的话，用户取消之后看到满屏
    // 「远端已有」，会以为远端已经同步好了 —— 而实际一个字节都没传。
    // 新值追加在末尾，不改动已有枚举的数值。
    enum FileState { StateWait = 0, StateSyncing, StateOk, StateFailed, StateSkipped, StateCanceled };

    // 一轮同步结束时，队列里剩下的那些行统一落到哪个状态。
    //
    // 单独抽成函数是因为**这里就是接线点**：判定放在调用方（SyncPanel）里、
    // 判据再照抄一份，生产代码把"取消"改回 StateSkipped 时判据照样全绿 ——
    // 这个坑本项目已经踩过一次（判据测了谓词，洞在接线）。
    // 生产路径和 --selfcheck 调的是同一个函数，判据才有牙。
    //
    // canceled 优先于其它一切：用户点了取消，没传完的行就是"没传"。
    static FileState resolveRestState(bool canceled, bool leftoverUntransferred, bool ok);
    // 徽章上的附加文字（例如"同步中 (45%)"），没有就空。
    enum Role {
        StateRole = Qt::UserRole + 1,
        StateNoteRole,
        AbsolutePathRole,
        SuffixRole
    };

    explicit FileTableModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    // ---- 数据 ----
    void setFiles(const QVector<FileItem> &files, const QHash<QString, ToolRule> &toolRules,
                  const QString &localRoot);
    const QVector<FileItem> &files() const { return m_files; }

    // 状态更新走 dataChanged，不整表重建 —— 重建会让滚动位置和选中行乱跳。
    void setFileState(int row, FileState st, const QString &note = QString());
    FileState fileState(int row) const;

    // 行号 <-> 相对路径。传输回调给的是路径，界面要的是行号。
    int rowForRelativePath(const QString &rel) const;
    int checkedCount() const;
    void setAllChecked(bool on);
    QVector<int> checkedRows() const;

    // ---- 后缀分组（Tab 的计数由这里出，不许写死在界面上）----
    QStringList suffixGroups() const;
    int groupTotal(const QString &suffix) const;      // 空 suffix = 全部
    int groupChecked(const QString &suffix) const;
    bool passesFilter(int row, const QString &suffixFilter) const;

    // ---- 工具列（1/2/3+ 三档）----
    // 这两个是 const 的：delegate 的 paint() 本身是 const，
    // 里面 qobject_cast 出来的是 const 指针，非 const 方法链上去直接编译不过。
    int toolsForRow(int row) const;
    QString toolNameAt(int row, int i) const;

signals:
    // 行内工具按钮被点了。toolIndex = -1 表示"更多▾"那个下拉被点开。
    void toolButtonClicked(int row, int toolIndex);

private:
    QVector<FileItem> m_files;
    QHash<QString, ToolRule> m_toolRules;
    QHash<QString, int> m_rowByPath;      // 相对路径 -> 行号
    QVector<FileState> m_states;
    QVector<QString>  m_notes;
    QVector<bool>     m_checked;
    QString m_localRoot;
};
