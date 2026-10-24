#include "FileTableModel.h"

#include <QApplication>
#include <QLocale>
#include <QBrush>
#include <QColor>
#include <QFont>
#include <QFontMetrics>
#include "Theme.h"

namespace {
// 界面文案一律走这里。**不能用 QStringLiteral 写 \xNN 转义**：
// Qt5 的 QStringLiteral 展开成 u"..."（char16_t 字面量），"\xE6" 会被当成
// 单个 U+00E6 码元而不是 UTF-8 字节，于是界面上全是「按 Latin-1 逐字节拆开」
// 的乱码（实测表头 7 列、状态徽章全部中招）。fromUtf8 才是按字节序列解码。
QString U(const char *s) { return QString::fromUtf8(s); }

// 大小列的单位右对齐到 2 字符宽（等宽字体下才真正齐）。
// 单位长度 B=1 / KB=MB=GB=2，整串右对齐时"B"会贴着右缘、"MB"占满两格，
// 同一列里数字的起始位置和单位的起始位置都对不齐，扫一列看着是波浪。
// 这里把单位**右**补空格：数字右缘对齐的同时，"B"落在"M/B"的那一格，
// 整列的右缘和单位列都对得上。左边补无效——那会让短单位贴右缘。
QString unit2(const char *s) { return QString::fromLatin1(s).rightJustified(2, QLatin1Char(' ')); }

// 状态徽章的 tooltip：说清"这一行到底是什么意思"。
//
// 为什么加（2026-10-24）：徽章那一格只有状态名，
// 完成 / 跳过 / 失败都不带原因，用户只能去日志里翻。
// 而「远端已有」（--ignore-existing 的正常结果）尤其容易被误读成
// "远端已经是最新的"—— 勾了「仅同步新增文件」时，
// 远端内容与本机不同**照样**是「远端已有」、照样不会被覆盖，
// 这件事界面上一个字都没有（台账 e2e-scale-log-ui-audit.md 的 F-05）。
//
// **只加 tooltip**：徽章文字、配色、85px 死锁列宽一个都不动
// （手册 §6.3 承诺的版式；StatusPillDelegate 对超宽文案会截成省略号）。
// 台账 next-batch-fixlist.md 的 U4 推荐的"最省事且零风险的版本"
// 就是这一条：原因短码进 tooltip，徽章文字不变。
QString statusToolTip(FileTableModel::FileState st)
{
    switch (st) {
    case FileTableModel::StateOk:
        return U("\u672C\u8F6E\u8FD9\u4E2A\u6587\u4EF6\u5DF2\u5904\u7406\u5B8C\uFF1A\u8981\u4E48\u6210\u529F\u4F20\u8F93\u5230\u4E86\u8FDC\u7AEF\uFF0C"
               "\u8981\u4E48\u8FDC\u7AEF\u672C\u6765\u5C31\u662F\u6700\u65B0\u7684\u3002");
    case FileTableModel::StateFailed:
        return U("\u672C\u8F6E\u8FD9\u4E2A\u6587\u4EF6\u6CA1\u6709\u4F20\u8F93\u6210\u529F\uFF0C\u5931\u8D25\u539F\u56E0\u89C1\u65E5\u5FD7\u3002");
    case FileTableModel::StateSkipped:
        return U("\u8FDC\u7AEF\u5DF2\u5B58\u5728\u540C\u540D\u6587\u4EF6\uFF0C\u672C\u8F6E\u6CA1\u6709\u4F20\u8F93\u5B83\u3002"
               "\u52FE\u9009\u300C\u4EC5\u540C\u6B65\u65B0\u589E\u6587\u4EF6\u300D\u65F6\u8FDC\u7AEF\u5185\u5BB9\u4E0D\u4F1A\u88AB\u8986\u76D6\uFF1B"
               "\u5982\u679C\u8FDC\u7AEF\u5185\u5BB9\u4E0E\u672C\u673A\u4E0D\u540C\uFF0C\u4E5F\u4E0D\u4F1A\u81EA\u52A8\u66F4\u6B63\u3002");
    case FileTableModel::StateCanceled:
        return U("\u672C\u8F6E\u88AB\u53D6\u6D88\uFF0C\u8FD9\u4E2A\u6587\u4EF6\u6CA1\u6709\u4F20\u8F93\u3002");
    case FileTableModel::StateSyncing:
        return U("\u6B63\u5728\u4F20\u8F93\u8FD9\u4E2A\u6587\u4EF6\u3002");
    case FileTableModel::StateWait:
    default:
        return U("\u672C\u8F6E\u8FD8\u6CA1\u6709\u5F00\u59CB\u4F20\u8F93\u8FD9\u4E2A\u6587\u4EF6\u3002");
    }
}
}

FileTableModel::FileTableModel(QObject *parent)
    : QAbstractTableModel(parent)
{
}

int FileTableModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_files.size();
}

int FileTableModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

FileTableModel::FileState FileTableModel::resolveRestState(bool canceled,
                                                            bool leftoverUntransferred,
                                                            bool ok)
{
    if (canceled)
        return StateCanceled;
    if (leftoverUntransferred)
        return StateSkipped;     // 远端本来就有，本次一个字节没碰
    return ok ? StateOk : StateFailed;
}

QVariant FileTableModel::data(const QModelIndex &index, int role) const
{
    const int row = index.row();
    if (row < 0 || row >= m_files.size())
        return QVariant();

    const FileItem &f = m_files.at(row);

    switch (role) {
    case Qt::CheckStateRole:
        // 选择框是独立的第 0 列，勾选逻辑走 dataChanged，不碰文件状态。
        if (index.column() == ColCheck)
            return m_checked.value(row) ? Qt::Checked : Qt::Unchecked;
        return QVariant();

    case AbsolutePathRole:
        return m_localRoot.isEmpty() ? f.relativePath
                                     : m_localRoot + QLatin1Char('/') + f.relativePath;

    case SuffixRole:
        return f.suffix;

    case StateRole:
        return static_cast<int>(m_states.value(row));

    case StateNoteRole:
        return m_notes.value(row);

    case Qt::DisplayRole: {
        switch (index.column()) {
        case ColCheck:
            return QVariant();
        case ColName:
            return f.name();
        case ColSize: {
            // 原型是 "24.5 MB" 这种一位小数。qint64 在 32 位上装不下 TB，
            // 这里先转 double 再格式化，避免手写除法在大文件上溢出。
            // 单位走 unit2() 补齐，别在这里直接 "%1 MB" —— 见 unit2 上方注释。
            const double bytes = static_cast<double>(f.size);
            if (bytes < 1024.0)
                return QObject::tr("%1 %2").arg(bytes, 0, 'f', 0).arg(unit2("B"));
            if (bytes < 1024.0 * 1024.0)
                return QObject::tr("%1 %2").arg(bytes / 1024.0, 0, 'f', 1).arg(unit2("KB"));
            if (bytes < 1024.0 * 1024.0 * 1024.0)
                return QObject::tr("%1 %2").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1).arg(unit2("MB"));
            return QObject::tr("%1 %2").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1)
                .arg(unit2("GB"));
        }
        case ColCreated:
            // 手册 §9.3.3：时间列 **100% 保留原有时间**。
            // 换成同步时间会让频谱时序回放错乱，二次增量判定也跟着失效。
            return f.created.isValid() ? f.created.toString(QStringLiteral("MM-dd HH:mm"))
                                      : QString();
        case ColModified:
            return f.modified.isValid() ? f.modified.toString(QStringLiteral("MM-dd HH:mm"))
                                        : QString();
        case ColStatus: {
            FileState st = m_states.value(row);
            switch (st) {
            case StateSyncing: {
                const QString note = m_notes.value(row);
                // 文案就是「同步中」三个字，**不带 ●**。
                // 原型是 <span class="pulse-dot"></span>同步中 —— 那个点是一个
                // 独立的 5px 元素（gap:4px），不是文字里的字符。
                // 这里原来在文字里塞了一个 U+25CF，而 StatusPillDelegate 又会
                // 画一个脉冲点，于是界面上是**两个点**，药丸也比原型宽了十几像素，
                // 在 85px 死锁列宽里会顶到隔壁列。
                return note.isEmpty() ? U("\xE5\x90\x8C\xE6\xAD\xA5\xE4\xB8\xAD")
                                      : U("\xE5\x90\x8C\xE6\xAD\xA5\xE4\xB8\xAD ") + note;
            }
            case StateOk:
                return U("\xE2\x9C\x93 \xE5\xB7\xB2\xE5\xAE\x8C\xE6\x88\x90");  // ✓ 已完成
            case StateFailed:
                return U("\xE2\x9D\x8C \xE5\xA4\xB1\xE8\xB4\xA5");                // ✕ 失败
            case StateSkipped:
                return U("\xE8\xBF\x9C\xE7\xAB\xAF\xE5\xB7\xB2\xE6\x9C\x89");    // 远端已有
            case StateCanceled:
                // 和「远端已有」必须分开：这一行是用户取消的结果，一个字节都没传。
                return U("\xE5\xB7\xB2\xE5\x8F\x96\xE6\xB6\x88");              // 已取消
            case StateWait:
            default:
                return U("\xE5\xBE\x85\xE4\xBC\xA0\xE8\xBE\x93");                // 待传输
            }
        }
        case ColTool:
            // 操作列不写字：按钮由 delegate 画，写了文字会和按钮重叠。
            return QVariant();
        default:
            return QVariant();
        }
    }

    // 徽章配色。按状态给药丸背景色，和 QSS 里的 statusXxx 保持同一组色。
    case Qt::BackgroundRole: {
        if (index.column() != ColStatus)
            return QVariant();
        FileState st = m_states.value(row);
        switch (st) {
        case StateSyncing: return QColor(0x1e, 0x3a, 0x8a);
        case StateOk:      return QColor(0x06, 0x4e, 0x3b);
        case StateFailed:  return QColor(0x7f, 0x1d, 0x1d);
        case StateSkipped: return QColor(0x13, 0x4e, 0x4a);
        case StateCanceled: return QColor(0x78, 0x35, 0x0f);
        default:           return QColor(0x2a, 0x30, 0x3c);
        }
    }
    case Qt::ForegroundRole: {
        if (index.column() == ColStatus) {
            FileState st = m_states.value(row);
            switch (st) {
            case StateSyncing: return QColor(0x60, 0xa5, 0xfa);
            case StateOk:      return QColor(0x34, 0xd3, 0x99);
            case StateFailed:  return QColor(0xf8, 0x71, 0x71);
            case StateSkipped: return QColor(0x2d, 0xd4, 0xbf);
            case StateCanceled: return QColor(0xfb, 0xbf, 0x24);
            default:           return QColor(0x94, 0xa3, 0xb8);
            }
        }
        if (index.column() == ColSize || index.column() == ColCreated || index.column() == ColModified)
            return Theme::textSub();
        return Theme::textMain();
    }
    case Qt::FontRole: {
        // 基线必须取应用字体，不能 QFont() 空构造：空构造出来的是
        // 平台默认字体，pixelSize/weight 全部对不上 QSS 设的那一套。
        QFont f = QApplication::font();
        // 原型：.td-size 等宽 + 次文字色；.td-ctime/.td-mtime 等宽 11px #64748b；
        //       .file-name-text 等宽 12px 500 主文字色；.td-status **不是**等宽。
        if (index.column() == ColSize) {
            const QString mono = Theme::resolvedMono();
            if (!mono.isEmpty())
                f.setFamily(mono);
        } else if (index.column() == ColCreated || index.column() == ColModified) {
            const QString mono = Theme::resolvedMono();
            if (!mono.isEmpty())
                f.setFamily(mono);
            f.setPixelSize(11);
            f.setWeight(QFont::Normal);
        } else if (index.column() == ColName) {
            const QString mono = Theme::resolvedMono();
            if (!mono.isEmpty())
                f.setFamily(mono);
            f.setPixelSize(12);
            f.setWeight(QFont::Medium);
        }
        return f;
    }
    case Qt::TextAlignmentRole: {
        switch (index.column()) {
        case ColSize:    return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        case ColStatus:  return Qt::AlignCenter;
        case ColCreated:
        case ColModified:return Qt::AlignCenter;
        default:         return static_cast<int>(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }
    case Qt::ToolTipRole:
        if (index.column() == ColName || index.column() == ColTool)
            return data(index, AbsolutePathRole);
        // 状态徽章也带 tooltip：徽章本身只有状态名（85px 死锁列宽放不下原因），
        // 原因放进悬停提示里 —— 见上面 statusToolTip() 的说明。
        if (index.column() == ColStatus)
            return statusToolTip(m_states.value(row));
        return QVariant();

    default:
        return QVariant();
    }
}

bool FileTableModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    const int row = index.row();
    if (row < 0 || row >= m_files.size())
        return false;
    if (role == Qt::CheckStateRole && index.column() == ColCheck) {
        m_checked[row] = (value.toInt() == Qt::Checked);
        emit dataChanged(index, index, QVector<int>() << Qt::CheckStateRole);
        return true;
    }
    return false;
}

QVariant FileTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return QVariant();
    if (orientation == Qt::Horizontal) {
        switch (section) {
        case ColCheck:    return QString();  // 表头画全选框，不写字
        case ColName:     return U("\xE6\x96\x87\xE4\xBB\xB6\xE5\x90\x8D");                    // 文件名
        case ColSize:     return U("\xE6\x96\x87\xE4\xBB\xB6\xE5\xA4\xA7\xE5\xB0\x8F");        // 文件大小
        case ColCreated:  return U("\xE5\x88\x9B\xE5\xBB\xBA\xE6\x97\xB6\xE9\x97\xB4");        // 创建时间
        case ColModified: return U("\xE4\xBF\xAE\xE6\x94\xB9\xE6\x97\xB6\xE9\x97\xB4");        // 修改时间
        case ColStatus:   return U("\xE5\x90\x8C\xE6\xAD\xA5\xE7\x8A\xB6\xE6\x80\x81");        // 同步状态
        case ColTool:     return U("\xE5\xB7\xA5\xE5\x85\xB7\xE8\xB0\x83\xE7\x94\xA8");        // 工具调用
        default:          return QVariant();
        }
    }
    return QVariant();
}

Qt::ItemFlags FileTableModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags f = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (index.column() == ColCheck)
        f |= Qt::ItemIsUserCheckable;
    // 文件名列可双击触发工具调用，所以额外给 ItemIsEditable 之外的编辑权限不用开，
    // 用 view 的 doubleClicked 信号处理，保持模型干净。
    return f;
}

void FileTableModel::setFiles(const QVector<FileItem> &files,
                              const QHash<QString, ToolRule> &toolRules,
                              const QString &localRoot)
{
    beginResetModel();
    m_files = files;
    m_toolRules = toolRules;
    m_localRoot = localRoot;
    m_rowByPath.clear();
    m_states.clear();
    m_notes.clear();
    m_checked.clear();
    m_states.resize(files.size());
    m_notes.resize(files.size());
    m_checked.resize(files.size());
    for (int i = 0; i < files.size(); ++i) {
        m_rowByPath.insert(files.at(i).relativePath, i);
        m_states[i] = StateWait;
        m_checked[i] = true;      // 默认全勾，和原型表头复选框的 checked 一致
    }
    endResetModel();
}

void FileTableModel::setFileState(int row, FileState st, const QString &note)
{
    if (row < 0 || row >= m_files.size())
        return;
    if (m_states.value(row) == st && m_notes.value(row) == note)
        return;                   // 同一状态重复刷不重绘，否则传输期会疯狂重画整列
    m_states[row] = st;
    m_notes[row] = note;
    emit dataChanged(index(row, ColStatus), index(row, ColStatus),
                     QVector<int>() << Qt::DisplayRole << Qt::BackgroundRole
                                    << Qt::ForegroundRole << StateRole << StateNoteRole);
}

FileTableModel::FileState FileTableModel::fileState(int row) const
{
    return m_states.value(row, StateWait);
}

int FileTableModel::rowForRelativePath(const QString &rel) const
{
    return m_rowByPath.value(rel, -1);
}

int FileTableModel::checkedCount() const
{
    int n = 0;
    for (int i = 0; i < m_checked.size(); ++i)
        if (m_checked.at(i))
            ++n;
    return n;
}

void FileTableModel::setAllChecked(bool on)
{
    if (m_checked.isEmpty())
        return;
    for (int i = 0; i < m_checked.size(); ++i)
        m_checked[i] = on;
    emit dataChanged(index(0, ColCheck), index(m_checked.size() - 1, ColCheck),
                     QVector<int>() << Qt::CheckStateRole);
}

QVector<int> FileTableModel::checkedRows() const
{
    QVector<int> rows;
    for (int i = 0; i < m_checked.size(); ++i)
        if (m_checked.at(i))
            rows.append(i);
    return rows;
}

QStringList FileTableModel::suffixGroups() const
{
    QStringList keys;
    keys.append(QString());          // 空 = 全部
    for (int i = 0; i < m_files.size(); ++i) {
        const QString s = m_files.at(i).suffix;
        if (!s.isEmpty() && !keys.contains(s))
            keys.append(s);
    }
    return keys;
}

int FileTableModel::groupTotal(const QString &suffix) const
{
    int n = 0;
    for (int i = 0; i < m_files.size(); ++i)
        if (suffix.isEmpty() || m_files.at(i).suffix == suffix)
            ++n;
    return n;
}

int FileTableModel::groupChecked(const QString &suffix) const
{
    int n = 0;
    for (int i = 0; i < m_files.size(); ++i)
        if ((suffix.isEmpty() || m_files.at(i).suffix == suffix) && m_checked.value(i))
            ++n;
    return n;
}

bool FileTableModel::passesFilter(int row, const QString &suffixFilter) const
{
    if (row < 0 || row >= m_files.size())
        return false;
    if (suffixFilter.isEmpty())
        return true;
    return m_files.at(row).suffix == suffixFilter;
}

int FileTableModel::toolsForRow(int row) const
{
    if (row < 0 || row >= m_files.size())
        return 0;
    return m_toolRules.value(m_files.at(row).suffix).toolCount();
}

QString FileTableModel::toolNameAt(int row, int i) const
{
    if (row < 0 || row >= m_files.size())
        return QString();
    return m_toolRules.value(m_files.at(row).suffix).nameAt(i);
}
