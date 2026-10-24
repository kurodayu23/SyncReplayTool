#pragma once

#include <QDateTime>
#include <QString>
#include <QTime>

// 需求 10 的后台调度：**程序内调度**，不写系统 crontab。
//
// 为什么是程序内而不是系统级：
//   - 客户是运维人员。让他们在麒麟机器上配 crontab 需要 root，
//     且 crontab 里要写死 SSH 路径、密钥路径，远端口令只能明文躺在文件里。
//   - 系统 crontab 对应的是"软件关了也能跑"，那是另一套产品
//     （开机自启 + systemd unit + 麒麟/统信双平台各做一遍）。
//   - 需求原文只说"支持手动/自动同步" + "不要让用户手动填 cron 配置"，
//     没说"软件关了也要自动同步"。程序内调度已经满足。
//
// 为什么用「间隔」而不是「每天几点」：
//   - 间隔语义能覆盖「每天一次」（86400 秒），不需要模式切换控件。
//   - 客户心智负担最小：一个数字 = 每 N 秒同步一次。
//   - 定点语义会带出「昨天没开机，今天补不补」这类边界；间隔语义没有。
//   - 需求原文要的是"类似 cron 配置"，重点在**不让用户手填 cron 表达式**
//     而不是"必须是定点"。
//
// 【2026-10-23 追加：定点也做了，但仍然不给 cron 表达式】
// 客户方（赵工）后续明确要求「系统常驻 / 开机自启 + 日历定点一起做了」，
// 所以本类现在有两种模式（见下面的 Mode 枚举）：
//
//   - 为什么**不**给 cron 表达式：需求 10 原文「不要让用户手填 cron」。
//     表达式会带出"看起来对但永远不触发"的组合（客户填错一个字段就静默停摆），
//     而"每天 HH:MM"这一档覆盖了绝大多数真实诉求，错了当场就能看出来。
//   - 为什么**不**直接写系统 crontab：仍然是程序内调度。
//     客户是运维人员，让他们配 crontab 需要 root，且 crontab 里要写死
//     SSH 路径、密钥路径，远端口令只能明文躺在文件里。
//     开机自启另算（见交付包里的「开机自动启动」勾选框与可选的
//     安装自启.sh —— user 级 systemd，同样不需要 root）。
//   - 定点与间隔的关系：**互斥**，界面二选一。
//     定点模式的 next 计算与间隔模式**同构**（同一个 lastRunAt 落盘键），
//     所以两边的"关机期间错过 → 开机补跑"语义完全一致。
//
// 这个类**不碰界面**：控件由 UI 侧负责，本类只负责
//   1) 算出"下一次该跑的时刻"
//   2) 心跳到了问一句"到点了吗"
//   3) 记住上一次真正开跑的时刻
//
// 纯函数 nextRunAt() 是刻意独立出来的：定时器依赖事件循环，
// --selfcheck 不跑事件循环，所以定时器类的判据写进自检就是假绿灯。
// 但"什么时候该跑"这个计算本身可以纯函数化并进自检。

class AutoSyncScheduler
{
public:
    // 间隔范围。**UI 与后台必须引用同一份**，否则会出现
    //「界面能填 10、后台截成 60」的错位 —— 客户填 10 秒、界面显示 10 秒、
    // 实际每 60 秒跑一次，且完全察觉不到。
    //
    // 下限 10 秒：一次自动同步 = 全目录深度扫描 + 起 rsync 进程 + 建 SSH
    // 连接。间隔越短，对本机和远端的压力越大，所以下限不是 0。
    // 上限 86400 秒 = 一天，这样"每天同步一次"在界面上表达得出来。
    // 默认 30 秒：客户勾上"自动同步"后不用改就能用，又不至于太频繁。
    static const int MinIntervalSec = 10;
    static const int MaxIntervalSec = 86400;
    static const int DefaultIntervalSec = 30;

    // 规整一个间隔值到合法范围。UI 的输入框校验与后台的落盘截断
    // 都调它，保证两边口径完全一致。
    static int clampIntervalSec(int rawSeconds);

    // ---- 自动同步的两种模式（需求 10 的"类似 cron"）----
    //
    // 参照物是 crontab，但**不给 cron 表达式**（需求 10 原文：
    // 「不要让用户手填 cron」）。所以只有两档：
    //
    //   ModeInterval —— 按间隔重复（每 N 秒一次）。这是**老版本的行为**，
    //                   老配置里没有模式键时一律落到这一档，行为一字不变。
    //   ModeDaily    —— 每天定点（每天 HH:MM 一次）。
    //
    // 两种模式**互斥**：界面二选一。
    // 切到 daily 时 interval 的数值保留但不生效；
    // 切回 interval 时 daily 的时刻保留但不影响调度。
    enum Mode {
        ModeInterval = 0,
        ModeDaily    = 1
    };

    // 算"下一次该跑的时刻"。
    //
    // now        : 当前时刻
    // lastRunAt  : 上一次**真正开跑**的时刻（不是"到点"的时刻）。
    //               无效（从未跑过）时传 QDateTime()。
    // fromNow    : true 时**忽略 lastRunAt**，一律按 now + intervalSec 算。
    //
    // 规则：next = lastRunAt + intervalSec。
    //   - fromNow = true → next = now + intervalSec
    //     （"从这一刻起算下一个周期"。用户点「保存设置」后要的是这个：
    //      刚改完配置就立刻全量跑一遍既突兀又打断他手上的操作。）
    //   - lastRunAt 无效 → next = now + intervalSec
    //     （刚启用时从启用那一刻起算第一个周期，不立刻跑一次）
    //   - lastRunAt 有效 → next = lastRunAt + intervalSec
    //     （**从上一次开跑算起**，不是从"上一次结束"算）
    //
    // 为什么用绝对时间点而不是倒计时计数器：
    //   倒计时会在"手动同步完成"时被重置（onAllFinished 里），
    //   于是手动操作会悄悄改变自动节奏 —— 用户点一次开始同步，
    //   自动同步的周期就往后挪了一段，节奏变得不可预期。
    //   绝对时间点不会：手动同步不碰 lastRunAt，自动节奏不动。
    //
    // 为什么不用"上次结束"：一次同步可能跑 10 分钟。用"上次开跑"算，
    // 下一次会在上一轮还在跑的时候到点 —— 那时会被 busy 挡掉，
    // 但**不会丢**（nextRunAt 不变，下一次心跳发现 now >= next 仍然成立，
    // 忙完立刻补上）。这正是想要的语义：不丢触发，但也不并发。
    //
    // 这是一个纯函数：不依赖任何成员、不读系统时钟、不写文件。
    // 返回无效 QDateTime 表示"没有下一次"。
    static QDateTime nextRunAt(int intervalSec,
                               const QDateTime &now,
                               const QDateTime &lastRunAt,
                               bool fromNow = false);

    // ---- 每天定点模式 ----
    //
    // 语义与间隔模式**同构**，只有一处不同：定点算的是
    // "上一个 lastRunAt 之后的**第一个** HH:mm"，
    // 而不是"上一个 lastRunAt 之后 + interval 秒"。
    //
    // 因为 lastRunAt 是**落盘**的（复用同一个 sync/autoSyncLastRunAt 键），
    // 于是关机期间错过的定点也会补跑：
    //   昨天 03:00 跑过 → next = 今天 03:00
    //   机器关了 3 天 → lastRunAt 是 3 天前 → 它之后的第一个 03:00
    //     已经落在过去 → 开机后**立刻补跑一轮**
    //   补跑完 lastRunAt = now → next = 明天 03:00
    //
    // 与 cron 的差别（cron 错过不补跑）写在手册里，理由见台账 §0.5。
    //
    // fromNow = true（用户刚改完时刻）→ 以 now 为基准算**下一个** HH:mm，
    // 改完立刻生效，但**不立刻跑一轮**（与间隔模式的 fromNow 同一个口径）。
    //
    // 纯函数：不读系统时钟、不写文件、可离线单测。
    static QDateTime nextDailyRunAt(const QTime &dailyTime,
                                    const QDateTime &now,
                                    const QDateTime &lastRunAt,
                                    bool fromNow = false);

    // ---- 自动同步**失败**之后的下一次重试时刻 ----
    //
    // 什么时候用到：目标电脑/账号或本机目录没配好时，autoSyncFire() 走失败分支，
    // 必须把排程推进出去（不推进的话心跳下一拍又"到点"，变成永远 0s 倒计时）。
    //
    // 为什么单独抽成一个纯函数：这段逻辑原来直接写在 SyncPanel::autoSyncFire() 里，
    // 而"定时器相关的行为"写进 --selfcheck 是假绿灯（自检不跑事件循环）。
    // 但"失败之后下一次什么时候重试"这个计算本身是**纯计算**，
    // 抽出来就能进自检 —— 判据验的是产品自己那份实现，不是影子算法。
    //
    // 三个调用点（用户刚改完设置 / 刚勾上自动同步 / 失败后重试）共用这一个实现，
    // 口径不会分叉：不会出现"改了一处、另一处还按老口径跑"却全绿。
    //
    // 口径：
    //   · 排程基准**截到整秒**（与 autoSyncScheduleNext 同一口径）。
    //     心跳的格子锚在定时器启动时刻上，基准带毫秒时某一拍可能落在
    //     名义到点时刻之前几毫秒 ⇒ 那一拍整拍跳过 ⇒ 实际间隔变成 N+心跳。
    //   · 每天定点模式重试到**下一个定点时刻**，不是"现在 + 间隔"：
    //     定点模式下「间隔」在设置界面里是隐藏的（SettingsDialog），
    //     值常年是上次留下的默认值 ⇒ 按间隔重试等于把「每天一次」
    //     变成「每 30 秒一次」，一整天都在重试，而日志因为失败去重只打一条，
    //     客户完全看不出来（界面只看到「下次同步 30s 后」在原地倒计时）。
    //   · 间隔模式重试一个**完整周期**（now + 间隔）。
    //
    // 纯函数：不读系统时钟、不写文件、可离线单测。
    static QDateTime nextAfterFailure(int intervalSec,
                                       const QDateTime &now,
                                       Mode mode,
                                       const QTime &dailyTime);

    // 默认定点时刻：每天凌晨 03:00。
    // 为什么是 03:00：客户机多半晚上不干活、凌晨也没人用带宽。
    // 但这只是**默认值** —— 客户可以在界面上改（不同客户的上传窗口不一样）。
    static QTime defaultDailyTime();

    // "HH:mm" <-> QTime 的**唯一**转换口。
    //
    // 写侧永远输出两位补零的 "HH:mm"（03:00 而不是 3:00），
    // 读侧接受 "H:mm" 与 "HH:mm" 两种，但只认**纯 ASCII 数字**：
    // 全角「０３：００」直接判非法（与端口那套 ASCII 判据同一条理由：
    // toInt() 对全角数字的行为不能当依据，逐字符比 ASCII 码才与 Qt 版本无关）。
    static QString dailyTimeToString(const QTime &t);
    static bool   parseDailyTime(const QString &raw, QTime *out);

    // 心跳到了没有？now >= next 才算到点。
    // 提前到（比如心跳 30 秒一次，目标时刻在 29 秒后）不算。
    static bool isDue(const QDateTime &now, const QDateTime &next);

    // 心跳周期。取 interval 的一半，下限 5 秒、上限 60 秒：
    //   - 间隔 60 秒 → 心跳 30 秒，到点误差 ≤ 30 秒
    //   - 间隔 86400 秒 → 心跳 60 秒，一天 1440 次唤醒，可接受
    //   心跳只是"问一次到没到点"，不是调度器本身。
    static int heartbeatMsFor(int intervalSec);

};
