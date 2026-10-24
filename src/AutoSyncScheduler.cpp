#include "AutoSyncScheduler.h"

namespace {

// ini 键。与 ConfigStore 里的键同属一个文件，但**刻意不放进 ConfigStore**：
// ConfigStore 那套是"用户可见的连接设置"，且带 lastWriteError() 的落盘保护；
// 调度配置是另一类东西，混进去会让"设置保存失败"的归因变得含糊。
//
// 与 ConfigStore 共用同一个 ini 文件路径 —— 用同一份数据源，
// 这样用户在设置弹窗里改的间隔，就是程序里读的那份。
//
// 注意：这个键由 SyncPanel 负责写，ConfigStore 负责读（同一个文件）。
// 不用 ConfigStore 的 openSettings() 是因为它是 private 且带落盘错误上报，
// 而这里只需要"读写一个整数"，不值得把落盘失败卷进来。

} // namespace

int AutoSyncScheduler::clampIntervalSec(int rawSeconds)
{
    if (rawSeconds < MinIntervalSec) return MinIntervalSec;
    if (rawSeconds > MaxIntervalSec) return MaxIntervalSec;
    return rawSeconds;
}

QDateTime AutoSyncScheduler::nextRunAt(int intervalSec,
                                       const QDateTime &now,
                                       const QDateTime &lastRunAt,
                                       bool fromNow)
{
    if (!now.isValid())
        return QDateTime();

    const int interval = clampIntervalSec(intervalSec);

    // "从这一刻起算"：忽略 lastRunAt。
    //
    // 用户点「保存设置」后要的是这个 —— 注释里承诺的是
    // "重新排程：从这一刻起算下一个周期，不立刻跑一次"，
    // 而原来这里算的是 lastRunAt + interval：
    // 上次开跑是一小时前、间隔 60 秒 → next 落在一小时前，
    // 心跳一醒来就判定"已到点"，用户刚点完保存程序自己开始全量同步。
    // 注释和实现正好相反，而**注释才是产品意图**。
    if (fromNow)
        return now.addSecs(interval);

    if (!lastRunAt.isValid())
        return now.addSecs(interval);

    // 系统时间被往回调（现场机器 NTP 对时把时钟拨回去）：
    // lastRunAt 落盘在 ini 里，比 now 还晚 → next 也在将来 →
    // isDue 恒假 → 自动同步停到"时钟追上来"为止（实测回拨 1 小时 = 停 1 小时，
    // 而且因为落盘了，重启后依然停摆）。
    //
    // 这里判定成"回拨了"，按"从现在重新开始一个周期"处理。
    // 判据用严格大于：lastRunAt == now 是正常情况（同一秒内跑完又排程）。
    if (lastRunAt > now)
        return now.addSecs(interval);

    // 从**上一次开跑**起算，不是从"到点"起算。
    // 手动同步不写 lastRunAt，所以手动操作不会挪动自动节奏。
    //
    // 若 next 比 now 还早很多，说明上次开跑时间很旧（比如机器关了三天），
    // 此时 next 在过去，心跳一醒来就会判定"已到点"并立刻补跑 —— 这是对的：
    // 关了三天的机器开机后应该立刻同步一次，而不是再等一个完整周期。
    return lastRunAt.addSecs(interval);
}

// ---- 每天定点模式 ----
//
// 纯函数：不读系统时钟、不写文件，可离线单测。
// 与间隔模式**同构**：用的是同一个 lastRunAt 落盘键、同一套"错过补跑"语义。

QTime AutoSyncScheduler::defaultDailyTime()
{
    // 凌晨 03:00：客户机多半晚上不干活、凌晨也没人占带宽。
    // 但这只是**默认值** —— 客户可以在界面上改（不同客户的上传窗口不一样）。
    return QTime(3, 0);
}

QString AutoSyncScheduler::dailyTimeToString(const QTime &t)
{
    // 写侧永远输出**两位补零**的 "HH:mm"（03:00，不是 3:00）：
    // 落盘格式统一，运维在 ini 里一眼能读出来。
    const QTime u = t.isValid() ? t : defaultDailyTime();
    return QString("%1:%2")
        .arg(u.hour(), 2, 10, QLatin1Char('0'))
        .arg(u.minute(), 2, 10, QLatin1Char('0'));
}

bool AutoSyncScheduler::parseDailyTime(const QString &raw, QTime *out)
{
    const QString s = raw.trimmed();

    // 只认 "H:mm" / "HH:mm"。不接受 "3点"、"0300"、"3:0"、全角数字。
    const int colon = s.indexOf(QLatin1Char(':'));
    if (colon < 1 || colon > 2)
        return false;
    const QString hh = s.left(colon);
    const QString mm = s.mid(colon + 1);
    if (hh.size() != colon)      // 冒号位置与小时位数对不上（"12:3" / "1:23"）
        return false;
    if (mm.size() != 2)
        return false;

    // 逐字符比 ASCII 码。**不用 toInt() 当判据**：
    // QString::toInt() 对全角数字「０３」的行为在 Qt 版本间不一致，
    // 而"手填时刻"和"手填端口"是同一类输入，必须按码点判才稳。
    for (const QChar &c : hh + mm) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9'))
            return false;
    }
    const int h = hh.toInt();
    const int m = mm.toInt();
    if (h > 23 || m > 59)
        return false;

    if (out)
        *out = QTime(h, m);
    return true;
}

QDateTime AutoSyncScheduler::nextDailyRunAt(const QTime &dailyTime,
                                            const QDateTime &now,
                                            const QDateTime &lastRunAt,
                                            bool fromNow)
{
    if (!now.isValid())
        return QDateTime();

    const QTime t = dailyTime.isValid() ? dailyTime : defaultDailyTime();

    // 基准时刻：从哪儿往后找**第一个** t。
    //
    // · fromNow = true（用户刚改完时刻）→ 以 now 为基准，**不立刻跑一轮**。
    // · lastRunAt 无效（从没跑过）→ 以 now 为基准（与间隔模式同构）。
    // · lastRunAt 在将来（系统时钟被往回调）→ 也以 now 为基准。
    //   否则 next 会一直落在过去，心跳一醒来就判定"已到点"，
    //   自动同步会停到"时钟追上来"为止 —— 与间隔模式的回拨处理同一条理由。
    QDateTime base = now;
    if (!fromNow && lastRunAt.isValid() && lastRunAt <= now)
        base = lastRunAt;

    QDateTime candidate(base.date(), t);

    // 必须是**严格晚于** base 的那一个。
    //
    // base 恰好落在 t 上（lastRunAt == 今天 03:00:00.000）时，
    // 若取 ">= base" 就会算回今天 03:00 —— 那个时刻已经过去，
    // 心跳会立刻判定已到点并开跑，跑完 next 又是同一个时刻 → 无限快跑。
    //
    // 循环而不是 if：base 可能是几天前（关机期间错过），
    // 要一路跨到"base 之后的第一个 t"为止。
    while (candidate <= base)
        candidate = candidate.addDays(1);

    // candidate 可能落在过去 —— 那正是"错过补跑"：
    // isDue(now, candidate) 立刻成立，心跳一醒来就补跑一轮。
    return candidate;
}

// 自动同步失败之后的下一次重试时刻。
//
// 契约见 AutoSyncScheduler.h 里 nextAfterFailure() 的注释，这里只讲实现要点。
QDateTime AutoSyncScheduler::nextAfterFailure(int intervalSec,
                                              const QDateTime &now,
                                              Mode mode,
                                              const QTime &dailyTime)
{
    if (!now.isValid())
        return QDateTime();

    // 排程基准**截到整秒**。
    //
    // 与 autoSyncScheduleNext / autoSyncMarkRunStarted 同一口径：
    // 心跳定时器的格子锚在**定时器启动时刻**上，基准带零点几毫秒时，
    // 「名义到点时刻」可能落在某一拍之后几毫秒 ⇒ 那一拍醒来判「未到点」跳过
    // ⇒ 实际间隔从 N 秒变成 N+心跳（实测间隔 30 秒跑成 45 秒）。
    const QDateTime base = now.addMSecs(-now.time().msec());

    if (mode == ModeDaily) {
        // 每天定点：重试到**下一个定点时刻**。
        //
        // fromNow = true ⇒ 以 base 为基准往后找第一个 HH:mm，
        // 今天的定点还没到就是今天，已过就是明天 —— 不会回头补跑，
        // 也就不会因为"上次的定点早已过去"而每次心跳都立刻到点、失败、再排同一个
        // 过去时刻（那会变成死循环）。
        return nextDailyRunAt(dailyTime, base, base, true);
    }

    // 间隔模式：重试一个完整周期。
    return base.addSecs(clampIntervalSec(intervalSec));
}

bool AutoSyncScheduler::isDue(const QDateTime &now, const QDateTime &next)
{
    if (!next.isValid() || !now.isValid())
        return false;
    return now >= next;
}

int AutoSyncScheduler::heartbeatMsFor(int intervalSec)
{
    const int interval = clampIntervalSec(intervalSec);
    int hb = interval / 2;
    if (hb < 5)   hb = 5;
    if (hb > 60)  hb = 60;
    return hb * 1000;
}

