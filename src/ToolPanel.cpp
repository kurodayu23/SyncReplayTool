#include "ToolPanel.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTableWidget>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

#include "ConfigStore.h"
#include "ToolRunner.h"

namespace {

enum ToolRuleColumn { ToolSuffix = 0, ToolExe = 1, ToolArgs = 2, ToolRuleColumnCount = 3 };
enum SelFileColumn { SelFileName = 0, SelFileSuffix = 1, SelFileSize = 2, SelFileColumnCount = 3 };

QString humanSize(qint64 bytes)
{
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    return QStringLiteral("%1 %2").arg(v, 0, 'f', u == 0 ? 0 : 1).arg(QLatin1String(units[u]));
}


QLabel *cardTitle(const QString &text, QWidget *parent)
{
    auto *label = new QLabel(text, parent);
    label->setObjectName(QStringLiteral("cardTitle"));
    return label;
}

} // namespace

ToolPanel::ToolPanel(QWidget *parent)
    : QWidget(parent)
{
    m_ruleTable   = new QTableWidget(0, ToolRuleColumnCount);
    m_addRule     = new QPushButton(QString::fromUtf8(u8"\u6DFB\u52A0\u89C4\u5219"));
    m_delRule     = new QPushButton(QString::fromUtf8(u8"\u5220\u9664\u89C4\u5219"));
    m_pendingFiles= new QLineEdit;
    m_pendingFiles->setReadOnly(true);
    m_pendingFiles->setPlaceholderText(QString::fromUtf8(u8"\u5C1A\u672A\u9009\u62E9\u672C\u5730\u6570\u636E\u6587\u4EF6"));
    m_btnPick     = new QPushButton(QString::fromUtf8(u8"\u9009\u62E9\u6587\u4EF6"));
    m_btnRun      = new QPushButton(QString::fromUtf8(u8"\u7528\u5339\u914D\u5DE5\u5177\u6253\u5F00"));
    m_btnRun->setObjectName(QStringLiteral("primaryAction"));
    m_btnRun->setMinimumWidth(124);
    m_btnRun->setEnabled(false);
    m_btnStop     = new QPushButton(QString::fromUtf8(u8"\u505C\u6B62"));
    m_btnStop->setObjectName(QStringLiteral("dangerAction"));
    m_btnStop->setMinimumWidth(80);
    m_btnStop->setVisible(false);
    m_fileTable   = new QTableWidget(0, SelFileColumnCount);
    m_statusLabel = new QLabel(QString::fromUtf8(u8"\u5C31\u7EEA"));
    m_statusLabel->setAlignment(Qt::AlignCenter);

    // ---- 页面标题 + 主操作 ----
    auto *pageTitle = new QLabel(QString::fromUtf8(u8"\u5DE5\u5177\u56DE\u653E"), this);
    pageTitle->setObjectName(QStringLiteral("pageTitle"));
    auto *pageHeader = new QHBoxLayout;
    pageHeader->setSpacing(10);
    pageHeader->addWidget(pageTitle);
    pageHeader->addWidget(m_statusLabel);
    pageHeader->addStretch(1);
    pageHeader->addWidget(m_btnPick);
    pageHeader->addWidget(m_btnRun);
    pageHeader->addWidget(m_btnStop);

    // ---- 规则卡片 ----
    auto *ruleCard = new QFrame(this);
    ruleCard->setObjectName(QStringLiteral("card"));
    auto *ruleLayout = new QVBoxLayout(ruleCard);
    ruleLayout->setContentsMargins(18, 16, 18, 16);
    ruleLayout->setSpacing(10);
    ruleLayout->addWidget(cardTitle(QString::fromUtf8(u8"\u5DE5\u5177\u8C03\u7528\u89C4\u5219"), ruleCard));
    auto *ruleTip = new QLabel(QString::fromUtf8(u8"\u53CC\u51FB\u300C\u672C\u5730\u5DE5\u5177\u8DEF\u5F84\u300D\u5FEB\u901F\u9009\u53D6\u7A0B\u5E8F\uFF1B\u53C2\u6570\u4E2D {file} \u4F1A\u66FF\u6362\u4E3A\u6570\u636E\u6587\u4EF6\u8DEF\u5F84\u3002"), ruleCard);
    ruleTip->setObjectName(QStringLiteral("hint"));
    // 不换行的 hint 会把整行文字宽度变成卡片的最小宽度，
    // 窄屏上直接撑爆布局（右侧卡片被挤出窗口）。
    ruleTip->setWordWrap(true);
    ruleLayout->addWidget(ruleTip);

    m_ruleTable->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"\u6587\u4EF6\u540E\u7F00")
                                            << QString::fromUtf8(u8"\u672C\u5730\u5DE5\u5177\u8DEF\u5F84")
                                            << QString::fromUtf8(u8"\u542F\u52A8\u53C2\u6570\u6A21\u677F"));
    m_ruleTable->horizontalHeader()->setStretchLastSection(true);
    m_ruleTable->verticalHeader()->setVisible(false);
    m_ruleTable->setAlternatingRowColors(true);
    m_ruleTable->verticalHeader()->setDefaultSectionSize(30);
    // 显式给前两列宽度。Qt 默认每列 100，窄屏上三列叠起来超出卡片宽度，
    // 列头文字会被直接裁掉半个。
    m_ruleTable->setColumnWidth(ToolSuffix, 110);
    m_ruleTable->setColumnWidth(ToolExe, 230);

    auto *ruleButtons = new QHBoxLayout;
    ruleButtons->setSpacing(8);
    ruleButtons->addWidget(m_addRule);
    ruleButtons->addWidget(m_delRule);
    ruleButtons->addStretch(1);
    ruleLayout->addWidget(m_ruleTable, 1);
    ruleLayout->addLayout(ruleButtons);

    // ---- 文件卡片 ----
    auto *fileCard = new QFrame(this);
    fileCard->setObjectName(QStringLiteral("card"));
    auto *fileLayout = new QVBoxLayout(fileCard);
    fileLayout->setContentsMargins(18, 16, 18, 16);
    fileLayout->setSpacing(10);
    fileLayout->addWidget(cardTitle(QString::fromUtf8(u8"\u5F85\u56DE\u653E\u6570\u636E\u6587\u4EF6"), fileCard));
    fileLayout->addWidget(m_pendingFiles);

    m_fileTable->setHorizontalHeaderLabels(QStringList() << QString::fromUtf8(u8"\u6587\u4EF6\u8DEF\u5F84")
                                         << QString::fromUtf8(u8"\u540E\u7F00")
                                         << QString::fromUtf8(u8"\u6587\u4EF6\u5927\u5C0F"));
    m_fileTable->horizontalHeader()->setStretchLastSection(false);
    m_fileTable->verticalHeader()->setVisible(false);
    m_fileTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_fileTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_fileTable->setAlternatingRowColors(true);
    m_fileTable->verticalHeader()->setDefaultSectionSize(30);
    // 路径列吃掉所有多余宽度，后缀和大小按内容给固定宽度
    m_fileTable->setColumnWidth(SelFileSuffix, 80);
    m_fileTable->setColumnWidth(SelFileSize, 90);
    m_fileTable->horizontalHeader()->setSectionResizeMode(SelFileName, QHeaderView::Stretch);
    fileLayout->addWidget(m_fileTable, 1);

    auto *hint = new QLabel(QString::fromUtf8(u8"\u9009\u4E2D\u8868\u683C\u4E2D\u7684\u884C\u53EF\u53EA\u5BF9\u90E8\u5206\u6587\u4EF6\u6267\u884C\uFF1B\u672A\u9009\u4E2D\u5219\u5904\u7406\u5168\u90E8\u3002\u591A\u4E2A\u6587\u4EF6\u6309\u987A\u5E8F\u9010\u4E2A\u6267\u884C\uFF0C\u53EF\u70B9\u300C\u505C\u6B62\u300D\u4E2D\u65AD\u3002"), fileCard);
    hint->setObjectName(QStringLiteral("hint"));
    hint->setWordWrap(true);
    fileLayout->addWidget(hint);

    // ---- 工具输出卡 ----
    // 工具自己的 stdout/stderr 必须有一处能让用户看到：客户接的第三方回放程序
    // 报进度、报警告、把结果写在哪，全在这些输出里。以前这些内容只进 m_output，
    // 而 m_output 只在失败弹窗里被用到一次 —— 跑成功时整段被丢掉。
    auto *logCard = new QFrame(this);
    logCard->setObjectName(QStringLiteral("card"));
    auto *logLayout = new QVBoxLayout(logCard);
    logLayout->setContentsMargins(18, 16, 18, 16);
    logLayout->setSpacing(10);
    auto *logHeader = new QHBoxLayout;
    logHeader->addWidget(cardTitle(QString::fromUtf8(u8"\u5DE5\u5177\u8F93\u51FA"), logCard));
    logHeader->addStretch(1);
    auto *logTail = new QLabel(QString::fromUtf8(u8"\u6700\u8FD1 200 \u884C"), logCard);
    logTail->setObjectName(QStringLiteral("hint"));
    logHeader->addWidget(logTail);
    logLayout->addLayout(logHeader);

    // 与同步页的运行日志同款：只读、不换行处横向滚动、封顶 200 行。
    m_logView = new QPlainTextEdit(logCard);
    m_logView->setObjectName(QStringLiteral("logView"));
    m_logView->setReadOnly(true);
    m_logView->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_logView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_logView->setMinimumHeight(84);
    m_logView->setMaximumHeight(150);
    m_logView->setMaximumBlockCount(200);
    m_logView->setPlaceholderText(QString::fromUtf8(u8"\u5DE5\u5177\u7684\u8F93\u51FA\u4F1A\u663E\u793A\u5728\u8FD9\u91CC"));
    logLayout->addWidget(m_logView);

    // 输出节流：见 onRunnerOutput 的说明。
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setSingleShot(true);
    m_logFlushTimer->setInterval(80);
    connect(m_logFlushTimer, &QTimer::timeout, this, &ToolPanel::flushLog);

    // 整页内容放进滚动区，和同步页保持一致。
    // 工具页现在有三块内容（规则表 / 文件表 / 输出），不加滚动区的话，
    // 窗口缩到最小高度时 QVBoxLayout 会从**所有**可收缩控件里扣高度，
    // 表格会被压成几行而用户看不全 —— 和 P36 是同一个成因。
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("pageScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    // 横向按需，理由与 SyncPanel 同一处：窗口下限被窄屏压低时，
    // 永远关掉的横向滚动条会让内容右边一截够不着。
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    auto *content = new QWidget(scroll);
    content->setObjectName(QStringLiteral("pageBg"));
    auto *root = new QVBoxLayout(content);
    root->setContentsMargins(18, 18, 18, 16);
    root->setSpacing(16);
    root->addLayout(pageHeader);
    root->addWidget(ruleCard, 2);
    root->addWidget(fileCard, 3);
    root->addWidget(logCard);
    scroll->setWidget(content);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);
    outer->addWidget(scroll);

    setStatus(QString::fromUtf8(u8"\u5C31\u7EEA"), false);

    const QVector<ToolRule> rules = ConfigStore::loadToolRules();
    for (const ToolRule &r : rules) {
        const int row = m_ruleTable->rowCount();
        m_ruleTable->insertRow(row);
        m_ruleTable->setItem(row, ToolSuffix, new QTableWidgetItem(r.suffix));
        m_ruleTable->setItem(row, ToolExe, new QTableWidgetItem(r.exePath));
        m_ruleTable->setItem(row, ToolArgs, new QTableWidgetItem(r.argsTemplate));
    }

    connect(m_addRule, &QPushButton::clicked, this, &ToolPanel::addRule);
    connect(m_delRule, &QPushButton::clicked, this, &ToolPanel::removeRule);
    connect(m_ruleTable, &QTableWidget::cellDoubleClicked, this, &ToolPanel::browseRuleExe);
    connect(m_btnPick, &QPushButton::clicked, this, &ToolPanel::pickFiles);
    connect(m_btnRun,  &QPushButton::clicked, this, &ToolPanel::runTool);
    connect(m_btnStop, &QPushButton::clicked, this, &ToolPanel::stopTool);
}

int ToolPanel::contentMinWidth() const
{
    QScrollArea *sa = findChild<QScrollArea *>(QStringLiteral("pageScroll"));
    return (sa && sa->widget()) ? sa->widget()->minimumSizeHint().width() : 0;
}

void ToolPanel::saveConfig()
{
    ConfigStore::saveToolRules(collectRules());
}

void ToolPanel::replaceRuleForTest(const QString &suffix, const QString &exe, const QString &args)
{
    m_ruleTable->setRowCount(0);
    const int row = 0;
    m_ruleTable->insertRow(row);
    m_ruleTable->setItem(row, ToolSuffix, new QTableWidgetItem(suffix));
    m_ruleTable->setItem(row, ToolExe,   new QTableWidgetItem(exe));
    m_ruleTable->setItem(row, ToolArgs,  new QTableWidgetItem(args));
}

QString ToolPanel::statusTextForTest() const
{
    return m_statusLabel->text();
}

bool ToolPanel::runButtonEnabledForTest() const
{
    return m_btnRun->isEnabled();
}

void ToolPanel::setStatus(const QString &text, bool error)
{
    const char *style = error
        ? "background:#fef2f2; color:#dc2626; border:1px solid #fbcfcf;"
          " border-radius:11px; padding:4px 14px; font-size:12px; font-weight:500;"
        : "background:#f1f3f7; color:#5b6272; border:1px solid #e6e8ee;"
          " border-radius:11px; padding:4px 14px; font-size:12px; font-weight:500;";
    m_statusLabel->setText(text);
    m_statusLabel->setStyleSheet(QString::fromLatin1(style));
}

void ToolPanel::browseRuleExe(int row, int column)
{
    if (column != ToolExe)
        return;
    QTableWidgetItem *item = m_ruleTable->item(row, ToolExe);
    const QString cur = item ? item->text() : QString();
    const QString path = QFileDialog::getOpenFileName(
        this, QString::fromUtf8(u8"\u9009\u62E9\u672C\u5730\u5DE5\u5177\u53EF\u6267\u884C\u6587\u4EF6"), cur);
    if (path.isEmpty())
        return;
    if (!item) {
        m_ruleTable->setItem(row, ToolExe, new QTableWidgetItem);
        item = m_ruleTable->item(row, ToolExe);
    }
    item->setText(path);
}

void ToolPanel::addRule()
{
    const int row = m_ruleTable->rowCount();
    m_ruleTable->insertRow(row);
    m_ruleTable->setItem(row, ToolSuffix, new QTableWidgetItem(QStringLiteral("spe")));
    m_ruleTable->setItem(row, ToolExe, new QTableWidgetItem);
    m_ruleTable->setItem(row, ToolArgs, new QTableWidgetItem);
}

void ToolPanel::removeRule()
{
    const int row = m_ruleTable->currentRow();
    if (row >= 0)
        m_ruleTable->removeRow(row);
}

QVector<ToolRule> ToolPanel::collectRules() const
{
    QVector<ToolRule> rules;
    for (int row = 0; row < m_ruleTable->rowCount(); ++row) {
        const QTableWidgetItem *s = m_ruleTable->item(row, ToolSuffix);
        const QTableWidgetItem *e = m_ruleTable->item(row, ToolExe);
        const QTableWidgetItem *a = m_ruleTable->item(row, ToolArgs);
        if (!s || !e || e->text().trimmed().isEmpty())
            continue;
        ToolRule r;
        r.suffix       = s->text().trimmed().toLower();
        r.exePath      = e->text().trimmed();
        r.argsTemplate = a ? a->text() : QString();
        rules.append(r);
    }
    return rules;
}

void ToolPanel::pickFiles()
{
    const QStringList files = QFileDialog::getOpenFileNames(
        this, QString::fromUtf8(u8"\u9009\u62E9\u672C\u5730\u6570\u636E\u6587\u4EF6"));
    if (files.isEmpty())
        return;

    m_selectedFiles = files;
    if (files.size() == 1) {
        m_pendingFiles->setText(QDir::toNativeSeparators(files.first()));
    } else {
        m_pendingFiles->setText(QString::fromUtf8(u8"\u5DF2\u9009\u4E2D %1 \u4E2A\u6587\u4EF6").arg(files.size()));
    }
    refreshFileList();
}

void ToolPanel::refreshFileList()
{
    m_fileTable->setRowCount(0);
    for (const QString &f : m_selectedFiles) {
        const int row = m_fileTable->rowCount();
        m_fileTable->insertRow(row);
        const QFileInfo fi(f);
        m_fileTable->setItem(row, SelFileName, new QTableWidgetItem(QDir::toNativeSeparators(f)));
        m_fileTable->setItem(row, SelFileSuffix, new QTableWidgetItem(fi.suffix()));
        m_fileTable->setItem(row, SelFileSize, new QTableWidgetItem(humanSize(fi.size())));
    }
    // 工具在跑时不能重新使能「用匹配工具打开」：runTool() 里的 delete m_runner
    // 会连带析构正在运行的 QProcess，把第三方回放程序直接杀掉、结果文件写坏。
    updateRunButtons();
}

bool ToolPanel::isToolRunning() const
{
    // 队列串行执行时，两个进程之间 m_runner->isRunning() 是 false，
    // 只看它会在这个空隙里把「用匹配工具打开」重新使能。
    return (m_runner && m_runner->isRunning()) || !m_queue.isEmpty();
}

void ToolPanel::updateRunButtons()
{
    const bool busy = isToolRunning();
    m_btnRun->setEnabled(!m_selectedFiles.isEmpty() && !busy);
    m_btnStop->setVisible(busy);
    m_btnStop->setEnabled(busy);
    // 回放运行中不允许换文件集合。原来的坑：运行中点「选择文件」，
    // 表格和「已选中 N 个文件」当场换成新的一批，而这轮跑的还是旧队列，
    // 界面显示的和正在跑的对不上；而且 getOpenFileNames() 会在工具进程
    // 还活着的时候开出一个嵌套事件循环。
    m_btnPick->setEnabled(!busy);
}

void ToolPanel::runTool()
{
    if (isToolRunning()) {
        setStatus(QString::fromUtf8(u8"\u4E0A\u4E00\u4E2A\u5DE5\u5177\u8FD8\u5728\u8FD0\u884C"), true);
        appendLog(QString::fromUtf8(u8"\u4E0A\u4E00\u4E2A\u5DE5\u5177\u8FD8\u5728\u8FD0\u884C\u3002"));
        return;
    }

    saveConfig();

    const QVector<ToolRule> rules = collectRules();
    if (rules.isEmpty()) {
        setStatus(QString::fromUtf8(u8"\u672A\u914D\u7F6E\u5DE5\u5177\u89C4\u5219"), true);
        appendLog(QString::fromUtf8(u8"\u5DE5\u5177\u8C03\u7528\u89C4\u5219\u6570\u4E3A 0\u3002"));
        return;
    }

    QStringList files;
    const QItemSelectionModel *sel = m_fileTable->selectionModel();
    if (sel && sel->hasSelection()) {
        for (const QModelIndex &idx : sel->selectedRows())
            files.append(m_selectedFiles.value(idx.row()));
    }
    if (files.isEmpty())
        files = m_selectedFiles;
    if (files.isEmpty()) {
        setStatus(QString::fromUtf8(u8"\u672A\u9009\u62E9\u6587\u4EF6"), true);
        appendLog(QString::fromUtf8(u8"\u672A\u9009\u62E9\u5F85\u56DE\u653E\u7684\u6570\u636E\u6587\u4EF6\u3002"));
        return;
    }

    const QVector<ToolRule> matched = ToolRunner::rulesFor(rules, QFileInfo(files.first()).suffix());
    if (matched.isEmpty()) {
        setStatus(QString::fromUtf8(u8"\u540E\u7F00\u672A\u914D\u7F6E\u5DE5\u5177"), true);
        appendLog(QString::fromUtf8(u8"\u540E\u7F00 .%1 \u5C1A\u672A\u914D\u7F6E\u5BF9\u5E94\u7684\u6267\u884C\u5DE5\u5177\u3002")
                  .arg(QFileInfo(files.first()).suffix()));
        return;
    }

    ToolRule rule = matched.first();
    if (matched.size() > 1) {
        QStringList labels;
        for (const ToolRule &r : matched)
            labels << QFileInfo(r.exePath).fileName();
        bool ok = false;
        const QString choice = QInputDialog::getItem(
            this, QString::fromUtf8(u8"\u9009\u62E9\u542F\u52A8\u5DE5\u5177"),
            QString::fromUtf8(u8"\u8BE5\u540E\u7F00\u5339\u914D\u4E86\u591A\u4E2A\u5DE5\u5177\uFF0C\u8BF7\u9009\u62E9\uFF1A"), labels, 0, false, &ok);
        if (!ok || choice.isEmpty())
            return;
        rule = matched.at(labels.indexOf(choice));
    }

    if (!QFileInfo(rule.exePath).isFile()) {
        setStatus(QString::fromUtf8(u8"\u5DE5\u5177\u7A0B\u5E8F\u4E0D\u5B58\u5728"), true);
        appendLog(QString::fromUtf8(u8"\u6307\u5B9A\u7684\u5DE5\u5177\u7A0B\u5E8F\u4E0D\u5B58\u5728: %1").arg(rule.exePath));
        return;
    }

    // 工具是按首个文件的后缀定的。选中区里混了别的后缀时，把全部文件都丢给
    // 同一个工具，第三方回放程序可能直接把结果文件写坏 —— 问清楚再跑。
    QStringList runnable;
    for (const QString &f : files) {
        if (QFileInfo(f).suffix().compare(rule.suffix, Qt::CaseInsensitive) == 0)
            runnable.append(f);
    }
    if (files.size() > runnable.size()) {
        QMessageBox box(QMessageBox::Warning, QString::fromUtf8(u8"\u63D0\u793A"),
                        QString::fromUtf8(u8"\u9009\u4E2D\u7684 %1 \u4E2A\u6587\u4EF6\u4E2D\uFF0C\u6709 %2 \u4E2A\u7684\u540E\u7F00\u4E0E\u6240\u9009\u5DE5\u5177\u4E0D\u5339\u914D\u3002")
                            .arg(files.size()).arg(files.size() - runnable.size()),
                        QMessageBox::NoButton, this);
        QPushButton *onlyMatched = box.addButton(
            QString::fromUtf8(u8"\u53EA\u5904\u7406\u5339\u914D\u7684 %1 \u4E2A").arg(runnable.size()),
            QMessageBox::AcceptRole);
        box.addButton(QString::fromUtf8(u8"\u4ECD\u7136\u5168\u90E8\u5904\u7406"), QMessageBox::DestructiveRole);
        box.setDefaultButton(onlyMatched);
        box.exec();
        // 两个按钮的结果以前是接反的：点「只处理匹配的 N 个」（也是默认按钮，
        // 直接回车走的就是它）会把 files 原样留成全部选中项，
        // 正好把这个对话框要防的「拿不匹配后缀的文件去喂第三方工具」照做一遍。
        //
        // 关窗（Esc / 右上角 ×）时 clickedButton() 是 nullptr。
        // 那不是"仍然全部处理"，必须整个放弃本次运行 ——
        // 用户在警告框上点掉它，语义是"我不想跑了"，不是"我确认全都跑"。
        QAbstractButton *clicked = box.clickedButton();
        if (!clicked)
            return;
        if (clicked == onlyMatched)
            files = runnable;
    }

    delete m_runner;
    m_runner = new ToolRunner(this);
    connect(m_runner, &ToolRunner::output, this, &ToolPanel::onRunnerOutput);
    connect(m_runner, &ToolRunner::finished, this, &ToolPanel::onRunnerFinished);

    // 选中的（未选中则全部）逐个串行跑：ToolRunner 一次只管一个进程，
    // 由 onRunnerFinished() 在上一个进程结束后启动下一个。
    m_activeRule      = rule;
    m_queue           = files;
    m_queueTotal      = files.size();
    m_queueFailures.clear();
    m_cancelRequested = false;
    // 每一轮开始前清空输出区：不清的话第二次运行的内容会接在上一轮后面，
    // 用户分不清哪一段是刚才这次跑的。失败弹窗用的是 m_output，本来就是按轮清。
    m_logView->clear();
    m_pendingLog.clear();

    startNextQueuedFile();
}

void ToolPanel::startNextQueuedFile()
{
    if (m_queue.isEmpty())
        return;

    m_queueCurrent = m_queue.takeFirst();
    m_output.clear();
    if (m_queueTotal > 1) {
        setStatus(QString::fromUtf8(u8"\u5DE5\u5177\u8FD0\u884C\u4E2D... (%1/%2)")
                      .arg(m_queueTotal - m_queue.size()).arg(m_queueTotal), false);
    } else {
        setStatus(QString::fromUtf8(u8"\u5DE5\u5177\u8FD0\u884C\u4E2D..."), false);
    }
    // 多文件时给每个文件的输出加个分隔行，否则三段输出连在一起，
    // 用户分不清哪段是哪个文件的结果。
    if (m_queueTotal > 1) {
        // 上一段如果没以换行收尾（工具自己没写 \n、或上一轮被取消），
        // 先补一个再写分隔行，否则这里会粘在上一行的尾巴上。
        QTextCursor sep = m_logView->textCursor();
        sep.movePosition(QTextCursor::End);
        sep.insertText(QStringLiteral("\n"));
        appendLog(QString::fromUtf8(u8"\u2500\u2500 %1/%2  %3")
                      .arg(m_queueTotal - m_queue.size())
                      .arg(m_queueTotal)
                      .arg(QDir::toNativeSeparators(m_queueCurrent)));
        appendLog(QStringLiteral("\n"));
    }
    // updateRunButtons() 必须在 m_runner->start() **之后**调。
    //
    // isToolRunning() 是 (m_runner->isRunning() || !m_queue.isEmpty())，
    // 而 ToolRunner::isRunning() 就是 m_proc != nullptr —— start() 之前它恒为 false。
    // 于是在 start() 之前调用时：
    //   选 1 个文件 -> takeFirst() 把队列取空 -> 两个条件都为 false -> busy=false
    //   ->「运行」保持可点、「停止」保持隐藏，而且**整个运行期间都不会再被纠正**
    //      （下一次 updateRunButtons() 要等工具结束）。
    // 后果：单文件跑工具时用户按不到「停止」，只能干等；再点一次「运行」
    // 会弹出"上一个工具还在运行"，看起来像按钮坏了。
    // 选 2 个以上文件时队列非空，busy 恰好为 true，所以这个 bug 只在单文件时露出来。
    m_runner->start(m_activeRule.exePath, ToolRunner::buildArgs(m_activeRule, m_queueCurrent));
    updateRunButtons();
}

void ToolPanel::stopTool()
{
    if (!isToolRunning())
        return;

    m_cancelRequested = true;
    m_btnStop->setEnabled(false);
    setStatus(QString::fromUtf8(u8"\u6B63\u5728\u505C\u6B62..."), false);

    if (m_runner && m_runner->isRunning())
        m_runner->cancel();

    // cancel() 里的 waitForFinished() 会同步触发 finished()，队列可能已经收尾；
    // 若此刻没有进程可停（正好卡在两个文件的间隙），这里补一次收尾。
    if (m_cancelRequested)
        finishQueue(true);
}

void ToolPanel::finishQueue(bool cancelled)
{
    // 幂等：同一轮只允许收一次尾。
    // stopTool() 里不等进程真的退出就先收尾（terminate 是异步的），
    // 工具随后发来的那个 finished() 会再走到这里一次；那时队列已空、
    // 取消标志已复位，不挡一下就会弹第二次框，还报成"共 0 个文件，1 个失败"。
    //
    // 但**不能直接 return**：上一次收尾发生在进程还没退出的时刻，
    // updateRunButtons() 当时看到 isToolRunning()==true，把「用匹配工具打开」
    // 置成了禁用。等工具真的被杀、这个迟到的 finished() 到达时，
    // 进程状态已经变了，必须在这里再刷一次按钮 ——
    // 否则那个按钮会一直禁用到用户重启程序为止。
    if (m_queueTotal <= 0) {
        updateRunButtons();
        return;
    }

    const int total     = m_queueTotal;
    const int failed    = m_queueFailures.size();
    const int remaining = m_queue.size();

    m_queue.clear();
    m_queueCurrent.clear();
    m_cancelRequested = false;

    if (cancelled) {
        setStatus(remaining > 0
                      ? QString::fromUtf8(u8"\u5DF2\u53D6\u6D88\uFF0C\u5269\u4F59 %1 \u4E2A\u6587\u4EF6\u672A\u5904\u7406").arg(remaining)
                      : QString::fromUtf8(u8"\u5DF2\u53D6\u6D88"), true);
    } else if (failed == 0) {
        setStatus(total > 1
                      ? QString::fromUtf8(u8"\u6267\u884C\u5B8C\u6210\uFF1A%1 \u4E2A\u6587\u4EF6\u5168\u90E8\u5B8C\u6210").arg(total)
                      : QString::fromUtf8(u8"\u6267\u884C\u5B8C\u6210"), false);
    } else {
        setStatus(QString::fromUtf8(u8"\u6267\u884C\u5B8C\u6210\uFF1A\u5171 %1 \u4E2A\uFF0C%2 \u4E2A\u5931\u8D25")
                      .arg(total).arg(failed), true);
    }

    // 一次失败弹一个框会把用户淹没；攒起来弹一个大框又装不下（每条失败都带着
    // 第三方程序的一整段输出）。全部进日志：芯片已标"N 个失败"，日志按时间摊开。
    if (failed > 0) {
        appendLog(QString::fromUtf8(u8"\u5171 %1 \u4E2A\u6587\u4EF6\uFF0C%2 \u4E2A\u5931\u8D25\uFF1A")
                  .arg(total).arg(failed));
        for (const QString &f : m_queueFailures)
            appendLog(f);
    }

    m_queueFailures.clear();
    m_queueTotal = 0;
    flushLog();          // 收尾时立刻补刷，别让用户看到少尾巴的日志
    updateRunButtons();
}

void ToolPanel::onRunnerOutput(const QString &text)
{
    m_output.append(text);
    if (m_output.size() > 4000)
        m_output.remove(0, m_output.size() - 4000);

    // 不能每来一块就直接往控件里插。
    //
    // QPlainTextEdit 开着按宽度换行时，每次插入都要给整篇内容重算换行布局，
    // 于是复杂度是 O(n²)。第三方工具一次吐 8MB 时实测把界面线程占满
    // 196 秒 CPU —— 期间界面完全无响应，"停止"按钮也点不动。
    // 探针 toolreplayprobe 的"输出风暴"用例就是拿这个量级写的。
    //
    // 改成先攒着、每 80ms 刷一次：一轮风暴最多刷 12 次，
    // 代价与输出总量脱钩，只跟时间有关。
    m_pendingLog.append(text);
    // 兜底上限：工具持续疯狂输出时别把 pending 自己撑爆
    if (m_pendingLog.size() > 100000)
        m_pendingLog.remove(0, m_pendingLog.size() - 100000);
    if (!m_logFlushTimer->isActive())
        m_logFlushTimer->start();
}

void ToolPanel::flushLog()
{
    if (!m_pendingLog.isEmpty()) {
        appendLog(m_pendingLog);
        m_pendingLog.clear();
    }

    // 还得按**字符数**裁一刀，而且必须放在"没有待刷内容"这个判断**之后**，
    // 不能因为本轮没新内容就跳过 —— 定时器刷完一次之后下一轮 pending 是空的，
    // 那一次的裁剪就被跳过了，最后留在控件里的是上一轮没裁干净的量。
    //
    // setMaximumBlockCount(200) 只管行数，管不住行有多长：工具一次吐 8MB 而
    // 中间没有换行的话整篇就是一个 block，200 行的上限等于没有上限，
    // 而按宽度换行的布局开销是 O(n)，界面线程照样被占满。
    // 实测只靠节流 + 行数上限，8MB 输出仍能把探针 CPU 跑到 80 秒以上。
    QTextDocument *doc = m_logView->document();
    const int kMaxChars = 40000;
    if (doc->characterCount() <= kMaxChars)
        return;

    QTextCursor trim(doc);
    // 从第一块之后开始删，保留最前面一行，让人看得出"日志被截断了"
    QTextBlock first = doc->begin();
    if (!first.isValid() || !first.next().isValid())
        return;
    trim.setPosition(first.next().position());
    trim.setPosition(doc->characterCount() - kMaxChars, QTextCursor::KeepAnchor);
    trim.removeSelectedText();
}

// 输出区只能**续写**，不能每来一块就另起一行。
//
// QPlainTextEdit::appendPlainText() 每调用一次就新开一个 block。而解码器在
// 管道把一行拆开时是逐段吐出来的（QProcess::readyRead 攒多少给多少），
// 于是一行中文被切成 22 个 block，界面上变成 22 行、每行一个字，日志没法读。
// 这不是"极端情况"：第三方工具一行一行 flush 自己的输出时，
// 只要一次 readyRead 落在行中间就会触发。
//
// 所以改成把光标移到末尾 insertText：文本里自带 \n 才换行，没有就接着写。
//
// 注意这里**不能**加"上一块没以换行结尾就补一个换行"的自作聪明：
// 同一行被管道拆成好几块时，每块都不带换行，补一次就是一行一个字 ——
// 写完第一版正是这样，探针直接量出一行一个字。
// 换行只该在**换文件**时补（见 startNextQueuedFile）。
void ToolPanel::appendLog(const QString &text)
{
    if (text.isEmpty())
        return;

    // 用户手动往上翻历史的时候不要把他拽回底部，只有本来就在底部才跟随滚动。
    QScrollBar *bar = m_logView->verticalScrollBar();
    const bool wasAtBottom = bar->value() >= bar->maximum() - 2;

    QTextCursor c = m_logView->textCursor();
    c.movePosition(QTextCursor::End);
    c.insertText(text);

    if (wasAtBottom)
        bar->setValue(bar->maximum());
}

void ToolPanel::onRunnerFinished(int exitCode)
{
    // "是不是被取消"必须同时看界面和进程两个来源。
    // 界面在 stopTool() 里立刻就收尾复位了，可 terminate() 是异步的，
    // 工具完全可能过一会儿才真的退出、才发出这个迟到的 finished()。
    // 只看界面那个标志的话，这次延迟结束会被当成一轮新的失败，
    // 凭空多记一条并再弹一次框（"共 0 个文件，1 个失败"）。
    const bool canceled = m_cancelRequested || (m_runner && m_runner->wasCanceled());

    if (exitCode != 0 && !canceled) {
        QString entry = QDir::toNativeSeparators(m_queueCurrent)
                      + QString::fromUtf8(u8": \u9000\u51FA\u7801 ") + QString::number(exitCode);
        const QString tail = m_output.trimmed();
        if (!tail.isEmpty())
            entry += QLatin1Char('\n') + (tail.size() > 2000 ? tail.right(2000) : tail);
        m_queueFailures.append(entry);
        ++m_totalFailures;
    }

    if (canceled) {
        finishQueue(true);
        return;
    }
    if (!m_queue.isEmpty()) {
        startNextQueuedFile();
        return;
    }
    finishQueue(false);
}
