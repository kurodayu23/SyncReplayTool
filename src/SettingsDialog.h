#pragma once

#include <QDialog>
#include <QHash>
#include <QVector>
#include <QTime>
#include "Model.h"
#include "AutoSyncScheduler.h"

class QLineEdit;
class QLabel;
class QPushButton;
class QCheckBox;
class QTimeEdit;
class QTableWidget;
class QFrame;
class QScrollArea;
class QWidget;

// 「传输与工具设置」弹窗。
//
// 尺寸与结构严格照图三、图四原型：560px 宽、14px 柔和曲度圆角卡片、整体 max-height 80vh，
// 无原生系统白边标题栏，头尾固定、只有中间滚动 —— 保存设置永远 100% 可见可点。
//
// 字段按图四从上到下：
//   本机文件目录 | 目标电脑 IP + SSH 端口 | 登录账号 + 登录密码 | 私钥文件
//   | 目标落盘路径 | 文件打开工具规则表（文件类型 / 工具名称 / 关联程序路径）
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    // 载入 / 取回。规则表里的三列分别是后缀、工具名、关联程序路径。
    void loadConfig(const TargetConfig &target,
                    const QString &localRoot,
                    const QVector<ToolRule> &toolRules);
    void applyConfig(TargetConfig *target,
                     QString *localRoot,
                     QVector<ToolRule> *toolRules) const;
    TargetConfig currentTargetConfig() const;

    // 测试连接的结果由外部算好后塞进来显示
    void showTestResult(const QString &text, bool ok, bool testing);

    int autoSyncInterval() const;

    // 自动同步的两种模式（按间隔重复 / 每天定点）。互斥，界面二选一。
    AutoSyncScheduler::Mode autoSyncMode() const;
    // 每天定点的时刻（界面上的时间选择器）。
    QTime autoSyncDailyTime() const;
    // 「开机自动启动」当前勾选状态（= 用户意图；系统里实际有没有落地
    // 由 ConfigStore::isAutoStartActive() 单独回答）。
    bool autoStartEnabled() const;

signals:
    // 点了「测试网络连接」
    void settingsTestConnectionRequested();
    // 点了「保存设置」
    void settingsAccepted();

public slots:
    void browseSourceDir();
    void browseKeyFile();
    void addRuleRow();
    void removeRuleRow(int row);
    void onRuleCellChanged(int row, int col);
    void onSaveClicked();
    // 两种自动同步模式之间切换：把不生效的那套控件**藏起来并禁用**，
    // 别让用户同时看到两个可填的框（填了不生效 = 客户以为软件坏了）。
    void onAutoSyncModeChanged();
    // 「开机自动启动」勾选框。**立即生效**（写系统），不等「保存设置」——
    // 写注册表 / 写 .desktop 是一个动作，用户勾了就是勾了。
    void onAutoStartToggled(bool on);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    QFrame      *m_card = nullptr;
    QFrame      *m_header = nullptr;
    QPushButton *m_btnClose = nullptr;
    QPushButton *m_addRule = nullptr;
    QPushButton *m_btnSave = nullptr;
    // 「自动同步」总开关。原先它摆在主界面底栏，方案评审后移进设置窗口，
    // 底栏不再重复同一个开关。
    QCheckBox   *m_autoSyncBox = nullptr;
    QLabel      *m_testLabel = nullptr;
    QLabel      *m_saveTip = nullptr;
    QTableWidget *m_ruleTable = nullptr;

    QLineEdit *m_sourceDir = nullptr;
    QLineEdit *m_host = nullptr;
    QLineEdit *m_port = nullptr;
    QLineEdit *m_user = nullptr;
    QLineEdit *m_password = nullptr;
    QLineEdit *m_keyPath = nullptr;
    QLineEdit *m_remoteRoot = nullptr;
    QLineEdit *m_autoSyncInterval = nullptr;

    // 自动同步模式二选一：按间隔重复 / 每天定时。
    //
    // 这两个是**分段按钮**（定稿图 .seg-group），不是单选圆点：
    // 两颗 radio 挂在设置窗口里太像表单控件，和整套 AP500BJ 的直角按钮语言不一致。
    // 互斥靠 setAutoExclusive(true) —— 和 radio 的 autoExclusive 同义。
    QPushButton *m_modeInterval = nullptr;
    QPushButton *m_modeDaily = nullptr;
    // 每天定点的时刻选择器（只有「每天定时」模式下可用）。
    QTimeEdit *m_autoSyncDailyTime = nullptr;
    // 间隔行 / 定点行的**整行**容器：模式切换时整行藏掉，
    // 只藏输入框的话，标签会变成孤儿（"自动同步间隔  秒"）。
    QWidget    *m_intervalRowBox = nullptr;
    QWidget    *m_dailyRowBox = nullptr;

    // 「开机自动启动」勾选框。
    //
    // 语义（2026-10-23 定稿）：
    //   · 只负责**启动程序**，不擅自开启自动同步；
    //     程序起来后沿用用户上次勾选的「自动同步」状态。
    //   · 默认关闭；老配置里没有 app/autoStartEnabled 一律当 false。
    //   · 勾上 → 写系统（Windows 注册表 Run 键 / Linux ~/.config/autostart）；
    //     取消 → 删掉，不留垃圾。
    //   · 写/删失败必须给用户看得见的提示（部署人员要能据此排查）。
    QCheckBox   *m_autoStartBox = nullptr;

    // 保持后端兼容的数据
    int     m_internalPort = 22;
    QString m_internalKeyPath;

    // 规则表里哪些行是"新加的还没填完"
    QVector<bool> m_rowExists;

    // loadConfig 时从传入的旧规则里抽出的「(后缀, exe) -> 启动参数」快照。
    //
    // 为什么需要：规则表只有 3 列（后缀 / 工具名 / 程序路径），没有参数列，
    // 所以 applyConfig 无论怎么写都采不到 args。参数是在**工具页**配的，
    // 而两个页面写的是同一份 toolRules —— 于是这里必须沿用旧值，
    // 否则用户「工具页配好参数 → 打开设置 → 点保存」就把参数全清空了，
    // 界面全程无提示，工具下次拉起来变成裸 exe。
    //
    // 键为什么必须带后缀（不要只用 exe）：ToolRule 里 toolNames / toolExePaths /
    // toolArgs 是**平行数组**，参数按「后缀的第 k 个工具」独立保存 —— 同一个 exe
    // 配给 .spe 和 .raw，两边的参数完全可以不同（`--play {file}` / `--raw {file}`）。
    // 只用 exe 做键时这两个参数挤进同一个槽位，后者覆盖前者：用户点一次「保存设置」，
    // .spe 的打开参数就被悄悄改成了 .raw 的。参数列在界面上**根本不存在**，
    // 所以这个损坏用户既看不见也改不回来。
    //
    // 同一后缀 + 同一 exe 配两行仍会互相覆盖 —— 但那本来就是重复配置，
    // 用户两条填的也是同一个程序的同一套用法，退化成覆盖不构成信息丢失。
    QHash<QString, QString> m_toolArgsByExe;

    static QString argKey(const QString &suffix, const QString &exe);

    // 拖拽窗口位移
    bool   m_dragging = false;
    QPoint m_dragOffset;
    // 规则表高度随规则数变化时，要跟着把滚动区/面板一起调高或调低。
    QScrollArea *m_scrollArea = nullptr;
};
