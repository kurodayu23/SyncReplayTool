#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include "Model.h"

// 「这个目录里到底有哪些后缀」的统计结果。
// 后缀为空串表示**无后缀文件**（比如 README、Makefile）—— 它们同样是用户数据，
// 不能因为取不到后缀就当看不见，所以单列一类而不是直接丢掉。
struct SuffixStat
{
    QString suffix;
    int     fileCount = 0;
    qint64  totalBytes = 0;
    QString sampleFile;   // 样例相对路径，让用户一眼认出这是什么东西
};

class FileScanner
{
public:
    // 深度遍历 root，suffixes 为空表示不过滤后缀。结果里的 relativePath 统一用 '/'。
    //
    // 三个上限（深度 64 / 条目 200 万 / visited 去重）触顶时会**提前停止**并
    // 把 wasTruncated() 置位。调用方必须在扫描结束后查一次并明确告诉用户
    // "结果不完整"，否则就是静默少传 —— 和 junction 环那一类问题一样，
    // 界面不报错、清单里就是少一半。
    //
    // scan() 是静态函数、没有信号可发，所以标志只能走这个静态访问器。
    // 扫描跑在后台线程，标志用 QAtomicInt 而不是裸 bool。
    static QVector<FileItem> scan(const QString &root, const QStringList &suffixes);

    // 反向能力：先扫一遍，把目录里实际存在的后缀统计出来。
    //
    // 为什么需要它：scan() 必须先拿到 suffixes 才能扫，可 suffixes 来自用户配置。
    // 于是客户第一次用就卡在一个死结上 —— 他得先知道自己的数据是什么后缀，
    // 才能配出规则，才能扫出文件。但"数据是什么后缀"本来就是这软件该告诉他的。
    // 没有这一步，客户只能去资源管理器里一个个数后缀再回来填。
    //
    // 排除口径必须和 scan() 逐条一致（.srt-filelist-*.txt、链接目录、三道上限、
    // 尾点归一化）。两边一旦不对称，用户会看到"识别出 5 种后缀、
    // 但按规则扫只出来 3 种"这种对不上的界面，而且没有任何报错。
    //
    // 返回按文件数降序。wasTruncated() 的语义与 scan() 一致。
    static QVector<SuffixStat> detectSuffixes(const QString &root);

    // 上一次 scan()/detectSuffixes() 是否因为触顶而提前停止。每次调用开头会清零。
    static bool wasTruncated();

private:
    static void setTruncated();
};
