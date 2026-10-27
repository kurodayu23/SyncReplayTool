#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QDateTime>
#include <QTime>
#include "Model.h"
#include "AutoSyncScheduler.h"

// 配置落盘到 ini，路径由 QSettings 决定：%APPDATA%/AP/SyncReplayTool.ini（win）
class ConfigStore : public QObject
{
    Q_OBJECT

public:
    static TargetConfig            loadTarget();
    static void                    saveTarget(const TargetConfig &target);
    static QVector<SyncRule>       loadSyncRules();
    static void                    saveSyncRules(const QVector<SyncRule> &rules);
    static QVector<ToolRule>       loadToolRules();
    static void                    saveToolRules(const QVector<ToolRule> &rules);

    // 配置文件实际路径，给"配置被改坏了"这类排查用
    static QString                 iniPath();

    // 「仅同步新增文件」勾选框的持久化。
    //
    // 缺陷（2026-10-18 实测，银河麒麟 V11 真实点击四轮对照）：
    // 这个勾选框**每次启动都被硬编码成勾上**（SyncPanel 构造里的 setChecked(true)），
    // 根本没有读写配置的地方。用户为了覆盖远端同名文件取消勾选、同步一次（确实生效），
    // 关掉程序再打开又变回勾上，于是「改了文件不重传」这件事会**反复出现**，
    // 而界面上没有任何地方说明它为什么不生效 —— 用户只会认为软件坏了。
    //
    // 老配置里没有这个键时按 false 处理（默认不勾 → --checksum 按内容比对），
    // 与需求 2「新增**或修改**」和手册步骤 5「不勾（默认）」一致。
    static bool                    loadOnlyNewFiles();
    static void                    saveOnlyNewFiles(bool onlyNew);

    // 「记住设置」勾选框的持久化（默认 true，允许用户选择下次是否沿用/记住设置）
    static bool                    loadRememberSettings();
    static void                    saveRememberSettings(bool remember);
    // 老配置里的 app/rememberSettings=false 迁移成 true。
    // 界面上那个勾选框已按客户意见移除 ⇒ 这个键只能恒为 true，
    // 不迁移就会在 ini 里躺着一个"程序里没有入口的开关"。
    static void                    migrateRememberSettingsToTrue();

    // 「自动同步」启用状态与检测周期的持久化（默认关闭）
    //
    // 周期的**合法范围**定义在 AutoSyncScheduler 里（10 ~ 86400 秒），
    // UI 的输入框校验也引用那同一份常量 ——
    // 两边各写一份范围，迟早会漂移成"界面填 5、后台存 60"，
    // 客户既看不出来也察觉不到。
    static bool                    loadAutoSyncEnabled();
    static void                    saveAutoSyncEnabled(bool enabled);
    static int                     loadAutoSyncInterval();
    static void                    saveAutoSyncInterval(int seconds);
    // 上一次自动同步**真正开跑**的时刻。
    // 用来算下一次（next = last + interval），也用来跨程序重启延续节奏。
    static QDateTime               loadAutoSyncLastRunAt();
    static void                    saveAutoSyncLastRunAt(const QDateTime &dt);

    // ---- 自动同步的两种模式：按间隔重复 / 每天定点 ----
    //
    // 参照物是 crontab，但**不给 cron 表达式**（需求 10 原文：
    // 「不要让用户手填 cron」）。只有两档，界面二选一：
    //
    //   sync/autoSyncMode      = "interval" | "daily"，默认 "interval"
    //   sync/autoSyncDailyTime = "HH:mm"，默认 "03:00"
    //
    // 老 ini 里没有这两个键 → 一律按默认值读（interval / 03:00），
    // **且不要因为读不到就把文件写回去** —— 老配置必须**逐字节不变**，
    // 否则"升级到新版本"就变成"顺手改了我的配置"。
    //
    // 两种模式互斥：切到 daily 时 interval 的数值保留但不生效，
    // 切回 interval 时 daily 的时刻保留但不影响调度。
    static AutoSyncScheduler::Mode  loadAutoSyncMode();
    static void                    saveAutoSyncMode(AutoSyncScheduler::Mode mode);
    static QTime                   loadAutoSyncDailyTime();
    static void                    saveAutoSyncDailyTime(const QTime &t);

    // ---- 带宽限制（rsync --bwlimit）----
    //
    // 档位集合（单位 MB/s）：**0 = 不限速**，另有 1 / 5 / 10 / 20 / 50 / 100，
    // 另外支持客户**自定义**一个 1~1024 之间的整数。
    // ini 键：sync/bandwidthLimitMBps
    //
    // 默认是 0（不限速），这是**必须**的：v2.2 交付给客户的版本就是不限速，
    // 下一版默认限速 5 MB/s 的话，客户升级后同步速度直接掉到 1/5，
    // 界面上还没有任何解释 —— 那是行为回归，不是新功能。
    //
    // 老 ini 里没有这个键 → 按不限速读，**且不要因为读不到就把文件写回去**
    // （和上面两个键同一条规矩：老配置必须逐字节不变）。
    //
    // 非法值（"abc"、-3、99999）→ 同样回落成不限速。
    // 宁可不限速，也不要给客户一个他没要求的降速。
    //
    // **自定义值是合法的**：客户自己填的 12 MB/s 不在预置档位里，
    // 但那是客户明确要的限速，不能当成"写坏了"给抹成不限速。
    static int                     loadBandwidthLimitMBps();
    static void                    saveBandwidthLimitMBps(int mbps);
    // 归一化：0 = 不限速；1 ~ bandwidthCustomMaxMBps() 的整数原样保留
    // （预置档位和自定义值都算）；其余一律归成不限速（0）。
    static int                     clampBandwidthMBps(int rawMBps);
    // **预置**档位，供界面铺菜单用（界面不自己另写一份，避免两边漂移）。
    // 注意这里**不含**自定义值：自定义值只有一个，是客户临时填的，
    // 不该混进"档位"里让人以为还有别的可选值。
    static QVector<int>            bandwidthLimitOptionsMBps();
    // 自定义值的合法区间（MB/s）。界面弹自定义输入框时用这两个值做上下限，
    // 越界值直接让输入框拒收，不要静默改写成别的数。
    static int                     bandwidthCustomMinMBps();
    static int                     bandwidthCustomMaxMBps();

    // ---- 「开机自动启动」----
    //
    // 持久化开关（app/autoStartEnabled，默认 false）。
    //
    // 三条口径（缺一不可）：
    //   1. **默认关闭**。客户机上的同步程序会主动连远端、起 rsync、建 SSH，
    //      开机就自动跑起来意味着"人还没坐下，网络已经在传数据"。
    //      crontab 也是管理员显式写进去才生效，不是装上就跑。
    //   2. **只负责启动程序**，不擅自开启自动同步。
    //      程序被自启拉起来后沿用用户上次勾选的「自动同步」状态
    //      （sync/autoSyncEnabled），"上次没勾"就只是把程序打开、不开始同步。
    //   3. 老 ini 里没有这个键 → 视为 false，
    //      **且不要因为读不到就把文件写回去**（只有用户真正点过勾选框才落盘）。
    static bool                    loadAutoStartEnabled();
    static void                    saveAutoStartEnabled(bool enabled);

    // 把「开机自动启动」真正落到系统上。
    //
    // Windows：HKCU\Software\Microsoft\Windows\CurrentVersion\Run 下的一个值，
    //          值 = 程序的绝对路径（带引号）。**免管理员权限**（HKCU 不需要提权）。
    // Linux  ：~/.config/autostart/ 下的 .desktop 文件（XDG autostart 规范）。
    //
    // 失败时把原因写进 errorOut 并返回 false —— 界面必须给用户看得见的提示，
    // 部署人员要能据此排查（路径不对？目录只读？）。
    static bool                    applyAutoStart(bool enable, QString *errorOut);

    // 系统里**当前实际**是不是处于自启状态（读注册表 / 读 .desktop）。
    // 与 loadAutoStartEnabled()（用户意图）分开：用户手工删了注册表值时，
    // 两者会不一致，界面据此提示"系统里已经没有了"。
    static bool                    isAutoStartActive();

    // 自启项的命令行（带引号的绝对路径；Linux 上优先用同目录的 run.sh）。
    static QString                 autoStartCommand();

    // 自启项落在哪里（给错误信息用，例如
    // 「HKCU\Software\Microsoft\Windows\CurrentVersion\Run\SyncReplayTool」）。
    static QString                 autoStartLocation();

    // 配置目录（单实例锁文件放这里）。
    //
    // 锁必须**跟着配置目录走**：同一份配置 = 同一个目录 = 同一把锁；
    // 不同配置目录（测试沙箱、并行用例）= 不同的实例，互不干扰。
    static QString                 configDir();

    // 「记住设置」到底管什么：
    // 勾上   —— 目标连接信息（IP、端口、账号、密码、私钥、本机目录、远端路径）
    //           写进配置文件，下次启动自动填回；
    // 不勾   —— 这些信息**不写**，并且配置文件里原有的那一份会被清除。
    // 工具规则与同步规则不受它影响（那两组在各自的面板里，不在设置弹窗里）。
    //
    // 原来这个复选框只记录自己被勾没被勾，实际的落盘与恢复走的是另一条路
    // （saveTarget 无条件写、loadTarget 无条件读），所以它等于摆设。
    // 界面上那个勾选框已按客户意见移除（Jira Tools-60）⇒ 这个键恒为 true，
    // saveTarget / loadTarget 两侧的闸门整段去掉，老键由
    // migrateRememberSettingsToTrue() 在启动时迁移。

    // 最近一次落盘失败的原因。空串 = 上次写成功了。
    //
    // QSettings 的错误是**延迟**报的（setValue 全返回 void，只有 sync() 之后
    // status() 才说得出真话），原来整个工程没有一处检查它，
    // 于是设置弹窗无条件显示「✓ 设置已保存」—— 盘满、目录只读的时候
    // 一个字节都没落盘，界面却什么异常都没有。
    static QString                 lastWriteError();

    // ---- 路径字段的两条处理，公开出来是为了让自检判据验到**同一份实现** ----
    //
    // 它们原本是 .cpp 匿名命名空间里的自由函数。自检只能照抄一份，
    // 而照抄的那份永远跟着实现漂移：改坏实现、判据照样全绿。
    // 暴露之后 saveTarget() 和 main.cpp 的自检调的是同一个函数。
    //
    // normalizePathForStore：写进 ini 之前把反斜杠换成正斜杠。
    //                       QSettings 的 INI 解码会把不认识的转义（\U \s \. \i …）
    //                       连反斜杠一起吃掉，而写侧不转义，所以每往返一次
    //                       就损坏一次路径。详见 .cpp 里的实测记录。
    // pathLooksCorrupted  ：认出一个**已经被吃掉**的旧值（盘符冒号后面
    //                       直接跟了普通字符）。只用于提示用户重填，不做修复。
    static QString                 normalizePathForStore(const QString &path);
    static bool                    pathLooksCorrupted(const QString &path);

    // 「配置里的路径已被损坏」这两条用户可见文案。
    //
    // 为什么要单独暴露：它们原本是 loadTarget() 里两段内联字面量，
    // 而它们**曾经把「损坏」写成「损坍」**（\u574D 是「坍」、\u574F 才是「坏」）。
    // 这种错字肉眼极难在 review 里发现，而它直接显示给客户。
    // 照抄一份到判据里等于测判据自己 —— 把这里改回错字，判据照样全绿。
    // 暴露之后 loadTarget() 和 main.cpp 的自检用的是**同一份字符串**。
    static QString                 corruptKeyPathNotice();
    static QString                 corruptLocalRootNotice();

    // 端口「不是纯数字」的用户可见文案。
    //
    // 缺陷（2026-10-18 实测，交付包 + 真实点击）：
    // 端口栏填全角数字（２２２６）、中间带空格（2 226）、超范围（99999）时，
    // QString::toInt() 全部失败 -> loadTarget() 和 SettingsDialog::applyConfig() 都**静默**
    // 退回默认端口 22。用户点「开始同步」看到的是
    //   「网络层：没有连到目标电脑：可能是 IP 填错、网络或交换机不通，
    //     或者目标电脑上这个端口没有服务」
    // 真因就在他自己屏幕上的端口栏，却被指去了防火墙和交换机。
    // ssh 那边也不会有补救机会：传下去的是合法的 22，ssh 无话可说，
    // 所以 RsyncRunner 的 "Bad port" 规则永远命中不了（那条只在传 0 时才触发）。
    //
    // 同理暴露出来，是为了让自检判据验到**同一份字符串**，
    // 照抄一份到判据里等于测判据自己。
    static QString                 invalidPortNotice(const QString &rawPort);

    // 端口字符串判据本身也暴露出来，理由同上：自检要验**同一份实现**。
    // 照抄一份到判据里，改坏实现时判据照样全绿。
    static bool                    isPlainPortText(const QString &text);

    // 设置弹窗也要用：用户在弹窗里当场填错，同样需要一句人话。
    // 因此 notice() 从 private 提到 public。
    static void                    notice(const QString &message);

    // 端口这一栏有问题就说一句 —— 判断和提示**绑在同一个函数里**。
    //
    // 上一轮（2026-10-18 端口修复）把判断留在了调用点：
    //     if (!portText.isEmpty() && !isPlainPortText(portText))
    //         notice(invalidPortNotice(portText));
    // 那个 `!portText.isEmpty()` 守卫写在**谓词外面**，把「端口根本没填」
    // 整类漏掉了：客户第一次装、端口栏空着就保存，界面一个提示都没有，
    // 静默退 22，同步时全部失败并把原因指去网络层和防火墙。
    //
    // 漏掉它还不算完 —— 自检里的 emptyRejected 反向锁**一直是对的**
    // （isPlainPortText("") 本来就返回 false），因为它测的是谓词，
    // 谓词没问题，洞在接线。判据测的不是它以为测的东西。
    //
    // 所以这里做成"无条件调用、判断在函数里面"：调用点已经没有条件可以写了，
    // 这类洞不可能再从同一个门进来。
    static void                    noticePortProblem(const QString &rawPort);

    // 自检用：这一刻还有几条提示没发出去（没人接收信号时 notice() 先攒着）。
    // 用来验**接线**而不是验谓词 —— 判据必须能测到"调用点有没有真的调它"。
    static int                     pendingNoticeCount();

    // 静态 API 发信号用的进程内唯一实例，界面连它的 configNotice。
    // 必须在 SyncPanel / ToolPanel 构造**之后**再连：这两个面板的构造函数
    // 里就会 loadTarget()，那时还没有接收者，ConfigStore 会把提示先攒着，
    // 等连接建立后在事件循环下一轮统一补发。
    static ConfigStore            *instance();

signals:
    // 落盘/读取环节的「静默丢数据」必须让用户看见：DPAPI 解密失败、加密失败、
    // 规则条数超过 kMaxRules 被截断。
    void configNotice(const QString &message);

    // 带宽限制档位被保存（**落盘之后**发出）。
    //
    // 谁需要知道：后台同步器。它要判断"这次改动是不是发生在同步过程中"——
    // rsync 的 --bwlimit 只在进程启动时生效，同步中途改档位，
    // 本次同步仍按旧值跑，新值只能下次生效。
    // 后台收到这个信号才能把这件事**如实**告诉用户，
    // 而不是显示一句"已生效"的假反馈。
    void bandwidthLimitSaved(int mbps);

private:
    void                           flushPending();

    QStringList                    m_pending;
    bool                           m_flushScheduled = false;
};
