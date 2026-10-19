#include "FileScanner.h"

#include <QDir>
#include <QFileInfo>
#include <QAtomicInt>
#include <QHash>
#include <QPair>
#include <QSet>
#include <QVector>

#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#endif

#ifdef Q_OS_LINUX
#  include <QFile>
#endif

namespace {

// 上一次扫描是否因触顶提前停止。见 FileScanner.h 的说明。
QAtomicInt g_truncated(0);

// 判断一个子目录是不是"链接出去的目录"，是就整棵跳过。
//
// 实测（本轮文件系统边界压测，组 8）：
//   本地数据根目录下放一个指向外部目录的 junction，外部目录里有 500 个 .spe，
//   扫描直接把 FILECOUNT 扫成 501，整棵外部目录被复制到远端（远端共 501 个文件）。
//
// 根因是 Qt 版本：
//   - QDirIterator 的 Subdirectories **默认不跟随符号链接**，但它认不出 junction。
//   - Windows 的 junction（目录联接）在 Win32 层就是一个带
//     FILE_ATTRIBUTE_REPARSE_POINT 的"目录"，Qt 5.9 的 QFileInfo/QDir
//     把它当普通目录（isJunction() 是 Qt 5.13 才加的），于是照常递归进去。
//   - 更糟的是环状 junction（指回祖先目录）会一路递归到 MAX_PATH 为止，
//     界面上表现为"扫描卡死"，而远端会收到一堆用户根本没打算传的目录。
//
// 所以这里不用 QDirIterator，改成自己走，并显式判 reparse point。
bool isLinkedDir(const QString &path)
{
#ifdef _WIN32
    // FILE_ATTRIBUTE_REPARSE_POINT 同时覆盖 junction / 符号链接 / 卷挂载点，
    // 这些都不该被当成数据目录跟进。
    const DWORD attr = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT);
#else
    return QFileInfo(path).isSymLink();
#endif
}

#ifdef Q_OS_LINUX
// Linux 上 bind mount / overlay mount 的挂载点集合。
//
// 为什么需要它（实测，WSL 上有真实 mount 权限的环境里验证过）：
//   mount --bind /var/lib/other /data/inner
// 之后 /data/inner 里的 50 个文件会出现在"朴素递归"的结果里 ——
// 也就是**数据根目录外部的数据被卷进清单，然后由 rsync 传到远端**。
//
// 而下面这些手段全都认不出 bind mount：
//   - QFileInfo::isSymLink()：bind mount 不是符号链接
//   - st_dev / 设备号：实测 bind mount 前后**完全相同**
//     （data 与 data/probe 的 fs 类型、st_dev 全部一致）
//   - 内核的环检测：那只能挡住"挂载点指回祖先"形成的硬环，
//     挡不住"挂载点指向一棵完全无关的外部目录"
//
// 唯一能认出来的是 /proc/self/mountinfo：内核为每一个挂载点写一条记录。
// 它属于 /proc/self（当前进程自己的挂载命名空间），权限 0444，
// **普通用户可读**，不需要 root。
//
// 之前有一轮把这条判成"不适用，扫描在 Windows 侧"。那个结论只对 Windows 版成立：
// 本产品正在编的麒麟 x86 客户端里 m_localRoot 就是 Linux 路径，
// 运维用 mount --bind 固定数据盘是常见写法，那正是命中场景。
QSet<QString> linuxMountPoints()
{
    static QSet<QString> s_points;
    static bool s_loaded = false;
    if (s_loaded)
        return s_points;
    s_loaded = true;

    QFile f(QStringLiteral("/proc/self/mountinfo"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return s_points;

    // 必须一次性 readAll，不能用 `while (!f.atEnd())` 逐行读。
    //
    // /proc 下的文件 st_size 恒为 0，而 QFile::atEnd() 是拿 size() 判出来的 ——
    // 刚 open 完它就返回 true，于是下面那个循环**一次都不进**，s_points 永远是空集，
    // crossesMountPoint() 恒 false，整条 bind mount 防护等于没写。
    //
    // 当时那次实测的三个数：size()=0、atEnd()=TRUE、readAll()=3257 字节 / 28 个挂载点。
    // QTextStream 换成按行 readLine() 也一样失败，同一个原因。
    //
    // QByteArray::split 不看 size()，所以整块读进来再切行才是对的。
    const QByteArray all = f.readAll();
    if (all.isEmpty())
        return s_points;
    // No second argument here on purpose.  QByteArray::split gained a
    // SplitBehavior parameter only in Qt 6, and the 5.14-era spelling
    // (Qt::SkipEmptyParts) does not exist in 5.9.6 at all -- so this line has
    // failed to compile on every supported toolchain until now.
    //
    // Dropping the flag costs nothing: a blank line yields an empty QByteArray,
    // indexOf(" - ") returns -1 and the loop skips it, which is exactly what
    // SkipEmptyParts would have done.  It also keeps the line valid on Qt 5.9,
    // 5.14 and 6.x without a version #if.
    const QList<QByteArray> lines = all.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i);
        // 格式：id parent major:minor root mountpoint options... - fstype source superopts
        // 挂载点是分隔符 " - " **之前**的第 5 个字段。
        const int sep = line.indexOf(" - ");
        if (sep < 0)
            continue;
        const QList<QByteArray> head = line.left(sep).split(' ');
        if (head.size() < 5)
            continue;
        // 内核把路径里的空格/制表/反斜杠/方括号写成八进制转义。
        QByteArray mp = head.at(4);
        mp.replace("\\040", " ");
        mp.replace("\\011", "\t");
        mp.replace("\\134", "\\");
        mp.replace("\\135", "]");
        s_points.insert(QDir::fromNativeSeparators(QString::fromUtf8(mp)));
    }
    return s_points;
}

// 这个目录是不是"越过数据根、进入了另一个挂载点"。
//
// 精确匹配挂载点，不做前缀匹配：用户把 /data 整个选为数据根时，
// /data 本身就是挂载点，但它必须放行；只有**根以下**的挂载点才挡。
// 前缀匹配会把 /data 本身也挡掉，界面上表现为"配了目录却一个文件都扫不到"。
bool crossesMountPoint(const QString &dirPath, const QString &basePath)
{
    const QString norm = QDir::fromNativeSeparators(QDir(dirPath).absolutePath());
    const QString base = QDir::fromNativeSeparators(QDir(basePath).absolutePath());
    if (norm == base)
        return false;
    return linuxMountPoints().contains(norm);
}
#endif

// 这个子目录该不该整棵跳过（符号链接 / junction / 越过挂载点）。
// 两处扫描（scan 与 detectSuffixes）必须走同一个判断，否则
// "表格里有几个文件"和"页签怎么分组"会给出两套答案。
bool shouldSkipDir(const QString &dirPath, const QString &basePath)
{
    if (isLinkedDir(dirPath))
        return true;
#ifdef Q_OS_LINUX
    if (crossesMountPoint(dirPath, basePath))
        return true;
#endif
    Q_UNUSED(basePath)
    return false;
}

} // namespace

QVector<FileItem> FileScanner::scan(const QString &root, const QStringList &suffixes)
{
    QVector<FileItem> items;

    // 空串必须直接拒掉，不能交给 QDir。
    //
    // QDir("") 的语义是"当前工作目录"，exists() 返回 true、absolutePath()
    // 给出进程 cwd —— 于是 scan("") 会把**程序启动时所在的那个目录**
    // 整棵扫一遍。界面上 scanFiles() 前面恰好有一道 isDir() 检查挡住了，
    // 所以这个坑平时踩不到；但只要有一处直接调 scan()（探针、将来的脚本、
    // 任何新调用方），就会得到一份"凭空多出来的文件清单"，
    // 而每个文件名看起来都完全正常。
    if (root.trimmed().isEmpty())
        return items;

    const QDir base(QDir::toNativeSeparators(root));
    if (!base.exists())
        return items;

    QStringList wanted;
    for (const QString &s : suffixes) {
        QString low = s.toLower().trimmed();
        // 后缀归一化：界面和配置文件里都可能存着带前导点的 ".spe"，
        // 而 QFileInfo::suffix() 返回的是不带点的 "spe"。
        while (low.startsWith(QLatin1Char('.')))
            low.remove(0, 1);
        // 尾点也必须去，而且必须在**规则侧**去。
        // 文件侧早就做了（见下面 QString raw 那段），规则侧漏了，于是：
        //   规则填 "spe."  ->  wanted 里是 "spe."
        //   文件 "a.spe"    ->  文件侧算出 "spe"
        //   wanted.contains("spe") 为假 -> **一个文件都不匹配**，界面报"未匹配到文件"
        // 现象和 P53、"traildot." 那一类完全一样：界面上不报错、清单里就是没有。
        // 规则和文件两侧的归一化必须逐字对称，缺一边就静默漏文件。
        while (low.endsWith(QLatin1Char('.')) && low.length() > 1)
            low.chop(1);
        if (!low.isEmpty() && !wanted.contains(low))
            wanted.append(low);
    }

    const QString basePath = base.absolutePath();

    // 每次扫描开头清一次，标志只反映**本次**扫描有没有触顶。
    g_truncated.storeRelease(0);

    // 手动深度优先。栈元素 = (目录绝对路径, 相对 basePath 的深度)。
    //
    // 为什么必须显式计深度、还要 visited 集合 —— Windows junction 和 Linux
    // symlink 都被 isLinkedDir() 挡掉了，但 **bind mount / overlay mount
    // 既不是 symlink 也不是 reparse point**，它认不出来。运维用
    //   mount --bind /data /data/sub/loop
    // 固定数据盘是常见写法，于是目录图里出现环。
    // 环一旦形成：栈永远不空、内存持续上涨、扫描永不结束；更糟的是
    // SyncPanel 析构里的 m_scanJob->wait() 会**卡住关窗**，用户只能强杀。
    //
    // 三道闸任一生效就能终止：visited 按已访问路径去重（挡自引用与软环）、
    // 深度上限 64 层（挡挂载环 —— Windows MAX_PATH 260 根本到不了这么深，
    // 正常数据目录也远不到）、条目上限 200 万（挡"合法但大到没边"的目录）。
    QVector<QPair<QString, int>> stack;
    stack.append(qMakePair(basePath, 0));
    QSet<QString> visited;
    visited.insert(QDir::fromNativeSeparators(basePath));

    const int kMaxDepth = 64;
    const int kMaxEntries = 2000000;
    bool stopAll = false;

    while (!stack.isEmpty() && !stopAll) {
        const QPair<QString, int> top = stack.takeLast();
        const QString dir = top.first;
        const int depth = top.second;
        if (depth > kMaxDepth) {
            // 到顶了就整棵停掉，而不是继续往下走 —— 继续走就等于没设上限。
            setTruncated();
            stopAll = true;
            break;
        }
        const QDir d(dir);

        // 一次 entryInfoList 拿全，比 QDirIterator 少很多 stat 调用；
        // QDir::Name 排序保证遍历顺序稳定（推送时逆序，弹出时就是字典序）。
        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);

        QStringList subDirs;
        subDirs.reserve(entries.size());

        for (const QFileInfo &fi : entries) {
            if (fi.isDir()) {
                // 链接出去的目录、以及越过数据根进入另一个挂载点的目录，整棵跳过。
                // 后者在 Linux 上就是 bind mount：数据根里挂一棵外部目录进来，
                // 外部文件会被卷进清单再由 rsync 传到远端（数据泄漏）。
                if (!shouldSkipDir(fi.absoluteFilePath(), basePath))
                    subDirs.append(fi.absoluteFilePath());
                continue;
            }
            if (!fi.isFile())
                continue;

            // 同步期间 rsync 会把临时文件清单写在本地根目录内、以
            // .srt-filelist- 开头。它不是用户数据：不排除的话，规则后缀命中
            // txt 时它会被扫进自己的传输列表（上一个进程残留的清单还会在
            // 下一次同步时被当成普通文件传走），远端多出垃圾文件。
            // 口径必须和**写入方**以及**清理方**完全一致：
            //   写入 RsyncRunner 写的是 .srt-filelist-<pid>.txt
            //   清理 RsyncRunner 的 sweepStaleListFiles 也是按 .txt glob 的
            // 这里原来只判前缀，于是用户真实存在的 .srt-filelist-报告.txt
            // （规则后缀命中 txt）会被当成残留清单**永远跳过**，
            // 且界面上零提示。少排一个自己写进产品名的文件，
            // 代价是这个文件名永远同步不出去。
            if (fi.fileName().startsWith(QLatin1String(".srt-filelist-"))
                && fi.fileName().endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)) {
                continue;
            }

            // 文件侧必须和规则侧做完全一样的归一化（第 63 行 toLower().trimmed()）。
            //
            // 之前这里只有 toLower()，两边不对称，实测（本轮规则矩阵）：
            // 数据采集程序写出 `data_20240101.spe `（文件名尾部带一个空格，字节
            // 以 0x20 结尾；某些 Windows SDK / 老程序会这么干），用户按正常习惯
            // 把规则填成 `spe` —— 规则侧 trim 成 "spe"，文件侧却是 "spe "，
            // wanted.contains("spe ") 为假，**这个文件被静默漏掉**。
            // 界面上不报错、不标黄、清单里就是没有它，用户只会以为"今天没数据"。
            //
            // 同理规则填 `spe ` 也不行：规则侧被 trim 成 "spe"，同样对不上。
            // 所以两边必须对称 trim，两种写法才都能匹配上。
            //
            // 注意这里 trim 的只是用于匹配的 suffix 字段，relativePath 里的
            // 真实文件名（含空格）原样保留，传到 rsync 的仍是完整真名。
            // 提取用于规则匹配的后缀。语义 = "最后一个 ASCII 点之后的部分"，
            // 与 QFileInfo::suffix() 一致，但补了它的一个坑。
            //
            // QFileInfo::suffix() 对 "traildot.spe." 返回**空字符串** —— 末尾那个点
            // 后面什么都没有。于是 wanted=["spe"] 时它永远匹配不上，这个文件被静默
            // 漏掉（实测 g2-special：磁盘上 32 个文件，扫描只回来 31 个）。
            // 现象和 P53 一样：界面上不报错、清单里就是没有，用户以为"今天没数据"。
            //
            // 文件名以点结尾时，先去掉尾部的点再取后缀，于是 "traildot.spe." 得到
            // "spe"，与用户直觉一致。只影响"文件名以点结尾"这一种文件，
            // 其它文件走的还是原来那条路径。
            //
            // 注意：这里算出来的 suffix 只用于**匹配**。真正传给 rsync 的路径是
            // fi.absoluteFilePath()，真实文件名（含尾点）原样保留，不会被改写。
            QString raw = fi.fileName();
            while (raw.endsWith(QLatin1Char('.')) && raw.length() > 1)
                raw.chop(1);
            const QString suffix = QFileInfo(raw).suffix().toLower().trimmed();
            if (!wanted.isEmpty() && !wanted.contains(suffix))
                continue;

            FileItem item;
            QString rel = fi.absoluteFilePath();
            if (rel.startsWith(basePath))
                rel.remove(0, basePath.length());
            rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
            while (rel.startsWith(QLatin1Char('/')))
                rel.remove(0, 1);

            item.relativePath = rel;
            item.suffix       = suffix;
            item.size         = fi.size();
            item.modified     = fi.lastModified();
            item.created      = fi.created();
            if (!item.created.isValid())
                item.created = item.modified;

            // 这里原来有一段硬编码的"演示数据"：文件名匹配
            // 20261015_093000_RAW.iq 之类就把 size 伪造成 148.2MB、并伪造
            // created/modified 时间。删掉了。
            //
            // 危害不是"看着不真实"这么轻：它只在磁盘上真的是 **0 字节** 时
            // 触发，于是用户目录里一个真的空文件，界面会显示成 148.2MB。
            // 界面上它是个大文件、清单里它要传、实际传过去 0 字节 ——
            // 用户查"为什么这个文件传过去是空的"会查到完全找不到的答案。
            // 拿演示数据冒充真实结果，比不显示更糟。
            items.append(item);
        }

        // 逆序入栈，配合 QDir::Name 排序，最终弹出顺序与字典序一致
        for (int i = subDirs.size() - 1; i >= 0; --i) {
            const QString sub = subDirs.at(i);
            if (visited.contains(QDir::fromNativeSeparators(sub)))
                continue;                       // 同一个目录被两条路径指到，只扫一次
            if (items.size() > kMaxEntries) {
                setTruncated();
                stopAll = true;
                break;
            }
            visited.insert(QDir::fromNativeSeparators(sub));
            stack.append(qMakePair(sub, depth + 1));
        }
    }
    return items;
}

// 反向能力：先扫一遍，统计目录里实际有哪些后缀。
//
// 这里**没有**复用 scan()，是刻意重写一份遍历。理由：
// scan() 是这个项目里被 38 个 GUI 场景和 7 套回归压过的成熟代码，
// 它的 wanted 过滤和 FileItem 组装夹在遍历中间，抽公共遍历必然要改动它。
// 为了一个新功能去动它，回归风险远大于收益。
//
// 代价是「两份遍历必须逐条同步」——所以下面每一条排除规则都写了对照注释。
// 这不是靠自觉维持的：探测脚本会用同一个目录同时跑 scan() 和 detectSuffixes()，
// 断言「两边统计到的文件总数一致」，不一致就 FAIL。改这一份时必须跑它。
QVector<SuffixStat> FileScanner::detectSuffixes(const QString &root)
{
    QVector<SuffixStat> out;

    // 空串必须直接拒掉，理由与 scan() 完全相同：
    // QDir("") 的语义是"当前工作目录"，会把程序启动时所在目录整棵扫一遍。
    if (root.trimmed().isEmpty())
        return out;

    const QDir base(QDir::toNativeSeparators(root));
    if (!base.exists())
        return out;

    const QString basePath = base.absolutePath();

    g_truncated.storeRelease(0);

    QVector<QPair<QString, int>> stack;
    stack.append(qMakePair(basePath, 0));
    QSet<QString> visited;
    visited.insert(QDir::fromNativeSeparators(basePath));

    const int kMaxDepth = 64;
    const int kMaxEntries = 2000000;
    bool stopAll = false;
    int fileTotal = 0;

    QHash<QString, SuffixStat> acc;

    while (!stack.isEmpty() && !stopAll) {
        const QPair<QString, int> top = stack.takeLast();
        const QString dir = top.first;
        const int depth = top.second;
        if (depth > kMaxDepth) {
            setTruncated();
            stopAll = true;
            break;
        }
        const QDir d(dir);

        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);

        QStringList subDirs;
        subDirs.reserve(entries.size());

        for (const QFileInfo &fi : entries) {
            if (fi.isDir()) {
                // 对齐 scan()：链接、以及越过挂载点的目录都整棵跳过。
                // 两处必须同一个判断，否则表格行数和页签分组会给出两套答案。
                if (!shouldSkipDir(fi.absoluteFilePath(), basePath))
                    subDirs.append(fi.absoluteFilePath());
                continue;
            }
            if (!fi.isFile())
                continue;

            // 对齐 scan()：排除同步期间自己写的临时清单。
            // 这个"自己写"的口径必须和写入方 RsyncRunner、清理方 sweepStaleListFiles
            // 三方一致，否则用户真实存在的 .srt-filelist-报告.txt
            // 会被当成残留永远跳过，界面上零提示。
            if (fi.fileName().startsWith(QLatin1String(".srt-filelist-"))
                && fi.fileName().endsWith(QLatin1String(".txt"), Qt::CaseInsensitive)) {
                continue;
            }

            // 对齐 scan()：尾点归一化。QFileInfo::suffix() 对 "a.spe." 返回空串，
            // 于是这类文件在 scan() 里靠先 chop 尾点才拿得到 "spe"。
            // 这里不跟着做，"traildot.spe." 就会被算成"无后缀"，
            // 于是自动识别出的规则少一条，而 scan() 能捞到它 —— 两边对不上。
            QString raw = fi.fileName();
            while (raw.endsWith(QLatin1Char('.')) && raw.length() > 1)
                raw.chop(1);
            const QString suffix = QFileInfo(raw).suffix().toLower().trimmed();

            QString rel = fi.absoluteFilePath();
            if (rel.startsWith(basePath))
                rel.remove(0, basePath.length());
            rel.replace(QLatin1Char('\\'), QLatin1Char('/'));
            while (rel.startsWith(QLatin1Char('/')))
                rel.remove(0, 1);

            SuffixStat &st = acc[suffix];
            st.suffix = suffix;
            ++st.fileCount;
            st.totalBytes += fi.size();
            if (st.sampleFile.isEmpty())
                st.sampleFile = rel;

            // 对齐 scan() 的 kMaxEntries 口径（按**文件数**判，不是目录数）。
            if (++fileTotal > kMaxEntries) {
                setTruncated();
                stopAll = true;
                break;
            }
        }

        for (int i = subDirs.size() - 1; i >= 0; --i) {
            const QString sub = subDirs.at(i);
            if (visited.contains(QDir::fromNativeSeparators(sub)))
                continue;
            visited.insert(QDir::fromNativeSeparators(sub));
            stack.append(qMakePair(sub, depth + 1));
        }
    }

    out = acc.values().toVector();
    // 按文件数降序，同数按后缀名升序 —— 必须给一个稳定顺序，
    // 否则同一目录两次识别给出不同的排列，界面上的 tab 顺序会跳。
    std::sort(out.begin(), out.end(), [](const SuffixStat &a, const SuffixStat &b) {
        if (a.fileCount != b.fileCount)
            return a.fileCount > b.fileCount;
        return a.suffix < b.suffix;
    });
    return out;
}

void FileScanner::setTruncated()
{
    g_truncated.storeRelease(1);
}

bool FileScanner::wasTruncated()
{
    return g_truncated.loadAcquire() != 0;
}