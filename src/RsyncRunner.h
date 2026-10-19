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
    // 层号与 FailLayer 枚举一致：0=本机 1=网络 2=认证 3=接收端 4=未知。
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
    };
    static ProgressFrame parseProgressFrame(const QString &line);

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
    void allFinished(bool ok);

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


private:
    // 把子进程 stderr 里还没被 onStderr 读走的内容并进 m_stderrTail。
    // 进程刚退出时 readyReadStandardError 不一定已经投递过，
    // 这时 readAllStandardError() 里还留着最后一段，直接丢掉就查不到失败原因。
    void takeStderrTail();
    struct Group
    {
        QString     remoteBase;
        QString     remoteDir;   // 远端路径（相对 home 或绝对），供 ssh mkdir -p 用
        QStringList files;
    };

    // 一次失败的归因：把 ssh / rsync 的原始报错判到四个层之一，
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
    // 单独留一份的失败原因，供失败弹框在所有组跑完之后还能拿到。
    // m_stderrTail 每组结束就 clear()，到弹框时是空的 —— 见 onFinished 里的说明。
    QString        m_lastFailure;
    bool           m_failed = false;
    bool           m_cancelRequested = false;

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
