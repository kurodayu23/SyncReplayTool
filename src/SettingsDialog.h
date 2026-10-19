#pragma once

#include <QDialog>
#include <QHash>
#include <QVector>
#include "Model.h"

class QLineEdit;
class QLabel;
class QPushButton;
class QCheckBox;
class QTableWidget;
class QFrame;

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
    QCheckBox   *m_rememberBox = nullptr;
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
};
