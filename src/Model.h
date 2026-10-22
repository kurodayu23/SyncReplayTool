#pragma once

#include <QMetaType>
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QFileInfo>

// 目标机器连接信息。keyPath 为空表示用密码登录。
struct TargetConfig
{
    QString host;
    int     port = 22;
    QString user;
    QString keyPath;
    QString password;
    // 配置里存着非空密文、但本机解不开（Windows 换用户/换机器，Linux 换 machine-id）。
    // 这时 password 是空的，却**不是因为用户没设过**。界面据此在口令栏旁边挂常驻提示，
    // 并且在用户真的重新输入之前，saveTarget 不允许把原密文删掉。
    bool    passwordUnreadable = false;
    QString remoteRoot;
    // 本机源目录（定稿 UI 收进了设置弹窗）。**必须落到 ini**：
    // 以前它只在 SyncPanel 内存里，进程一关就没了，用户每次开机都要重填一遍。
    QString localRoot;
    bool    autoAcceptHostKey = true;

    bool isValid() const { return !host.isEmpty() && !user.isEmpty(); }
    bool useKey()     const { return !keyPath.isEmpty(); }
};

// 一个后缀对应一条落地规则：spe -> remoteRoot/spe
struct SyncRule
{
    QString suffix;        // 不含点，小写
    QString remoteSubDir;  // 相对 remoteRoot，为空表示直接放根目录
};

// 后缀 -> 本地可执行工具。argsTemplate 里用 {file} 代表选中文件的绝对路径。
//
// 定稿方案：一个后缀可以配**多个**工具（spe 配「回放工具」+「分析工具」），
// 所以这里保留旧的单工具字段（兼容已存的 ini），同时用 tools 承载多条。
// exePath/argsTemplate 非空时视为 tools 之前的那一条，读旧配置不会被丢掉。
struct ToolRule
{
    QString suffix;
    QString exePath;
    QString argsTemplate;

    // 同一后缀的第 2..N 个工具。新版界面只用这个。
    QVector<QString> toolNames;
    QVector<QString> toolExePaths;
    QVector<QString> toolArgs;

    // 这里原来按后缀名硬编码工具数量和中文名（spe→2 个「回放工具」「分析工具」、
    // iq→「IQ解析」、h5→「查看数据」、dat→0 个），删掉了。
    //
    // 危害不是"占位"，是它排在 !exePath.isEmpty() **之前**，于是读旧格式 ini
    // （有 suffix + exePath、无 toolNames）时：
    //   toolCount() 对 spe 返回 2，而实际只有 1 个工具
    //     → 表格的「工具调用」列凭空多画一个按钮，点下去报「工具不存在：<空>」
    //   nameAt(0) 返回硬编码的「回放工具」，而不是用户真实配置的工具名
    //     → 用户配的名字被一个假的显示名顶掉，且他改不掉
    // 界面上它看起来像"程序已经知道该用什么工具打开这个文件"，
    // 实际是编造出来的。这与 FileScanner 里伪造文件大小是同一类问题。
    //
    // 现在只按实际配置计数：配了几个就是几个，一个都没配就是 0。
    int toolCount() const
    {
        int n = toolNames.size();
        if (n > 0)
            return n;
        if (!exePath.isEmpty())
            return 1;
        return 0;
    }

    QString nameAt(int i) const
    {
        if (i >= 0 && i < toolNames.size())
            return toolNames.at(i);
        if (i == 0 && !exePath.isEmpty()) {
            QString base = QFileInfo(exePath).completeBaseName();
            return base.isEmpty() ? exePath : base;
        }
        return QString();
    }

    QString exeAt(int i) const
    {
        if (i >= 0 && i < toolExePaths.size())
            return toolExePaths.at(i);
        if (i == 0 && !exePath.isEmpty())
            return exePath;
        return QString();
    }

    QString argsAt(int i) const
    {
        if (i >= 0 && i < toolArgs.size())
            return toolArgs.at(i);
        if (i == 0 && !exePath.isEmpty())
            return argsTemplate;
        return QString();
    }
};

struct FileItem
{
    QString   relativePath;  // 相对本地根目录，统一用 '/'
    QString   suffix;
    qint64    size = 0;
    QDateTime modified;
    QDateTime created;       // 创建时间。定稿表格要单独一列，FileScanner 原来不采。

    QString name() const { return relativePath.section('/', -1); }
};

// FileItem 要跨线程传（目录扫描在后台线程跑，结果回到界面线程），
// 必须注册成元类型，否则 Qt 的排队连接会**直接把这个信号丢掉**：
// 界面上表现为点了"扫描"没反应、永远停在"扫描中"。
Q_DECLARE_METATYPE(FileItem)
