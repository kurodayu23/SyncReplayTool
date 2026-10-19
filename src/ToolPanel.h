#pragma once

#include <QWidget>
#include <QVector>
#include <QStringList>
class QTimer;
#include "Model.h"

class QTableWidget;
class QPushButton;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class ToolRunner;

// 本地工具调用：选文件 -> 按后缀匹配工具规则 -> 本地执行。与同步无关。
class ToolPanel : public QWidget
{
    Q_OBJECT

public:
    explicit ToolPanel(QWidget *parent = nullptr);

    void saveConfig();

    // 同步页的同名函数。窗口下限取两页里更大的那个 ——
    // 两个页面要么都完整、要么都靠滚动，不能一个完整一个残废。
    int contentMinWidth() const;

    // 以下三个只给自动化测试用。工具回放的正确性完全体现在"点了运行之后
    // 状态怎么变、有没有多弹框"上，从外面看不到，而 QFileDialog 让探针没法
    // 真的走一遍选文件流程。这里开的口子只是把界面控件里的值直接塞进去，
    // 不新增任何界面没有的能力。
    void setSelectedFilesForTest(const QStringList &files) { m_selectedFiles = files; updateRunButtons(); }
    void replaceRuleForTest(const QString &suffix, const QString &exe, const QString &args);
    QString statusTextForTest() const;
    // 进程或队列是否还在跑。队列串行执行时两个进程之间 m_runner->isRunning()
    // 是 false，所以对外暴露的"有没有在跑"必须是包含队列判断的那个版本。
    bool isToolRunning() const;
    // 本面板启动以来累计记下的失败文件数。finishQueue() 每轮结束都会清空
    // 本轮清单，只看那一处的计数抓不到"延迟到达的失败被记成新一轮失败"
    // 这种跨轮的错误。
    int totalFailuresForTest() const { return m_totalFailures; }
    // 只给自动化测试用：「用匹配工具打开」按钮此刻是否可点。
    // 取消之后按钮能不能自己恢复可用，只能从界面上看，
    // 而"按钮一直禁用到重启程序"这种问题从状态文案里完全看不出来。
    bool runButtonEnabledForTest() const;

private slots:
    void addRule();
    void removeRule();
    void browseRuleExe(int row, int column);
    void pickFiles();
    void runTool();
    void stopTool();
    void onRunnerOutput(const QString &text);
    void onRunnerFinished(int exitCode);
    void flushLog();

private:
    QVector<ToolRule> collectRules() const;
    void refreshFileList();
    void appendLog(const QString &text);
    void setStatus(const QString &text, bool error);
    void updateRunButtons();
    void startNextQueuedFile();
    void finishQueue(bool cancelled);

    QTableWidget    *m_ruleTable;
    QPushButton     *m_addRule;
    QPushButton     *m_delRule;
    QLineEdit       *m_pendingFiles;
    QPushButton     *m_btnPick;
    QPushButton     *m_btnRun;
    QPushButton     *m_btnStop;
    QTableWidget    *m_fileTable;
    QLabel          *m_statusLabel;
    // 第三方工具自己的输出（stdout + stderr）。以前只存在 m_output 里，
    // 而 m_output 只在**失败**弹窗里用到 —— 跑成功的时候工具说了什么、
    // 报了什么警告、结果文件写到哪，用户一个字都看不到。
    // 同步那页一直有运行日志，工具这页没有，两页行为不一致。
    QPlainTextEdit  *m_logView;
    QTimer          *m_logFlushTimer = nullptr;
    QString          m_pendingLog;

    QString         m_output;
    QStringList      m_selectedFiles;
    ToolRunner       *m_runner = nullptr;

    // 多文件串行队列：一个工具进程跑完再起下一个。
    // 以前只跑 files.first()，多选文件时静默只处理第一个却报"执行完成"。
    ToolRule         m_activeRule;
    QString          m_queueCurrent;
    QStringList      m_queue;
    QStringList      m_queueFailures;
    int              m_queueTotal = 0;
    int              m_totalFailures = 0;
    bool             m_cancelRequested = false;
};
