QT       += core gui widgets
greaterThan(QT_MAJOR_VERSION, 4)

# ---- Qt 版本闸门（2026-11-02 补，原 N2）----
#
# 代码里用了 QTextCodec、QDesktopWidget、QFontDatabase::families() 的非静态调用。
# 这三样在 Qt 6 里要么被删、要么改了签名 —— 换一台机器装 Qt 6 会在编译**中途**
# 炸掉，而且报错信息跟"版本不对"八竿子打不着，很容易被误判成代码写错、
# 越改越乱。这里直接拦住：不是 Qt 5 一律不给编。
#
# 已验证基线：Qt 5.9.6（Windows msvc2015_64 / 麒麟容器 bionic 5.9.5 头 + 5.9.6 运行时）。
greaterThan(QT_MAJOR_VERSION, 5) {
    error("本项目锁定 Qt 5.9.x（当前 QT_VERSION = $$QT_VERSION）。Qt 6 移除了 QTextCodec 等本项目在用的 API，编译必然失败且报错难懂。")
}
lessThan(QT_MAJOR_VERSION, 5) {
    error("本项目需要 Qt 5.9.x（当前 QT_VERSION = $$QT_VERSION），Qt 4 不支持。")
}

TARGET    = SyncReplayTool
TEMPLATE  = app
CONFIG   += c++11

# .pro 的来历（写清楚，别再被当成"原始 .pro"）：
#   2026-10-19 重建过一次 —— 当时从产品目录读到的 .pro 没有随源码树保留下来，
#   这一份是按当时的意图重建的。**自 10-19 起所有构建（Windows / 麒麟容器 /
#   交付包）用的都是这一份**，它就是事实上的构建依据，此后的每一次构建与
#   验收都在替它背书。
#   ⚠️ 若日后拿到原始 .pro：**先比对本文件再替换**，直接覆盖可能丢掉下面这几行
#   按事故加上去的开关（/utf-8、/MANIFEST:NO、-ldl、Qt 版本闸门）。
#
# Source files are UTF-8 WITH BOM; /utf-8 makes MSVC store
# narrow literals as UTF-8 instead of the system ANSI code page (936/GBK),
# otherwise every CJK string in the UI renders as mojibake.
win32-msvc*: QMAKE_CXXFLAGS += /utf-8 -wd4819

# rc.exe manifest embedding fails with LNK1158 when several Windows SDKs are
# in PATH; the manifest only affects UAC virtualization on XP/Vista.
win32-msvc*: CONFIG -= embed_manifest_exe embed_manifest_dll
win32-msvc*: QMAKE_LFLAGS += /MANIFEST:NO

SOURCES += \
    src/main.cpp \
    src/ConsoleOutput.cpp \
    src/ConfigStore.cpp \
    src/SecretStore.cpp \
    src/FileScanner.cpp \
    src/RsyncRunner.cpp \
    src/ToolRunner.cpp \
    src/FileTableModel.cpp \
    src/ToolColumnDelegate.cpp \
    src/StatusPillDelegate.cpp \
    src/SettingsDialog.cpp \
    src/SyncPanel.cpp \
    src/ToolPanel.cpp \
    src/AutoSyncScheduler.cpp \
    src/MainWindow.cpp

HEADERS += \
    src/Model.h \
    src/AppVersion.h \
    src/ConsoleOutput.h \
    src/ConfigStore.h \
    src/SecretStore.h \
    src/FileScanner.h \
    src/RsyncRunner.h \
    src/ToolRunner.h \
    src/FileTableModel.h \
    src/ToolColumnDelegate.h \
    src/StatusPillDelegate.h \
    src/SettingsDialog.h \
    src/SyncPanel.h \
    src/ToolPanel.h \
    src/AutoSyncScheduler.h \
    src/MainWindow.h \
    src/ScreenFit.h \
    src/Theme.h

win32:LIBS += -lcrypt32

# SecretStore 用 dlopen 在 libcrypto.so.3 / .1.1 / .1.0.0 / libcrypto.so 之间挑，
# 不链接任何 OpenSSL —— 这是刻意的：把它链进主程序，NEEDED 里就会焊死一个
# 具体版本号，客户机上没有那个文件就直接起不来。
#
# 交付包里实际带的是 lib/sshv2/libcrypto.so.3（OpenSSL 3，5.5MB），
# 由 SecretStore 在运行时按顺序挑，不依赖主程序链接。
#
# 原先没有下面这一行，Linux 上直接链接失败：
#   undefined reference to `dlclose' / `dlopen' / `dlsym'（来自 SecretStore.o）
unix:LIBS += -ldl

# 曾经在这里写 `unix:LIBS += -lcrypto`。实测没有任何 qmake 赋值能把它删掉，
# 而"客户机上跑得起来"完全依赖链接器默认带 --as-needed（Ubuntu 有，
# 麒麟 / CentOS / 部分交叉工具链不一定有）。一旦不带，它就会作为 NEEDED
# 焊进 ELF，指向本机根本不存在的 libcrypto.so.1.0.0，客户机上直接起不来。
#
# 这道 NEEDED 断言放在 **scripts/build-kylin-qt596.sh** 的第 7 步
#（那才是仓库里真正的 Linux 构建脚本）。
# 早先这里写的"linux-build.sh"从来就不存在 —— 闸门等于没有，
# 已于 2026-11-02 补上（原 N3）。
