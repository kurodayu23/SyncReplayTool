QT       += core gui widgets
greaterThan(QT_MAJOR_VERSION, 4)

TARGET    = SyncReplayTool
TEMPLATE  = app
CONFIG   += c++11

# Reconstructed .pro -- identical in intent to the one read from the product
# tree at 18:38.  Source files are UTF-8 WITH BOM; /utf-8 makes MSVC store
# narrow literals as UTF-8 instead of the system ANSI code page (936/GBK),
# otherwise every CJK string in the UI renders as mojibake.
win32-msvc*: QMAKE_CXXFLAGS += /utf-8 -wd4819

# rc.exe manifest embedding fails with LNK1158 when several Windows SDKs are
# in PATH; the manifest only affects UAC virtualization on XP/Vista.
win32-msvc*: CONFIG -= embed_manifest_exe embed_manifest_dll
win32-msvc*: QMAKE_LFLAGS += /MANIFEST:NO

SOURCES += \
    src/main.cpp \
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
    src/Theme.h

win32:LIBS += -lcrypt32

# SecretStore 用 dlopen 在 libcrypto.so.3 / .1.1 / .1.0.0 / libcrypto.so 之间挑，
# 不链接任何 OpenSSL —— 这是刻意的：交付包自带的是 2019 年 EOL 的 OpenSSL 1.0.2g，
# 把它链进主程序会让口令加密悄悄退回旧库。所以这里只需要 dlopen 所在的 libdl。
#
# 原先没有这一行，Linux 上直接链接失败：
#   undefined reference to `dlclose' / `dlopen' / `dlsym'（来自 SecretStore.o）
unix:LIBS += -ldl

# 曾经在这里写 `unix:LIBS += -lcrypto`。实测没有任何 qmake 赋值能把它删掉，
# 而"客户机上跑得起来"完全依赖链接器默认带 --as-needed（Ubuntu 有，
# 麒麟 / CentOS / 部分交叉工具链不一定有）。一旦不带，它就会作为 NEEDED
# 焊进 ELF，指向本机根本不存在的 libcrypto.so.1.0.0，客户机上直接起不来。
# 这里的 NEEDED 断言放在 linux-build.sh 里。
