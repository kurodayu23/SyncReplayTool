#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>
#include "Model.h"

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
    // 老配置里没有这个键时按 true 处理，保持「开箱即用」那一档不变。
    static bool                    loadOnlyNewFiles();
    static void                    saveOnlyNewFiles(bool onlyNew);

    // 「记住设置」勾选框的持久化（默认 true，允许用户选择下次是否沿用/记住设置）
    static bool                    loadRememberSettings();
    static void                    saveRememberSettings(bool remember);

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

private:
    void                           flushPending();

    QStringList                    m_pending;
    bool                           m_flushScheduled = false;
};
