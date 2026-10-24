#include "FileScanner.h"

#include "ToolRunner.h"

#include <QDir>
#include <QFileInfo>
#include <QAtomicInt>
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QPair>
#include <QSet>
#include <QVector>

#include <algorithm>

#ifdef _WIN32
#  include <windows.h>
#endif

// QFile::Permissions 在下面判断"目录读不读得了"时要用，
// 两个平台都要 —— 原来的 #ifdef Q_OS_LINUX 让 Windows 上编不过这一段。
#include <QFile>

namespace {

// 上一次扫描是否因触顶提前停止。见 FileScanner.h 的说明。
QAtomicInt g_truncated(0);

// 上一次 scan() 跳过的符号链接个数 / 读不了的目录个数。见 FileScanner.h 的说明。
QAtomicInt g_skippedLinks(0);
QAtomicInt g_unreadableDirs(0);

// 上一次 scan() 跳过的**链接目录**个数（junction / 符号链接目录 / 卷挂载点）。
//
// 为什么单独一个计数：链接出去的目录以前是**完全静默**地跳掉的 ——
// 既不进清单，也没有任何提示。客户在数据根下放一个 junction，
// 界面上一行提示都没有，他以为那棵目录已经同步过了，
// 实际一个文件都没传（而且那本来就是不该传的：链接不是数据）。
//
// 口径上它和"跳过的链接文件"是同一类问题（链接不是数据），
// 所以 skippedLinkCount() 把两者**加起来**一起报出去，
// 上层那一句"有 N 个符号链接没有同步"因此一定会为 junction 点名。
QAtomicInt g_skippedLinkDirs(0);

// 扫描阶段的"跳过清单"：**所有"跳过了但没告诉用户"的情况都记在这里**。
//
// 为什么要有它（B 线 D-1 实测，2026-10-24）：
//   路径总长超过平台上限（Windows MAX_PATH 260 / Linux PATH_MAX 4096）的文件，
//   stat 会失败 → QFileInfo::isFile() 返回假 → 文件**根本没进清单**，
//   界面显示"发现 2 个待同步文件"（实际目录里是 3 个）、远端没有它、
//   **日志里一个字都没有**。用户完全不知道自己有个文件被悄悄吃掉了。
//
// 同一个病根还有另外三条（本批一并统一成一份清单）：
//   - 链接目录（junction / 符号链接目录 / 卷挂载点）：整棵跳过
//   - 深度触顶被跳过的那棵子树：整棵跳过
//   - 读不了的目录：整棵跳过
//
// 原则只有一条：**跳过必须留痕**。清单交给上层写进日志，
// 用户至少能知道"哪些东西没同步、为什么"。
QMutex g_skipMutex;
QStringList g_skipNotes;
QAtomicInt g_skipCount(0);
QAtomicInt g_skippedLongPaths(0);
// 每次完整遍历 +1。上层用它判断"这条清单是不是已经播报过了"
// （自动同步一轮一轮跑，每轮都刷一遍同样的清单会把日志刷爆）。
QAtomicInt g_scanGeneration(0);

// 清单最多留这么多条，超出的只留计数 —— 免得链接/junction 特别多的目录把日志刷爆。
const int kMaxSkipNotes = 50;

// 本平台能安全使用的路径长度上限（字符数）。
//
// Windows：MAX_PATH = 260（含结尾 NUL，实际可用 259）。超过之后传统 Win32 路径 API
//   一律失败；Qt 5.9 的 QFileSystemEngine 走的也是那套 API（只有路径自带 "\\?\" 前缀
//   时才走扩展长度那套，而后续拼路径、rsync 传文件都不支持那个形式）。
// Linux/macOS：PATH_MAX = 4096。
//
// **两个平台必须分开，不能一个常量一刀切**：拿 4096 去卡 Windows 会放过真正传不了的路径，
// 拿 260 去卡 Linux 会把一堆完全正常的深目录误杀成"跳过"。
int pathLengthLimit()
{
    // 测试钩子：SRT_PATH_LIMIT 可以把上限改小。
    //
    // 为什么需要它：能不能处理超长路径**取决于机器**（Windows 的长路径开关、
    // Qt 版本、文件系统），在"长路径其实能用"的机器上，超限分支根本走不到，
    // 没法验证"跳过 + 留痕"这条路径真的有效。
    // 生产环境不该设这个变量（设小了只会让程序多跳几个文件 —— 而跳过一定留痕）。
    const QByteArray forced = qgetenv("SRT_PATH_LIMIT");
    bool ok = false;
    const int value = forced.toInt(&ok);
    if (ok && value > 0)
        return value;
#if defined(_WIN32)
    return 260;      // MAX_PATH
#elif defined(Q_OS_UNIX)
    return 4096;     // PATH_MAX
#else
    return 260;
#endif
}

// 路径是否超过本平台上限。
bool exceedsPathLimit(const QString &path)
{
    const int limit = pathLengthLimit();
    return limit > 0 && path.length() > limit;
}

// 往"跳过清单"里加一条，并累加总数。
void addSkipNote(const QString &note)
{
    g_skipCount.fetchAndAddOrdered(1);
    QMutexLocker lock(&g_skipMutex);
    if (g_skipNotes.size() < kMaxSkipNotes)
        g_skipNotes.append(note);
}

// 清空跳过清单（scan() 开头调用一次）。
void resetSkipNotes()
{
    g_skipCount.storeRelease(0);
    g_skippedLongPaths.storeRelease(0);
    QMutexLocker lock(&g_skipMutex);
    g_skipNotes.clear();
}

// 目录能不能读。
//
// 直接问操作系统，不靠 Qt 的抽象：
//
//   Windows：CreateFileW(dir, GENERIC_READ, ..., FILE_FLAG_BACKUP_SEMANTICS)
//            打不开 = 真的读不了。
//            Qt 对**目录**的权限位在 Windows 上给不出来（ACL 语义和 POSIX 位
//            也不是一回事），硬判会把客户每一个子目录都报成读不了 ——
//            这是"误报"，代价是客户白折腾半天，必须避免。
//            FILE_FLAG_BACKUP_SEMANTICS 是打开目录的前提，缺了它 CreateFile
//            一定失败，所以这个标志不能省。
//
//   Unix：stat 的 mode 位（读位一个都没有 = 真的列不出来）。
//            即使用 root 运行也照样能判出来 —— 这里看的是 mode 位，
//            不是 access() 的结果。
//
// 拿不准的时候一律返回 true（= 别乱报）：漏判只是少报一条警告，
// 误判却会让客户以为自己的目录坏了。
bool hasReadPermission(const QString &path)
{
#if defined(_WIN32)
    const std::wstring w = QDir::toNativeSeparators(path).toStdWString();
    const HANDLE h = CreateFileW(w.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
#elif defined(Q_OS_UNIX)
    const QFileInfo fi(path);
    if (!fi.isDir())
        return true;
    const QFile::Permissions p = fi.permissions();
    const QFile::Permissions anyRead = QFile::ReadOwner | QFile::ReadUser
                                     | QFile::ReadGroup | QFile::ReadOther;
    return (p & anyRead) != QFile::Permissions();
#else
    Q_UNUSED(path);
    return true;
#endif
}

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
        // 规则侧的归一化**必须和传输侧逐字对称**，所以这里不再自己写一份，
        // 直接调 ToolRunner::normSuffix —— 它是唯一实现（去空白、去前导点、
        // 去尾点、转小写），RsyncRunner 组建分组时调的是同一个函数。
        //
        // 之前两侧各写各的，结果是规则填 "spe." 时：
        //   扫描侧去掉了尾点 -> "spe"，文件 "a.spe" 匹配得上，列表里有它；
        //   传输侧没去尾点   -> "spe."，和文件的 "spe" 对不上 -> 那一组是空的
        // 空的组报的是「没有匹配到要同步的文件。」+ 成功，
        // 界面显示「同步完成」而远端一个文件都没有。
        //
        // 归一化收敛到一处之后，这种"少一边就静默漏文件"就不会再发生。
        const QString low = ToolRunner::normSuffix(s);
        if (!low.isEmpty() && !wanted.contains(low))
            wanted.append(low);
    }

    const QString basePath = base.absolutePath();

    // 每次扫描开头清一次，标志只反映**本次**扫描有没有触顶。
    g_truncated.storeRelease(0);
    g_skippedLinks.storeRelease(0);
    g_skippedLinkDirs.storeRelease(0);
    g_unreadableDirs.storeRelease(0);
    // 跳过清单每次扫描重新算（"跳过必须留痕"，见上面的说明）。
    resetSkipNotes();
    g_scanGeneration.fetchAndAddOrdered(1);

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
        // 数据根本身就超长（用户把目录选在一个很深的路径下）：整棵都列不出来。
        // 子目录的超长在下面的"条目循环"里判（那里的路径才是完整路径）。
        if (exceedsPathLimit(dir)) {
            // 目录路径超长 → 列目录的接口一定失败（枚举本身就有长度限制），
            // 这一棵子树整个看不见。跳过 + 留痕，不让它变成"静默少了一棵树"。
            g_skippedLongPaths.fetchAndAddOrdered(1);
            addSkipNote(QString::fromUtf8(
                u8"因路径过长跳过：%1（路径长度上限 %2 字符，该路径 %3 字符；该目录整棵列不出来）")
                .arg(dir).arg(pathLengthLimit()).arg(dir.length()));
            setTruncated();
            continue;
        }
        if (depth > kMaxDepth) {
            // 到顶了：**跳过这一棵子树**，但**继续扫别的分支**。
            //
            // 原来这里是 stopAll = true; break; —— 深度触顶会让**整趟扫描**
            // 中断：根目录下其它分支、以及比这条路径浅的所有目录，
            // 一个文件都扫不到。现象是"配好目录扫出来只有零星几个文件"，
            // 而日志一个字都不说 —— 看起来像"今天没数据"。
            //
            // 现在只丢这一棵子树（它的子目录还没入栈，所以整棵都跳过），
            // 别的分支照扫。结果仍然是"不完整"，setTruncated() 继续置位，
            // 上层照旧提示"结果不完整" —— 提示不变，少传的范围小得多。
            setTruncated();
            addSkipNote(QString::fromUtf8(
                u8"因目录层级过深跳过：%1（超过 %2 层，该目录整棵未扫描）")
                .arg(dir).arg(kMaxDepth));
            continue;
        }
        const QDir d(dir);

        // 当前用户**读不了**的目录：整棵跳过，里面的文件不会进清单。
        //
        // 不报出来的话，客户看到的是「同步完成，共 N 个文件」，
        // 实际少了一整棵目录 —— 静默丢数据。
        //
        // 判据要"权限位上没有读" **且** "列不出任何条目" 同时成立：
        // 只看列不出条目 → 正常的**空目录**会被误判；
        // 只看权限位 → Windows 上 Qt 对目录权限位不可信，会把能读的目录也误判。
        if (!hasReadPermission(dir) && d.isEmpty(QDir::NoDotAndDotDot)) {
            ++g_unreadableDirs;
            if (exceedsPathLimit(dir)) {
                // 权限位读不到**也可能**是因为路径太长（打不开这个目录）。
                // 这时必须说"路径过长"，说"权限不够"会把人引到错的方向。
                g_skippedLongPaths.fetchAndAddOrdered(1);
                addSkipNote(QString::fromUtf8(
                    u8"因路径过长跳过：%1（路径长度上限 %2 字符，该路径 %3 字符；该目录整棵列不出来）")
                    .arg(dir).arg(pathLengthLimit()).arg(dir.length()));
            } else {
                addSkipNote(QString::fromUtf8(
                    u8"因目录读不了跳过：%1（请检查访问权限）").arg(dir));
            }
            continue;
        }

        // 一次 entryInfoList 拿全，比 QDirIterator 少很多 stat 调用；
        // QDir::Name 排序保证遍历顺序稳定（推送时逆序，弹出时就是字典序）。
        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);

        QStringList subDirs;
        subDirs.reserve(entries.size());

        for (const QFileInfo &fi : entries) {
            // **路径过长先判**（B 线 D-1）。
            //
            // 超长路径上 stat 会失败，QFileInfo 的 isDir() / isFile() 全是假，
            // 文件于是既不进清单、也不进任何计数，日志里一个字都没有 ——
            // 界面显示"发现 2 个待同步文件"，目录里其实是 3 个。
            //
            // 所以这里**先按路径长度判一遍**：超长的直接跳过、并且留痕。
            // 至于它到底是文件还是目录：超长路径上 stat 不到、问不出来，
            // 只能笼统地说"若它是目录，该目录整棵未扫描"。
            const QString entryPath = fi.absoluteFilePath();
            if (exceedsPathLimit(entryPath)) {
                // 超长路径：**先问操作系统它是什么**，问得到就正常处理。
                //
                // 为什么不能"超长就一刀切跳过"：
                //   能不能处理长路径**取决于机器**（Windows 的长路径开关、Qt 版本），
                //   这台机器上明明传得过去的文件，一刀切跳过就是把好数据扔了。
                //
                // 问不到（isDir()/isFile()/isSymLink() 全假）才是真正会**静默丢掉**
                // 的那一种：下面的 isDir()/isFile() 同样返回假，文件不进清单、
                // 不进任何计数、日志里一个字都没有（台账 D-1 的原始症状）。
                // 这种必须在这里显式跳过、并且留痕。
                if (!fi.isDir() && !fi.isFile() && !fi.isSymLink()) {
                    g_skippedLongPaths.fetchAndAddOrdered(1);
                    addSkipNote(QString::fromUtf8(
                        u8"因路径过长跳过：%1（路径长度上限 %2 字符，该路径 %3 字符；操作系统也认不出它是什么）")
                        .arg(entryPath).arg(pathLengthLimit()).arg(entryPath.length()));
                    setTruncated();
                    continue;
                }
            }
            if (fi.isDir()) {
                // 链接出去的目录、以及越过数据根进入另一个挂载点的目录，整棵跳过。
                // 后者在 Linux 上就是 bind mount：数据根里挂一棵外部目录进来，
                // 外部文件会被卷进清单再由 rsync 传到远端（数据泄漏）。
                //
                // **链接出去的目录必须计数。**
                // 原来这里只是"跳过"，一个数都不报：界面上零提示，
                // 客户以为那棵目录同步过了，实际一个文件都没传过去。
                // 跳过的链接目录与跳过的链接文件是同一类问题
                // （链接不是数据），所以并进同一个计数报给用户。
                const QString subPath = fi.absoluteFilePath();
                if (isLinkedDir(subPath)) {
                    ++g_skippedLinkDirs;
                    addSkipNote(QString::fromUtf8(
                        u8"因是链接目录跳过：%1（链接不是数据）").arg(subPath));
                } else if (!shouldSkipDir(subPath, basePath)) {
                    subDirs.append(subPath);
                } else {
                    // 越过数据根进入另一个挂载点（Linux 的 bind mount / overlay）：
                    // 整棵跳过，同样要留痕 —— 不然客户不知道那棵目录没同步。
                    addSkipNote(QString::fromUtf8(
                        u8"因越过数据根跳过：%1（挂载到数据根之外的目录，不同步）")
                        .arg(subPath));
                }
                continue;
            }
            // 符号链接**不是数据**。
            //
            // QFileInfo::isFile() 会跟随符号链接，所以 link → /outside/secret.spe
            // 会被当成普通文件收进清单；随后 rsync 用 -a（含 -l）把链接**原样重建**，
            // 远端拿到的是一个指向别的机器上路径的链接、不是数据 ——
            // 而界面会把它标成「已同步」，客户毫无察觉。
            //
            // 实测（麒麟真机 + 真 rsync 3.1.1）：远端落地的是符号链接，
            // 把源路径移走后远端读该文件报 No such file or directory。
            //
            // 口径与"链接出去的目录整棵跳过"一致：链接不是数据。
            // 指向目录的链接在上面那个分支里已经处理掉了，
            // 这里处理的是指向文件的、以及指向不存在目标的（断链）。
            if (fi.isSymLink()) {
                ++g_skippedLinks;
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

            // 本程序自己的配置文件永远不进同步清单。
            //
            // 正常情况下它在 %APPDATA%\AP\ 下，不在用户选的数据目录里，扫不到。
            // 但「本机文件目录」选得大时（比如选到用户家目录）它就在扫描范围内，
            // 规则后缀命中 ini 就会把它当普通数据文件传出去 —— 里面是目标 IP、
            // 登录账号、远端路径、私钥路径、工具路径和加密后的密码密文。
            // 这里是硬防护：不管目录怎么选、规则怎么配，它都排掉。
            // 判据用**文件名**而不是绝对路径：SRT_CONFIG_DIR 沙箱、便携模式、
            // 相对路径启动都会让绝对路径对不上，文件名是唯一稳定的口径。
            if (fi.fileName().compare(QLatin1String("SyncReplayTool.ini"),
                                      Qt::CaseInsensitive) == 0) {
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

// 找出"只有大小写不同"的重名文件。
//
// 为什么需要它：接收端是 Linux（大小写敏感），而发送端可能是 Windows
//（默认大小写不敏感）。同一个目录里若有 A.spe 和 a.spe：
//   - 在 Windows 上它们**本来就是同一个文件**，用户看到的是一个，
//     磁盘上只有一份内容 —— 另一份的数据在这台机器上就已经没了
//   - 用户若从别的机器把两个文件拷进来，拷的瞬间就只剩一个
//   - 在 Linux 上如果两个都传过去，远端会得到两个内容不同的文件，
//     与用户在 Windows 上看到的"只有一个"对不上
//
// 实测（Windows，NTFS）：写入 A.spe 再写 a.spe，磁盘上只剩 A.spe 一个，
// 内容被后者替换，全程没有任何提示。程序在扫描时检出并报出来，
// 用户至少知道"这两个名字在 Windows 上是同一个文件"。
//
// 判据：同一目录内，文件名忽略大小写后相同。跨目录不算 ——
// Linux 上 "a/f.spe" 和 "b/f.spe" 是两个不同路径，本来就该都传。
QStringList FileScanner::caseCollisionPaths(const QVector<FileItem> &items)
{
    QHash<QString, QString> seen;   // 目录(小写) + "/" + 文件名(小写) -> 原名
    QSet<QString> dirs;             // 已经报过的目录
    QStringList out;
    for (int i = 0; i < items.size(); ++i) {
        const QString &rel = items.at(i).relativePath;
        const int slash = rel.lastIndexOf(QLatin1Char('/'));
        const QString dir  = slash >= 0 ? rel.left(slash) : QString();
        const QString base = slash >= 0 ? rel.mid(slash + 1) : rel;
        if (base.isEmpty())
            continue;
        const QString key = dir + QLatin1Char('/') + base.toLower();
        const QString prev = seen.value(key);
        if (prev.isEmpty()) {
            seen.insert(key, base);
            continue;
        }
        // 同一个小写名已经出现过，且原名不同 —— 这才是冲突。
        // 同一个名字出现两次是别的问题（扫描重复），不归这里管。
        if (prev == base)
            continue;
        if (dirs.contains(dir))
            continue;
        dirs.insert(dir);
        out.append(dir.isEmpty() ? base
                                  : dir + QLatin1Char('/') + base);
    }
    return out;
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
            // 与 scan() 完全同一口径：到顶只跳过这一棵子树，继续扫别的分支。
            // 两边必须一致 —— 扫描出来的文件数和识别出来的后缀数对不上时，
            // 用户看到的是"识别出 5 种后缀、按规则扫只出来 3 种"，
            // 而界面上没有任何报错。
            setTruncated();
            continue;
        }
        const QDir d(dir);

        const QFileInfoList entries = d.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name);

        QStringList subDirs;
        subDirs.reserve(entries.size());

        for (const QFileInfo &fi : entries) {
            // 对齐 scan()：超长路径同样整条排除（D-1）。
            // 本函数**不清空**跳过清单（清单的归属是 scan()，本函数目前没有调用方），
            // 但同样记录一条，免得这里也变成"静默跳过"。
            const QString entryPath = fi.absoluteFilePath();
            if (exceedsPathLimit(entryPath)) {
                // 对齐 scan()：问得到是什么就正常处理，问不到才显式跳过 + 留痕。
                if (!fi.isDir() && !fi.isFile() && !fi.isSymLink()) {
                    g_skippedLongPaths.fetchAndAddOrdered(1);
                    addSkipNote(QString::fromUtf8(
                        u8"因路径过长跳过：%1（路径长度上限 %2 字符，该路径 %3 字符；操作系统也认不出它是什么）")
                        .arg(entryPath).arg(pathLengthLimit()).arg(entryPath.length()));
                    setTruncated();
                    continue;
                }
            }
            if (fi.isDir()) {
                // 对齐 scan()：链接、以及越过挂载点的目录都整棵跳过。
                // 两处必须同一个判断，否则表格行数和页签分组会给出两套答案。
                if (!shouldSkipDir(fi.absoluteFilePath(), basePath))
                    subDirs.append(fi.absoluteFilePath());
                continue;
            }
            // 对齐 scan()：符号链接不是数据，不进后缀统计。
            // （这里不计数 —— 计数由 scan() 独占，读它的时机在扫描收尾。）
            if (fi.isSymLink())
                continue;
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

            // 对齐 scan()：本程序自己的配置文件（SyncReplayTool.ini）同样排掉。
            // 两边口径必须一致，否则 scan() 排掉了、自动识别规则那条路径没排，
            // 规则里就会凭空多出一个 ini 后缀，用户照着配就去同步配置文件了。
            if (fi.fileName().compare(QLatin1String("SyncReplayTool.ini"),
                                      Qt::CaseInsensitive) == 0) {
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

int FileScanner::skippedLinkCount()
{
    // 链接文件（符号链接）与链接目录（junction / 符号链接目录 / 卷挂载点）
    // 一起报：两者都是"链接不是数据"，都被整条跳过，
    // 少报一个就等于又一次静默丢数据。
    return g_skippedLinks.loadAcquire() + g_skippedLinkDirs.loadAcquire();
}

QStringList FileScanner::skippedNotes()
{
    QMutexLocker lock(&g_skipMutex);
    return g_skipNotes;
}

int FileScanner::skippedCount()
{
    return g_skipCount.loadAcquire();
}

int FileScanner::skippedLongPathCount()
{
    return g_skippedLongPaths.loadAcquire();
}

int FileScanner::scanGeneration()
{
    return g_scanGeneration.loadAcquire();
}

int FileScanner::unreadableDirCount()
{
    return g_unreadableDirs.loadAcquire();
}