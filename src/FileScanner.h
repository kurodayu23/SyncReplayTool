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
    // 三个上限（深度 64 / 条目 200 万 / visited 去重）触顶时会把
    // wasTruncated() 置位。调用方必须在扫描结束后查一次并明确告诉用户
    // "结果不完整"，否则就是静默少传 —— 和 junction 环那一类问题一样，
    // 界面不报错、清单里就是少一半。
    //
    // 深度触顶的处置口径（2026-10-24 改）：**只跳过超深的那一棵子树**，
    // 同一趟扫描里的其它分支照常扫完。原来是"触顶就整趟中止"，
    // 于是一条很深的路径会把根目录下其它分支的结果一起吃掉。
    // wasTruncated() 的语义相应变成"结果不完整（有东西没扫到）"。
    // 条目上限（200 万）仍然是整趟中止 —— 那是"再扫下去也没意义"的量级。
    //
    // scan() 是静态函数、没有信号可发，所以标志只能走这个静态访问器。
    // 扫描跑在后台线程，标志用 QAtomicInt 而不是裸 bool。
    static QVector<FileItem> scan(const QString &root, const QStringList &suffixes);

    // 找出"只有大小写不同"的重名文件（同一目录内）。
    //
    // 发送端在 Windows 上时，A.spe 和 a.spe 在磁盘上本来就是同一个文件 ——
    // 后写的把先写的覆盖了，全程没有提示。接收端是 Linux（大小写敏感），
    // 到底该传哪一个、还是两个都传，程序替用户决定不了，只能报出来。
    //
    // 返回相对路径列表（每个冲突目录报一次）。跨目录同名不算冲突。
    static QStringList caseCollisionPaths(const QVector<FileItem> &items);

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

    // 上一次 scan() 跳过的**链接**个数。
    //
    // 含两类：指向文件的符号链接、指向目录的链接（junction / 符号链接目录 /
    // 卷挂载点）。两类都是"链接不是数据"，都被整条跳过。
    // **链接目录原来一个数都不报**（静默丢弃），客户以为那棵目录同步过了；
    // 现在并进同一个计数，界面那一句"有 N 个符号链接没有同步"必然点名。
    //
    // 链接不是数据：rsync 用 -a（含 -l）会把链接**原样重建**到远端，
    // 远端拿到的是一个指向别的机器上路径的链接、不是文件内容 ——
    // 而界面会把它标成「已同步」。
    // 实测（麒麟真机 + 真 rsync 3.1.1）：源目录 link → /outside/secret.spe，
    // 远端落地的是符号链接；把源路径移走后远端读该文件报 No such file or directory。
    //
    // 所以扫描时直接跳过（口径与"链接出去的目录整棵跳过"一致），
    // 但**必须报出个数** —— 静默跳过就是静默丢数据。
    //
    // 读取时机与 wasTruncated() 一样：只有 scan() 会清零，读完就没了。
    static int skippedLinkCount();

    // 上一次 scan() 的**跳过清单**（"跳过必须留痕"，D-1 起）。
    //
    // 每一项是一条已经拼好的中文说明，形如：
    //   因路径过长跳过：D:\data\a\b\....dat（本平台路径上限 260 字符，该路径 312 字符；若它是目录，该目录整棵未扫描）
    //   因目录层级过深跳过：D:\data\a\b（超过 64 层，该目录整棵未扫描）
    //   因是链接目录跳过：D:\data\link（链接不是数据）
    //   因目录读不了跳过：D:\data\x（请检查访问权限）
    //   因越过数据根跳过：D:\data\mnt（挂载到数据根之外的目录，不同步）
    //
    // 为什么要有它：这些条目**本来就不会被同步**，而原来一条提示都没有 ——
    // 客户看到"同步完成，共 N 个文件"就以为全传了，实际少了一部分。
    //
    // 读取时机：只有 scan() 会清零（detectSuffixes 只追加不重置），
    // 所以要在扫描结束之后读。最多返回 50 条，总数看 skippedCount()。
    static QStringList skippedNotes();

    // 上一次 scan() 跳过的条目总数（链接、读不了的目录、超深子树、超长路径都算）。
    static int skippedCount();

    // 其中"因路径过长"的条数（D-1）。
    static int skippedLongPathCount();

    // 每次完整遍历 +1。上层用它判断"这条清单是不是已经播报过了"
    // —— 自动同步一轮一轮跑，每轮都刷一遍同样的清单会把日志刷爆。
    static int scanGeneration();

    // 上一次 scan() 遇到的**读不了**的目录个数（访问权限不足）。
    //
    // 这类目录整棵被跳过，里面的文件不会进清单、不会被同步，
    // 但日志里一个字都没有，最后照样报「同步完成」—— 同样是静默丢数据。
    static int unreadableDirCount();

private:
    static void setTruncated();
};
