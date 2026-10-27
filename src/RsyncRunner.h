#pragma once

#include <QObject>
#include <QDateTime>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>
#include "Model.h"

// 按同步规则把文件分组，串行跑 rsync 进程传输。
//
// 实测得出的三个硬约束（改代码前务必知道）：
// 1. 源路径不能用 Windows 原生形式，rsync 会把盘符冒号当成远程地址分隔符，
//    直接报 "source and destination cannot both be remote"。做法是把工作目录
//    设成本地根目录，源参数写 "./"。
// 2. 文件列表不能用 --files-from=- 从 stdin 灌：msys 版 rsync 会把首行读成
//    带 BOM 的路径，首个文件必然 link_stat 失败。改成写临时文件再 --files-from。
//    临时文件放在本地根目录内、用相对路径，绕开盘符问题。
// 3. rsync / ssh 必须先在应用目录里找，客户机不一定把它们装进 PATH。
// 4. rsync 不会自己创建目标路径的中间层：远端 /data/spe 不存在时直接报
//    "mkdir failed: No such file or directory"。所以每组同步前先用 ssh mkdir -p
//    把目录建出来。老版本 rsync 没有 --mkpath，这个办法对所有版本都成立。
class RsyncRunner : public QObject
{
    Q_OBJECT

public:
    explicit RsyncRunner(QObject *parent = nullptr);
    ~RsyncRunner() override;

    void start(const TargetConfig &target,
               const QString &localRoot,
               const QVector<SyncRule> &rules,
               const QVector<FileItem> &files,
               bool onlyNewFiles = false);

    void cancel();
    bool isRunning() const { return m_proc != nullptr; }
    // allFinished(false) 有两种原因：出错，或者用户主动取消。界面靠这个区分文案，
    // 否则点"取消"会看到红色的"同步异常"弹窗。
    bool wasCanceled() const { return m_cancelRequested; }

    // 本轮是不是被**看门狗中止**的（不是取消，也不是数据失败）。
    //
    // allFinished(false) 现在有三种原因：数据失败、用户取消、看门狗中止。
    // 看门狗中止时后续分组根本没跑，界面文案应当是"同步未完成"，
    // 而不是"同步失败" —— 后者会让用户以为数据没传过去
    // （核对阶段被中止时数据其实全传完了，见 onWatchdogTimeout）。
    // 界面层要用它区分文案；本接口 2026-10-24 就位。
    bool wasAborted() const { return m_watchdogAborted; }

    // 看门狗中止时，**数据到底有没有问题**是未知的（核对阶段中止）。
    // true = "结果未确认"，false = "数据确实有问题"。
    bool abortedUnfinished() const { return m_abortedUnfinished; }

    // ---- 带宽限制（rsync --bwlimit）----

    // 本轮同步**实际生效**的限速（MB/s，0 = 不限速）。
    //
    // 在 start() 时快照 —— 一次同步可能分成好几组、每组起一个 rsync 进程，
    // 本轮所有组都用同一个值。中途改档位不会改变本轮（见 bandwidthLimitNotice）。
    int currentBandwidthMBps() const;

    // 限速改动的**真实**反馈文案。
    //
    // 返回空串 = 不用特别提示（没在同步，或选的就是当前正在用的值）。
    // 非空 = 客户应该看到的话，例如「本次同步仍在按 5 MB/s 进行，
    // 新的限速（10 MB/s）将在下次同步时生效」。
    //
    // 为什么不给"已生效"：rsync 的 --bwlimit 只在进程启动时生效，
    // 同步中途改档位，本次同步根本用不上 —— 说"已生效"是骗人。
    QString bandwidthLimitNotice(int newMBps) const;

    // 限速档位（MB/s）→ rsync --bwlimit 的值（KB/s）。
    // 1 MB/s = 1024 KB/s。0（不限速）→ 0，调用方据此**不加**这个参数。
    static int bwlimitKBpsFor(int mbPerSec);

    // 口令登录时 ssh 没有终端可输密码，必须靠 SSH_ASKPASS。
    // createAskpass 返回空串表示成功，调用方负责事后 remove() 掉两个临时文件。
    static QString createAskpass(const TargetConfig &target, QString *askpassPath, QString *passFilePath);
    static void    applyAskpassEnv(QProcess *proc, const QString &askpassPath);

    // askpass .bat 落在哪个目录。Windows OpenSSH 直接 CreateProcess 跑这个
    // 路径，带空格会被拆成两段导致认证必然失败，所以要挑无空格目录；
    // cleanupStaleSecrets() 也跟着扫这里，否则挪走之后残留清不掉。
    static QString askpassDir();

    // 启动时清掉上一次崩溃/强杀残留的临时文件（含**明文口令**）。
    // 只删写它的进程已经不在了的那些，多实例并存时不会误删。
    static void    cleanupStaleSecrets();

    // 拿一段 ssh/rsync 的原始报错，判它属于哪一层（给 --selfcheck 用）。
    //
    // classifyLayer 本身在 RsyncRunner.cpp 的匿名命名空间里，没法从外面链进来。
    // 但"归因判对没有"恰恰是最该被自动守住的东西：它一错，客户就被引导去
    // 查错的方向。所以把真机抓到的原文固化成一张表，跑 --selfcheck 逐条对。
    // 层号与 FailLayer 枚举一致：0=本机 1=网络 2=认证 3=接收端 4=未知
    // 5=对端 SSH 服务未应答（2026-10-24 新增；新值追加在末尾，0~4 一个不动）。
    // 6=接收端同名目录挡住普通文件（2026-10-24 新增，C 线 F-1；同样追加在末尾）。
    static int     layerOf(const QString &raw);

    // 看门狗预算，纯函数，可单独自检。
    static int     watchdogMs(int fileCount = 0, qint64 totalBytes = 0);

    // 删掉给定的 askpass / 口令文件，并把"本进程 pid 名下"的同名文件全扫一遍。
    // 两件事必须放一起：成员被下一次 createAskpass 覆盖之后，
    // 上一对文件就再也没有任何人知道路径，只删成员等于漏。
    static void    purgeSecrets(const QString &askpassPath, const QString &passFilePath);

    // 补扫 dir 下"本进程 pid 名下"的 srt_pass_* / srt_askpass_*，返回删掉的数量。
    // 从 purgeSecrets() 里抽成独立函数是为了能被 --selfcheck 直接判：
    // 这段代码唯一的失败模式是"一条都没删"，而那和**根本没写这段代码**
    // 在输出上长得一模一样 —— 通配符末尾少一个 '*'，QDir::entryList 就只匹配
    // 名字恰好等于前缀的文件，found 恒为空，循环体一次都不执行，全程零报错。
    // 只删 selfPid 名下的：别的实例（另一个 pid）正在用的文件动了就是在打断它。
    static int     purgeSelfPidSecretFiles(const QString &dir, int selfPid);

    // 清掉老版本建的 srt_askpass_xxx/ **目录**型残留，返回删掉的目录数。
    // 抽成独立函数是为了能被 --selfcheck 直接判：它唯一的失败模式是
    // "一条都没删"，而那和没写这段代码在输出上长得一模一样。
    static int     purgeLegacySecretDirs(const QString &dir, int keepPid);

    // 应用目录（含 bin/）-> PATH -> 空串。ssh 的 -e 参数和 rsync 的程序名都用它。
    static QString locateTool(const QString &name);

    // 解析 rsync 的一帧进度输出。抽出来是为了能被 --selfcheck 直接判 ——
    // 它唯一的失败模式是"帧全被丢掉"，而那和不解析在界面上长得一模一样
    // （进度条只是不动，没有任何报错）。
    struct ProgressFrame
    {
        bool    valid    = false;
        int     percent  = 0;
        qint64  bytes    = 0;
        double  speedBps = 0.0;      // 已换算成字节/秒
        // 帧尾 "(xfr#N, to-chk=a/b)" 里的 N = **rsync 已完成传输的文件数**。
        // 0 = 这一帧里没有 xfr#（不是每一帧都带这个计数）。
        int     xfr      = 0;
    };
    static ProgressFrame parseProgressFrame(const QString &line);

    // 把 rsync 输出的**转义名**还原成真实文件名。
    //
    // rsync 的 --out-format=%n 不是原样打印文件名的：它会把「不可打印」的字节
    // 转义成 \#NNN（反斜杠 + # + 3 位八进制）。
    //
    // 实测（2026-10-21，rsync 3.2.7 与 3.1.1 各跑一遍全字节扫描 1..255，
    // locale 取 C / C.utf8 / en_US.UTF-8 三档，规则完全一致）：
    //   0x01-0x08、0x0A-0x1F、0x80-0xFF  ->  \#NNN
    //   0x09（制表符）、0x20-0x7F       ->  原样输出
    // locale 改不改都一样，所以「run.sh 里设 LC_ALL=C.utf8」并不能绕开它。
    //
    // 为什么这条必须修：中文文件名在 stdout 上长成
    //   混\#344\#270\#255文.spe
    // 而清单里存的是真实名「混中文.spe」，两者**永远对不上**。
    // 于是这个文件既不计数、也不点亮行，最后靠 leftover 兜底规则
    // 显示成「✓ 已完成」—— 传是传过去了，但界面全程显示 0 个文件，
    // 直到结束才被强拉到 100%。客户看到的是「进度条不动、计数是 0」，
    // 而日志里那句「已传 N 个文件」里的 N 一直是错的。
    //
    // 反解义是**有歧义**的：真实文件名里本来就可能带字面量 "\#012"，
    // 而 rsync 不转义反斜杠（实测 0x5C 原样输出），于是
    // 名字 "a\#012b" 和 "a<换行>b" 在 stdout 上长得一模一样。
    // 所以调用点必须**两个都试**：先拿还原结果查清单，没命中再拿原始串查。
    // 先还原后原样：中文这类绝大多数情况一次就中；
    // 先原样后还原：字面量 "\#NNN" 的名字也不会被误还原。
    static QString unescapeRsyncName(const QString &raw);

    // 把 rsync 的一行输出还原成本组清单里的真实文件名；不在清单里就返回空串。
    // 候选怎么取、还原与原样谁先试，都在这里，--selfcheck 直接拿真机原文来判。
    static QString resolveRsyncName(const QString &line, const QSet<QString> &groupSet);

    // 主机名 / 账号能不能安全地拼进 "user@host:path"。
    // 空格、冒号、引号、分号这些字符会让这一整串被 rsync/ssh 拆开或二次解释，
    // 结果是报错报天书，或者更糟：安静地连到别的地方去。
    // Windows 计算机名是允许含空格的，这个坑在客户现场很常见，
    // 所以在「测试连接」和「开始同步」两条路径上都要提前拦下来并说清楚。
    static bool isSafeLoginField(const QString &value, bool isHost);

    // 自动接受主机公钥时，丢弃 known_hosts 的落地文件（Windows NUL / Linux /dev/null）
    static QString nullKnownHosts();

    // 子进程 stderr 的最近若干行，供界面把失败原因说清楚。
    //
    // 为什么需要它：rsync 报的原始原因（Permission denied / No space left /
    // cannot stat destination …）一直都在 m_stderrTail 里、也已经打进了同步
    // 日志，但弹框只有一句"请检查远端连接与磁盘空间"。权限不足根本不在这句
    // 话列的两个方向里，用户照着提示去查网络和磁盘，永远查不到点上。
    //
    // 只暴露滚动尾部（几 KB），不给整段：远端不可写时 rsync 会给每个文件报
    // 一行，整段塞进消息框会把真正有用的那几行淹掉。
    QString lastStderr() const { return m_lastFailure; }

    // 以下两个只给自动化测试用。远端路径的归一化是整个同步里最容易出错、
    // 又最不容易从外部看出来的一步（mkdir 和 rsync 必须拿到同一个字符串，
    // 而 "~"、空根、重复斜杠、子目录穿越都会改写它），
    // 光看"同步成功了"区分不出"建到了 A 目录、文件写到了 B 目录"。
    QString remoteDirForTest(const TargetConfig &t, const QString &subDir) const
    { return remoteDir(t, subDir); }
    QString remoteSpecForTest(const TargetConfig &t, const QString &subDir) const
    { return remoteSpec(t, subDir); }
    QString askpassDirForTest() const
    { return askpassDir(); }

    // 只探测"网络通不通 + 账号对不对"，不传任何文件、不建任何远端目录。
    // 设置弹窗的「测试网络连接」用它。**同步阻塞**，调用方必须放到后台线程。
    //
    // 为什么不用 start()：start 会 mkdir、建清单文件、起 rsync，
    // 用户只是想确认一下 IP 和密码对不对，凭空在远端留一堆目录。
    // （非静态：要复用 sshArgs()，那是按目标配置拼选项的私有函数。）
    bool probeConnection(const TargetConfig &target, QString *detail, int timeoutMs = 15000) const;

    // ---- 远端路径的两条判定，公开出来是为了让自检判据验到**同一份实现** ----
    //
    // 它们原本是 .cpp 匿名命名空间里的自由函数。自检只能照抄一份，
    // 而照抄的那份永远跟着实现漂移：改坏实现、判据照样全绿。
    // 暴露之后 start() 和 main.cpp 的自检调的是同一个函数。
    //
    // remoteRootWithoutTilde：把 "~" / "~/" 展开成等价的相对路径。
    //                        remoteDir() 拼远端路径时用的就是它。
    // subDirEscapesRoot      ：这段相对路径会不会爬出所填的根目录。
    static QString remoteRootWithoutTilde(const QString &raw);
    static bool    subDirEscapesRoot(const QString &subDir);
    // 上面两步合起来：用户填的远端根（含 ~ 展开）会不会写到家目录之外去。
    static bool    remoteRootEscapesHome(const QString &rawRemoteRoot);

    // 少数几类错误的专用建议（端口填错 / 远端路径填成文件 / 安全中心拦截）。
    // 空串 = 该用本层的通用建议。同样暴露出来，是为了让自检判据能验到
    // "换了建议"这件事本身，而不只是验到"换了个层名"。
    static QString specialAdviceFor(const QString &raw);

    // 收尾清理的**唯一次序来源**，按执行先后返回步骤名：
    //   "remote-markers"（删接收端 .srt-verify-*，要 ssh 凭据）
    //   "secrets"       （删 askpass 脚本和明文口令文件）
    // 标记必须在口令之前：否则清理用的 ssh 已经没口令、连不上接收端，
    // 标记就永久留在客户机器上。判据读的是这张产品表（不是照抄），
    // 所以谁把次序对调回去，判据立刻变红。
    static QStringList finishCleanupSteps();

signals:
    void fileStarted(const QString &name);
    void fileTransferred(const QString &name, int done, int total);
    void byteProgress(int percent);
    // 同一个进度帧里的"已传字节 / 当前速度"。界面用它显示已传、速度，
    // 用户才分得清"正在传"和"卡住不动"（只有百分比时压缩阶段可以长时间不跳）。
    void byteStats(qint64 bytes, double bytesPerSec);
    void logText(const QString &text);
    // 本组里被 rsync 明确报失败的文件（相对路径）。
    // 界面用它把那些已经被 --out-format=%n 标成「已完成」的行改回「失败」。
    void fileFailedInGroup(const QString &relativePath);
    void allFinished(bool ok);
    // 同步进行中改了限速：本次同步仍按旧值跑，新值下次同步生效。
    void bandwidthLimitDeferred(int effectiveMBps, int selectedMBps);

private slots:
    void onStdout();
    void onStderr();
    void onProcError(QProcess::ProcessError err);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onDirsFinished(int exitCode, QProcess::ExitStatus status);
    void onVerifyFinished(int exitCode, QProcess::ExitStatus status);
    // rsync 起来时把它挂进 Windows 作业对象并放行（仅 Windows 生效，D11）。
    void onJobAttach();
    // 关掉作业句柄。作业带 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE，
    // 最后一个句柄一关，作业里还活着的进程（含 rsync fork 出来的 ssh）全部被终止。
    void closeJob();
    void onVerifyStdout();
    void onWatchdogTimeout();
    // 配置里的限速档位被改了：判断这次改动是"立刻生效"还是"本次同步结束后生效"。
    void onBandwidthLimitSaved(int mbps);


private:
    // 把子进程 stderr 里还没被 onStderr 读走的内容并进 m_stderrTail。
    // 进程刚退出时 readyReadStandardError 不一定已经投递过，
    // 这时 readAllStandardError() 里还留着最后一段，直接丢掉就查不到失败原因。
    void takeStderrTail();
    // 把 stderr **逐行**解析一遍，把里面带到的文件名收进两个候选集合。
    //
    // 为什么必须逐行、而且与 m_stderrTail 解耦：
    // m_stderrTail 是给"原始报错展示"用的滚动窗口（只留尾部 4000 字符），
    // 拿它当失败名单的数据源，错的行数一多，最早那批失败文件的名字就永远
    // 进不了名单 —— 而名单是唯一能把「已同步」改回「失败」的通道，
    // 于是它们一直绿着。
    // 实测（80 个文件全部写失败）：远端 0 个真名文件 + 80 个 .srt-partial/，
    // 界面前 30 个显示 ok、失败名单只有 f31–f80。
    //
    // 两个集合的区别（与原来两个函数各自的正则口径一一对应）：
    //   m_failedNameHints —— 只认"报了 errno 的那种行"，用来把**行**标成失败；
    //   m_quotedPathNames —— 认任何被引号包住的绝对路径，用来列"未能完整传输"的清单。
    void harvestStderrNames(const QString &text);
    // 把跨块剩下的**半行**当成完整一行补解析。
    //
    // 为什么要单独冲一下：rsync 的 stderr 最后一行**常常没有换行符**。
    // 那一行如果一直留在半行缓冲里，就永远不会被解析 ——
    // 实测（80 个文件全部写失败）：最后一个文件 f80 的报错行没进名单，
    // 名单少一个名字（虽然那一行本身还是会被标成失败）。
    void flushPendingStderrNames();
    // 从 stderr 里把 rsync 明确报失败的文件挑出来，逐个发 fileFailedInGroup。
    // 只在本组 rsync 退出码非 0 时调 —— 退出码 0 时 stderr 里出现的路径是
    // 别的来源的（远端 shell 告警等），拿它当失败会把好文件标成坏的。
    void reportFailedNames();
    struct Group
    {
        QString     remoteBase;
        QString     remoteDir;   // 远端路径（相对 home 或绝对），供 ssh mkdir -p 用
        QStringList files;
    };

    // 一次失败的归因：把 ssh / rsync 的原始报错判到层之一（层号见 layerOf 的注释），
    // 再按【归因】/【原始报错】/【下一步】三段打进同步日志。
    // 判据见 RsyncRunner.cpp 里 classifyLayer 与 kFailRules 的注释。
    // forceLayer >= 0 时跳过文本分类（预检类失败没有原文可依）；
    // adviceOverride 非空时替掉本层自带的建议（“被弹窗挡住”走这条）。
    void reportAttribution(const QString &raw, int forceLayer = -1,
                           const QString &adviceOverride = QString());
    void ensureRemoteDirs();
    void launchNext();
    // rsync 退出 0 之后核对目标目录还是不是同一个目录，见 verifyRemoteDir 注释。
    void verifyRemoteDir(const QString &remoteDir);
    void drainBuffer();
    void handleLine(const QString &line);
    // 从一行 stdout 里剥掉进度帧前缀，剩下的文本就是被帧粘住的 %n 文件名。
    static QString nameAfterProgressFrame(const QString &line);
    // 处理 rsync 报出来的一个**文件名**（%n）：记账 + 广播"开始传输"。
    void noteRsyncName(const QString &rawLine);
    // 处理进度帧里的 xfr#N（rsync 已完成传输的文件数）：结算对应文件。
    void noteXfrCount(int xfr);
    // 结算本组第 index 个（0 基）名字 —— 它刚刚传完。
    void settleNameAt(int index);
    // 组尾对账：把"其实失败了、却被 xfr# 结算过"的文件从计数里扣回来。
    void reconcileGroupCount();
    QString sshOption(const TargetConfig &target) const;
    QStringList sshArgs(const TargetConfig &target, bool quotePath) const;
    QString remoteSpec(const TargetConfig &target, const QString &subDir) const;
    QString remoteDir(const TargetConfig &target, const QString &subDir) const;
    void cleanupSecrets();
    void cleanupListFiles();
    void cleanupRemoteMarkers();
    // 按 finishCleanupSteps() 给出的次序执行收尾清理。
    void runFinishCleanup();

    // 传输前记下的源文件快照（大小 + 修改时间）。
    // rsync 传的是"打开那一刻"的内容，用户中途改源文件它无从知晓，
    // 结果是远端拿到改动前的版本、界面照报成功。传输结束后重新 stat
    // 一遍就能发现，比让用户自己去远端比对便宜得多。
    struct SourceSnapshot { QString rel; qint64 size; QDateTime modified; };
    QVector<SourceSnapshot> m_sourceSnapshot;
    QStringList filesMutatedDuringTransfer() const;
    void sweepStaleListFiles() const;
    void reportIncompleteRemoteFiles();

    TargetConfig   m_target;
    QString        m_localRoot;

    QVector<Group> m_groups;
    int            m_groupIndex = 0;
    int            m_done  = 0;
    int            m_total = 0;
    // 本组开始时的 m_done 快照。用来判断"这一组有没有产生传输动作" ——
    // m_done 是整轮累计的，光看它分不出是上一组传的还是这一组传的。
    int            m_doneAtGroupStart = 0;

    // 本组里"rsync 已经报过名字、但还没被确认传完"的那一个文件。
    //
    // 为什么要多存这么一个：随包的 msys rsync 3.5.1 的 **--out-format=%n
    // 是在文件开始传输时打印的，不是传完时**（2026-10-18 本地实测：
    // 2GB 单文件，stdout 第 0 行就是 "c.spe"，其后才是 0%、18%、34%…；
    // 63% 时 kill 掉，目标文件只有 1.55GB）。
    // 也就是说"打印了名字"根本不能当"传完了"用。
    //
    // rsync 不并行（我们没给 --parallel），所以**下一个文件开始传，
    // 就意味着上一个传完了**。用它把"开始"翻译成"上一个的结束"，
    // 界面上的「已完成」才是真的已完成，中途取消时那个 94.6% 的截断文件
    // 也不会再被标成已完成（实测曾报 finalStateOk=1 / finalCountText="1 / 1"）。
    // 空串 = 当前没有在传的文件。
    QString        m_startedName;

    // 本组里 rsync 用 %n 报出来的文件名，**按到达顺序**。
    //
    // xfr#N 与它是 1:1 的：第 N 个 %n 名字对应第 N 次传输。
    // 实测（2026-10-23）：目录也打 %n（带尾斜杠），但**不占** xfr# ——
    // 只有 sub/c.dat 一个文件的清单，%n 打出 "sub/" 与 "sub/c.dat" 两个名字，
    // 而 xfr# 峰值是 1。所以这里只收"清单里的文件"，目录不进这个序列。
    QStringList     m_groupNames;
    // m_groupNames 里已经结算（发过 fileTransferred）的前多少个。
    int            m_settledCount = 0;
    // 本组见过的最大 xfr#（0 = 一个都没见过）。
    int            m_xfrCount = 0;

    // 本组里被 rsync 明确报为失败的文件的**文件名**（不含目录）。
    //
    // 为什么需要它：--out-format=%n 是「开始传输」时打印的，实测（3.2.7 客户端，
    // 2026-10-19）在文件只传了一半就失败时**照样打印**它的名字 ——
    // 所以 onStdout 那一套「下一个开始 == 上一个结束」的推断，会把一个
    // 只传了 63% 就失败的文件标成「已完成」。
    // 以前靠退出码兜底只能救**最后一个**文件，中间那些失败的一律误标。
    //
    // 判据是 rsync 自己 stderr 里的报错行，实测三种形态都带**带引号的完整路径**：
    //   rsync: [generator] opendir "/remote/sub/f.spe" failed: Permission denied (13)
    //   rsync: [receiver] open "/remote/f.spe" failed: Permission denied (13)
    //   rsync: [receiver] write failed on "/remote/f.spe": File too large (27)
    // 取 basename 后与本组清单核对，对得上的才算失败。
    QSet<QString>   m_failedNames;

    // 当前这一组要传的文件清单（相对路径）。
    // rsync 的 --out-format=%n 输出里，统计行和真实文件名长得几乎一样：
    // 一个叫 "50% xfr#1 to-chk=0.spe" 的文件会被当成进度帧，
    // "sent 40960 bytes ..." 会被当成文件名。靠字符串猜必然误伤，
    // 所以只认"确实在这一组清单里"的行。
    QStringList     m_currentGroupFiles;
    // 本组文件总大小，供 watchdogMs() 算校验阶段的时间预算。只在本组跑的时候有意义。
    qint64          m_currentGroupBytes = 0;
    // 「仅同步新增文件」勾选项的值。
    //
    // 这个勾选框以前只被创建、从来没被读过：同步一律走 --checksum 全量比对，
    // 界面上一个用户看得见、点得动、理所当然会生效的开关，实际是摆设。
    // true  -> 传 --ignore-existing：远端已存在的文件一律跳过，只传新增的。
    // false -> 传 --checksum：同大小同 mtime 但内容变了也必须重传。
    // 两者互斥（--ignore-existing 根本不看内容），所以按取值二选一，
    // 不能两个一起加，也不能都不加。
    bool            m_onlyNew = false;
    // 本轮快照的限速（MB/s，0 = 不限速）。在 start() 里从配置读一次。
    //
    // 为什么是"每轮快照"而不是"每次起 rsync 都去读"：一次同步会分好几组、
    // 每组起一个 rsync 进程。用户在传输途中改了档位，后面的组就用新值、
    // 前面的组用旧值 —— 半程换挡，既不像"已生效"也不像"没生效"。
    // 快照成一轮一个值，改动统一到下一轮，行为才说得清。
    int             m_bwlimitMBps = 0;
    // 本轮同步是不是正在进行中（从 start() 真正开跑，到 allFinished）。
    //
    // 不用 m_proc 判断：两组之间 m_proc 是空的（进程退出到下一个起来之间），
    // 拿它当"正在同步"的判据会在组间窗口里判错。
    bool            m_roundActive = false;
    // 同一份清单的哈希索引。contains() 在 stdout 热路径上，rsync 每吐一行就调一次；
    // 用 QStringList 的线性查找，一组 5000 个文件就是 2500 万次字符串比较。
    // 列表本身保留，onFinished 还要按顺序遍历。
    QSet<QString>   m_currentGroupSet;
    QProcess      *m_proc = nullptr;
    // rsync 整棵进程树所在的 Windows Job Object 句柄（D11）。
    //
    // 为什么这里必须用作业对象：rsync 是用 `-e "ssh ..."` 起的，
    // 真正的远端通道 ssh.exe 是 rsync **自己 fork 出来的子进程**。
    // Windows 的 TerminateProcess 不级联，kill() 只杀得掉 rsync 本身，
    // 那个 ssh 会留下来继续握着 stdout/stderr 管道不放手，
    // 于是 QProcess 永远等不到 EOF，finished 信号永不发出。
    // 网络一断就是这个形态：界面永久停在"同步中"，取消也没用，只能强杀程序
    // （外部实测 3/3 复现，600 秒仍未结束）。
    //
    // 声明成 void* 而不是 HANDLE，是为了不把 <windows.h> 拖进每一个 include
    // 本头文件的人；HANDLE 本身就是 void*，两边转换零成本。
    // nullptr = 这次没拿到作业对象，行为退回修复前：只杀直接子进程。
    // 把新建的 QProcess 挂进 m_job 作业对象，并让它用 CREATE_SUSPENDED 起来。
    // 跨组复用同一个作业：每次重建会把上一组退出瞬间还没收完尾的 ssh 误杀。
    // 仅 Windows 生效，其他平台是空实现。
    void          attachToJob(QProcess *proc);
    void         *m_job = nullptr;
    QTimer         m_watchdog;
    QString        m_buffer;
    // 子进程 stderr 的滚动保留，只留尾部若干行，
    // 用来在进程退出时补上还没投递的最后一段错误信息。
    QString        m_stderrTail;

    // stderr 里**逐行**解析出来的候选文件名。
    //
    // 这两个集合不受 4000 字符尾窗限制 —— 尾窗只管"原始报错展示"，
    // 失败名单必须看**全部**报错，否则错的行数一多就漏标，
    // 漏标 = 失败的文件在界面上显示成「✓ 已完成」（P0）。
    //
    // 内存有界：一个集合里存的是**去重后的**文件名，个数不会超过本组文件数。
    QSet<QString>   m_failedNameHints;
    // 远端**完整路径**（来自 rsync 报错行里被引号包住的那一段）。
    //
    // 为什么单列一个集合：m_failedNameHints 存的是 basename，
    // 而 reportFailedNames() 的规则 1/3 是按**相对路径**写的 ——
    // basename 永远命中不了那两条，于是"不同目录下同名文件"里
    // 真正失败的那个归不了因，界面一直显示「✓ 已完成」。
    // 远端路径 = 远端根 + 相对路径，剥前缀能把它精确还原回来。
    QSet<QString>   m_failedRemotePaths;
    // 保留插入顺序（界面要按清单顺序念出来），去重靠 contains 查。
    QStringList     m_quotedPathNames;
    // 跨块拼接用的半行：一次 readAll 拿到的可能只是半行。
    QString        m_stderrPending;
    // 单独留一份的失败原因，供失败弹框在所有组跑完之后还能拿到。
    // m_stderrTail 每组结束就 clear()，到弹框时是空的 —— 见 onFinished 里的说明。
    QString        m_lastFailure;
    bool           m_failed = false;
    bool           m_cancelRequested = false;
    // 看门狗超时后置位：**本轮到此为止，不再起下一组**。
    //
    // 为什么必须有这个标志（实测 P0 缺陷）：
    //   onWatchdogTimeout() 里 `m_proc->kill(); m_proc->waitForFinished(1000);`
    //   —— waitForFinished 会**同步触发** finished() 信号，
    //   于是 onFinished() → launchNext() 起了下一组 rsync，
    //   然后代码才回到 closeJob()，把**刚起来的下一组**也一起杀了。
    //   实测（假 rsync 夹具 + 看门狗 1ms + 2 组）：
    //     runDone=0，10 个文件全标 failed，进度 0%，
    //     日志里第 1 组和第 2 组都报「长时间没有任何输出，已终止本组」。
    //   也就是说：**看门狗超时 = 整轮陪葬**，后面几组一个都没跑。
    //
    //   自动同步是无人值守场景，进程会反复触发看门狗 ——
    //   客户看到的是"每次自动同步都全军覆没"，而日志把他引向
    //   「本机弹窗挡住子进程」，真因（自己的收尾逻辑杀错了进程）没人看得见。
    //
    // 置位后 launchNext() 走收尾分支，本轮以失败告终。
    bool           m_watchdogAborted = false;
    // 看门狗在**核对阶段**到点：数据已由 rsync 退出码 0 证明传完，
    // 卡住的是"核对"这个手段，所以不算"数据有问题"。
    bool           m_abortedUnfinished = false;
    // 上一次播报"扫描跳过清单"时的扫描代次（-1 = 还没播报过）。
    // 用来避免自动同步一轮一轮跑时把同一份清单反复刷进日志。
    int            m_lastSkipGeneration = -1;
    // 看门狗当前守着的是**哪个阶段**的子进程。
    //
    // 为什么必须区分（实测）：
    //   看门狗同时守着三个阶段 —— 建远端目录（ssh mkdir）、
    //   核对远端目录（ssh stat）、真正传文件（rsync）。
    //   三个阶段的卡法与归因完全不同，但文案写死成
    //   「长时间没有任何输出，已终止本组（本组 N 个文件）」——
    //   在建目录阶段超时时，本组还没开始（N=0），
    //   客户看到的是"本组 0 个文件"，完全不知所云。
    //
    // 阶段取值：
    //   StageNone       没有子进程在看门狗视野里
    //   StageRemoteDirs 建远端目录（连不上/没权限就是死在这一步）
    //   StageVerify     核对远端目录
    //   StageTransfer   真正传文件
    enum WatchStage { StageNone = 0, StageRemoteDirs, StageVerify, StageTransfer };
    WatchStage     m_watchStage = StageNone;

    // 远端目标目录在 mkdir 那一刻放的标记文件，用来在传输结束后确认"还是同一个目录"。
    QString          m_verifyToken;    // 标记文件里写的随机串
    QString          m_verifyMarker;   // 标记文件名（含 token）
    // 正在核对的目录、核对命令的输出。
    QString          m_verifyDir;
    QString          m_verifyBuffer;
    bool             m_inVerify = false;
    // 本轮已经核对通过的远端目录。核对命令是 cat marker; rm -f marker，
    // 读完即删。多条规则落到同一个远端目录时（相对路径都留空就是这种情况），
    // 第一组核对完就把标记删了，第二组再也读不到 -> 文件全传对了，
    // 界面却报「远端目录在传输过程中被删除或替换」，重跑依旧失败。
    QSet<QString>    m_verifiedDirs;

    QString m_askpassPath;
    QString m_passFilePath;
    QString m_listPath;
};
