#include "RsyncRunner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QtGlobal>

#ifdef _WIN32
#include <windows.h>
// tlhelp32.h 用到 windows.h 里的 WIN32_FIND_DATA / MAX_PATH，
// 顺序反了会在 SDK 头文件里炸出一百多个看不懂的错误。
#include <tlhelp32.h>
#else
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace {

// PATH 里各项之间的分隔符。Windows 是 ';'，POSIX 是 ':'。
//
// 写死 ';' 会在 Linux 上把 PATH 首段拼成 "/opt/app;/usr/bin" 这样一个不存在的
// 目录 —— 程序目录等于没进 PATH，而且白吃掉一个路径分量。Qt 5.9.6 没有
// QDir::listSeparator()（5.10 才有），QDir::separator() 两个平台都返回 '/'
// 也不对，所以只能显式分平台。
#ifdef Q_OS_WIN
// 建一个"最后一个句柄一关就全员陪葬"的作业对象。
// 返回 nullptr = 没拿到（创建失败或系统不支持），调用方必须退回旧行为。
void *createKillTreeJob()
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job)
        return nullptr;

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info;
    ZeroMemory(&info, sizeof(info));
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                &info, sizeof(info))) {
        CloseHandle(job);
        return nullptr;
    }
    return job;
}

// 放行一个用 CREATE_SUSPENDED 拉起来、还没有跑过任何代码的进程。
//
// 为什么必须走线程快照：作业成员身份是**创建时从父进程继承**的。
// 只要 rsync 在挂进作业之前就已经 fork 出了真正的 ssh，那个 ssh 就永远不在作业里，
// kill() 只能杀掉 rsync 自己，ssh 变成孤儿继续握着 stdout/stderr 管道不放，
// 于是 QProcess 永远等不到 EOF，界面永久停在"同步中"，取消也没用。
// 先挂起 -> 挂作业 -> 放行，是唯一能关死这个窗口的顺序。
//
// 为什么按 pid 反查主线程、而不是读 CreateProcessArguments::processInformation：
// 这里原来的注释写的是「started() 是排队信号，startProcess 的栈帧早已消失，
// 那个指针在槽里读是野内存」—— 结论对，**理由是错的**。
// 核对 Qt 5.9.6 上游源码后的实际情况是：Windows 后端的 start() 全程同步，
// started() 就是在 startProcess() 的栈上同步发出的，栈帧并没有消失；
// 而且 processInformation 是 new 出来的堆对象（QProcessPrivate::pid），
// 不是什么野内存。所以照抄那段注释只会把后来人带进错误推理。
//
// 仍然按 pid 反查，是因为它不依赖对 Qt 内部时序的任何假设：
// 进程被挂起着、一个字节都没执行，必然只有主线程这一个线程，
// 枚举到的第一个属于它的线程就是主线程。理由换了，结论更硬。
bool resumePrimaryThread(DWORD pid)
{
    if (pid == 0)
        return false;

    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return false;

    THREADENTRY32 te;
    te.dwSize = sizeof(te);
    bool resumed = false;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid)
                continue;
            const HANDLE th = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
            if (!th)
                continue;
            if (ResumeThread(th) != static_cast<DWORD>(-1))
                resumed = true;
            CloseHandle(th);
            if (resumed)
                break;
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return resumed;
}
#endif // Q_OS_WIN

QChar pathListSeparator()
{
#ifdef _WIN32
    return QLatin1Char(';');
#else
    return QLatin1Char(':');
#endif
}

// pid 还在不在，用来判断 %TEMP% 里的残留是不是别的活着的实例正在用。
bool processAlive(int pid)
{
    if (pid <= 0)
        return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h)
        return false;
    DWORD code = 0;
    const bool exited = (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE);
    CloseHandle(h);
    return !exited;
#else
    // EPERM 说明进程存在但不属于当前用户 —— 一样算活着。
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
#endif
}

// rsync 的远端规格加了 --protect-args 之后**不过远端 shell**，路径里的字符
// 原样送达。实测（本机 rsync 3.5.1 客户端 + 远端 3.1.3）就是这一点决定了行为：
//
//   '   单引号**不需要**转义。原先把 ' 转成 '\''\'' ，--protect-args 下
//        远端收到的就是这三个字符的字面串，rsync 于是建出一个真名叫
//        q'\''uote x 的目录；而 ensureRemoteDirs 用的是另一套（正确的）
//        shell 转义，建的是真名 q'uote x。两处目录不一致，数据全部落进
//        转义串那个目录，用户按自己填的路径找不到，界面还报成功。
//        实测：传转义串 → 远端出现 q'\''uote x/ 且文件在内，rc=0；
//              传字面串 → 远端出现 q'uote x/ 且文件在内，rc=0（这才是对的）。
//
//   { }  brace expansion 在**产品真实调用路径**上会真的发生（--files-from + ./ 源）：
//        用户填 gA{}B，文件传到 gAB —— {} 整段消失，远端凭空多出一个用户没填过的
//        目录，界面报同步完成，用户按自己填的路径去找找不到那个目录里的数据。
//        此前给 '{' 加反斜杠同样是错的：远端建出 gA\{}B（名字里真带一个反斜杠），
//        与 ensureRemoteDirs 建的 gA{}B 不是同一个目录。
//        转义已删除，改由 rejectGlob() 把这两个字符直接拒掉。
//
// 注意：这个函数是恒等的，但它**不负责**防 glob。--protect-args 挡不住 rsync
// 自己展开远端路径（* ? [ ] { } 实测都会被展开或吞掉），证据与修法见 rejectGlob。
//
// 注意这个函数**只**用于 rsync 的目标规格（remoteSpec）。ensureRemoteDirs /
// verifyRemoteDir 那几条命令是要过远端 shell 的，它们各自用 '...' 包裹 +
// '\'' 替换，那套是对的，两者不能混用。
// 上面两条实测结论合起来就是：**这个位置什么都不用转义，原样返回**。
// 保留一个恒等函数是为了让"为什么不转义"和实测证据留在原地，
// 免得后人看到没有转义又以为漏了、顺手加回来。
QString escapeForRemoteShell(const QString &path)
{
    Q_UNUSED(path);
    return path;
}

// rsync 的汇总输出也走 stdout，而且在 --info=progress2 下会被打开：
//   building file list ... 200/200 [0/0]  done
//   sent 40,960,000 bytes  received 123 bytes  12,345.00 bytes/sec
//   total size is 40,960,000  speedup is 331.36
// 以前靠 startsWith("sent ") 这类前缀把它们滤掉，但真实文件名可以就叫
// "sent 1 bytes ..."，于是文件真传了、计数却被吞掉。现在统一按
// "这一行是不是本组清单里的文件"来判断，见 handleLine()。

// 规则里的远端相对路径会不会爬到 remoteRoot 之外。
// 逐段数深度：`..` 前面没有可抵消的普通段就说明要逃出去。
//
// 不能靠 cleanPath 判断——cleanPath("/..") 会返回 "/"（根的父亲还是根），
// 于是 "/.." 被"归一化"成合法路径，看起来没毛病，其实用户写的根本不是这个意思。
// 也不能只比字符串："/.."、"./.."、"a/../.." 三种写法里前两种会漏。
bool subDirEscapesRoot(const QString &subDir)
{
    int depth = 0;
    const QStringList parts = subDir.split(QLatin1Char('/'), QString::SkipEmptyParts);
    for (const QString &seg : parts) {
        if (seg == QLatin1String("."))
            continue;
        if (seg == QLatin1String("..")) {
            if (depth == 0)
                return true;
            --depth;
        } else {
            ++depth;
        }
    }
    return false;
}

// remoteRoot 里的 "~" 必须在这里统一剥掉：remoteDir() 真正拿去拼远端路径的
// 是**剥掉之后**的串，而越界校验查的是**原始**串 —— 两边看的不是同一个东西，
// 于是 "~" 被当成一个普通目录段，正好抵消掉紧跟其后的那一个 ".."。
//
// 实测（2026-10-18，参数矩阵攻击）：远端目录填 `~/atk-sib`
// （用户本意是家目录下的 atk-sib）→ 校验算出来 depth("~")=1、("..")=0、
// ("atk-sib")=1，判定"没有越界"；而 remoteDir 剥掉 `~/` 之后实际拼出去的是
// `../atk-sib`，文件真的落到了 /home/atk-sib/（家目录**之外**），
// 界面照样报"同步完成"。填 `~/../../` 两级则被正确拦住了。
//
// 剥 "~" 这件事只允许有一处实现：remoteDir() 和越界校验都调它。
// 各写一份的话，改了一边就会重新出现"校验通过、实际写到别处"，
// 而且两边都编译得过、都不报错。
QString remoteRootWithoutTilde(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s == QLatin1String("~"))
        return QString();                 // "~" == 家目录根 == 空路径
    if (s.startsWith(QLatin1String("~/")))
        return s.mid(2);
    return s;
}


// ===========================================================================
// 失败归因：把 ssh / rsync / ssh-keygen 的原始报错分到四个层
//
// 为什么要有这一段：客户现场的失败日志全是英文原文，程序再自己猜一句
// "私钥可能已损坏"，就会把人带偏 —— 客户照着提示去查私钥文件，
// 而真因是本机的安全中心弹窗把子进程挡住了，私钥一点问题都没有。
// 改成：**先看原始报错落在哪一层，再给一句这一层该做的事**，不用猜。
//
// 判据是能在 ssh / rsync 输出里逐字匹配到的英文词串，不含任何推断。
// 每条规则对应客户端真实打印过的原话，注释里写明了它对应哪一句。
//
// **规则顺序就是优先级，先命中先算。** 认证必须排在接收端前面：
// "Permission denied (publickey,password)" 里也含 "Permission denied"，
// 只认后面这个会把"密码错了"说成"远端目录没写权限"，方向正好反过来 ——
// 而这正是客户最容易白跑一趟的那种错。
// ===========================================================================
enum FailLayer {
    LayerLocal = 0,      // 本机（发送端）
    LayerNetwork = 1,    // 网络 / 连接
    LayerAuth = 2,       // 认证
    LayerReceiver = 3,   // 接收端
    LayerUnknown = 4
};

struct FailRule { int layer; const char *needle; };

const FailRule kFailRules[] = {
    // ---- 认证：网络是通的，对面不认这个身份 ----
    { LayerAuth, "publickey" },                        // Permission denied (publickey,password).
    { LayerAuth, "Host key verification failed" },      // StrictHostKeyChecking=yes 时的主机公钥不符
    { LayerAuth, "is invalid for remote host" },       // Host key for 'x' is invalid for remote host
    { LayerAuth, "Permission denied, please try again" },
    { LayerAuth, "Too many authentication failures" },
    { LayerAuth, "no supported authentication methods" },
    { LayerAuth, "Auth fail" },                        // 部分国产发行版 sshd 的原话
    { LayerAuth, "passphrase" },                       // Enter passphrase for key ... / incorrect passphrase supplied
    { LayerAuth, "Authentication refused" },
    { LayerAuth, "account is locked" },
    { LayerAuth, "password has expired" },
    { LayerAuth, "Access denied" },                    // 另一些发行版 sshd 的原话

    // ---- 网络 / 连接：根本没走到对面 ----
    //
    // 这一组必须排在接收端**前面**，理由不是"网络更重要"，而是 rsync 的一次
    // 连接失败会同时吐出两类话：真因和它自己那个善后的症状。例如端口没服务在听时，
    // stderr 里既有 "ssh: connect to host ... Connection refused"（真因），
    // 又有 "rsync: connection unexpectedly closed (0 bytes received so far)"（症状）。
    // 谁先命中谁说了算，所以症状类的那几条也放进了这一组，不能留在接收端里 ——
    // 那样一次"端口不通"会被判成"远端目录或环境有问题"，让用户去查对面机器。
    { LayerNetwork, "Connection timed out" },          // 防火墙把包丢了
    { LayerNetwork, "Operation timed out" },
    { LayerNetwork, "Connection refused" },            // 端口上没有服务在听
    { LayerNetwork, "No route to host" },
    { LayerNetwork, "Network is unreachable" },
    { LayerNetwork, "Could not resolve hostname" },    // IP 填错 / 机器名解析不了
    { LayerNetwork, "Name or service not known" },
    { LayerNetwork, "Temporary failure in name resolution" },
    { LayerNetwork, "Connection reset by peer" },
    { LayerNetwork, "Connection closed by remote host" },
    { LayerNetwork, "connection unexpectedly closed" },// rsync 自己的善后症状，归到网络层
    { LayerNetwork, "kex_exchange_identification" },   // 连上了但不是 ssh 服务
    { LayerNetwork, "Bad protocol version identification" },
    { LayerNetwork, "Connection aborted" },
    { LayerNetwork, "Broken pipe" },
    { LayerNetwork, "ssh: connect to host" },          // 连接阶段的兜底

    // ---- 接收端：连上了也登进去了，是对面机器的事 ----
    //
    // [receiver] 是 rsync 3.1 起给"接收端自己报的错"加的固定前缀，形如
    //   rsync: [Receiver] mkdir "/data/sub" failed: No such file or directory (2)
    // 放在这一组最前面，因为它比任何自然语言都精确。注意不要顺着它去补
    // "failed: No such file or directory" 这种通用短语：本地源文件被删掉的
    // 报错形态一模一样（link_stat "x.spe" failed: No such file or directory），
    // 补了就会把本机的问题甩给对面机器。
    // 本机侧对称的那一条是 [sender]（见下面本机组），它排在最后也是刻意的：
    // [sender=3.1.1] 这种版本尾注带的是等号，不会误命中 "[sender]"。
    { LayerReceiver, "[receiver]" },
    { LayerReceiver, "remote:" },                      // remote: Permission denied / remote: mkdir ...
    { LayerReceiver, "mkdir failed" },                 // mkdir failed: Permission denied / No such file ...
    { LayerReceiver, "write failed on" },              // write failed on "/p/x.spe": ...
    { LayerReceiver, "couldn't remove existing directory" },
    { LayerReceiver, "Read-only file system" },
    { LayerReceiver, "No space left on device" },      // 磁盘满（rsync 只在远端写临时文件）
    { LayerReceiver, "mkstemp" },                      // mkstemp "/p/.x.spe.XXXXXX" failed
    { LayerReceiver, "Protocol data stream seems to be garbage" },
    { LayerReceiver, "rsync: command not found" },     // 远端没装 rsync（sh: rsync: command not found）
    { LayerReceiver, "unexpected token in output" },   // 远端 rsync 版本太老，协议对不上
    { LayerReceiver, "Permission denied" },            // 兜底的"远端不可写"，见上面关于顺序的说明

    // ---- 本机（发送端）：自己这边就办不成 ----
    { LayerLocal, "[sender]" },                        // read errors mapping / link_stat，读不了本地源文件
    { LayerLocal, "Identity file" },                   // Warning: Identity file ... not accessible
    { LayerLocal, "Unprotected private key" },         // 私钥文件权限太宽，ssh 拒绝使用
    { LayerLocal, "are too open" },                    // Permissions 0644 for '...' are too open.
    { LayerLocal, "Could not open a connection to your authentication agent" },
    { LayerLocal, "Load key" },                        // ssh-keygen -y 的 Load key "...": ...
    { LayerLocal, "invalid format" },                  // 私钥内容损坏
    { LayerLocal, "bad permissions" },                 // ssh-keygen -y 的 bad permissions
    { LayerLocal, "error in libcrypto" },
    { LayerLocal, "no such identity" },
    { LayerLocal, "unknown option" },                  // 我们自己拼错了 rsync / ssh 选项
    { LayerLocal, "Too many arguments" },
    { LayerLocal, "couldn't change directory" }        // change_dir "..." failed（本地工作目录没了）
};

// 每个层的名字、一句"这一层到底是什么意思"、一句"下一步该做什么"。
const char *kLayerName[] = {
    u8"\u672C\u673A\u7AEF\u95EE\u9898\uFF08\u53D1\u9001\u7535\u8111\uFF09",
    u8"\u7F51\u7EDC\u94FE\u8DEF\u95EE\u9898\uFF08\u672A\u8FDE\u901A\uFF09",
    u8"\u8EAB\u4EFD\u8BA4\u8BC1\u95EE\u9898\uFF08\u8D26\u53F7\u5BC6\u7801/\u79C1\u94A5\uFF09",
    u8"\u63A5\u6536\u7AEF\u95EE\u9898\uFF08\u76EE\u6807\u7535\u8111\uFF09",
    u8"\u672A\u80FD\u5F52\u56E0"
};

const char *kLayerMeaning[] = {
    u8"\u3010\u8D23\u4EFB\u5F52\u5C5E\uFF1A\u672C\u673A\u8F6F\u4EF6/\u672C\u5730\u73AF\u5883\u3011\u62A5\u9519\u539F\u6587\u547D\u4E2D\u672C\u673A\u5C42\u5339\u914D\u3002",
    u8"\u3010\u8D23\u4EFB\u5F52\u5C5E\uFF1A\u7F51\u7EDC\u94FE\u8DEF/\u9632\u706B\u5899\u3011\u62A5\u9519\u539F\u6587\u547D\u4E2D\u7F51\u7EDC\u5C42\u5339\u914D\u3002",
    u8"\u3010\u8D23\u4EFB\u5F52\u5C5E\uFF1A\u767B\u5F55\u51ED\u8BC1\u3011\u5DF2\u8FDE\u4E0A\u76EE\u6807\u7535\u8111\uFF0C\u4F46\u8D26\u53F7\u3001\u5BC6\u7801\u6216\u79C1\u94A5\u88AB\u5BF9\u65B9\u62D2\u7EDD\u3002",
    u8"\u3010\u8D23\u4EFB\u5F52\u5C5E\uFF1A\u63A5\u6536\u7AEF\u7535\u8111\u3011\u62A5\u9519\u539F\u6587\u7531\u63A5\u6536\u7AEF rsync \u8FDB\u7A0B\u8F93\u51FA\u3002",
    u8"\u672A\u80FD\u4ECE\u539F\u59CB\u62A5\u9519\u91CC\u5224\u65AD\u51FA\u5177\u4F53\u5F52\u5C5E\u3002"
};

const char *kLayerAdvice[] = {
    u8"\u8BF7\u68C0\u67E5\u672C\u673A\uFF1Arsync \u548C ssh \u5BA2\u6237\u7AEF\u662F\u5426\u9F50\u5168\u3001\u79C1\u94A5\u6587\u4EF6\u662F\u5426\u5B58\u5728\u4E14\u53EF\u8BFB\u3001\u672C\u673A\u78C1\u76D8\u662F\u5426\u6709\u7A7A\u95F4\u3002",
    u8"\u8BF7\u5728\u672C\u673A ping \u76EE\u6807 IP\u3001\u6838\u5BF9\u7AEF\u53E3\u53F7\uFF0C\u5E76\u8054\u7CFB\u7BA1\u7406\u5458\u786E\u8BA4\u5BF9\u7AEF\u662F\u5426\u5F00\u673A\u53CA\u9632\u706B\u5899\u662F\u5426\u653E\u884C SSH \u7AEF\u53E3\u3002",
    u8"\u8BF7\u6838\u5BF9\u76EE\u6807\u7535\u8111\u7684\u767B\u5F55\u8D26\u53F7\u548C\u5BC6\u7801\uFF1B\u82E5\u4F7F\u7528\u79C1\u94A5\uFF0C\u786E\u8BA4\u76F8\u5E94\u516C\u94A5\u5DF2\u6DFB\u52A0\u81F3\u76EE\u6807\u7535\u8111\u7684 authorized_keys\u3002",
    u8"\u8BF7\u8BA9\u76EE\u6807\u7535\u8111\u8D1F\u8D23\u4EBA\u68C0\u67E5\uFF1A\u76EE\u6807\u76EE\u5F55\u662F\u5426\u5B58\u5728\u3001\u8D26\u53F7\u662F\u5426\u5177\u5907\u5199\u6743\u9650\u3001\u78C1\u76D8\u7A7A\u95F4\u662F\u5426\u5145\u8DB3\u3001\u76EE\u6807\u7AEF\u662F\u5426\u5DF2\u5B89\u88C5 rsync\u3002",
    u8"\u8BF7\u5C06\u4EE5\u4E0A\u539F\u59CB\u62A5\u9519\u53D1\u9001\u7ED9\u6280\u672F\u652F\u6301\u4EBA\u5458\u6392\u67E5\u3002"
};


// "子进程被卡住"专用的建议。
//
// 为什么单独一条：客户现场的典型故障是本机的安全中心 / 杀毒软件弹出一个
// 确认框，子进程在等这个框，程序这边看上去就是"什么都没发生"。
// 这种情况既不是私钥坏、也不是网络断，把它归进任何一层都会把人带偏，
// 所以这一条不参与文本分类，只在确认"进程真的卡住了"时直接给出。
const char *kBlockedAdvice =
    u8"\u5B50\u8FDB\u7A0B\u6CA1\u6709\u5728\u8BBE\u5B9A\u65F6\u95F4\u5185\u7ED3\u675F\uFF0C\u5DF2\u7EC8\u6B62\u3002";

// "跑着跑着卡住"专用的建议。
//
// 和 kBlockedAdvice 的区别：卡住可能来自本机弹窗，也可能来自链路半断开
// （链路断了 rsync 同样一个字都不吐）。这两种在日志上完全一样，
// 但要分别去查两个方向，所以这条建议把两边都点一遍，而不是二选一乱猜。
const char *kStallAdvice =
    u8"\u5B50\u8FDB\u7A0B\u957F\u65F6\u95F4\u6CA1\u6709\u8F93\u51FA\uFF0C\u5DF2\u7EC8\u6B62\u3002";

// 强证据：只可能由**接收端进程自己**打出来的字样。
//
// 为什么要在 kFailRules 之前单独判一层：
// 单张顺序表表达不了"真因 vs 善后症状"的优先级。实测两种真实故障：
//
//   对方没装 rsync（rc=127）
//     bash: line 1: rsync: command not found
//     rsync: connection unexpectedly closed (0 bytes received so far) [sender]
//     rsync error: remote command not found (code 127) at io.c(226) [sender=3.1.1]
//   对方磁盘满（rc=11）
//     rsync: [receiver] write failed on "/p/big.spe": No space left on device (28)
//     rsync error: error in file IO (code 11) at receiver.c(381) [receiver=3.2.7]
//     rsync: [sender] write error: Broken pipe (32)
//
// 症状行和真因行**同时**出现在 stderr 里，而症状行在 kFailRules 里排得更靠前
// （"connection unexpectedly closed" 在网络层、"Broken pipe" 也在网络层），
// 于是"对方没装 rsync"和"对方磁盘满"都被判成网络层 —— 用户去查 IP/端口/防火墙，
// 而真正要做的是"让对方装 rsync"、"让对方清磁盘"。方向完全带错。
//
// 这里列的都是**接收端进程**才写得出的字样，不会出现在纯粹的连接失败里：
// 端口不通时 stderr 里只有 ssh 的 connect 报错 + rsync 的 connection unexpectedly
// closed，一条都不命中，照旧落到网络层（正确）。
const char *kHardReceiver[] = {
    "[receiver]",
    "rsync: command not found",
    "No space left on device",
    "write failed on",
    "mkdir failed",
    "mkstemp",
    "Read-only file system",
    "Protocol data stream seems to be garbage",
    "unexpected token in output",
    "remote: ",
    // ENOTDIR：远端路径的某一段是**文件**而不是目录。
    // 实测（2026-10-18 参数矩阵）：远端目录栏填了一个已存在的文件名，
    // 接收端建议却是"让对方管理员确认目录存在、写权限、磁盘空间、已安装 rsync"，
    // 四条里没有一条是真的 —— 问题就在这台电脑上那一栏，填错了。
    // 留在接收端层是对的（错在远端），但**建议**必须换，见 specialAdvice。
    "Not a directory"
};

// 本机层的强证据。和 kHardReceiver 同一个道理：这些字样只可能由**这台
// 发送电脑自己**的进程写出来，不会出现在"连不上对面"的场景里。
//
// 真机实测（麒麟 V11，--selfcheck-secrets 打出来的原文）：
//   rsync: [sender] Failed to exec ssh: No such file or directory (2)
//   rsync error: error in IPC code (code 14) at pipe.c(85) [sender=3.2.7]
//   rsync: [sender] safe_write failed to write 6 bytes to fd 5: Broken pipe (32)
//   rsync error: error in rsync protocol data stream (code 12) at io.c(327) [sender=3.2.7]
//
// 真因是本机的 ssh 起不来（包不完整：bin/ssh 是 wrapper，它 exec 的
// ../libexec/ssh 不存在），但 "Broken pipe" 和 "connection unexpectedly closed"
// 在 kFailRules 里排在网络层，于是一次**本机文件缺失**被判成"没连上目标电脑"，
// 建议用户去 ping 目标 IP、查防火墙 —— 方向完全带偏。
//
// 这里只放"只有本机才写得出来"的句子。不要顺手加 "error in IPC code"：
// 那个在远端侧崩掉时也会出现，放进来就会把真接收端故障重新误判成本机。
const char *kHardLocal[] = {
    "Failed to exec ssh",                 // rsync 连 ssh 都没能拉起来
    "Could not load ssh",                 // 同上的另一种措辞
    "error in libcrypto",                 // 本机 dlopen 到的 libcrypto 有问题
    "Could not open a connection to your authentication agent",
    "Load key",
    "Identity file",
    // ssh 自己解析端口失败。原文形如 `Bad port '0'`。
    // 它排在 kFailRules 的 "connection unexpectedly closed" 后面，
    // 于是全角端口（ConfigStore 的 toInt() 得 0）被判成"网络层"，
    // 建议用户 ping 目标 IP、查防火墙 —— 而真正的动作只是把端口栏改回数字。
    "Bad port",
    // ssh 读不懂**这台电脑上自己的** ssh 配置文件，在解析阶段就退出了。
    //
    // 实测原文（银河麒麟 V11 / Ubuntu 20.04，包内自带 OpenSSH 7.2p2）：
    //   /etc/ssh/ssh_config: line 19: Bad configuration option: include
    //   /etc/ssh/ssh_config: terminating, 1 bad configuration options
    //
    // Include 是 OpenSSH 7.3 才加的指令，7.2 解析到就直接终止。
    // 关键在于它死在**读配置**阶段，一个字节都没往目标机器发，
    // 而 stderr 里必然还跟着 ssh 自己的收尾行（connection unexpectedly closed
    // / Broken pipe），那些在 kFailRules 里排网络层 —— 于是一次
    // "本机配置文件太老" 被判成"没连上目标电脑"，用户去 ping IP、查防火墙，
    // 查一圈也查不出问题。
    //
    // 两条都要放：一条认不出时另一条未必认得出
    //（有的发行版只打 terminating 那行，不打 Bad configuration option）。
    "ssh_config",
    "bad configuration option"
};

// 原文落在哪一层。判据全在 kFailRules 里，逐条匹配英文原文。
int classifyLayer(const QString &text)
{
    for (const char *needle : kHardReceiver) {
        if (text.contains(QLatin1String(needle), Qt::CaseInsensitive))
            return LayerReceiver;
    }
    for (const char *needle : kHardLocal) {
        if (text.contains(QLatin1String(needle), Qt::CaseInsensitive))
            return LayerLocal;
    }
    for (const FailRule &r : kFailRules) {
        if (text.contains(QLatin1String(r.needle), Qt::CaseInsensitive))
            return r.layer;
    }
    return LayerUnknown;
}

// 从原始报错里挑出最多 maxLines 行关键行。
//
// 只留尾部：ssh / rsync 都是把最终原因打在最后一行，前面那些
// "Warning: Permanently added ..." 之类的噪音会把真正有用的那行挤下去。
// 每行再截一次长，避免一整段远端路径把日志撑变形。
QString failureKeyLines(const QString &text, int maxLines = 3)
{
    QStringList keep;
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &raw : lines) {
        QString l = raw;
        l.remove(QLatin1Char('\r'));            // rsync 用 \r 刷新进度条
        l = l.trimmed();
        if (l.isEmpty())
            continue;
        // 首次连接时必然出现、对排障没有任何帮助，直接丢掉。
        if (l.startsWith(QLatin1String("Warning: Permanently added"))
            || l.startsWith(QLatin1String("debug1:"))
            || l.startsWith(QLatin1String("debug2:"))
            || l.startsWith(QLatin1String("debug3:")))
            continue;
        if (l.length() > 200)
            l = l.left(200) + QStringLiteral("...");
        keep.append(l);
    }
    while (keep.size() > maxLines)
        keep.removeFirst();
    return keep.join(QStringLiteral(" / "));
}

// 给「测试网络连接」用的归因文案，压成一行。
//
// 这个出口和同步日志不是一回事：结果直接显示在设置弹窗的一个标签上，
// 不能拆成三段行，所以这里只给"哪一层 + 接下来做什么"。
// 原始报错由调用点自己带出去（那边本来就在返回 ssh 的最后一行）。
QString probeLayerDetail(const QString &raw, int forceLayer = -1)
{
    const int layer = (forceLayer >= 0) ? forceLayer : classifyLayer(raw);
    return QString::fromUtf8(u8"\u3010\u5F52\u56E0\u3011")
           + QString::fromUtf8(kLayerName[layer])
           + QString::fromUtf8(u8"\uFF1A")
           + QString::fromUtf8(kLayerAdvice[layer]);
}

} // namespace

// 公开静态：实现就在上面那个匿名命名空间里，这里只是把它接出来，
// 让 start() 和自检判据走同一条代码路径（理由见 RsyncRunner.h 的注释）。
QString RsyncRunner::remoteRootWithoutTilde(const QString &raw)
{
    return ::remoteRootWithoutTilde(raw);
}

bool RsyncRunner::subDirEscapesRoot(const QString &subDir)
{
    return ::subDirEscapesRoot(subDir);
}

bool RsyncRunner::remoteRootEscapesHome(const QString &rawRemoteRoot)
{
    return ::subDirEscapesRoot(::remoteRootWithoutTilde(rawRemoteRoot));
}

// classifyLayer 在上面的匿名命名空间里，没法从外部链进来。
// --selfcheck 需要直接判一段原始报错，所以在这里开一个薄口。
// 必须定义在匿名命名空间外面：命名空间内不能定义外部类的成员（MSVC 报 C2888）。
int RsyncRunner::layerOf(const QString &raw)
{
    return classifyLayer(raw);
}

// 解析 rsync 的一帧进度。判据用完整形态而不是"含 % 且含 /s"：
// 光靠子串会把真实文件名误判成进度帧（文件名可以含 %，
// 路径里也可能有 /sub/ 这种以 s 开头的目录）。
//
// 进度帧的结构是固定的：行首空白 + 数字 + 空白 + 百分比 + 空白 + 速度 + 单位，
// 真文件名长成这样的概率可以忽略。
//
// **单位必须同时认大小写**，这是实测踩到的：
// rsync 3.x 的 human-readable 输出里，**千位是小写 k、其余是大写**：
//     ^M          3,000 100%   43.07kB/s    0:00:00 (xfr#3000, to-chk=0/3000)
//     ^M    157,286,400  9%  470.12MB/s    0:00:06 (xfr#1, to-chk=0/2)
// 原来的字符类是 [KMGTP]?，没有小写 k，于是**速度只要低于 1MB/s，
// 整行就匹配不上**，每一帧被静默丢掉 —— 进度条全程停在 0%，
// 而文件计数和日志全都正确（只有真文件名能过白名单），症状极其隐蔽。
//
// 实测：3000 个 1 字节文件传 21.5 秒，stdout 上有 3000 个百分比帧，
// 用产品这条正则匹配到 **0** 个。中招的恰好是最该有反馈的两种场景：
// 慢 WAN、海量小文件。
//
// 抽成成员函数是为了让 --selfcheck 能拿上面那两行**原文**当测试数据判它，
// 而不是靠"跑一次看看进度条动没动"——后者在低速下要等 20 多秒才有结论。
RsyncRunner::ProgressFrame RsyncRunner::parseProgressFrame(const QString &line)
{
    static const QRegularExpression progressRe(
        QStringLiteral("^\\s*([\\d,.]+)\\s+(\\d{1,3})%\\s+([\\d.]+)([KkMmGgTtPp]?)B/s"));
    const QRegularExpressionMatch fm = progressRe.match(line.trimmed());
    ProgressFrame f;
    if (!fm.hasMatch())
        return f;

    f.valid   = true;
    f.percent = fm.captured(2).toInt();

    // 同一帧里还有"已传字节"和"当前速度"，界面上一直缺这两个量。
    // 只有百分比的时候，大文件在压缩阶段可以几十秒不动，
    // 用户分不清"在传"和"卡住"；把字节和速度一起抛出去就能区分。
    QString bytesText = fm.captured(1);
    bytesText.remove(QLatin1Char(','));
    f.bytes = static_cast<qint64>(bytesText.toDouble());

    f.speedBps = fm.captured(3).toDouble();
    const QString unit = fm.captured(4);
    // 单位换算。**必须查表，不能用 `字母 - 'K'` 算下标** ——
    // 字母表不连续：G=71 < K=75 < M=77 < P=80 < T=84。
    //
    // 这里面藏着两个错，都是同一个根因：
    //   原版 `switch (c - 'K')`：
    //     K -> 0 落进"不换算"   => kB/s 从没乘过 1024
    //     M -> 2 * 1024^2      => 恰好对
    //     G -> -4 / T -> 9 / P -> 5 全部落到 switch 之外 => 完全不换算
    //   所以原版只对 M 正确，其余三种单位的速度显示都是错的。
    //   改成 "K" 起点查表时，如果不小心把 M 也挪了一格，
    //   M 又会错成 1024^3 —— --selfcheck 拿真机帧的期望值当场就抓到了
    //   （fmSpd=504787506298.88 vs fmExpect=492956549.12，正好差 1024 倍）。
    static const QLatin1Char kUnits[] = { QLatin1Char('K'), QLatin1Char('M'),
                                          QLatin1Char('G'), QLatin1Char('T'),
                                          QLatin1Char('P') };
    static const double kMul[6] = { 1.0,                    // 无单位：字节/秒
                                    1024.0,
                                    1024.0 * 1024.0,
                                    1024.0 * 1024.0 * 1024.0,
                                    1024.0 * 1024.0 * 1024.0 * 1024.0,
                                    1024.0 * 1024.0 * 1024.0 * 1024.0 * 1024.0 };
    int unitIdx = 0;                          // 认不出来的单位一律按字节/秒，不再静默丢帧
    if (!unit.isEmpty()) {
        const QChar up = unit.at(0).toUpper();
        for (int i = 0; i < 5; ++i) {
            if (kUnits[i] == up) {
                unitIdx = i + 1;
                break;
            }
        }
    }
    f.speedBps *= kMul[unitIdx];
    return f;
}

// 把刚建好的 m_proc 挂进 m_job 作业，并让它用 CREATE_SUSPENDED 起来。
// 必须定义在这个匿名命名空间外面：命名空间内不能定义外部类的成员（MSVC 报 C2888）。
// 作业整个生命周期只建一次、跨组复用：每次重建都会把上一组退出瞬间还没收完尾的
// 那个 ssh 一起杀掉（作业带 KILL_ON_JOB_CLOSE，句柄一关全员陪葬）。
#ifdef Q_OS_WIN
void RsyncRunner::attachToJob(QProcess *proc)
{
    if (!proc)
        return;

    // 这里原来写的是 if (!proc || m_job) return; —— 一次同步会按顺序建三个
    // QProcess：先 mkdir 的 ssh，然后每组的 rsync，再每组的核对 ssh。
    // 第一个（mkdir ssh）建好作业后 m_job 就非空了，于是后面两个全部早退：
    // 既没加 CREATE_SUSPENDED，也没进作业。而 rsync 恰恰是整条链上唯一会
    // fork 出 ssh、也是唯一会攥着管道不放的那个。
    // 结果：D11 的修复看着在，实际一步都没生效 —— cancel() 和看门狗里的
    // closeJob() 关的都是早已退出的 mkdir ssh，原始的永久卡死原样保留。
    //
    // 现在：作业只建一次（跨组复用），但**每个**进程都要挂起并入这个作业。
    if (!m_job)
        m_job = createKillTreeJob();
    if (!m_job)
        return;   // 拿不到就完全不挂起，行为与修复前一致

    proc->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *a) { a->flags |= CREATE_SUSPENDED; });
    connect(proc, &QProcess::started, this, &RsyncRunner::onJobAttach);
}
#else
void RsyncRunner::attachToJob(QProcess *)
{
    // 非 Windows 没有作业对象，调用点也都被 #ifdef 包掉了，这里是空实现。
}
#endif

// 主机名 / 账号能不能直接拼进 "user@host:path"。
//
// 这一串是 rsync 和 ssh 共同认的"目标地址"，里面混进特殊字符会被拆开：
//   空格   Windows 的计算机名是允许带空格的（NetBIOS 名如 "AP 测试机"），
//          用户照抄机器名填进来，"user@AP 测试机:/data" 就被拆成两个参数，
//          rsync 要么报错报天书，要么连到别的地方去。
//   :      rsync 用它分隔主机和路径，"a:b" 里的 b 会被当成路径的开头。
//          只有整体写成 [::1] 这种 IPv6 字面量时冒号才是合法的。
//   引号、反引号、$ ; & | > <   会被远端 shell 二次解释。
//   控制字符（含换行） 配置被手改时能混进来，ssh 会把它当成另一个参数。
//
// 这些都是"填了以后行为不对而不是填不了"，所以必须在开跑前拦下来，
// 并说清楚是哪个字段有问题 —— 默默传错地方是最坏的结果。
bool RsyncRunner::isSafeLoginField(const QString &s, bool isHost)
{
    // 空串直接判否：这一位的语义是"能不能安全地拼进 user@host:path"，
    // 空值当然不行。TargetConfig::isValid() 也拦一次，两边都拦不浪费 ——
    // 这是唯一一处从外部配置读进来、别人能直接改的地方。
    if (s.isEmpty())
        return false;

    // 以 "-" 开头会被 ssh 当成选项，而不是主机名。
    //
    // 这一串最终是作为**一个 argv 元素**交给 ssh 的（rsync 传给 ssh 的就是
    // "user@host"）。OpenSSH 解析命令行时先扫选项再去找 @，所以账号填
    // "-oProxyCommand=x" 时，ssh 收到的是 "-oProxyCommand=x@host"，
    // 被解释成 -o ProxyCommand=x@host —— 而 ProxyCommand 的内容是交给
    // shell 执行的，等于任意命令执行。
    //
    // 合法主机名和 POSIX 用户名都不会以 "-" 开头，所以直接拒掉不会误伤任何人。
    if (s.startsWith(QLatin1Char('-')))
        return false;

    const bool ipv6Literal = isHost && s.startsWith(QLatin1Char('[')) && s.endsWith(QLatin1Char(']'));
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u < 0x20 || u == 0x7F)
            return false;                                   // 控制字符（含换行、制表）
        if (c.isSpace())
            return false;                                   // 空格（含全角空格）
        if (c == QLatin1Char('\'') || c == QLatin1Char('"') || c == QLatin1Char('`')
            || c == QLatin1Char('$') || c == QLatin1Char(';') || c == QLatin1Char('&')
            || c == QLatin1Char('|') || c == QLatin1Char('>') || c == QLatin1Char('<'))
            return false;
        if (c == QLatin1Char(':') && !ipv6Literal)
            return false;
    }
    return true;
}

int RsyncRunner::watchdogMs(int fileCount, qint64 totalBytes)
{
    const QByteArray override = qgetenv("SRT_WATCHDOG_MS");
    bool ok = false;
    const int value = override.toInt(&ok);
    if (ok && value > 0)
        return value;

    // 固定 180 秒对大目录是错的。
    //
    // --checksum 要求 rsync 每次同步都把远端**已存在**的文件整个读一遍算校验和，
    // 这个阶段它不吐任何输出（--info=progress2 只在真正传文件时才刷）。
    // 实测（Windows 本地 -> WSL，同一条链路）：
    //   3 万个文件  首次 114.0s / 二次 75.5s —— 二次 transferred=0，
    //              一个文件都不用传，光校验就花了 75 秒
    //   5 万个文件  撞上 180s 上限，只传了 33394 个
    //   6 万个文件  撞上 180s 上限，只传了 10149 个
    // 于是看门狗在"正在逐个哈希远端文件"的中途把活着的 rsync 杀了，
    // 界面只说"长时间没有任何输出"，用户会去查私钥和网络 —— 方向完全错。
    //
    // 为什么不去掉 --checksum：那个参数正是为了堵"同大小 + 同 mtime + 内容已变"
    // 才加的，去掉之后 rsync 一个文件都不传、远端留着旧内容、界面照样报成功
    // （负向验证记录在 launchGroup 的 args 注释里）。拿静默丢数据换速度不能做。
    //
    // 所以按规模放大预算。实测校验吞吐约 400 文件/秒，取 N/200 秒 = 2 倍余量，
    // 下限仍是 180 秒，上限 15 分钟 —— 再大也不该无限等下去。
    qint64 budget = 3 * 60 * 1000;
    if (fileCount > 0) {
        const qint64 scaled = static_cast<qint64>(fileCount) * 1000 / 200;
        if (scaled > budget)
            budget = scaled;
    }
    budget = qMin<qint64>(budget, 15 * 60 * 1000);

    // 字节项：文件数这一项只覆盖了"逐个文件"的开销，没覆盖"逐字节读远端"。
    // --checksum 的静默期正比于远端已存在内容的字节数，本组文件总大小是它的
    // 上界（远端最多能有这么多同内容），拿来当预算是安全的。
    //
    // 20 MiB/s 是按实测 246 MiB/s 留了 12 倍余量取的低估值，慢链路也不至于
    // 立刻不够用；字节项单独封顶 30 分钟，所以再大的目录也只是多等，
    // 不会变成"永远等下去"，用户随时可以点取消。
    if (totalBytes > 0) {
        const qint64 byBytes = totalBytes / (20LL * 1024 * 1024) * 1000;
        budget += qMin<qint64>(byBytes, 30LL * 60 * 1000);
    }
    return static_cast<int>(qBound<qint64>(budget, 3 * 60 * 1000, 45LL * 60 * 1000));
}

// 把一次失败归到某一层，并按固定三段打进同步日志：
//     【归因】<层名>：<这一层是什么意思>
//     原始报错：<ssh / rsync 的关键原文>          （没有原文时省略这一行）
//     下一步：<这一层该做的事>
//
// 三段都要有，缺一不可：
//   只有原文  -> 客户看到 Permission denied (publickey,password) 也不知道该找谁；
//   只有层名  -> 说了"网络层"但没说查哪里，还是得自己猜；
//   只有建议  -> 又回到"可能是私钥坏了"这种误导性的断言。
//
// raw        ssh / rsync / ssh-keygen 的原始输出，可以是空串。
// forceLayer >= 0 时跳过文本分类。预检那几步（客户端不存在、私钥文件不存在、
//             本地目录不存在）压根没有 ssh 的原文可依，只能由调用点直接指定层；
//             而"进程卡住"这类根本还没有退出码的失败，硬套任何一层都是错的。
// adviceOverride 非空时用它替掉本层自带的建议（"被弹窗挡住"就走这条）。
// 少数几类错误，通用建议会把人带偏，单独给一条。
//
// 头一个是银河麒麟安全中心（应用执行控制 → 网络访问控制）拦下的连接。
// 实测（麒麟 V11 2603）：
//   ssh: connect to host 192.168.139.1 port 22: Operation not permitted
// 通用建议是"ping 一下目标 IP、确认端口号、让对方放行防火墙"，三条一条都不对：
//   - ping 得通（ICMP 不受这条控制管）
//   - 端口号没错（同一时刻别的机器用同一个端口能连）
//   - 对面防火墙没有任何问题，连接是在**本机**被拒的，返回值是 EPERM 而不是
//     ECONNREFUSED / ETIMEDOUT —— 这三种在应用层是能分开的，分不开就会把人引到对面。
// 同一个组件还会拦二进制执行，报的是
//   exec: /.../libexec/ssh: Operation not permitted
// 现象是"刚解压出来能跑，几分钟后突然不能用了"，因为扫描器是异步标记的。
QString specialAdvice(const QString &raw)
{
    // 端口栏填的不是数字/含全角字符。ssh 连尝试都没做就退出了，
    // 通用建议（ping 目标 IP、查防火墙、让对方放行端口）没有一条沾边。
    if (raw.contains(QLatin1String("Bad port"), Qt::CaseInsensitive))
        return QString::fromUtf8(
            u8"ssh \u8FD4\u56DE Bad port\uFF1A\u6536\u5230\u7684\u7AEF\u53E3\u503C\u4E0D\u53EF\u7528\u3002"
            u8"\u7AEF\u53E3\u9700\u8981\u5199\u6210 1 \u5230 65535 \u4E4B\u95F4\u7684\u6570\u5B57\u3002");

    // 这台电脑上 ssh 读不懂系统 ssh_config，在**读配置**阶段就退出了，
    // 一个字节都没往目标机器发。网络层那套建议（ping、查防火墙、放行端口）
    // 没有一条沾边，而且会让人以为链路有问题。
    //
    // 判据只看这两条字样，不猜"是不是配置坏了"：带 ssh_config 的报错
    // 原文就是 ssh 自己打的（见 kHardLocal 的注释与 P0-D 实测）。
    if (raw.contains(QLatin1String("ssh_config"), Qt::CaseInsensitive)
        || raw.contains(QLatin1String("bad configuration option"), Qt::CaseInsensitive))
        return QString::fromUtf8(
            u8"ssh \u7684\u62A5\u9519\u539F\u6587\u91CC\u542B ssh_config \u6216 bad configuration option \u5B57\u6837\u3002"
            u8"\u7A0B\u5E8F\u81EA\u5E26\u7684 ssh \u5728\u8BFB\u53D6\u7CFB\u7EDF /etc/ssh/ssh_config \u65F6\u5224\u5B9A\u5176\u4E2D\u7684\u4E00\u4E2A\u6307\u4EE4\u4E0D\u53EF\u7528\uFF0C"
            u8"\u5728\u914D\u7F6E\u89E3\u6790\u9636\u6BB5\u9000\u51FA\u3002\u539F\u59CB\u62A5\u9519\u89C1\u4E0A\u65B9\u300C\u539F\u59CB\u62A5\u9519\u300D\u884C\u3002");

    // 远端路径栏填成了一个已存在的文件。接收端层的通用建议（查磁盘、查权限、
    // 问对方装没装 rsync）一条都不对，而真正要改的是这台电脑上那一栏。
    if (raw.contains(QLatin1String("Not a directory"), Qt::CaseInsensitive))
        return QString::fromUtf8(
            u8"\u8FDC\u7AEF\u8DEF\u5F84\u8FD9\u4E00\u680F\u586B\u7684\u662F\u4E00\u4E2A\u5DF2\u7ECF"
            u8"\u5B58\u5728\u7684\u6587\u4EF6\uFF0C\u4E0D\u662F\u76EE\u5F55\u3002rsync \u62A5 Not a"
            u8" directory\uFF0C\u610F\u601D\u662F\u8FD9\u6761\u8DEF\u5F84\u7684\u67D0\u4E00\u6BB5"
            u8"\u662F\u6587\u4EF6\u800C\u4E0D\u662F\u6587\u4EF6\u5939\u3002\u8BF7\u628A\u5B83\u6539\u6210"
            u8"\u8981\u63A5\u6536\u6570\u636E\u7684\u90A3\u4E2A\u76EE\u5F55\uFF0C\u76EE\u5F55\u4E0D"
            u8"\u5B58\u5728\u6CA1\u5173\u7CFB\uFF0C\u7A0B\u5E8F\u4F1A\u81EA\u52A8\u521B\u5EFA\uFF0C"
            u8"\u4E0D\u8981\u586B\u67D0\u4E2A\u6587\u4EF6\u540D\u3002");

    if (!raw.contains(QLatin1String("Operation not permitted"), Qt::CaseInsensitive))
        return QString();
    return QString::fromUtf8(
        u8"\u62A5\u9519\u539F\u6587\u91CC\u7684\u201COperation not permitted\u201D\u610F\u601D\u662F"
        u8"\u201C\u672C\u673A\u62D2\u7EDD\u4E86\u8FD9\u4E2A\u8FDE\u63A5\u201D\u3002"
        u8"\u5728\u8FD9\u53F0\u7535\u8111\u4E0A\u6253\u5F00\u3010\u5B89\u5168\u4E2D\u5FC3\u3011"
        u8"\u2192\u3010\u5E94\u7528\u6267\u884C\u63A7\u5236\u3011"
        u8"\uFF0C\u628A\u672C\u7A0B\u5E8F\u8BBE\u4E3A\u3010\u59CB\u7EC8\u5141\u8BB8\u3011"
        u8"\uFF08\u8FDE\u540C\u76EE\u5F55\u4E0B\u7684 plugins\u3001lib\u3001libexec\u3001bin \u4E00\u8D77\u653E\u884C\uFF09\uFF0C"
        u8"\u7136\u540E\u91CD\u65B0\u542F\u52A8\u7A0B\u5E8F\u3002"
        u8"\u82E5\u679C\u4E0D\u80FD\u6539\u8BBE\u7F6E\uFF0C\u628A\u6574\u4E2A\u89E3\u538B\u76EE\u5F55"
        u8"\u590D\u5236\u5230\u684C\u9762\u6216\u4E0B\u8F7D\u76EE\u5F55\u518D\u8FD0\u884C\u4E00\u6B21\u2014\u2014"
        u8"\u65B0\u590D\u5236\u7684\u6587\u4EF6\u4F1A\u88AB\u91CD\u65B0\u8BC4\u4F30\u3002");
}

// 公开静态：接出上面那个专用建议函数，让自检判据能验到"换了建议"本身。
QString RsyncRunner::specialAdviceFor(const QString &raw)
{
    return specialAdvice(raw);
}

void RsyncRunner::reportAttribution(const QString &raw, int forceLayer, const QString &adviceOverride){
    const int layer = (forceLayer >= 0) ? forceLayer : classifyLayer(raw);

    QString advice = adviceOverride;
    if (advice.isEmpty())
        advice = specialAdvice(raw);
    if (advice.isEmpty())
        advice = QString::fromUtf8(kLayerAdvice[layer]);

    emit logText(QString::fromUtf8(u8"\u3010\u5F52\u56E0\u3011") + QString::fromUtf8(kLayerName[layer])
                 + QString::fromUtf8(u8"\uFF1A") + QString::fromUtf8(kLayerMeaning[layer]));

    const QString keys = failureKeyLines(raw);
    if (!keys.isEmpty())
        emit logText(QString::fromUtf8(u8"\u539F\u59CB\u62A5\u9519\uFF1A") + keys);

    emit logText(QString::fromUtf8(u8"\u8BF4\u660E\uFF1A") + advice);
}
QString RsyncRunner::locateTool(const QString &name)
{
    // 调用方给了绝对路径就当它说了算
    if (QFileInfo(name).isAbsolute())
        return QFileInfo(name).isFile() ? name : QString();

    // 候选名按平台排：先本平台习惯的那个，再把另一个也带上。
    //
    // 为什么 Linux 侧也要试 .exe：客户的 Linux 机器上偶尔会先装一份 Windows 版
    // 的 openssh（或者干脆把随包的 bin/ 原样拷过来），那里面躺的是 ssh.exe。
    // 只认裸名的话会判定"缺少 ssh 客户端"然后直接拒绝，**程序本身完全正常**。
    // 这是那种"用户永远说不清但确实存在"的现场，多认一个名字成本是零。
#ifdef _WIN32
    const QStringList candidates = QStringList() << (name + QStringLiteral(".exe")) << name;
#else
    const QStringList candidates = QStringList() << name << (name + QStringLiteral(".exe"));
#endif

    // 1. 打包目录里的可执行文件优先，客户机不需要自己装
    const QString appDir = QCoreApplication::applicationDirPath();

    // 1a. appDir 根目录。Windows 包就是平铺的（ssh.exe 就在 exe 旁边）。
    foreach (const QString &exe, candidates) {
        const QString bundled = appDir + QLatin1Char('/') + exe;
        if (QFileInfo(bundled).isFile())
            return bundled;
    }

    // 1b. appDir/bin/。Linux 包的 5 个工具**只**放在这里，因为 bin/ssh 是
    // wrapper 脚本：它把 lib/sshv2/（隔离的 OpenSSL 1.0.2）加进 LD_LIBRARY_PATH
    // 再 exec libexec/ssh。libexec/ 里是裸二进制，缺那个环境变量直接
    // "error while loading shared libraries"，所以这里只认 bin/。
    //
    // 为什么必须自己找：Linux 包靠 run.sh 里的 `export PATH="$HERE/bin:$PATH"`
    // 才找得到。一旦用户绕过 run.sh 直接双击二进制、或从别的启动器
    // （systemd unit、cron、别的 .desktop）拉起来，PATH 里没有 bin/，
    // locateTool 就返回空，同步功能整体不可用 —— 而界面能正常打开，
    // 用户只会看到"一个文件都传不出去"，完全猜不到是这个原因。
    // 实测踩过：VM 上直接跑 ./SyncReplayTool --selfcheck-secrets，
    // 日志里是「未找到 ssh 客户端」，而同一台机器上 ssh 明明在包里。
    foreach (const QString &exe, candidates) {
        const QString wrapped = appDir + QLatin1String("/bin/") + exe;
        if (QFileInfo(wrapped).isFile())
            return wrapped;
    }

    // 2. 再退回 PATH
    foreach (const QString &exe, candidates) {
        const QString found = QStandardPaths::findExecutable(exe);
        if (!found.isEmpty())
            return found;
    }
    return QString();
}

QString RsyncRunner::askpassDir()
{
    // Windows OpenSSH 直接 CreateProcess 跑 SSH_ASKPASS 指向的那个 .bat，
    // 路径带空格时会被当成"程序名 + 参数"拆开：
    //   'C:\Users\jss18\AppData\Local\Temp\srt' is not recognized ...
    // 然后 ssh 拿不到口令，认证必然失败（255）。实测对照：同样内容放
    // %TEMP%\srt 密码测试\ 下必然失败，放 %TEMP%\srt_askpass_xxx.bat 下成功。
    //
    // 触发条件不是罕见配置：Windows 用户名可以带空格（John Smith、
    // 张三 测试），而 QDir::tempPath() 就在用户目录下。密码认证（同步 +
    // 测试连接）会整体不可用，私钥模式不受影响。
    //
    // 所以 askpass 脚本必须挑一个"没有空格、纯 ASCII"的目录。
    // 口令文件（srt_pass_*）不用挪：bat 里 type 的路径是带引号的，
    // cmd 解析得对，含空格无碍。
    static const QString dir = []() -> QString {
        const QStringList candidates = {
            QDir::tempPath(),
            // 程序目录只在"装在可写位置"时才算数：装进 Program Files 的
            // 标准用户对它没有写权限，选中它等于把口令创建推到失败路径上。
            QCoreApplication::applicationDirPath(),
            QDir::homePath(),
            QStringLiteral("C:/Windows/Temp"),
        };
        for (const QString &candidate : candidates) {
            if (candidate.isEmpty())
                continue;
            const QDir probe(candidate);
            if (!probe.exists() || !QFileInfo(probe.absolutePath()).isWritable())
                continue;
            const QString native = QDir::toNativeSeparators(probe.absolutePath());
            bool clean = true;
            for (const QChar &ch : native) {
                // 空格会被 CreateProcess 拆开；非 ASCII 要经过系统代码页，
                // 中文用户目录下写 .bat 本身就不可靠，一并排除。
                if (ch == QLatin1Char(' ') || ch.unicode() > 0x7F) {
                    clean = false;
                    break;
                }
            }
            if (clean)
                return probe.absolutePath();
        }
        return QDir::tempPath();     // 全都不合格时只能退回原行为
    }();
    return dir;
}

QString RsyncRunner::createAskpass(const TargetConfig &target, QString *askpassPath, QString *passFilePath)
{
    // 临时文件名里必须带 pid：cleanupStaleSecrets() 靠它精确判断
    // "这份残留是不是还活着的进程写的"。
    const QString pidTag = QString::number(QCoreApplication::applicationPid());

    // 用 QTemporaryFile，而不是 QDir::tempPath() + 固定名字。三个理由，
    // 每一个都是真实的攻击面：
    //
    //   1) 固定名 %TEMP%\srt_pass_<pid> 完全可预测（pid 枚举一下就有），
    //      本机任何进程都能在同步进行中把它读走 —— 里面是**明文口令**。
    //   2) QFile::open(WriteOnly|Truncate) 会跟着符号链接走。攻击者先在那个
    //      路径上建一个指向别处的链接，口令就被写到他指定的位置去了。
    //   3) QTemporaryFile 用 O_EXCL + 随机名：路径猜不到，而且任何已存在的
    //      条目（包括符号链接）都会让创建直接失败，既不会被跟随也不会被覆盖。
    //
    // 残留的明文口令文件不是理论风险：本机 %TEMP% 里实测躺着崩溃/强杀留下的
    // srt_pass_* 文件，内容就是当时的测试口令。
    //
    // 仍然存在的残余风险（同一用户下的进程仍可枚举 %TEMP% 读走文件）：
    // 这个级别的隔离要靠 DPAPI 之类的用户态密钥，但 askpass 是个 .bat/.sh，
    // 没法在里面解密。同一用户本来就能读本进程内存，不构成更大的泄漏面。
    //
    // 口令文件必须和 askpass 脚本放在**同一个**无空格、纯 ASCII 的目录里。
    // 只挪脚本是不够的：.bat 里写的是口令文件的路径，那条路径是用
    // toLocal8Bit()（本机 ANSI 代码页）写进 bat 的，cmd 再按同一个代码页读回来。
    // 路径里只要有非 ASCII 字符而本机代码页又不是对应的（例如英文
    // Windows 的 1252 遇上中文 TEMP），bat 里就是一堆 "????"，
    // type 报 "The system cannot find the path specified."，口令永远送不出去，
    // ssh 只能报 Permission denied。实测：TEMP 带空格 → 修复后同步成功；
    // TEMP 带中文 → 同样要挪，挪之前必败。
    const QString secretDir = askpassDir();
    // 连续同步两趟时，成员还指着上一趟那一对；先清掉再新建，
    // 否则上一对被成员覆盖后就再也没有人知道它落在哪，只能留在磁盘上。
    // （正常路径 launchNext() 收尾已经清过，这里是兜底 ——
    //   中途取消、startSync 在有旧成员时被重入等路径都可能绕过收尾。）
    purgeSecrets(askpassPath ? *askpassPath : QString(),
                 passFilePath ? *passFilePath : QString());
    QTemporaryFile pass(secretDir + QStringLiteral("/srt_pass_") + pidTag
                        + QStringLiteral("_XXXXXX"));
    // 生命周期交给 cleanupSecrets()：中途取消、析构、正常结束都会走到那里。
    // 这里若交给 QTemporaryFile 析构自动删，askpass 脚本在同步期间就读不到了。
    pass.setAutoRemove(false);
    if (!pass.open()) {
        return QString::fromUtf8(u8"\u65E0\u6CD5\u5199\u5165\u4E34\u65F6\u53E3\u4EE4\u6587\u4EF6\uFF1A%1")
                   .arg(secretDir);
    }

    // 口令里带 CR / LF 必须在提交前就拦下来。
    //
    // askpass 的输出是按行读的，ssh 在第一个 CR/LF 处截断，
    // 后面再长的部分都送不出去。实测（本机 ssh 9.9p2 + 远端 sshd）：
    //   远端口令 a   客户端发 a<CR>b   -> 认证**成功**（发出去的其实只有 "a"）
    //   远端口令 ab  客户端发 a<CR>b   -> 认证失败（"a" != "ab"）
    //   远端口令 ab  客户端发 ab<CR>b  -> 认证成功（尾部 CR 被吃掉）
    // 第一行是最危险的：它意味着"实际口令的前缀只要对上就能过"，
    // 而用户看到的是 Permission denied，完全想不到是这个原因。
    // 直接说清楚比让它以 255 失败强。
    if (target.password.contains(QLatin1Char('\r')) || target.password.contains(QLatin1Char('\n'))) {
        pass.close();
        const QString where = pass.fileName();
        QFile::remove(where);
        passFilePath->clear();
        return QString::fromUtf8(u8"\u5BC6\u7801\u4E0D\u80FD\u5305\u542B\u6362\u884C\u6216\u56DE\u8F66\uFF1A"
                                 u8"\u5BC6\u7801\u4F1A\u88AB ssh \u5728\u7B2C\u4E00\u4E2A\u6362\u884C\u5904\u622A\u65AD\uFF08\u53EA\u4F1A\u53D1\u51FA\u524D\u9762\u90E8\u5206\uFF09\u3002");
    }
    *passFilePath = pass.fileName();
    if (pass.write(target.password.toUtf8()) < 0) {
        const QString where = *passFilePath;
        pass.close();
        QFile::remove(where);
        passFilePath->clear();
        return QString::fromUtf8(u8"\u65E0\u6CD5\u5199\u5165\u4E34\u65F6\u53E3\u4EE4\u6587\u4EF6\uFF1A%1")
                   .arg(where);
    }
    pass.close();
    pass.setPermissions(QFile::ReadOwner | QFile::WriteOwner);

    // 明文口令文件一旦落盘就必须有人负责清掉。下面每条失败路径都直接删，
    // 否则脚本写失败时调用方会带着错误串 return，
    // 没人再回头删这个文件，明文口令就留在磁盘上了。
#ifdef _WIN32
    QTemporaryFile script(secretDir + QStringLiteral("/srt_askpass_") + pidTag
                          + QStringLiteral("_XXXXXX.bat"));
    script.setAutoRemove(false);
    if (!script.open()) {
        QFile::remove(*passFilePath);
        passFilePath->clear();
        return QString::fromUtf8(u8"\u65E0\u6CD5\u5199\u5165\u4E34\u65F6 askpass \u811A\u672C");
    }
    *askpassPath = script.fileName();
    // 全 ASCII，cmd 按 ANSI 读，路径加引号防空格
    script.write("@echo off\r\ntype \"" + QDir::toNativeSeparators(*passFilePath).toLocal8Bit() + "\"\r\n");
    script.close();
#else
    QTemporaryFile script(QDir::tempPath() + QStringLiteral("/srt_askpass_") + pidTag
                          + QStringLiteral("_XXXXXX.sh"));
    script.setAutoRemove(false);
    if (!script.open()) {
        QFile::remove(*passFilePath);
        passFilePath->clear();
        return QString::fromUtf8(u8"\u65E0\u6CD5\u5199\u5165\u4E34\u65F6 askpass \u811A\u672C");
    }
    *askpassPath = script.fileName();
    script.write("#!/bin/sh\ncat '" + passFilePath->toUtf8() + "'\n");
    script.close();
    script.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
#endif
    return QString();
}

void RsyncRunner::cleanupStaleSecrets()
{
    // 崩溃、被任务管理器强杀、断电，cleanupSecrets() 一次都不会跑到，
    // 明文口令就原地留在 %TEMP% 里了。本机实测确实残留着这种文件。
    // 所以每次启动主动清一遍。
    //
    // 只删"写它的进程已经不在了"的那些：同一台机器上完全可能同时开着两个
    // 实例，按时间一刀切会把另一个正在同步的实例的口令文件删掉。
    // 文件名里带了 pid，判据是精确的，不需要猜。
    //
    // pid 复用时会把文件误判成"还活着"而留下 —— 这是安全的方向
    // （多留一个文件，不会误删正在用的）。
    const QString self = QString::number(QCoreApplication::applicationPid());
    // askpass 不一定写在 tempPath()（含空格时要挪走，见 askpassDir()），
    // 所以这里把两个目录都扫一遍，否则挪走之后残留反而永远清不掉。
    QStringList dirs;
    dirs << QDir::tempPath();
    const QString scriptDir = askpassDir();
    if (!dirs.contains(scriptDir))
        dirs << scriptDir;

    QStringList names;
    QStringList nameDirs;      // 与 names 一一对应：同名的文件可能落在不同目录
    for (const QString &dir : dirs) {
        const QStringList found = QDir(dir).entryList(QStringList() << QStringLiteral("srt_pass_*")
                                                                      << QStringLiteral("srt_askpass_*"),
                                                     QDir::Files | QDir::NoDotAndDotDot);
        for (const QString &name : found) {
            names << name;
            nameDirs << dir;
        }
    }
    for (int i = 0; i < names.size(); ++i) {
        const QString &name = names.at(i);
        const QString full = nameDirs.at(i) + QLatin1Char('/') + name;
        const QStringList parts = name.split(QLatin1Char('_'));
        // 连 3 段都凑不齐的（srt_pass_x / srt_askpass_x）同样不是本产品
        // 任何版本的产物：契约里 pid 永远在第 3 段。
        // 原先是 `if (parts.size() < 3) continue;`，和解析失败一样静默放过 ——
        // 清扫"有保留地跳过"在这种场景下就等于"不工作"，而且永不报错。
        if (parts.size() < 3) {
            QFile::remove(full);
            continue;
        }
        // The pid segment carries the extension in the legacy naming:
        //   old build : srt_askpass_<pid>.bat          -> at(2) == "12345.bat"
        //   new build : srt_pass_<pid>_XXXXXX          -> at(2) == "12345"
        //   new askpass: srt_askpass_<pid>_XXXXXX.bat  -> at(2) == "12345"
        // QString::toInt() on "12345.bat" returns false, so every legacy file
        // was skipped FOREVER. Measured on this machine: four stale
        // srt_askpass_<pid>.bat files carrying plaintext passwords survived
        // cleanupStaleSecrets() across many product starts. That silently
        // defeats the whole point of the sweep, and it fails in the safe
        // direction only by accident -- the file is not protected, it is just
        // never considered. Strip the extension before parsing.
        QString pidPart = parts.at(2);
        const int dot = pidPart.indexOf(QLatin1Char('.'));
        if (dot > 0)
            pidPart.truncate(dot);
        bool ok = false;
        const int pid = pidPart.toInt(&ok);

        // 解析不出 pid 的文件必须删，不能 continue 跳过。
        //
        // 原来这里写的是 `if (!ok || pid <= 0 || ...) continue;`，
        // 于是凡是第三段不是纯数字的文件都被静默放过。明文口令文件
        // 恰恰是最不该"静默放过"的东西：实测 srt_askpass_fs0fs0fs、
        // srt_pass_fs0fs0fs 这类非数字命名在每次启动后都原地不动。
        //
        // 删掉是安全的，理由不是"大概没事"，而是命名契约：
        //   本版本 srt_pass_<pid>_XXXXXX / srt_askpass_<pid>_XXXXXX.bat
        //   老版本 srt_askpass_<pid>.bat
        // 两种都把 pid 放在第 3 段，**没有任何一种形态的 pid 不是数字**。
        // 第三段不是数字 => 这个文件不可能是本产品任何版本的运行实例写的
        // => 它一定不属于任何正在同步的进程 => 删掉不会打断别人。
        //
        // 同时把 pid<=0 也归到这里："0" 和负数同样不是合法 pid，
        // 原先一并 continue，等于又留了一条永久残留的口令。
        //
        // 唯一还保留的保护是"自己正在用的那一份"：自己 pid 的文件必须留下，
        // 否则本进程同步中途启动清扫会把自己的口令文件删掉。
        if (!ok || pid <= 0)
            QFile::remove(full);
        else if (pid == QCoreApplication::applicationPid())
            continue;
        else if (!processAlive(pid))
            QFile::remove(full);
    }

    for (const QString &dir : dirs)
        purgeLegacySecretDirs(dir, QCoreApplication::applicationPid());
}

// 老版本建的还可能是**同名目录**：把 askpass 脚本放进 srt_askpass_xxx/ 里，
// 目录本身就是 srt_askpass_*。
//
// 上面那段 entryList(..., QDir::Files) 匹配不到目录，于是它连同里面的明文口令
// 永久留下 —— 本机实测：%TEMP%\srt_askpass_fs\askpass.bat，
// 2026-10-13 起一直在，内容是当时的测试口令。
// 这正是"清理逻辑看起来做了，实际上一条都没匹配上"的那类静默失效：
// 文件过滤条件决定了它只能看见文件。
//
// 只删目录里**这一层**的文件再 rmdir，不递归：用户自己在别处手工建了
// 同名目录的话，不该连带删掉里面的东西。里面有子目录就整个跳过。
//
// 返回真正删掉的目录数。这个函数之所以要独立出来、还要报数量，
// 就是因为它唯一的失败模式是"一条都没删" —— 而那和不写这段代码
// 在输出上完全一样。--selfcheck 用它做判据（见 main.cpp）。
int RsyncRunner::purgeLegacySecretDirs(const QString &dir, int keepPid)
{
    if (dir.isEmpty() || !QFileInfo(dir).isDir())
        return 0;

    const QStringList sub = QDir(dir).entryList(QStringList() << QStringLiteral("srt_pass_*")
                                                              << QStringLiteral("srt_askpass_*"),
                                                 QDir::Dirs | QDir::NoDotAndDotDot);
    int removed = 0;
    for (const QString &name : sub) {
        // 同一个 pid 保护：正在同步的实例不能被中途掏空
        const QStringList parts = name.split(QLatin1Char('_'));
        if (parts.size() >= 3 && parts.at(2) == QString::number(keepPid))
            continue;

        QDir sd(dir + QLatin1Char('/') + name);
        const QStringList inner = sd.entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                        | QDir::Hidden);
        bool hasSubDir = false;
        for (const QString &f : inner) {
            if (QFileInfo(sd.filePath(f)).isDir()) {
                hasSubDir = true;
                break;
            }
        }
        if (hasSubDir)
            continue;
        for (const QString &f : inner)
            QFile::remove(sd.filePath(f));
        if (QDir().rmdir(sd.absolutePath()))
            ++removed;
    }
    return removed;
}

void RsyncRunner::applyAskpassEnv(QProcess *proc, const QString &askpassPath)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (!askpassPath.isEmpty()) {
        env.insert(QStringLiteral("SSH_ASKPASS"), askpassPath);
        env.insert(QStringLiteral("SSH_ASKPASS_REQUIRE"), QStringLiteral("force"));
        // DISPLAY 只是 OpenSSH 判断"能不能用 askpass"的开关，它并不会真的
        // 连那个 X server 去显示窗口。所以这里只做兜底：**已经在的时候一律保留**。
        //
        // 原先是无条件写死 ":0"。在多用户 / Xwayland / :1 / :2 的会话里，
        // 这会把用户真实的 DISPLAY 覆盖掉 —— 而且是在**已经配置正确**的机器上
        // 把对的改错的。这类"兜底把好值改坏"比不兜底更糟：它只在特定环境下发作，
        // 在我自己的测试机上永远看不出来（单会话、:0）。
        if (env.value(QStringLiteral("DISPLAY")).isEmpty())
            env.insert(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
    }
    // rsync / ssh 靠 PATH 找彼此，必须把应用目录一起传下去，
    // 否则客户机没把 ssh 装进系统 PATH 时同步会直接起不来。
    //
    // 分隔符必须按平台取。原来写死 ';' —— 在 Linux 上这会把 PATH 首段拼成
    // "/opt/app;/usr/bin" 这样一个**根本不存在的目录**：等于程序目录没进 PATH，
    // 而且白吃掉一个路径分量。rsync 再去找 ssh 就可能找不到，直接报
    // "ssh: command not found"。客户机是麒麟/统信时必现，Windows 上永远看不出来。
    //
    // QDir::listSeparator() 要 Qt 5.10，这套是 Qt 5.9.6，所以显式分平台。
    const QChar pathSep = pathListSeparator();
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString oldPath = env.value(QStringLiteral("PATH"));
    // 必须加**两个**目录，不是只加 appDir。
    //
    // 因为产品是故意用**裸命令名**去起子进程的：
    //   - rsync：m_proc->start("rsync", args)
    //   - ssh  ：-e "ssh -p 22 -o ..."（不能用绝对路径，见 launchNext 的注释：
    //            路径带空格时 rsync 会把整串当成一个文件名去 exec）
    // 这两处都靠 PATH 解析，所以 PATH 里有没有工具目录就是"能不能同步"的开关。
    //
    // Windows 包是**平铺**的（ssh.exe 紧挨着 SyncReplayTool.exe），只加 appDir 就够。
    // Linux 包不是：5 个工具全在 bin/ 下，因为 bin/ssh 是 wrapper 脚本，
    // 它要把 lib/sshv2/（隔离的 OpenSSL 1.0.2）加进 LD_LIBRARY_PATH 再 exec
    // libexec/ssh；libexec/ 里是裸二进制，直接调会缺那个环境变量起不来。
    //
    // 只加 appDir 的后果（真机实测，银河麒麟 V11）：
    //   rsync: [sender] Failed to exec ssh: No such file or directory (2)
    //   rsync error: error in IPC code (code 14) at pipe.c(85) [sender=3.2.7]
    //   rsync: [sender] safe_write failed to write 6 bytes to fd 5: Broken pipe (32)
    // 界面能正常打开、一个文件都传不出去，用户完全猜不到是 PATH 的事。
    // 走 run.sh 启动时恰好没事（run.sh 自己 export 了 PATH="$HERE/bin:$PATH"），
    // 于是这个缺陷只在本测试台上、绕过 run.sh 直接跑二进制时暴露 ——
    // 而客户从 systemd、cron、别的 .desktop 启动时，走的就是后一条路。
    QStringList prepend;
    prepend << appDir;
    const QString appBin = appDir + QStringLiteral("/bin");
    if (QFileInfo(appBin).isDir())
        prepend << appBin;
    const QString head = prepend.join(pathSep);
    env.insert(QStringLiteral("PATH"),
               oldPath.isEmpty() ? head : head + pathSep + oldPath);
    // ssh 用 HOME 定位 ~/.ssh，没有 HOME 就没法记录 known_hosts。
    // 客户机环境里 HOME 常常是空的（图形界面程序没有 tty），这里兜一下。
    // 原来只回退到 USERPROFILE，那是纯 Windows 变量 —— 在 Linux 上等于没兜底：
    // HOME 为空时这个分支什么都不做，known_hosts 照样落不下来。
    // QDir::homePath() 走 passwd / SHGetFolderPath，与环境变量无关，两个平台都真能用。
    if (env.value(QStringLiteral("HOME")).isEmpty()) {
        const QString profile = env.value(QStringLiteral("USERPROFILE"));
        if (!profile.isEmpty())
            env.insert(QStringLiteral("HOME"), QDir::toNativeSeparators(profile));
        else
            env.insert(QStringLiteral("HOME"), QDir::homePath());
    }
    proc->setProcessEnvironment(env);
}

RsyncRunner::RsyncRunner(QObject *parent)
    : QObject(parent)
{
    m_watchdog.setSingleShot(true);
    connect(&m_watchdog, &QTimer::timeout, this, &RsyncRunner::onWatchdogTimeout);
}

RsyncRunner::~RsyncRunner()
{
    if (m_proc) {
        // 同样必须先断连：kill 之后 QProcess 会发 finished，
        // onFinished 会往下走 launchNext()，在析构过程中再起一个 rsync，
        // 还会把结果 emit 给正在销毁的 SyncPanel。
        // SyncPanel 的析构函数会先 cancel()（设了 m_cancelRequested），
        // 所以大部分情况走的是"已取消"分支不会弹框；但那依赖对面先调过 cancel，
        // 这里不能把正确性寄托在调用顺序上。断开是零成本的保险。
        m_proc->disconnect(this);
        m_proc->kill();
        m_proc->waitForFinished(2000);
    }
    // 顺序不能反：作业带 KILL_ON_JOB_CLOSE，最后一个句柄一关，
    // 作业里还活着的进程会被系统终止；先关作业等于提前杀掉还没退干净的 ssh。
    closeJob();
    cleanupRemoteMarkers();
    cleanupSecrets();
    cleanupListFiles();
}

QStringList RsyncRunner::sshArgs(const TargetConfig &target, bool quotePath) const
{
    QStringList a;
    a << QStringLiteral("-p") << QString::number(target.port);

    if (target.useKey()) {
        // 私钥路径可能带空格；交给 rsync -e 时要带引号，自己拼路径时不能带。
        const QString key = quotePath
            ? QLatin1Char('"') + target.keyPath + QLatin1Char('"')
            : target.keyPath;
        a << QStringLiteral("-i") << key;
        // 私钥模式下必须禁用密码回退：密钥认证失败时 ssh 会悄悄改用登录口令，
        // 用户明明选了"私钥认证"却不知道实际走的是密码，既不安全也让人误判。
        a << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=publickey");
        a << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
    } else {
        // 明确只走密码，避免 ssh 先挨个试密钥把界面卡住。
        a << QStringLiteral("-o") << QStringLiteral("PreferredAuthentications=password");
        a << QStringLiteral("-o") << QStringLiteral("PubkeyAuthentication=no");
        a << QStringLiteral("-o") << QStringLiteral("NumberOfPasswordPrompts=1");
    }

    a << QStringLiteral("-o")
      << (target.autoAcceptHostKey ? QStringLiteral("StrictHostKeyChecking=no")
                                   : QStringLiteral("StrictHostKeyChecking=yes"));
    if (target.autoAcceptHostKey) {
        // 主机公钥既然自动接受，就没必要再往 known_hosts 里写。
        // 客户机的 HOME 下常常没有 .ssh 目录，ssh 会刷两条
        // "Could not create directory" / "Failed to add the host to the list of known hosts"，
        // 同步日志里看着像故障，其实是无害的。
        a << QStringLiteral("-o") << (QStringLiteral("UserKnownHostsFile=") + nullKnownHosts());
        // 再把 ssh 自己的 warning 级日志压掉，否则每次同步开头都会冒一行
        // "Warning: Permanently added ... to the list of known hosts"。
        // 只压 ssh 客户端的，rsync 自己的输出不受影响，权限错误照常报。
        a << QStringLiteral("-o") << QStringLiteral("LogLevel=ERROR");
    }
    return a;
}

// 自动接受主机公钥时丢弃 known_hosts 的落地文件。
//
// 这里两个平台都用 /dev/null，不能在 Windows 上写 "NUL"：
// 实测打包进来的 msys 版 ssh.exe 不把 NUL 当空设备，会在工作目录下
// 真的创建一个名为 NUL 的文件（内容就是 known_hosts），
// 客户机上就变成"程序跑过的地方多一个怪文件"。
// /dev/null 由 msys 运行时映射成真正的空设备，实测不落盘。
QString RsyncRunner::nullKnownHosts()
{
    return QStringLiteral("/dev/null");
}

// 返回的只是选项串，不含 "ssh" 程序名：launchNext 会自己拼上。
QString RsyncRunner::sshOption(const TargetConfig &target) const
{
    return sshArgs(target, true).join(QLatin1Char(' '));
}

// 连接探测：跑一条 ssh 空命令，能回显就说明网络 + 账号 + 密码都对。
//
// 选 `echo` 而不是 `exit`：两者的认证流程一样，但 echo 会真的走一遍远端 shell，
// 能顺带验证远端 shell 能不能起（目标机 OpenSSH 装了但 shell 路径不对时，
// exit 成功、echo 失败，那才是用户真正会遇到的故障）。
bool RsyncRunner::probeConnection(const TargetConfig &target, QString *detail, int timeoutMs) const
{
    if (detail)
        detail->clear();

    if (!target.isValid()) {
        if (detail)
            *detail = QObject::tr("\xE7\x9B\xAE\xE6\xA0\x87\xE7\x94\xB5\xE8\x84\x91 IP \xE6\x88\x96\xE7\x99\xBB\xE5\xBD\x95\xE8\xB4\xA6\xE5\x8F\xB7\xE4\xB8\xBA\xE7\xA9\xBA\xE3\x80\x82");
        return false;
    }
    if (!RsyncRunner::isSafeLoginField(target.user, false)
        || !RsyncRunner::isSafeLoginField(target.host, true)) {
        if (detail)
            *detail = QObject::tr("\u76EE\u6807\u7535\u8111 IP \u6216\u767B\u5F55\u8D26\u53F7\u542B\u6709\u975E\u6CD5\u5B57\u7B26");
        return false;
    }

    const QString ssh = locateTool(QStringLiteral("ssh"));
    if (ssh.isEmpty()) {
        if (detail)
            *detail = QObject::tr("\xE6\x9C\xAA\xE6\x89\xBE\xE5\x88\xB0 ssh \xE5\xAE\xA2\xE6\x88\xB7\xE7\xAB\xAF\xE5\x8F\xAF\xE6\x89\xA7\xE8\xA1\x8C\xE6\x96\x87\xE4\xBB\xB6\xE3\x80\x82");
        return false;
    }

    QString askpass, passFile;
    // createAskpass **成功时返回空串**，失败时返回一句具体的错误说明。
    //
    // 这里的条件原来是反的：
    //     if (createAskpass(...).isEmpty() && !askpass.isEmpty()) { 报错; return false; }
    // "建成功"恰好同时满足"返回空串"和"askpass 有路径"两条 —— 于是**每建成功一次**
    // 就进这个分支，把刚建好的两个临时文件删掉，再报一句
    // 「无法准备登录口令，请检查软件目录是否可写」，然后 return false。
    //
    // 后果是「测试网络连接」**永远失败**，而且报的方向跟真实原因无关：
    // 真失败时返回的是**非空**错误串，条件不成立，代码静默继续往下走，
    // 带一个空 askpass 去跑 ssh，最后让用户看到的是 ssh 的认证报错，
    // 而真正的失败原因（临时口令文件建不起来）在日志里一个字都没有。
    //
    // 修法：按返回串判断，并把 createAskpass 自己的那句具体原因直接透出去
    // （它分得清"写不进临时目录"和"密码里含换行"，不该被压成同一句话）。
    QString askpassErr = createAskpass(target, &askpass, &passFile);
    if (!askpassErr.isEmpty()) {
        QFile::remove(askpass);
        QFile::remove(passFile);
        if (detail)
            *detail = askpassErr;
        return false;
    }

    QProcess p;
    applyAskpassEnv(&p, askpass);
    p.setProgram(ssh);
    p.setArguments(sshArgs(target, false)
                   << QStringLiteral("%1@%2").arg(target.user, target.host)
                   << QStringLiteral("echo srt-ok"));
    p.start();

    bool ok = p.waitForFinished(timeoutMs);
    if (!ok) {
        p.kill();
        p.waitForFinished(2000);
        QFile::remove(askpass);
        QFile::remove(passFile);
        if (detail)
            *detail = QObject::tr("\xE8\xBF\x9E\xE6\x8E\xA5\xE8\xB6\x85\xE6\x97\xB6\xE3\x80\x82")
                      + QStringLiteral("  ")
                      + probeLayerDetail(QString(), LayerNetwork);
        return false;
    }

    const QString out = QString::fromLocal8Bit(p.readAllStandardOutput()).trimmed();
    const QString err = QString::fromLocal8Bit(p.readAllStandardError()).trimmed();
    const int code = p.exitCode();
    QFile::remove(askpass);
    QFile::remove(passFile);

    if (code == 0 && out.contains(QLatin1String("srt-ok"))) {
        if (detail)
            *detail = QObject::tr("\u8FDE\u63A5\u6210\u529F");
        return true;
    }

    if (detail) {
        // 把 ssh 的原话带回去：认证失败 / 主机不可达 / 端口不通，
        // 三者的提示词完全不同，笼统一句"连接失败"会让人自己猜。
        const QString last = err.section(QLatin1Char('\n'), -1).trimmed();
        *detail = last.isEmpty()
            ? QObject::tr("\u8FDE\u63A5\u5931\u8D25\uFF08\u9000\u51FA\u7801 %1\uFF09").arg(code)
            : QObject::tr("\u8FDE\u63A5\u5931\u8D25\uFF1A%1").arg(last);
        // 归因：Connection refused / Connection timed out /
        // Permission denied (publickey,password) 三种原话方向完全不同，
        // 光把原文回显出去还是要用户自己猜，这里直接判到层并给下一步。
        *detail += QStringLiteral("  ") + probeLayerDetail(err);
    }
    return false;
}

QString RsyncRunner::remoteDir(const TargetConfig &target, const QString &subDir) const
{
    QString base = target.remoteRoot.trimmed();
    // 只砍多余的末尾斜杠："/" 本身就是根，砍成空就变成 home 了。
    while (base.size() > 1 && base.endsWith(QLatin1Char('/')))
        base.chop(1);
    // 用户可能填 /data//spe/ 这种重复斜杠，不归一化远端就会多出一层空目录名
    //
    // 远端的 "~"：实测 msys rsync 3.5.1 会把 remoteSpec 里的 "~" 原样传给远端
    // rsync 服务端，服务端不做 shell 展开，于是远端 mkdir "~/x" 直接失败，整组
    // 同步退出码 11。而 rsync 的远端相对路径是相对登录目录（home）解析的，所以
    // 把 "~" 开头统一改写为等价的相对路径：
    //   ~        -> （空，即 home 根）
    //   ~/xxx    -> xxx
    // 这样 remoteRoot 留空（= home 根）、填 "~" 或 "~/xxx" 都能正常工作。
    //
    // 剥 "~" 必须走 remoteRootWithoutTilde()：start() 里的越界校验用的是同一个
    // 函数，两边看的串必须一致，否则 `~/../x` 会校验通过却写到 home 之外去。
    base = remoteRootWithoutTilde(base);
    // ~user/xxx 这种形式**必须明确拒绝**，不能悄悄放行。
    // 实测：ssh "mkdir -p -- '~apuser/x'" 在远端 home 下建出的是一个字面量
    // 名为 "~apuser" 的垃圾目录（单引号里 shell 不展开），而 rsync 那边
    // 又把它当成 "~apuser" 用户的主目录，于是报
    // "--files-from hostname is not the same as the transfer hostname"，
    // 这句话完全不像是在说用户输入的路径有问题，会把人带偏。
    // 正确写法是填绝对路径（/home/apuser/x），或者干脆用 ~/ 走自己的家目录。
    if (base.startsWith(QLatin1Char('~')))
        return QStringLiteral("\x01~user\x01");   // 哨兵：调用方看到就报错

    // 子目录必须先自己归一化再拼。
    // 以前是 base + '/' + subDir 直接拼，base 为空时拼出来的是 "/spe" 这种
    // **绝对路径**：remoteRoot 留空（= 远端 home 根）时，规则写 "spe"
    // 会把文件放到文件系统根目录 /spe，而不是 ~/spe。
    // 而且 mkdir 和 rsync 拿到的是两种不同的字符串，一个建 /spe 一个写 ~/spe，
    // 建好的目录空着、文件落在别处，界面还显示"同步完成"。
    const QString sub = subDir.isEmpty()
                            ? QString()
                            : QDir::cleanPath(QLatin1Char('/') + subDir);   // 形如 "/spe"
    if (base.isEmpty())
        return sub.isEmpty() ? QString() : sub.mid(1);      // home 根下的相对路径："spe"

    base = QDir::cleanPath(base);
    if (sub.isEmpty())
        return base;
    if (base.endsWith(QLatin1Char('/')))                     // base 是文件系统根 "/"
        return base + sub.mid(1);
    return base + sub;                                       // "/data" + "/spe"
}

QString RsyncRunner::remoteSpec(const TargetConfig &target, const QString &subDir) const
{
    // 必须和 mkdir 用的是同一个函数、同一套归一化结果：
    // 两边算出不同字符串时，目录建在一个地方、文件写到另一个地方，
    // 而界面照样报成功。整条路径（含子目录）一起转义，不要分开拼。
    const QString path = escapeForRemoteShell(remoteDir(target, subDir));

    // 空路径 = home 根：这里必须保留尾冒号。实测 msys rsync 3.5.1 把
    // "user@host"（无冒号）当成本地路径：会在本地当前目录创建一个名为
    // "user@host" 的目录、exit 0 假成功，远端什么都没收到。
    // "user@host:"（尾冒号）= 远端 home 根；"user@host:/" 才是系统根，
    // 所以不能加 "/"。
    if (path.isEmpty())
        return QStringLiteral("%1@%2:").arg(target.user, target.host);
    return QStringLiteral("%1@%2:%3/").arg(target.user, target.host, path);
}

void RsyncRunner::start(const TargetConfig &target,
                        const QString &localRoot,
                        const QVector<SyncRule> &rules,
                        const QVector<FileItem> &files,
                        bool onlyNewFiles)
{
    m_onlyNew = onlyNewFiles;
    m_groups.clear();
    m_groupIndex = 0;
    m_done  = 0;
    m_total = files.size();
    m_failed = false;
    m_cancelRequested = false;
    // 复用同一个 RsyncRunner 连跑多次时，上一轮的标记不能带过来，
    // 否则会把这一轮正常同步判成"目录被换掉"。
    m_verifyToken.clear();
    m_verifyMarker.clear();
    m_verifyDir.clear();
    m_verifyBuffer.clear();
    m_verifiedDirs.clear();
    m_inVerify = false;
    m_target = target;
    m_localRoot = localRoot;
    cleanupListFiles();

    // 主机名 / 账号的字符校验放在最前面：这两个字段拼进的是 "user@host:path"，
    // 里面有空格或冒号时 rsync 会把这一串拆开，可能报一堆看不懂的错，
    // 也可能安静地连到别的地方去。必须在起任何子进程之前拦下来。
    if (!isSafeLoginField(target.host, true) || !isSafeLoginField(target.user, false)) {
        emit logText(QString::fromUtf8(u8"\u4E3B\u673A\u540D\u6216\u8D26\u53F7\u91CC\u542B\u4E0D\u5141\u8BB8\u7684\u5B57\u7B26\uFF08\u7A7A\u683C\u3001\u5192\u53F7\u3001\u5F15\u53F7\u3001\u5206\u53F7\u7B49\uFF09\u3002"));
        emit allFinished(false);
        return;
    }

    const QString rsyncBin = locateTool(QStringLiteral("rsync"));
    if (rsyncBin.isEmpty()) {
        reportAttribution(QString(), LayerLocal);
        emit logText(QString::fromUtf8(u8"\u672A\u627E\u5230 rsync \u53EF\u6267\u884C\u6587\u4EF6\u3002"));
        emit allFinished(false);
        return;
    }
    if (locateTool(QStringLiteral("ssh")).isEmpty()) {
        reportAttribution(QString(), LayerLocal);
        emit logText(QString::fromUtf8(u8"\u672A\u627E\u5230 ssh \u5BA2\u6237\u7AEF\u53EF\u6267\u884C\u6587\u4EF6\u3002"));
        emit allFinished(false);
        return;
    }
    if (target.useKey() && !QFileInfo(target.keyPath).isFile()) {
        reportAttribution(QString(), LayerLocal);
        // ssh 拿到一个不存在的私钥不会立刻报错退出，而是继续往下走、
        // 干等到看门狗超时（默认 3 分钟）。用户看到的就是"点了同步没反应"。
        emit logText(QString::fromUtf8(u8"\u79C1\u94A5\u6587\u4EF6\u4E0D\u5B58\u5728\u6216\u4E0D\u53EF\u8BFB\uFF1A%1").arg(target.keyPath));
        emit allFinished(false);
        return;
    }
    if (target.useKey()) {
        // 私钥预检：文件存在但内容损坏、或带了口令的私钥，ssh 一样会干等到
        // 看门狗超时才报失败。这里用 ssh-keygen -y 免密解析一遍，坏密钥立刻
        // 暴露。图形界面没有终端可输口令，私钥模式只支持无口令密钥，
        // 带口令密钥用户应该改用密码登录。
        const QString keygenBin = locateTool(QStringLiteral("ssh-keygen"));
        if (!keygenBin.isEmpty()) {
            QProcess probe;
            probe.start(keygenBin,
                        QStringList() << QStringLiteral("-y")
                                      << QStringLiteral("-P") << QString()
                                      << QStringLiteral("-f") << target.keyPath);
            // 正常私钥解析只要十几毫秒，所以 1500ms 是相当宽的预算（这段是阻塞
            // 等待，跑在 UI 线程上，给太长等于点了同步要干等好几秒）。
            //
            // 关键：**"跑完了但失败"和"根本没跑完"必须分开**，以前这两件事被
            // 合并成一个条件（waitForFinished(...) || exitCode() != 0），于是
            // "子进程被卡住"也被报成"私钥已损坏或带口令"。客户现场的典型故障
            // 恰恰是后者：麒麟的安全中心弹窗挡住子进程，私钥本身完全正常，
            // 客户照着提示去查私钥文件，永远找不到真因。
            const bool keyProbeDone = probe.waitForFinished(1500);
            const QString keyProbeErr =
                QString::fromUtf8(probe.readAllStandardError()).trimmed();
            if (!keyProbeDone) {
                probe.kill();
                probe.waitForFinished(1000);
                emit logText(QString::fromUtf8(u8"\u79C1\u94A5\u9884\u68C0\u7A0B\u5E8F ssh-keygen \u5728 1.5 \u79D2\u5185\u6CA1\u6709\u8DD1\u5B8C\uFF0C\u5DF2\u88AB\u7EC8\u6B62\u3002"));
                reportAttribution(keyProbeErr, LayerLocal,
                                   QString::fromUtf8(kBlockedAdvice));
                emit allFinished(false);
                return;
            }
            if (probe.exitCode() != 0) {
                emit logText(QString::fromUtf8(u8"\u79C1\u94A5\u65E0\u6CD5\u514D\u5BC6\u89E3\u6790\uFF1A%1")
                                 .arg(target.keyPath));
                // 这一支才是「私钥本身真的有问题」这一情形。上一句是既有的概括文案，
                // 这里补上 ssh-keygen 的原话和归因，两者必须分开：
                // 带口令的私钥落在认证层（换个不带口令的密钥，或改用密码登录即可），
                // 损坏 / 权限不对的私钥落在本机层。处置完全不同，不能再混着说。
                emit logText(QString::fromUtf8(u8"ssh-keygen \u539F\u59CB\u8F93\u51FA\uFF1A")
                             + keyProbeErr);
                reportAttribution(keyProbeErr);
                emit allFinished(false);
                return;
            }
        }
    }
    if (!QFileInfo(m_localRoot).isDir()) {
        reportAttribution(QString(), LayerLocal);
        emit logText(QString::fromUtf8(u8"\u672C\u5730\u6839\u76EE\u5F55\u4E0D\u5B58\u5728\u6216\u4E0D\u53EF\u8BFB\uFF1A%1").arg(m_localRoot));
        emit allFinished(false);
        return;
    }

    // 同一后缀命中多条规则时只取第一条，否则同一批文件会被反复传输一遍
    QVector<QString> usedSuffixes;
    for (const SyncRule &rule : rules) {
        if (usedSuffixes.contains(rule.suffix))
            continue;
        usedSuffixes.append(rule.suffix);

        Group g;
        g.remoteBase = remoteSpec(target, rule.remoteSubDir);
        g.remoteDir  = remoteDir(target, rule.remoteSubDir);
        for (const FileItem &f : files) {
            if (f.suffix == rule.suffix)
                g.files.append(f.relativePath);
        }
        if (!g.files.isEmpty())
            m_groups.append(g);
    }

    if (m_groups.isEmpty()) {
        emit logText(QString::fromUtf8(u8"\u6CA1\u6709\u5339\u914D\u5230\u8981\u540C\u6B65\u7684\u6587\u4EF6\u3002"));
        emit allFinished(true);
        return;
    }

    // " 和 \ 在远端路径里无法可靠传给 rsync：
    //   "  被当成引号边界，rsync 直接失败；
    //   \  QDir::cleanPath 会先把它转成路径分隔符（back\slash 变成 back/slash），
    //      用户以为是一个目录名，实际落在了另一个目录，而且界面显示同步成功。
    // 没有任何转义写法能救回来，只能在归一化之前对用户原始输入拒绝，
    // 否则就是静默把数据写到错地方。
    //
    // 控制字符（\r \n \t DEL 之类）同样必须拒，理由见下面循环里的注释。
    auto reject = [this, &target](const QString &raw) {
        for (const QChar c : raw) {
            // 控制字符：判据直接搬 isSafeLoginField 里那一条
            // （本文件 "if (u < 0x20 || u == 0x7F) return false;"），
            // 但**只搬这一条**，不搬它对空格/引号/冒号的整套要求 ——
            // 远端路径是允许 ' 和空格的（带单引号的远端根是已定案的正常用法，
            // 见 run-guifull 的 P19），照搬整套会把正常用户挡在门外。
            //
            // 为什么必须拒：路径是原样拼进 remoteSpec() 的（escapeForRemoteShell
            // 是恒等函数），控制字符就这么进了 rsync / ssh 的 argv。ssh 会拿它当
            // 参数边界，换行还能把远端那条命令拆成两条 —— 等于在客户机上执行
            // 任意程序。ini 能被手改就能混进来，所以开跑前拦。
            //
            // 提示只显示 U+XXXX 码位、不显示那个字符本身：控制字符打进日志会把
            // 日志行从中间截断（界面上是一片空白），用户也看不见它是什么。
            const ushort u = c.unicode();
            if (u < 0x20 || u == 0x7F) {
                emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u8DEF\u5F84\u4E0D\u80FD\u542B\u63A7\u5236\u5B57\u7B26\uFF08U+%1\uFF09\u3002")
                                     .arg(u, 4, 16, QLatin1Char('0')));
                emit allFinished(false);
                return true;
            }
            if (c != QLatin1Char('"') && c != QLatin1Char('\\'))
                continue;
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u8DEF\u5F84\u4E0D\u542B\u5B57\u7B26 [%1]\uFF1A\u8BE5\u5B57\u7B26\u65E0\u6CD5\u53EF\u9760\u5730\u4F20\u7ED9\u8FDC\u7AEF\uFF0C\u6587\u4EF6\u4F1A\u843D\u5230\u9519\u8BEF\u76EE\u5F55\u3002")
                             .arg(c));
            emit allFinished(false);
            return true;
        }

        // Windows 盘符开头的路径（"C:/test"、"d:\\data"）：在 Linux 远端上
        // ':' 和 '/' 都是合法目录名字符，所以上面那圈字符检查放它过去了，
        // 结果是远端 home 下凭空多出一个字面名叫 "C:" 的目录，
        // 文件老老实实传进去、退出码 0、界面显示"同步完成"——
        // 全程没有任何提示，用户以为传到了 D 盘其实根本没这回事。
        //
        // 明确按"盘符 + 冒号 + 路径分隔符"的完整形态拒绝，而不是见 ':' 就拒：
        // Linux 上 ": " 开头的目录名并不罕见，无差别拒绝会把正常用户挡在门外。
        // 判据取自本机填写的那一份，用户填的是 Windows 路径才命中。
        const QString t = raw.trimmed();
        if (t.size() >= 3
            && t.at(0).isLetter() && t.at(1) == QLatin1Char(':')
            && (t.at(2) == QLatin1Char('/') || t.at(2) == QLatin1Char('\\'))) {
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u8DEF\u5F84 [%1] \u770B\u8D77\u6765\u50CF Windows \u76D8\u7B26\u3002")
                             .arg(t));
            emit logText(QString::fromUtf8(u8"\u8FD9\u53F0\u8FDC\u7AEF\u662F Linux\uFF0C\u5B83\u4F1A\u5728\u5BB6\u76EE\u5F55\u4E0B\u5EFA\u4E00\u4E2A\u5B57\u9762\u540D"
                                 u8"\u53EB %1 \u7684\u76EE\u5F55\u3002"
                                 u8"\u8BF7\u6539\u6210\u7EDD\u5BF9\u8DEF\u5F84\uFF08\u5982 /home/\u7528\u6237\u540D/\u76EE\u5F55\uFF09"
                                 u8"\u6216\u8005\u7528 ~ \u8868\u793A\u5F53\u524D\u8D26\u53F7\u7684\u5BB6\u76EE\u5F55\u3002")
                             .arg(t.section(QLatin1Char(':'), 0, 0)));
            emit allFinished(false);
            return true;
        }
        return false;
    };
    // glob 元字符：--protect-args 挡不住 rsync 自己对远端路径做 glob 展开，
    // 实测证据见 escapeForRemoteShell 上方注释。这类字符不能靠转义补救
    // （转义反而会让目录名多出反斜杠，落到另一个目录去），只能拒。
    auto rejectGlob = [this](const QString &raw) {
        // 六个字符都是在**产品真实调用路径**上实测会被 rsync 展开/吞掉的：
        //   * ? [ ]   远端 glob 展开（父目录有匹配项时文件全落进那个匹配项，
        //             匹配多项时只有第一项收到文件，其余静默丢失）
        //   { }       brace expansion，{} 整段消失：用户填 gA{}B，文件传进 gAB，
        //             远端凭空多出一个用户没填过的目录
        // 上一轮曾用"直接传文件名"的简化复刻测出 gA{}B 是字面保留，据此把 {}
        // 放出行列；用产品真实路径（--files-from + ./ 源）复测才发现被吞。
        // **判定这类字符只能走产品真实调用路径**，简化复刻会得出相反结论。
        const QString globChars = QStringLiteral("*?[]{}");
        for (const QChar c : raw) {
            if (!globChars.contains(c))
                continue;
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u8DEF\u5F84\u4E0D\u80FD\u542B [%1]\u3002")
                             .arg(c));
            emit logText(QString::fromUtf8(u8"\u8FD9\u662F glob \u901A\u914D\u7B26\uFF0C"
                                            u8"rsync \u4F1A\u628A\u5B83\u5F53\u6210\u901A\u914D\u6A21\u5F0F\u5C55\u5F00\uFF0C"
                                            u8"\u6587\u4EF6\u4F1A\u843D\u5230\u4F60\u6CA1\u6709\u6307\u5B9A\u7684\u76EE\u5F55\uFF0C"
                                            u8"\u6216\u8005\u53EA\u6709\u7B2C\u4E00\u4E2A\u5339\u914D\u9879\u80FD\u6536\u5230\u6587\u4EF6"
                                            u8"\uFF08\u5176\u4F59\u9759\u9ED8\u4E22\u5931\uFF09\u3002"));
            emit allFinished(false);
            return true;
        }
        return false;
    };

    if (reject(target.remoteRoot) || rejectGlob(target.remoteRoot))
        return;

    // ~user/xxx 形式明确拒绝。remoteDir() 对这种输入返回 "\x01~user\x01" 哨兵。
    // 放行的话 ssh 会在远端 home 下建出一个字面名叫 "~user" 的垃圾目录，
    // rsync 再报一句 "hostname is not the same as the transfer hostname"，
    // 从错误信息完全看不出是用户路径写错了。
    {
        const QString trimmed = target.remoteRoot.trimmed();
        if (trimmed.startsWith(QLatin1Char('~'))
            && trimmed != QLatin1String("~")
            && !trimmed.startsWith(QLatin1String("~/"))) {
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u8DEF\u5F84\u4E0D\u80FD\u7528 ~\u7528\u6237\u540D \u8FD9\u79CD\u5F62\u5F0F\u3002"
                                             u8"\u8FD9\u91CC\u7684\u5F15\u53F7\u4E0D\u4F1A\u88AB\u8FDC\u7AEF\u5C55\u5F00\uFF0C"
                                             u8"\u6362\u6210\u7EDD\u5BF9\u8DEF\u5F84\uFF08\u5982 /home/\u7528\u6237\u540D/\u76EE\u5F55\uFF09"
                                             u8"\u6216\u8005\u7528 ~\u8868\u793A\u5F53\u524D\u8D26\u53F7\u7684\u5BB6\u76EE\u5F55\u3002")
                             .arg(trimmed));
            emit allFinished(false);
            return;
        }
    }

    // 远端相对路径里的 ".."：写 ".." 时文件会落到 remoteRoot 的**同级**目录，
    // 逃出了用户填的远端根，而界面照报"同步完成"。
    // 用户多半是想写一个叫 ".." 的目录名（极少见）或者手滑多打了点，
    // 两种都不该由程序替他猜。宁可拒绝。
    //
    // 校验必须放在建组之后、开跑之前：以前放在建组之前的精确字符串比较漏掉了
    // "/.."、"./.."、"a/../.."，这几种一路放行，最后既没传成文件又报绿色成功。
    const auto rejectEscapes = [this](bool escapes, const QString &what) {
        if (!escapes)
            return false;
        // 占位符必须是 %1，不能写 %s。
        //
        // QString::arg() 只认 %1 / %2 这种带编号的占位符，遇到 "%s" 一个都找不到，
        // 于是**什么都不替换**、也不报错。实测用户看到的是
        //     "远端%s不能包含 ..：它会把文件写到远端根目录之外的地方。"
        // 拦截本身是好的（文件确实没传出去），但这句话等于没告诉用户
        // 是他填的"根目录"还是"相对路径"出了问题，反而让人以为是程序乱码。
        emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF %1 \u4E0D\u80FD\u5305\u542B ..\uFF1A"
                                       u8"\u5B83\u4F1A\u628A\u6587\u4EF6\u5199\u5230\u8FDC\u7AEF\u6839\u76EE\u5F55\u4E4B\u5916\u7684\u5730\u65B9\u3002")
                         .arg(what));
        emit allFinished(false);
        return true;
    };
    // 判据验的就是这两个函数本身（见 RsyncRunner.h）：判据照抄一份的话，
    // 改坏实现、判据照样全绿。`~/../x` 正是靠 remoteRootEscapesHome 才拦得住 ——
    // 传原始串时 "~" 会被当成一个普通目录段，正好抵消掉它后面那一个 ".."，
    // 于是校验通过、文件真写到 home 之外，界面还报"同步完成"。
    if (rejectEscapes(remoteRootEscapesHome(target.remoteRoot),
                      QString::fromUtf8(u8"\u6839\u76EE\u5F55")))
        return;
    for (const SyncRule &rule : rules) {
        if (reject(rule.remoteSubDir) || rejectGlob(rule.remoteSubDir))
            return;
        if (rejectEscapes(subDirEscapesRoot(rule.remoteSubDir),
                          QString::fromUtf8(u8"\u76F8\u5BF9\u8DEF\u5F84")))
            return;
    }

    if (!target.useKey() && !target.password.isEmpty()) {
        const QString err = createAskpass(target, &m_askpassPath, &m_passFilePath);
        if (!err.isEmpty()) {
            emit logText(err);
            emit allFinished(false);
            return;
        }
    }

    emit logText(QString::fromUtf8(u8"\u5171 %1 \u4E2A\u6587\u4EF6\uFF0C\u5206 %2 \u7EC4\u540C\u6B65\u3002").arg(m_total).arg(m_groups.size()));
    // 开跑前把每个源文件的大小和修改时间记下来，传输结束后再比一遍。
    // 少了这一步，"传输中改了源文件"这件事只能靠用户自己去远端发现 ——
    // 而界面在这期间一直报"同步完成"。
    sweepStaleListFiles();
    m_sourceSnapshot.clear();
    m_sourceSnapshot.reserve(files.size());
    for (const FileItem &f : files) {
        const QFileInfo fi(QDir(m_localRoot).absoluteFilePath(f.relativePath));
        SourceSnapshot s;
        s.rel      = f.relativePath;
        s.size     = fi.size();
        s.modified = fi.lastModified();
        m_sourceSnapshot.append(s);
    }

    ensureRemoteDirs();
}

// 清掉上一次崩溃/强杀留在本地根目录里的 .srt-filelist-<pid>.txt。
//
// 这个清单是 rsync 的 --files-from 参数，**写在用户的数据目录里**。
// 正常路径 cleanupListFiles() 会删，崩溃、被强杀、断电时不会跑，
// 文件就永久留在那儿。
//
// 为什么在意：内容是用户全部待传文件名。它不会被误传走
// （FileScanner 在第 104 行已经排除了 .srt-filelist- 前缀，实测确认），
// 但它是用户数据目录里一堆看不懂的隐藏文件，攒多了会让人以为目录被污染，
// 也会让下一次同步的清单里混进上一轮的痕迹。
//
// 判据和清扫口令文件（F4）保持同一套口径：形态合法 ⇒ 数字 pid ⇒ 进程已死才删；
// 形态不合法（非数字、pid<=0）直接删。
// 这里和 %TEMP% 的取舍是一样的：glob 是 .srt-filelist-*.txt，
// 双向都夹死了专有前缀和扩展名，不可能是用户自己的文件；
// 而命名契约里 pid 永远在中间且必为数字，解析不出来就不属于任何运行实例。
// 代价是"将来某个版本改了命名"时可能误删它正在用的清单 ——
// 那种情况下 rsync 会立刻报 files-from 读不到并失败退出，是可见的，
// 不会静默传错数据；反之留一个永久清不掉的残留则永远是看不见的。
void RsyncRunner::sweepStaleListFiles() const
{
    if (m_localRoot.isEmpty())
        return;
    const QDir root(m_localRoot);
    const QStringList names = root.entryList(QStringList() << QStringLiteral(".srt-filelist-*.txt"),
                                             QDir::Files | QDir::NoDotAndDotDot);
    const QString prefix = QStringLiteral(".srt-filelist-");
    const QString suffix = QStringLiteral(".txt");
    const int self = QCoreApplication::applicationPid();
    for (const QString &name : names) {
        if (!name.startsWith(prefix) || !name.endsWith(suffix))
            continue;
        const QString seg = name.mid(prefix.size(),
                                     name.size() - prefix.size() - suffix.size());
        bool ok = false;
        const int pid = seg.toInt(&ok);
        // 解析不出合法 pid 的直接删，口径和清扫口令文件（F4）保持一致。
        // 留着的理由和那里一样：形态对不上 ⇒ 不属于任何运行实例 ⇒ 删掉安全。
        if (!ok || pid <= 0) {
            QFile::remove(root.absoluteFilePath(name));
            continue;
        }
        if (pid == self || processAlive(pid))
            continue;
        QFile::remove(root.absoluteFilePath(name));
    }
}

// 比对"开跑前"和"跑完后"的源文件状态，返回期间被改动过的相对路径。
//
// 只比大小和修改时间，不比内容：传输几百 MB 再全量重算一遍校验和的代价
// 和重新传一次差不多，而且在 rsync 已经读过的那些页上再读一遍会明显拖慢同步。
//
// 这条判据**覆盖不到**"同大小 + 同 mtime 的整体重写"：那种情况下
// 上面两项都没变，只能靠内容比对才看得出来，而那正是同步期间最不该做的事。
// 换句话说，能抓的是"文件被截短/追加/换名"这类常见的写入中途状态，
// 不是所有可能的并发改写。这里不夸大成"杜绝快照问题"。
QStringList RsyncRunner::filesMutatedDuringTransfer() const
{
    QStringList changed;
    if (m_localRoot.isEmpty())
        return changed;
    const QDir root(m_localRoot);
    for (const SourceSnapshot &s : m_sourceSnapshot) {
        const QFileInfo fi(root.absoluteFilePath(s.rel));
        if (!fi.exists()) {
            // 传输中源文件被删了：远端留着的是删除前的版本，同样要报
            changed.append(s.rel);
            continue;
        }
        if (fi.size() != s.size || fi.lastModified() != s.modified)
            changed.append(s.rel);
    }
    return changed;
}

// rsync 自己不会创建目标路径的中间层（老版本也没有 --mkpath），
// 所以先用一次 ssh 把所有组的目标目录建出来，失败只记日志不中断：
// 目录本来就在时 mkdir -p 也是成功的，权限不足时让 rsync 去报真实原因。
void RsyncRunner::ensureRemoteDirs()
{
    if (m_cancelRequested) {
        cleanupSecrets();
        emit allFinished(!m_failed && !m_cancelRequested);
        return;
    }

    QStringList dirs;
    bool needHomeRoot = false;
    for (const Group &g : m_groups) {
        // 空 remoteDir 表示远端 home 根，本来就在，不用建
        if (g.remoteDir.isEmpty())
            needHomeRoot = true;
        else if (!dirs.contains(g.remoteDir))
            dirs.append(g.remoteDir);
    }

    const QString sshBin = locateTool(QStringLiteral("ssh"));
    if (sshBin.isEmpty() || (dirs.isEmpty() && !needHomeRoot)) {
        launchNext();
        return;
    }

    // 远端命令交给远端 shell 解析，路径里的空格和引号必须自己转义。
    // remoteDir() 已经把 "~" 开头改写成了等价的相对路径（见 remoteDir 注释），
    // 所以这里统一用单引号包裹、原样传给远端 shell，落点与 rsync 一致；
    // 不需要再处理 "$HOME" 展开那种情况。
    QStringList quoted;
    for (const QString &d : dirs) {
        QString e = d;
        e.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        quoted << (QLatin1Char('\'') + e + QLatin1Char('\''));
    }

    // rsync 自己发现不了目标目录被换掉这件事，只能靠传前传后对暗号。
    //
    // 踩过的坑：本来记的是目录 inode，实测在 tmpfs/overlayfs 上不可靠——
    // 目录被 rm -rf 之后 rsync 重建的新目录很可能复用了同一个 inode 号，
    // 前后一比完全相同，静默丢数据照样漏过去（实测 1200 个文件只剩 379 个，
    // 界面还报"同步完成"）。改成在目标目录里放一个带随机串的标记文件，
    // 传完再读回来并删掉：目录被删或被替换，标记一定不在。
    //
    // 标记用点开头的名字，不在任何一组的文件清单里，rsync 不会碰它。
    QStringList statArgs = dirs;
    if (needHomeRoot)
        // ssh 登录后的工作目录就是远端 home，"." 就是 home 根
        statArgs << QStringLiteral(".");

    m_verifyToken = QString::number(QCoreApplication::applicationPid())
                    + QLatin1Char('-') + QString::number(QDateTime::currentMSecsSinceEpoch());
    m_verifyMarker = QStringLiteral(".srt-verify-") + m_verifyToken;

    QStringList drops;
    for (const QString &p : statArgs) {
        QString dir = p;
        dir.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        drops << (QStringLiteral("printf %s '") + m_verifyToken + QLatin1String("' > '")
                  + dir + QLatin1Char('/') + m_verifyMarker + QLatin1Char('\''));
    }

    m_proc = new QProcess(this);
#ifdef Q_OS_WIN
    attachToJob(m_proc);
#endif
    applyAskpassEnv(m_proc, m_askpassPath);
    connect(m_proc, &QProcess::readyReadStandardError, this, &RsyncRunner::onStderr);
    // FailedToStart 不会发 finished()，只等 finished 的话这一组会永久挂死：
    // 看门狗到点后 kill 一个 NotRunning 进程也等不到 finished，
    // 界面就一直卡在"同步中"，连取消都没用。
    connect(m_proc, &QProcess::errorOccurred, this, &RsyncRunner::onProcError);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RsyncRunner::onDirsFinished);
    m_watchdog.start(watchdogMs());

    QStringList args = sshArgs(m_target, false);
    args << (m_target.user + QLatin1Char('@') + m_target.host)
         << (QStringLiteral("mkdir -p -- ") + quoted.join(QLatin1Char(' '))
             + QStringLiteral("; ") + drops.join(QStringLiteral("; ")));
    m_stderrTail.clear();
    m_proc->start(sshBin, args);
}

void RsyncRunner::onDirsFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    m_watchdog.stop();
    // readyReadStandardError 不保证在 finished 之前投递过，
    // 不补这一次读取，目录创建失败就只剩一句"退出码 255"没有原因。
    takeStderrTail();

    if (exitCode != 0) {
        emit logText(QString::fromUtf8(u8"\u521B\u5EFA\u8FDC\u7AEF\u76EE\u5F55\u5931\u8D25\uFF08\u9000\u51FA\u7801 %1\uFF09%2")
                         .arg(exitCode)
                         .arg(m_stderrTail.trimmed().isEmpty()
                                  ? QString()
                                  : QStringLiteral("  ") + m_stderrTail.trimmed()));
    }
    // mkdir -p 自己就失败（连不上 / 没权限）时，原因同样要留给弹框：
    // 这一步的失败往往才是真正的根因，rsync 后面报的是它的次生结果。
    if (exitCode != 0 && !m_stderrTail.trimmed().isEmpty())
        m_lastFailure = m_stderrTail.trimmed();
    // 四层归因：mkdir 这一步到底是连不上、没权限还是磁盘满，说的是完全不同的
    // 三个方向，而后面 rsync 报出来的都是它的次生结果，所以必须在这里按原文判一次。
    if (exitCode != 0)
        reportAttribution(m_stderrTail);
    m_stderrTail.clear();

    // mkdir 那步自己就失败了（连不上、没权限），标记多半也没写进去，
    // 这时再起一次 ssh 去删纯属多余——目标本来就连不上，还容易卡住收尾。
    if (exitCode != 0)
        m_verifyMarker.clear();

    m_proc->deleteLater();
    m_proc = nullptr;
    launchNext();
}

// rsync 退出码 0 不等于数据真的到了。
//
// 实测复现的丢数据路径：传输过程中有人（或清理脚本）把远端目标目录删掉。
// Linux 上目录一旦被 unlink，已经打开它的进程仍能往那个 inode 里写，
// rsync 就是这种情况——它把剩下的文件全部写进一个已经没有目录项的 inode，
// 退出码 0，界面显示"同步完成"，远端实际少了开头已经落地的那些文件。
// 实测：3000 个文件在落地 224 个时被删，rsync 报成功，远端只剩 2657 个。
//
// rsync 自己发现不了这个，退出码也不带任何信息。能在传输前后各 stat 一次
// 目标目录的 inode：目录被删或被换成别的目录，inode 必然对不上，直接判失败。
// 每组多一次 ssh 的 stat，几十毫秒；核对不出来（没记录到 inode、远端没有 stat、
// 或 ssh 本身失败）一律放行，不能因为核对手段失效就把正常同步判成失败。
void RsyncRunner::verifyRemoteDir(const QString &remoteDir)
{
    // 这个远端目录本轮已经核对过了：直接跳过。
    //
    // 标记核对用的是 "cat marker; rm -f marker"，读完即删。而分组只按后缀去重、
    // 不按远端目录去重，所以"两条规则的相对路径都留空"这种最自然的配置会让
    // 多组分到同一个目录：第一组核对完把标记删了，第二组 cat 读回空串，
    // != token，于是报"远端目录在传输过程中被删除或替换"——文件其实全传对了，
    // 重跑还是同样失败。记下已核对的目录，这条死路就堵住了。
    if (m_verifiedDirs.contains(remoteDir)) {
        ++m_groupIndex;
        launchNext();
        return;
    }

    if (m_verifyToken.isEmpty()) {
        launchNext();
        return;
    }

    const QString sshBin = locateTool(QStringLiteral("ssh"));
    if (sshBin.isEmpty()) {
        launchNext();
        return;
    }

    // 空 remoteDir 是远端 home 根，标记写在 ssh 登录的工作目录里。
    // 这里必须跟着用 "."，拼成 "/.srt-verify-xxx" 会去系统根下找，读不到，
    // 把一次完全正常的 home 根同步判成失败。
    QString dir = remoteDir.isEmpty() ? QStringLiteral(".") : remoteDir;
    dir.replace(QLatin1Char('\''), QLatin1String("'\\''"));
    const QString marker = QLatin1Char('\'') + dir + QLatin1Char('/') + m_verifyMarker + QLatin1Char('\'');
    m_verifyDir = remoteDir;
    m_verifyBuffer.clear();
    m_inVerify = true;

    m_proc = new QProcess(this);
#ifdef Q_OS_WIN
    attachToJob(m_proc);
#endif
    applyAskpassEnv(m_proc, m_askpassPath);
    connect(m_proc, &QProcess::errorOccurred, this, &RsyncRunner::onProcError);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, &RsyncRunner::onVerifyStdout);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RsyncRunner::onVerifyFinished);
    m_watchdog.start(watchdogMs());

    QStringList args = sshArgs(m_target, false);
    args << (m_target.user + QLatin1Char('@') + m_target.host)
         // 读回标记再删掉。标记对不上就是目录在中途被换掉了。
         << (QStringLiteral("cat -- ") + marker + QStringLiteral(" 2>/dev/null; rm -f -- ") + marker);
    m_proc->start(sshBin, args);
}

void RsyncRunner::onVerifyStdout()
{
    if (!m_proc)
        return;
    m_verifyBuffer += QString::fromUtf8(m_proc->readAllStandardOutput());
}

void RsyncRunner::onVerifyFinished(int exitCode, QProcess::ExitStatus status)
{
    Q_UNUSED(status)
    m_watchdog.stop();
    if (m_proc)
        m_verifyBuffer += QString::fromUtf8(m_proc->readAllStandardOutput());

    // ssh 自己没跑起来、或远端目录已经彻底没了，exitCode 非 0：
    // 这种情况下标记本来就读不回来，分不清是"被换掉"还是"核对没做成"，
    // 只能放行。核对手段失效不能反过来把一次正常同步判成失败。
    if (exitCode == 0 && m_verifyBuffer.trimmed() != m_verifyToken) {
        m_failed = true;
        emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u76EE\u5F55 %1 \u5728\u4F20\u8F93\u8FC7\u7A0B\u4E2D\u88AB\u5220\u9664\u6216\u66FF\u6362\uFF0C"
                                         u8"\u672C\u7EC4\u4F20\u8F93\u7ED3\u679C\u4E0D\u53EF\u4FE1\uFF0C\u5DF2\u5224\u5B9A\u5931\u8D25\u3002")
                             .arg(m_verifyDir.isEmpty()
                                      ? QString::fromUtf8(u8"\uFF08\u8FDC\u7AEF\u4E3B\u76EE\u5F55\uFF09")
                                      : m_verifyDir));
    } else if (exitCode == 0) {
        // 核对通过。记下来，同一目录的下一组直接跳过 —— 见 verifyRemoteDir 注释。
        m_verifiedDirs.insert(m_verifyDir);
    }

    m_verifyBuffer.clear();
    m_verifyDir.clear();
    m_inVerify = false;
    m_stderrTail.clear();

    m_proc->deleteLater();
    m_proc = nullptr;
    ++m_groupIndex;
    launchNext();
}

// 收尾清理的**唯一次序来源**：先远端标记、后口令文件。
//
// 为什么必须把次序做成一张可读的表，而不是在 launchNext() 里直接写死两次调用：
// 判据要能验"标记清理跑在凭据被删之前"。写死的话判据只能读到一句注释，
// 谁把两行对调了它照样全绿 —— 这正是本条要防的回归。
// 表和实现共用同一个 kFinishOrder，判据读的是产品这一份，不是照抄。
//
// 顺序反了会怎样：cleanupRemoteMarkers() 要 ssh 到接收端执行 `rm -f`，
// 而它要的凭据（SSH_ASKPASS 脚本 + 口令文件）正是 cleanupSecrets() 删掉的那两个。
// 先删凭据再清理 ⇒ 那个 ssh 没有口令、连不上接收端 ⇒ `rm` 没执行 ⇒
// 客户远端目录长期躺着 .srt-verify-<pid>-<时间戳>。
// 实测（Windows 进程内探针，取消同步 3/3）：原顺序残留 >0；
// 注入一个能工作的 SSH_ASKPASS 后残留 = 0；成功路径残留同样是 0。
QStringList RsyncRunner::finishCleanupSteps()
{
    static const char *kFinishOrder[] = {
        "remote-markers",   // 必须在前：此刻凭据还在，ssh 连得上接收端
        "secrets"           // 在后：删掉 askpass 脚本和明文口令文件
    };
    QStringList out;
    for (const char *s : kFinishOrder)
        out.append(QString::fromLatin1(s));
    return out;
}

void RsyncRunner::runFinishCleanup()
{
    const QStringList steps = finishCleanupSteps();
    for (const QString &s : steps) {
        if (s == QLatin1String("remote-markers"))
            cleanupRemoteMarkers();
        else if (s == QLatin1String("secrets"))
            cleanupSecrets();
    }
}

void RsyncRunner::launchNext()
{
    if (m_cancelRequested || m_groupIndex >= m_groups.size()) {
        // 取消也算"没干完"，报 false 让界面别显示"同步完成"。
        // 具体文案由调用方看 wasCanceled() 区分。
        //
        // **顺序不能反：远端标记必须先于口令文件清理。**
        //
        // cleanupRemoteMarkers() 是靠 ssh 连到接收端执行 `rm -f` 的，
        // 而它要的凭据（SSH_ASKPASS 脚本 + 口令文件）正是 cleanupSecrets()
        // 要删掉的那两个文件。原来这里是先 cleanupSecrets() 再
        // cleanupRemoteMarkers()，于是清理用的那个 ssh 进程**没有凭据**，
        // 连不上接收端，`rm` 根本没执行 —— 客户远端目录里就长期躺着
        // .srt-verify-<pid>-<时间戳>。
        //
        // 实测（Windows，进程内探针，取消同步 3/3）：按原顺序残留 >0；
        // 同一份代码注入一个能工作的 SSH_ASKPASS 后残留 = 0。
        // 对照成功路径残留同样是 0（成功时标记是核对阶段自己 rm 掉的），
        // 说明"清理代码本身没问题，缺的只是凭据"。
        //
        // 放在这里而不是 cancel() 里：cancel() 只该做"停下来"这一件事，
        // 而这里本来就是收尾分支，取消 / 失败 / 正常跑完三条路都汇到这儿。
        //
        // 次序本身由 finishCleanupSteps() 给出（见上），**不要**在这里
        // 直接写死两次调用的先后 —— 写死的话判据只能读到一份注释，
        // 改坏实现它照样全绿。
        runFinishCleanup();
        // 「仅同步新增文件」= --ignore-existing：远端已有同名文件时 rsync 静默跳过，
        // **一个字节都不提示**，退出码还是 0，界面照样显示"同步完成"。
        // 客户在远端手工改过的同名文件会就此和本机永久分叉，而他完全不知情。
        // 这里把被跳过的数量说出来 —— m_done 是 rsync 用 --out-format=%n 报回来的
        // 真实传输数，m_done < m_total 且开了这个选项，就说明差额全是被跳过的。
        if (m_onlyNew && !m_failed && !m_cancelRequested && m_total > m_done)
            emit logText(QString::fromUtf8(u8"\u52FE\u4E86\u300C\u4EC5\u540C\u6B65\u65B0\u589E\u6587\u4EF6\u300D\uFF1A\u672C\u6B21\u6709 %1 \u4E2A\u6587\u4EF6\u6CA1\u6709\u4F20\u8F93\uFF0C\u56E0\u4E3A\u8FDC\u7AEF\u5DF2\u7ECF\u5B58\u5728\u540C\u540D\u6587\u4EF6\uFF0C\u8FDC\u7AEF\u7684\u5185\u5BB9\u4FDD\u6301\u539F\u6837\u3001\u4E0D\u4F1A\u88AB\u8986\u76D6\u3002\u5982\u679C\u4F60\u60F3\u8986\u76D6\u8FDC\u7AEF\u540C\u540D\u6587\u4EF6\uFF0C\u53D6\u6D88\u8FD9\u4E2A\u52FE\u9009\u518D\u540C\u6B65\u4E00\u6B21\u3002")
                         .arg(m_total - m_done));
        emit allFinished(!m_failed && !m_cancelRequested);
        return;
    }

    const Group &g = m_groups.at(m_groupIndex);
    m_currentGroupFiles = g.files;
    // 新的一组开始：上一组没收尾的那个"在传文件名"必须清掉，
    // 否则它会被算进这一组的成果里（onProcFinished 里的结账会重复计一次）。
    m_startedName.clear();
    // 本组总字节：看门狗的校验阶段预算要按它放大（见 watchdogMs 注释）。
    // 只在这里算一次，每一行 stdout/stderr 都会重置看门狗，不能每次重算。
    m_currentGroupBytes = 0;
    for (const QString &f : m_currentGroupFiles) {
        const QFileInfo fi(QDir(m_localRoot).absoluteFilePath(f));
        if (fi.isFile())
            m_currentGroupBytes += fi.size();
    }
    // 同步建哈希索引：stdout 热路径每来一行都要查一次成员资格，
    // 拿 QStringList 线性查，一组几千个文件就是千万级字符串比较。
    m_currentGroupSet.clear();
    m_currentGroupSet.reserve(m_currentGroupFiles.size());
    for (const QString &f : m_currentGroupFiles)
        m_currentGroupSet.insert(f);

    // 文件列表写到本地根目录内、用相对路径名传进去。
    // 放本地根目录是因为：工作目录就是它，相对路径不含盘符，rsync 不会误判成远程。
    const QString listName = QStringLiteral(".srt-filelist-%1.txt")
                                 .arg(QCoreApplication::applicationPid());
    const QString listAbs = m_localRoot + QLatin1Char('/') + listName;
    QFile listFile(listAbs);
    if (!listFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        reportAttribution(QString(), LayerLocal);
        emit logText(QString::fromUtf8(u8"\u65E0\u6CD5\u5728\u672C\u5730\u6839\u76EE\u5F55\u5199\u5165\u4E34\u65F6\u6587\u4EF6\u5217\u8868\uFF1A%1").arg(listAbs));
        m_failed = true;
        ++m_groupIndex;
        launchNext();
        return;
    }
    QByteArray payload;
    for (const QString &rel : g.files) {
        // 以 '#' 开头的相对路径必须补 './' 前缀。
        //
        // rsync 的 --files-from 把 '#' 开头的条目当**注释**静默跳过，
        // 剩下的条目照常传、退出码照样是 0，界面也只按 --out-format=%n
        // 收行 —— 漏掉的文件既不进进度也不报错，用户看到的是
        // "同步完成"，实际少传了文件。实测（本机 rsync 3.5.1 客户端 +
        // 远端 3.1.3）：清单 ['#x.spe','ok.spe'] 传完 rc=0，
        // 远端只有 ok.spe，#x.spe 不存在；同一份清单加 './' 前缀后
        // 两个文件都到位（rsync 会把 './' 剥掉，远端仍是 #x.spe）。
        QByteArray entry = rel.toUtf8();
        if (entry.startsWith('#'))
            entry.prepend("./");
        payload += entry;
        payload += '\0';
    }
    // 必须核对实际写入的字节数：磁盘满 / 配额超限时 QFile::write() 会短写甚至
    // 写 0 字节并返回 -1，不检查的话 rsync 拿到的是一份**被截断的清单**，
    // 只传清单里有的那部分文件、退出码还是 0，
    // 界面照报「同步完成」，剩下的文件静默不传——这比报错糟糕得多。
    const qint64 written = listFile.write(payload);
    if (written != payload.size()) {
        reportAttribution(QString(), LayerLocal);
        emit logText(QString::fromUtf8(u8"\u4E34\u65F6\u6587\u4EF6\u5217\u8868\u53EA\u5199\u5165\u4E86 %1 / %2 \u5B57\u8282"
                                         u8"\uFF0C\u672C\u7EC4\u5DF2\u4FDD\u7559\u7684\u90E8\u5206\u6E05\u5355\u672A\u6267\u884C"
                                         u8"\u3002")
                         .arg(written).arg(payload.size()));
        listFile.close();
        m_failed = true;
        ++m_groupIndex;
        launchNext();
        return;
    }
    if (!listFile.flush()) {
        reportAttribution(QString(), LayerLocal);
        emit logText(QString::fromUtf8(u8"\u4E34\u65F6\u6587\u4EF6\u5217\u8868\u5199\u5165\u78C1\u76D8\u5931\u8D25"
                                         u8"\uFF0C\u672C\u7EC4\u672A\u6267\u884C\u3002"));
        m_failed = true;
        ++m_groupIndex;
        launchNext();
        return;
    }
    listFile.close();
    m_listPath = listAbs;

    // -e 的值里不能带引号：QProcess 不经过 shell，rsync 会把带引号的整串
    // 当成一个可执行文件名去 exec，直接 "No such file or directory"。
    // 程序目录已经注入到子进程 PATH，这里只写命令名，装在带空格的路径下也没问题。
    const QString sshCmd = QStringLiteral("ssh %1").arg(sshOption(m_target));

    QStringList args;
    args << QStringLiteral("-a")
         << QStringLiteral("--no-o")     // 目标机未必有权限改属主，避免整组失败
         << QStringLiteral("--no-g")
         << QStringLiteral("--partial")
         // 半截文件不能留在目标真名上。
         //
         // 裸 --partial 的行为是：中断时把已收到的那几 MB 直接写成目标路径下的
         // 真名文件，靠 mtime 停在 1970-01-01 提示"这是半个"。
         // 配合默认勾上的 --ignore-existing，这个半截文件从此每次同步都被
         // "远端已存在"跳过 —— 用户看到界面报"已完成"，实际远端残缺且永不自愈。
         //
         // --partial-dir 把半截文件改放到目标目录下的旁路目录 .srt-partial/，
         // 真名永远不会被创建。下一次同步（不管 --ignore-existing 勾没勾）
         // 都会当真名不存在，正常重传；半截文件还在则被续传复用，传完自动清理。
         //
         // 相对路径是相对**目标目录**解析的，正是要的行为。
         // 依赖：rsync ≥ 3.0；本工具两端的 rsync 都远高于这个版本。
         << QStringLiteral("--partial-dir=.srt-partial")
         // 「是否重复判定」这一层，按界面上的「仅同步新增文件」勾选框二选一。
         //
         // 勾上 = --ignore-existing：远端已有的路径一律不碰，只传新增文件。
         //        这也正好是大目录唯一跑得动的档位 —— 不用逐个校验远端文件。
         // 不勾 = --checksum：按内容校验和逐个比对，同大小同 mtime 但内容变了
         //        也会重传。代价是每次同步都要把远端已存在文件整个读一遍。
         //
         // 两者互斥，不能同时加也不能都不加：--ignore-existing 根本不看内容，
         // 同时给会让 --checksum 形同虚设；而都不给就退回 rsync 默认的
         // 「大小 + mtime」，本工具的文件粒度就是秒，踩中就是静默丢数据。
         //
         // 负向验证记录在各自那一支的注释里。
         << (m_onlyNew ? QStringLiteral("--ignore-existing") : QStringLiteral("--checksum"))
         << QStringLiteral("--protect-args")   // 远端路径不过远端 shell，避免 glob 写错目录
         // --from0：--files-from 的分隔符从换行改成 NUL。
         // Windows 的 NTFS 只禁掉 \ / : * ? " < > | 和 NUL，换行和回车都是合法文件名字符。
         // 用换行分隔时，一个带换行的文件名会被拆成两条，
         // rsync 对这两条都 link_stat 失败，整组退出码 23，这些文件永远传不过去，
         // 界面上却只显示"部分文件传输失败"。
         << QStringLiteral("--from0")
         << QStringLiteral("--files-from=") + listName
         << QStringLiteral("--out-format=%n")
         << QStringLiteral("--info=progress2")
         << QStringLiteral("-z")
         << QStringLiteral("-e") << sshCmd
         << QStringLiteral("./") << g.remoteBase;

    emit logText(QString::fromUtf8(u8"\u2192 %1  (%2 \u4E2A\u6587\u4EF6)")
                     .arg(g.remoteBase).arg(g.files.size()));

    m_proc = new QProcess(this);

    // D11：作业对象 + CREATE_SUSPENDED。rsync 入作业之后 fork 出来的 ssh
    // 自动继承作业成员身份，kill() 时整棵树一起回收。
    // 拿不到作业对象就完全不挂起 —— 挂起后放不回来的同步会永远卡住，
    // 那比留一个孤儿严重得多。这种情况下行为与修复前完全一致。
#ifdef Q_OS_WIN
    attachToJob(m_proc);
#endif

    // 无条件把程序目录塞进子进程 PATH：rsync 靠它找到旁边的 rsync.exe / ssh.exe
    applyAskpassEnv(m_proc, m_askpassPath);

    m_proc->setWorkingDirectory(m_localRoot);

    connect(m_proc, &QProcess::readyReadStandardOutput, this, &RsyncRunner::onStdout);
    connect(m_proc, &QProcess::readyReadStandardError,  this, &RsyncRunner::onStderr);
    connect(m_proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &RsyncRunner::onFinished);

    // 看门狗：ssh 在等私钥口令、或者链路半死不活时都不会有任何输出，
    // 没有这道保护界面就会一直卡在"同步中"。
    // 预算按本组文件数放大 —— --checksum 的校验阶段同样不吐输出，
    // 但那是正常干活，见 watchdogMs() 里的实测数据。
    m_watchdog.start(watchdogMs(m_currentGroupFiles.size(), m_currentGroupBytes));

    m_buffer.clear();
    m_stderrTail.clear();
    m_proc->start(QStringLiteral("rsync"), args);

    // rsync 启动实测 30~80ms，3 秒还起不来就是环境问题（缺 DLL、被杀软拦）
    if (!m_proc->waitForStarted(3000)) {
        reportAttribution(QString(), LayerLocal,
                           QString::fromUtf8(kBlockedAdvice));
        emit logText(QString::fromUtf8(u8"\u542F\u52A8 rsync \u5931\u8D25\uFF1A%1").arg(m_proc->errorString()));
        m_failed = true;
        closeJob();
        delete m_proc;
        m_proc = nullptr;
        cleanupListFiles();
        ++m_groupIndex;
        launchNext();
        return;
    }

    // 文件列表已经写进 --files-from，stdin 必须直接关掉，否则 rsync 会一直等
    m_proc->closeWriteChannel();
}

void RsyncRunner::drainBuffer()
{
    // rsync 用 '\r' 刷新百分比行，用 '\n' 输出每个完成的条目
    while (true) {
        int idx = -1;
        for (int i = 0; i < m_buffer.size(); ++i) {
            const QChar c = m_buffer.at(i);
            if (c == QLatin1Char('\r') || c == QLatin1Char('\n')) {
                idx = i;
                break;
            }
        }
        if (idx < 0)
            break;

        const QString line = m_buffer.left(idx);
        m_buffer.remove(0, idx + 1);
        handleLine(line);
    }
}

void RsyncRunner::onStdout()
{
    m_watchdog.start(watchdogMs(m_currentGroupFiles.size(), m_currentGroupBytes));
    m_buffer += QString::fromUtf8(m_proc->readAllStandardOutput());
    drainBuffer();
}

void RsyncRunner::handleLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    if (trimmed.isEmpty())
        return;

    // 进度帧必须**先于**文件名白名单判据处理，顺序反了会前功尽弃。
    //
    // 实测（800MB 走真实 ssh，6 秒）：进度条从头到尾停在 0%，
    // 用户完全看不出后台在处理数据 —— 而代码里 emit byteProgress 明明写着。
    //
    // 原因是判据顺序：白名单检查在上面先 return 了。进度帧整行是
    //   "157,286,400 100%  470.12MB/s  0:00:00 (xfr#1, to-chk=0/2)"
    // 它**永远不可能**是文件名，于是每一帧都被挡在门外，
    // 下面的进度判断成了一段永远执行不到的代码。
    // 文件计数照样是对的（只有真文件名能通过白名单），
    // 所以症状特别隐蔽：日志和计数都对，只有进度条是死的。
    //
    // 进度帧的结构是固定的：行首空白 + 数字 + 空白 + 百分比 + 空白 + 速度，
    // 真文件名长成这样的概率可以忽略。
    //
    // **单位必须同时认大小写**，这是实测踩到的：
    // rsync 3.x 的 human-readable 输出里，**千位是小写 k、其余是大写**：
    //     ^M          3,000 100%   43.07kB/s    0:00:00 (xfr#3000, to-chk=0/3000)
    //     ^M    157,286,400  9%  470.12MB/s    0:00:06 (xfr#1, to-chk=0/2)
    // 原来的字符类是 [KMGTP]?，没有小写 k，于是**速度只要低于 1MB/s，
    // 整行就匹配不上**，每一帧被静默丢掉 —— 进度条全程停在 0%，
    // 而文件计数和日志全都正确（只有真文件名能过白名单），症状极其隐蔽。
    //
    // 实测：3000 个 1 字节文件传 21.5 秒，stdout 上有 3000 个百分比帧，
    // 用产品这条正则匹配到 **0** 个。
    //
    // 中招的恰好是最该有反馈的两种场景：慢 WAN、海量小文件。
    //
    // 下面的换算用 toUpper() 归一，所以这里大小写都收，不用为每种写法各写一条分支。
    const ProgressFrame frame = parseProgressFrame(trimmed);
    if (frame.valid) {
        emit byteProgress(frame.percent);
        emit byteStats(frame.bytes, frame.speedBps);
        return;
    }

    // --out-format=%n 会把 rsync 进入/创建的目录也打出来（子目录 "xxx/"，根目录 "."）。
    if (trimmed.endsWith(QLatin1Char('/')))
        return;

    QString name = trimmed;
    if (name.startsWith(QLatin1String("./")))
        name.remove(0, 2);
    if (name.isEmpty() || name == QLatin1String("."))
        return;

    // 只有"确实在这一组清单里"的行才算数。
    //
    // 这一条取代了原来两套字符串猜测（isRsyncNoise 前缀过滤 + 进度帧正则），
    // 因为真实文件名可以长成 rsync 自己的输出：
    //   "50% xfr#1 to-chk=0.spe"  会被进度帧正则吃掉，文件传了却不计
    //   "sent 40 bytes ..."      会被前缀过滤当成噪声，文件同样漏计
    // 这两种情况下文件其实都在远端，界面只显示"已同步 1 / 2 个文件"。
    // 按清单认，猜不出来就不计——宁可少报也不能虚报成功。
    if (!m_currentGroupSet.contains(name))
        return;
    // 同一行重复到达（管道缓冲重放、\r 覆写残留）时不重复计数。
    if (name == m_startedName)
        return;

    // **上一行报出来的那个文件，到这一刻才算真的传完。**
    //
    // --out-format=%n 是"开始传输"时打印的（2026-10-18 实测，见头文件
    // m_startedName 的注释）。原来在这里直接 ++m_done + 发 fileTransferred，
    // 等于把"开始"当成"结束"：
    //   - 每个文件刚**开始**传就被标成「已完成」，下一行立刻点亮；
    //   - 中途取消时那个 94.6% 的截断文件被标成「已完成」，计数显示 1/1，
    //     接收端却只有半个文件，界面是绿的。
    // rsync 串行传输（没有 --parallel），下一个开始 == 上一个结束。
    if (!m_startedName.isEmpty()) {
        ++m_done;
        emit fileTransferred(m_startedName, m_done, m_total);
    }
    m_startedName = name;
    emit fileStarted(m_startedName);
}

void RsyncRunner::onStderr()
{
    m_watchdog.start(watchdogMs(m_currentGroupFiles.size(), m_currentGroupBytes));
    const QString err = QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
    if (err.isEmpty())
        return;
    m_stderrTail += err + QLatin1Char('\n');
    // 只留尾部：远端目录不可写时 rsync 会每个文件报一行，几千行会把内存吃光，
    // 而排查真正需要的是最后那几行（最终错误和退出码说明）。
    if (m_stderrTail.size() > 4000)
        m_stderrTail = m_stderrTail.right(4000);
    emit logText(QStringLiteral("[rsync] ") + err);
}

void RsyncRunner::takeStderrTail()
{
    if (!m_proc || m_proc->state() == QProcess::NotRunning)
        return;
    const QString err = QString::fromUtf8(m_proc->readAllStandardError()).trimmed();
    if (err.isEmpty())
        return;
    m_stderrTail += err + QLatin1Char('\n');
    if (m_stderrTail.size() > 4000)
        m_stderrTail = m_stderrTail.right(4000);
}

void RsyncRunner::onProcError(QProcess::ProcessError err)
{
    if (err != QProcess::FailedToStart || !m_proc)
        return;
    // FailedToStart 不会发 finished()，只能在这里自己收尾并推进下一组。
    emit logText(QString::fromUtf8(u8"\u542F\u52A8\u8FDC\u7AEF\u547D\u4EE4\u5931\u8D25\uFF1A%1")
                     .arg(m_proc->errorString()));
    // 子进程压根没起来：只可能是本机这边的事（程序被移走/被杀软拦/缺 DLL），
    // 归因固定落本机层，并提示去点掉挡住它的弹窗。
    reportAttribution(m_proc->errorString(), LayerLocal,
                       QString::fromUtf8(kBlockedAdvice));
    m_watchdog.stop();
    m_stderrTail.clear();
    m_buffer.clear();
    // 核对阶段的 ssh 起不来：这一组 rsync 本身是成功的，只是兜底核对没做成。
    // 核对失败要放行，不能把一次正常的同步判成失败。
    const bool wasVerify = m_inVerify;
    m_verifyBuffer.clear();
    m_verifyDir.clear();
    m_inVerify = false;
    if (!wasVerify)
        m_failed = true;
    // 不能直接 delete：现在正处在 m_proc 自己发出来的信号里，
    // 删掉后信号还要沿着 QProcess 内部继续往上抛。
    m_proc->deleteLater();
    m_proc = nullptr;
    if (wasVerify)
        ++m_groupIndex;
    // 清单文件也要清。FailedToStart 说明 rsync 连启动都没启动，
    // 不会走 onFinished，而那里有 cleanupListFiles()。
    // 不清的话 .srt-filelist-<pid>.txt 就永久留在用户的本地根目录里
    // （PID 不复用，它自己永远不会消失），下次扫描还会被当成用户数据。
    cleanupListFiles();
    // 不清 askpass：后续组还要用同一个口令文件，
    // 这里删了等于让后面的组全部密码认证失败。收尾交给析构函数。
    launchNext();
}

void RsyncRunner::onFinished(int exitCode, QProcess::ExitStatus status)
{
    m_buffer += QString::fromUtf8(m_proc->readAllStandardOutput());
    drainBuffer();
    // rsync 结束时最后一行不一定带换行符，把它当文件名收尾
    if (!m_buffer.trimmed().isEmpty())
        handleLine(m_buffer);
    m_buffer.clear();

    // 同 onDirsFinished：退出瞬间 stderr 里还剩一段没投递，
    // 不取出来的话"远端目录被删/磁盘写满"这类失败在界面上只有一句
    // "部分文件未完成传输"，看不到 rsync 到底报的什么。
    takeStderrTail();

    if (m_cancelRequested) {
        // 用户主动取消时进程必然是非零退出，但那不是故障。
        // 以前这里照样置 m_failed，界面弹红框"同步异常，部分文件未完成传输"，
        // 用户明明是自己点的取消却被告知出错。
        emit logText(QString::fromUtf8(u8"\u5DF2\u53D6\u6D88\uFF0C\u672C\u6B21\u540C\u6B65\u7ED3\u675F\u3002"));
    } else if (status == QProcess::CrashExit || exitCode != 0) {
        m_failed = true;
        emit logText(QString::fromUtf8(u8"\u672C\u7EC4\u5931\u8D25\uFF0C\u9000\u51FA\u7801 %1\uFF08\u5DF2\u7EE7\u7EED\u4E0B\u4E00\u7EC4\uFF09\u3002").arg(exitCode));
        if (!m_stderrTail.trimmed().isEmpty())
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u62A5\u9519\uFF1A") + m_stderrTail.trimmed());
        // rsync 的报错是**英文原文**，而界面其余部分全是中文。
        // 客户现场最常见的一种失败就是远端目录没有写权限
        // （目录属主是别人、权限被改成 r-x、或者 SELinux 挡了），
        // rsync 只会说 "Permission denied (13)"。只把这句英文原样转发的话，
        // 用户看到的是一句他不一定认得的英文 + 一句泛泛的"同步失败"，
        // 而真正该做的动作（找管理员开写权限）一个字都没提到。
        // 所以命中这个形态时补一句中文，把"是谁的问题、该找谁"说清楚。
        // 判断只看 stderr 里有没有 "Permission denied" / "mkstemp ... failed: Permission denied"
        // 这一族；没有就不加，不做无依据的猜测。
        //
        // 但必须先排掉认证形态。原来只看有没有 "Permission denied" 这一个词，
        // 于是 "Permission denied (publickey,password,keyboard-interactive)"
        // —— 也就是密码错、密钥不被接受——也会命中这里，再冒一句
        // "请联系管理员开通写权限"。管理员查完发现权限根本没问题，
        // 而下面紧跟着的【归因】还写着"认证层"，两句话当场互相打脸。
        // 现在改成：归因判到接收端时才说"远端不可写"，两层保持一致。
        if (classifyLayer(m_stderrTail) == LayerReceiver
            && m_stderrTail.contains(QLatin1String("Permission denied"), Qt::CaseInsensitive))
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u8FD4\u56DE Permission denied\u3002"));
        // 四层归因：把上面那整段 stderr 判到「本机 / 网络 / 认证 / 接收端」之一，
        // 并给这一层该做的事。放在既有日志之后，是为了让"发生了什么 → 原文是什么
        // → 该找谁"按顺序读下来，而不是先丢一句结论让人猜。
        reportAttribution(m_stderrTail);
        reportIncompleteRemoteFiles();
    } else {
        // 传输过程中源文件被改过：rsync 传的是它**打开那一刻**的快照，
        // 用户在传输中途改了文件，远端拿到的就是改动前的版本，
        // 而界面照报"本组完成"—— 实测过：50MB 的源文件传到一半被截成 8MB，
        // 远端留下完整的 50MB 旧内容，全程零警告。
        //
        // 这不是 rsync 能管的事（它没有理由知道你在改源文件），
        // 但本工具可以在**本地**比对：传输开始前记下每个文件的
        // 大小 + 修改时间，全部传完后再 stat 一遍。
        // 不一致就明确告诉用户"远端是改动前的快照"，并把本组判为失败。
        //
        // 只在退出码 0 时查：非零退出本来就进失败分支，
        // 再叠一条"源文件被改过"只会把真正的原因淹掉。
        const QStringList mutated = filesMutatedDuringTransfer();
        if (!mutated.isEmpty()) {
            m_failed = true;
            emit logText(QString::fromUtf8(u8"\u3010\u6CE8\u610F\u3011\u4EE5\u4E0B\u6587\u4EF6\u5728\u4F20\u8F93\u8FC7\u7A0B\u4E2D\u88AB\u4FEE\u6539\uFF1A%1")
                             .arg(mutated.join(QString::fromUtf8(u8"\u3001"))));
            emit logText(QString::fromUtf8(u8"\u8FDC\u7AEF\u6536\u5230\u7684\u662F\u4FEE\u6539\u524D\u7684\u5185\u5BB9\uFF08\u672C\u6B21\u4F20\u8F93\u53D7\u5F71\u54CD\uFF09\u3002"));
        }
        // 增量同步时 rsync 不会输出任何文件名（文件本来就一致），靠退出码判定成功
        //
        // 但结账发生在**下面**（rsyncOk 时才 ++m_done），所以本组只有 1 个文件时
        // 走到这里 m_done 必然还是 0 —— 照字面判会把「真传了一个文件」报成
        // 「本组无变化，全部已是最新」。m_startedName 非空表示 rsync 的 %n 已经
        // 打过（确实开传了），把这个即将结账的文件算进去。
        const bool lastFilePending = (m_total == 1) && !m_startedName.isEmpty();
        if (m_done == 0 && !lastFilePending)
            emit logText(QString::fromUtf8(u8"\u672C\u7EC4\u65E0\u53D8\u5316\uFF0C\u5168\u90E8\u5DF2\u662F\u6700\u65B0\u3002"));
        else
            emit logText(QString::fromUtf8(u8"\u672C\u7EC4\u5B8C\u6210\u3002"));
        // rsync 退出码 0 但 stderr 有内容的情况是存在的（接收端报了可恢复的错误、
        // 或者远端 shell 打了一行告警）。不能因为退出码是 0 就当没事发生过，
        // 至少要让用户看到，否则"同步完成"就是在骗人。
        if (!m_stderrTail.trimmed().isEmpty())
            emit logText(QString::fromUtf8(u8"[rsync] ") + m_stderrTail.trimmed());
    }
    // 失败原因要在 clear() **之前**留下一份。
    //
    // 为什么：m_stderrTail 每组 rsync 结束就被清空，而 SyncPanel::onAllFinished
    // 要等**所有组**跑完才弹框。多组同步时这里清完，最后一次失败的原因到
    // 弹框时早就没了 —— 分类函数拿到空串，只能退回兜底文案，等于白改。
    //
    // 只在真的失败时记：成功组里 rsync 也可能打无害的告警行（退出码 0 但
    // stderr 有内容），那些不是失败原因，不该出现在弹框里。
    const bool groupOk = !m_cancelRequested && !m_failed
                         && status != QProcess::CrashExit && exitCode == 0;
    if (!groupOk && !m_stderrTail.trimmed().isEmpty())
        m_lastFailure = m_stderrTail.trimmed();
    m_stderrTail.clear();

    m_watchdog.stop();
    // 同 onDirsFinished / onVerifyFinished：这里是 rsync 自己的 finished() 槽，
    // 直接 delete 会让信号在已释放的 QProcess 上继续往上抛。
    // 三处曾经各写各的，只有 onProcError 那处记得用 deleteLater()。
    m_proc->deleteLater();
    m_proc = nullptr;
    cleanupListFiles();

    const bool rsyncOk = groupOk;

    // 整组收尾：把最后一个"报了名字但还没被下一个文件顶掉"的文件结账。
    //
    // 只有 rsync 报成功时才结账 —— 它退出码为 0 就意味着清单里所有该传的
    // 文件都传完了。失败或被取消时**不能**结账：那一个多半只传了一半
    // （实测取消后接收端是 94.6% 的截断文件），让它留在 m_pendingRows 里，
    // 由界面按 wasCanceled() 标成「已跳过」，而不是谎报「已完成」。
    if (rsyncOk && !m_startedName.isEmpty()) {
        ++m_done;
        emit fileTransferred(m_startedName, m_done, m_total);
    }
    m_startedName.clear();

    // 只有 rsync 自己报成功时才核对目标目录：已经失败或被取消的组不用查，
    // 查了只会多出一条噪音日志。
    if (rsyncOk && m_groupIndex < m_groups.size()) {
        verifyRemoteDir(m_groups.at(m_groupIndex).remoteDir);
        return;
    }

    ++m_groupIndex;
    launchNext();
}

void RsyncRunner::cancel()
{
    if (!isRunning())
        return;

    m_cancelRequested = true;
    emit logText(QString::fromUtf8(u8"\u6B63\u5728\u53D6\u6D88\u2026"));
    // 必须用 kill()，不能用 terminate()。
    //
    // 这里原来写的是 terminate()，注释还断言"Windows 上 Qt 的 terminate()
    // 就是 TerminateProcess"——那条断言是错的，本机 Qt 5.9.6 + msys2015_64 上
    // 实测 terminate() 对随包的 MSYS2 rsync.exe 完全无效：调完之后进程状态
    // 仍是 Running，--info=progress2 的进度帧一路从 0% 涨到 56%，
    // 整个文件照传完，最后才由 finished() 收尾。
    //
    // 实测数据（产品真实 argv，400MB 单文件，1.5 秒时调 terminate）：
    //   T=1511ms  calling terminate() state=Running
    //   T=12161ms state=Running  ← 10 秒后仍在跑
    //   远端落盘 248470992 字节（截断在途中）
    //   换成 kill() 后立刻停住。
    //
    // 界面表现是：用户点"取消"，按钮 24 秒后才变成"已取消"，
    // 而这 24 秒里 400MB 全部传完了——用户以为停下来了，数据其实一直在落盘。
    // ToolRunner 里同一个坑被"terminate() + 1.5 秒后 kill() 兜底"掩盖了，
    // 所以只有同步这条路径暴露出来。
    //
    // 这里不 waitForFinished：kill() 后要等 rsync 自己收尾（发 finished、
    // 走 onFinished），阻塞式等待会让界面卡住。收尾交给 onFinished 槽异步做。
    // 强杀不会给 rsync 清理的机会，远端会留下截断的 .partial；
    // --partial 下次能续，但第一次要整份重传。
    // 关作业 = 连同 fork 出来的 ssh 一起回收。这才是 D11 的正解：
    // 只 kill() 直接子进程的话，ssh 会变成孤儿继续持有管道不放，
    // finished 永不发出，界面永远卡在"同步中"。
    m_proc->kill();
    closeJob();
}

void RsyncRunner::onWatchdogTimeout()
{
    if (!m_proc)
        return;
    // 旧文案只说"私钥带口令、或网络卡死"，把用户往错方向引。
    // 实际最常见的触发条件是**本组文件太多**：--checksum 的校验阶段
    // rsync 一个字节都不吐，看门狗到点把活着的进程杀了。
    // 这里把本组文件数一并报出来，用户一眼能判断是不是撞上了这个。
    emit logText(QString::fromUtf8(
        u8"\u957F\u65F6\u95F4\u6CA1\u6709\u4EFB\u4F55\u8F93\u51FA\uFF0C\u5DF2\u7EC8\u6B62\u672C\u7EC4\uFF08\u672C\u7EC4 %1 \u4E2A\u6587\u4EF6\uFF09\u3002")

        .arg(m_currentGroupFiles.size()));
    m_failed = true;
    // 上面那句是既有的归因猜测，这一轮补的是"到底该去做什么"。
    // 卡住时进程一个字都没吐，没有原文可判，但排除法已经排掉了大部分可能：
    // 剩下的要么是本机弹了窗挡住子进程（客户现场的麒麟安全中心就是这一类），
    // 要么是链路半断开。这两个方向要分别去查，所以建议里把两边都点一遍。
    reportAttribution(QString(), LayerLocal,
                       QString::fromUtf8(kStallAdvice));
    // 只等 1 秒：kill 之后进程会立刻结束，而 waitForFinished 不跑事件循环，
    // finished 信号稍后由事件循环发出，真正的收尾在 onFinished 槽里做。
    m_proc->kill();
    m_proc->waitForFinished(1000);
    // D11：网络半死不活时 rsync 杀了、fork 出来的 ssh 还活着并攥着管道不放，
    // 于是什么信号都没有，界面永久停在"同步中"。关作业把整棵树一起回收。
    // 注释里那句"kill 之后进程会立刻结束"只对直接子进程成立，对孙进程不成立。
    closeJob();
}

void RsyncRunner::purgeSecrets(const QString &askpassPath, const QString &passFilePath)
{
    if (!askpassPath.isEmpty())
        QFile::remove(askpassPath);
    if (!passFilePath.isEmpty())
        QFile::remove(passFilePath);

    // 光删上面这两个路径不够。m_askpassPath / m_passFilePath 是**成员**，
    // 同一个 RsyncRunner 连续同步两次时，第二次 createAskpass 会直接
    // 覆盖成员指向，上一对就再也没有任何人知道它在哪了。
    //
    // 这里再按"本进程 pid"扫一遍两个目录，只删带自己 pid 的：
    //   - 别的实例（另一个 pid）正在用的文件不会被动到；
    //   - 本进程早先漏掉的文件全部补清；
    //   - 已经死掉的进程留下的，由 cleanupStaleSecrets() 在下次启动清。
    // 命名契约和 cleanupStaleSecrets() 完全一致（第 3 段是 pid）。
    QStringList dirs;
    dirs << QDir::tempPath();
    const QString scriptDir = askpassDir();
    if (!dirs.contains(scriptDir))
        dirs << scriptDir;

    const int self = QCoreApplication::applicationPid();
    for (const QString &dir : dirs)
        purgeSelfPidSecretFiles(dir, self);
}

int RsyncRunner::purgeSelfPidSecretFiles(const QString &dir, int selfPid)
{
    if (dir.isEmpty())
        return 0;
    // 末尾的 '*' 不能少。QDir::entryList(nameFilters) 走的是通配符匹配，
    // 不带 '*' 只能匹配名字**恰好等于该串**的文件；而命名契约是
    // srt_pass_<pid>_XXXXXX（见 createAskpass 里的 QTemporaryFile 模板），
    // 于是 found 恒为空、循环体一次都不执行，而且**全程不报任何错**。
    // 这一条和同一文件里 cleanupStaleSecrets() 的 srt_pass_* 只差两个星号，
    // 语义却完全相反，所以判据必须单独盯住它。
    const QStringList found = QDir(dir).entryList(QStringList() << QStringLiteral("srt_pass_*")
                                                                  << QStringLiteral("srt_askpass_*"),
                                             QDir::Files | QDir::NoDotAndDotDot);
    int removed = 0;
    const QString self = QString::number(selfPid);
    for (const QString &name : found) {
        const QStringList parts = name.split(QLatin1Char('_'));
        if (parts.size() < 3)
            continue;
        QString pidPart = parts.at(2);
        const int dot = pidPart.indexOf(QLatin1Char('.'));
        if (dot > 0)
            pidPart.truncate(dot);
        if (pidPart != self)
            continue;
        if (QFile::remove(dir + QLatin1Char('/') + name))
            ++removed;
    }
    return removed;
}

void RsyncRunner::cleanupSecrets()
{
    purgeSecrets(m_askpassPath, m_passFilePath);
    m_askpassPath.clear();
    m_passFilePath.clear();
}

void RsyncRunner::cleanupRemoteMarkers()
{
    // 正常路径里每组传完都会顺手删掉自己的标记；这里兜的是中途取消、
    // 进程被杀这类收不回来的情况，别把 .srt-verify-* 留在客户的远端目录里。
    if (m_verifyMarker.isEmpty() || m_groups.isEmpty())
        return;
    const QString sshBin = locateTool(QStringLiteral("ssh"));
    if (sshBin.isEmpty())
        return;

    QStringList quoted;
    for (const Group &g : m_groups) {
        QString d = g.remoteDir.isEmpty() ? QStringLiteral(".") : g.remoteDir;
        d.replace(QLatin1Char('\''), QLatin1String("'\\''"));
        quoted << (QLatin1Char('\'') + d + QLatin1Char('/') + m_verifyMarker + QLatin1Char('\''));
    }
    if (quoted.isEmpty())
        return;

    QProcess p;
    applyAskpassEnv(&p, m_askpassPath);
    QStringList args = sshArgs(m_target, false);
    args << (m_target.user + QLatin1Char('@') + m_target.host)
         << (QStringLiteral("rm -f -- ") + quoted.join(QLatin1Char(' ')));
    p.start(sshBin, args);
    p.waitForFinished(2000);
    // 栈上的 QProcess 一析构就彻底没了：连不上的目标（端口不通、网络黑洞）
    // 会让 ssh 一直挂着，不先 kill 掉就是 "QProcess: Destroyed while process
    // is still running"，而且这条警告会把测试输出冲得七零八落。
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished(1000);
    }
    m_verifyMarker.clear();
}

// 一组传输失败后，点名告诉用户远端留下了什么。
//
// 实测场景（远端挂 48MB tmpfs，传 96MB 文件）：rsync rc=11，远端留下
// 50327552 字节的 big.spe（原 100663296），**没有任何标记**——
// 文件名普通、大小"看着像那么回事"、mtime 停在 epoch（rsync 是先建文件
// 最后才设时间，传输中断就停在 1970）。从远端目录本身**完全看不出**
// 这个文件是写了一半的，下游照单全收就是静默消费损坏数据。
//
// 这里不去 stat 远端文件（那要新起一次 ssh，而且解析回来的文件名
// 还得处理 NTFS 允许的换行 —— NTFS 只禁 \ / : * ? " < > | 和 NUL，
// 换行是合法文件名字符，用空格/制表符分隔的输出会被文件名里的换行撕开）。
// rsync 自己的 stderr 已经把失败的文件名带出来了，直接取：
//   "write failed on \"/path/big.spe\": No space left on device (28)"
//   "rsync: link_stat \"/path/x.spe\" failed: ..."
//   "rsync: [sender] read errors mapping \"/path/y.spe\": ..."
// 三种形态的共同点是 `on "<path>"`，一条正则全覆盖。
void RsyncRunner::reportIncompleteRemoteFiles()
{
    if (m_stderrTail.trimmed().isEmpty())
        return;

    // rsync 的报错形态不止一种，只匹配 `on "<path>"` 会漏掉大半：
//   write failed on   "/p/big.spe": No space left on device (28)
//   [sender] read errors mapping "/p/x.spe": ...
//   link_stat "/p/y.spe" failed: ...
// 前一种有 `on "`，后两种没有。所以改成抓**任何被引号包住的绝对路径**，
// 再拿文件名去和本组清单对一遍，对得上的才报 —— 既覆盖多种报错形态，
// 又不会把命令里的临时文件路径、远端目录这类噪音当成"未完成的数据文件"。
static const QRegularExpression re(QStringLiteral("\"(/[^\"]+)\""));
    QStringList names;
    const QStringList lines = m_stderrTail.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        QRegularExpressionMatchIterator it = re.globalMatch(line);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString n = m.captured(1);
            const int slash = n.lastIndexOf(QLatin1Char('/'));
            if (slash < 0)
                continue;
            const QString base = n.mid(slash + 1);
            if (!base.isEmpty() && !names.contains(base))
                names.append(base);
        }
    }
    if (names.isEmpty())
        return;

    // 和本组清单对一遍：报出来的必须确实是这次要传的数据文件
    QStringList mine;
    for (const QString &f : m_currentGroupFiles) {
        const int slash = f.lastIndexOf(QLatin1Char('/'));
        const QString base = slash >= 0 ? f.mid(slash + 1) : f;
        if (names.contains(base) && !mine.contains(base))
            mine.append(base);
    }
    if (mine.isEmpty()) {
        // 兜底：清单一个都对不上时，宁可多报也不要漏报。
        //
        // 但"多报"不能报到目录名上。上面那条正则抓的是**任何**被引号包住的
        // 绝对路径，里面既有数据文件，也有远端目录本身（rsync 报
        // `mkdir failed on "/remote/sub"` 之类），原样报出去就成了
        // "下列文件未能完整传输：sub" —— 客户拿着一串目录名去找文件。
        //
        // 判据：本组清单里每个文件的**父目录名**集合。抓到的 basename 命中
        // 这个集合，说明它是目录，跳过；剩下的才当数据文件报。
        QSet<QString> dirNames;
        for (const QString &f : m_currentGroupFiles) {
            const int slash = f.lastIndexOf(QLatin1Char('/'));
            if (slash <= 0)
                continue;
            dirNames.insert(f.left(slash).section(QLatin1Char('/'), -1));
        }
        for (const QString &n : names) {
            if (dirNames.contains(n))
                continue;
            mine.append(n);       // names 抓取时已去重，这里不用再查 contains
        }
        // 过滤后一个不剩：说明这批路径全是目录名。宁可什么都不说，
        // 也不能把目录报成文件（漏报下一次同步会重来，报错要人去翻日志）。
        if (mine.isEmpty())
            return;
    }

    emit logText(QString::fromUtf8(u8"\u3010\u6CE8\u610F\u3011\u4E0B\u5217\u6587\u4EF6\u672A\u80FD\u5B8C\u6574\u4F20\u8F93\uFF1A%1")
                     .arg(mine.join(QString::fromUtf8(u8"\u3001"))));
    emit logText(QString::fromUtf8(u8"\u672C\u6B21\u4F20\u8F93\u672A\u5B8C\u6210\u7684\u6587\u4EF6\uFF0C\u5176\u5DF2\u4F20\u8F93\u90E8\u5206\u7559\u5728\u8FDC\u7AEF\u7684 .srt-partial/ \u76EE\u5F55\u4E0B\uFF0C\u6CA1\u6709\u4EE5\u539F\u6587\u4EF6\u540D\u843D\u76D8\u3002"));
    emit logText(QString::fromUtf8(u8"\u4E0B\u6B21\u540C\u6B65\u4F1A\u56E0\u5927\u5C0F\u4E0D\u4E00\u81F4\u81EA\u52A8\u91CD\u4F20\uFF0C"
                                 u8"\u4E0D\u9700\u8981\u624B\u52A8\u5220\u9664\u3002"));
}

void RsyncRunner::cleanupListFiles()
{
    if (!m_listPath.isEmpty()) {
        QFile::remove(m_listPath);
        m_listPath.clear();
    }
}

void RsyncRunner::onJobAttach()
{
#ifdef Q_OS_WIN
    if (!m_proc || !m_job)
        return;

    const qint64 pid = m_proc->processId();
    if (pid > 0) {
        // PROCESS_SET_QUOTA 是 AssignProcessToJobObject 要求的，别省。
        const HANDLE ph = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | SYNCHRONIZE,
                                      FALSE, static_cast<DWORD>(pid));
        if (ph) {
            AssignProcessToJobObject(static_cast<HANDLE>(m_job), ph);
            CloseHandle(ph);
        }
    }

    // 放行是无条件的，优先级高于一切：挂起状态放不回来的同步会永远卡死，
    // 用户看到的是"点了没反应"，那比留一个孤儿严重得多。
    // 挂不进作业也只是退回修复前的行为，不会更糟。
    if (!resumePrimaryThread(static_cast<DWORD>(pid))) {
        emit logText(QString::fromUtf8(u8"\u542F\u52A8\u540E\u65E0\u6CD5\u6062\u590D\u6267\u884C\uff0C\u5DF2\u7EC8\u6B62\u3002"));
        m_failed = true;
        m_proc->kill();
        closeJob();
    }
#else
    Q_UNUSED(this)
#endif
}

void RsyncRunner::closeJob()
{
#ifdef Q_OS_WIN
    if (!m_job)
        return;
    CloseHandle(static_cast<HANDLE>(m_job));
    m_job = nullptr;
#endif
}
