#pragma once

#include <QMainWindow>
#include <QStringList>
#include <QPointer>

class SyncPanel;
class ToolPanel;
class SettingsDialog;
class QFrame;
class QLabel;
class QPushButton;
class QToolButton;
class QTimer;

// 主窗体。结构照定稿原型：
//
//   ┌ 标题栏（名称 | 设置 + 最小化 + 最大化 + 关闭）  44px
//   ├ SyncPanel（表格 + 进度区 + 底部条 + 日志抽屉）
//
// 无边框（Qt::FramelessWindowHint）+ 半透明背景 + 圆角根容器，
// 这样才有手册 §10.1 要的"微晶悬浮"质感，而不是 Windows 原生灰白标题栏。
//
// 窗口宽固定 980px：这一版按用户给的定稿图（1477×691 截图，1.5 倍缩放）
// 反推出来的，HTML 定稿 .window-frame 也是 980px。原来的 830px 是从更早一版
// 原型抄下来的，导致同一份设计稿"HTML 预览 980 / 真程序 830"对不上。
class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // 启动页时长（毫秒）。自动化测试把它压到 0，好让截图不用等动画。
    static void setSplashDuration(int ms);
    // 跳过启动页（自动化测试用）。
    static void setSkipSplash(bool skip);

    SyncPanel *syncPanel() const { return m_syncPanel; }
    ToolPanel *toolPanel() const { return m_toolPanel; }

    // 只给 offscreen 截图探针用。直接走私有槽，省得探针再连一遍信号。
    // 声明在这里，实现在 .cpp：头文件里内联调 m_settings->show() 需要
    // SettingsDialog 的完整类型，而这个头只前置声明了它。
    void showSettingsForProbe();

    // 走**用户点「保存设置」时的同一条链路**把设置弹窗里的值落到面板并落盘。
    //
    // 探针之前用 QMetaObject::invokeMethod(this, "onSettingsAccepted") 去打私有槽，
    // 靠不住：能不能命中取决于该槽有没有进元对象，而失败时 invokeMethod 只是
    // 返回 false，不报任何错。于是"设置弹窗里改了 600 条规则，ini 里还是 2 条"
    // 这种结果，读起来像产品把规则丢了，真实原因是这一枪压根没提交。
    // 公开一个确定性的入口，探针不用再赌。
    void saveSettingsForProbe();

    // 走「开始同步」按钮的同一条链路把一轮同步拉起来（自动化测试用）。
    //
    // 为什么需要：口令模式的明文口令临时文件原本只在析构时清，
    // 正常跑完一趟一次都不清（已修）。要证明这条修复生效，
    // 探针必须能**反复触发真实同步**再回头扫临时目录 ——
    // 用 QMetaObject::invokeMethod 打私有槽不可靠（槽没进元对象时
    // 它只是静默返回 false），所以给一个确定性的公开入口。
    void startSyncForProbe();

    // 自动化自检要直接配置面板并触发同步，给一个确定性的取法。
    class SyncPanel *syncPanelForTest() { return m_syncPanel; }

    // 定稿 UI 是**单栏**（左侧导航和页签栈都删了，工具调用并进了表格操作列），
    // 旧的 GUI 回归探针还在按 m_stack->setCurrentIndex(0/1) 来回切页。
    // 这里给它们一个等价物：0 = 收起日志抽屉，1 = 展开日志抽屉。
    // 同样是"反复切界面"的压测动作，能压到重绘、布局、滚动条，
    // 所以旧探针里那些压测/泄漏场景的**意图**没有丢，只是换了触发方式。
    // 真实的产品路径（点工具按钮、双击行）由 guifull 探针直接点 delegate 测。
    void switchPageForTest(int index);

    // 设置弹窗本体。只给 --selfcheck-gui 用：判据要能拿到弹窗里的
    // 真实控件（规则表、「+ 添加规则」按钮…）去点、去读。
    // 没有它，GUI 层的交互就只能靠人肉点，判据永远覆盖不到。
    SettingsDialog *settingsForTest() const { return m_settings; }

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onOpenSettings();
    void onSettingsAccepted();
    void onTestConnectionRequested();
    void onTestConnectionResult(const QString &text, bool ok, bool testing);
    void onConfigNotice(const QString &message);
    void flushConfigNotices();
    void finishSplash();
    void tickSplashMorph();
    void tickSplashPulse();
    void onToggleMaximize();
    void onLogDrawerToggled(bool open);

private:
    void buildSplash();
    void buildTitleBar();
    void buildBody();
    void applyWindowFrame();
    void refitToScreen();

    QWidget          *m_root = nullptr;
    QFrame           *m_splash = nullptr;
    QLabel           *m_splashLogo = nullptr;    // 形态 1 -> 形态 2 要换这张图的 pixmap
    QFrame           *m_splashDot = nullptr;     // 副文本左侧的呼吸点
    QLabel           *m_splashName = nullptr;
    QLabel           *m_splashStatus = nullptr;
    QFrame           *m_splashBarFill = nullptr;
    QTimer           *m_splashPulse = nullptr;
    QTimer           *m_splashTimer = nullptr;
    QTimer           *m_splashMorph = nullptr;
    int               m_splashPhase = 0;
    bool              m_splashPulseOn = false;

    QWidget          *m_body = nullptr;
    QFrame           *m_titleBar = nullptr;
    QPushButton      *m_settingsHead = nullptr;   // 标题栏右侧的「设置」文字按钮
    QToolButton      *m_maxBtn = nullptr;         // 最大化 / 还原
    SyncPanel        *m_syncPanel = nullptr;
    ToolPanel        *m_toolPanel = nullptr;
    SettingsDialog   *m_settings = nullptr;

    QPoint            m_dragOffset;
    bool              m_dragging = false;
    QStringList       m_configNotices;
    bool              m_noticeScheduled = false;
    qreal             m_startDpr = 1.0;
    // 探针截图模式：主窗和设置面板都不上屏、不抢焦点，只把控件渲染进 PNG。
    bool              m_probeNoScreen = false;
    // 是否已经摆过位置：第一次按屏幕居中，之后保住用户拖出来的位置
    // （只在越界时往里挪）。见 refitToScreen()。
    bool              m_placed = false;
};
