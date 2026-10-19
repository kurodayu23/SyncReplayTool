#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QVector>
#include "Model.h"

// 本地工具调用。与同步无关，用户自选文件、自选工具执行。
class ToolRunner : public QObject
{
    Q_OBJECT

public:
    explicit ToolRunner(QObject *parent = nullptr);
    // 关窗/面板销毁时必须杀掉还在跑的第三方回放程序。
    // QProcess 的析构**不会**终止子进程，只打一条 "Destroyed while process is
    // still running" 警告就撒手，结果文件还在被继续写，用户已经以为程序关了。
    ~ToolRunner() override;

    // 同一后缀可以配多条规则（回放 / 合并 / IQ 抽取），返回全部匹配项
    static QVector<ToolRule> rulesFor(const QVector<ToolRule> &rules, const QString &suffix);
    static QString buildArgs(const ToolRule &rule, const QString &filePath);

    // 后缀归一化：去空白、转小写、剥掉开头的点。
    //
    // 原来它只以 static 私有函数的形式活在 ToolRunner.cpp 里，工具页用得上、
    // 同步表格页用不上 —— 那边是自己写的一条裸 `suffix == suf`。
    // 于是用户在设置里写 ".spe"、文件实际是 "spe" 时，表格当次会话永远
    // 判「未配置工具」，重启一次（读回配置文件时剥了点）又好了。
    // 提为公开接口，让两条路径只留一个口径。
    static QString normSuffix(const QString &s);

    // 参数构造 + 分词的一条龙：和工具页 start() 内部走的是同一条链。
    //
    // 同步表格页原来自己写了一份只认双引号的 splitArgs，不认反斜杠转义，
    // 路径含空格时被撕成两段，界面却全程报「执行完成」。
    // 复刻一份简化版的代价就是下一次修 P29 / T-A 时只改了一边。
    static QStringList buildArgv(const ToolRule &rule, const QString &filePath);
    // 只给自动化测试用：把同一段输出按给定分块喂进解码器，返回拼回来的文本。
    // 走的是运行期同一条路径（onReadyRead 用的就是它），不是另写一份实现，
    // 所以这里过了才能说明真实进程输出不会解坏。
    static QString decodeChunks(const QList<QByteArray> &chunks);

    void start(const QString &exePath, const QString &args);
    void cancel();
    bool isRunning() const { return m_proc != nullptr; }
    // 当前这个进程是不是被 cancel() 停掉的。
    //
    // 界面不能只看自己的取消标志：cancel() 里 terminate() 是异步的，
    // 第三方工具可能在 1.5 秒 kill 之前就活着，界面已经把队列收尾并把
    // 取消标志复位了。这时候迟到的 finished() 如果按"不是取消"处理，
    // 会凭空多记一条失败，弹出"共 0 个文件，1 个失败"的二次失败框。
    // 取消这件事必须跟着进程走，不能跟着界面走。
    bool wasCanceled() const { return m_cancelRequested; }

signals:
    void output(const QString &text);
    void finished(int exitCode);

private:
    void onReadyRead();
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void onError(QProcess::ProcessError error);
    // QProcess 报 started() 时把子进程挂进作业对象并放行它（仅 Windows 生效）。
    void onJobAttach();
    // 关掉作业句柄。作业带 JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE，
    // 最后一个句柄一关，作业里还活着的进程全部被终止。
    void closeJob();
    // 放掉作业里**还活着的进程**，但仍然关掉句柄。
    //
    // 与 closeJob() 的区别就是这一条：cancel() / 析构走 closeJob()（该杀），
    // 工具正常跑完走 releaseJob()（不该杀 —— 见 ToolRunner.cpp 里的实测对照）。
    void releaseJob();

    QProcess *m_proc = nullptr;
    bool m_cancelRequested = false;
    // 工具整棵进程树所在的 Windows Job Object 句柄（D5）。
    //
    // 声明成 void* 而不是 HANDLE，是为了不把 <windows.h> 拖进每一个 include
    // 本头文件的人；HANDLE 本身就是 void*，两边转换零成本。
    // nullptr = 这次没拿到作业对象，行为退回修复前：只杀直接子进程。
    void *m_job = nullptr;
    // 工具输出的流式解码器（UTF-8 / 本地编码自适应）。
    // 完整定义放在 ToolRunner.cpp 里：它带状态，拖进头文件会让每个
    // include 本头文件的人都白白拖进 QTextCodec。
    // 用裸指针而不是 unique_ptr：VS2015 的 unique_ptr 在不完整类型上
    // 连 operator-> 都解析不了（成员函数里一用就报 C2039）。
    class OutputDecoder;
    OutputDecoder *m_decoder = nullptr;
};
