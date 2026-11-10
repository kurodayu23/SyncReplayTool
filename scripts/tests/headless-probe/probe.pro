QT       += core gui widgets
greaterThan(QT_MAJOR_VERSION, 5)

TARGET    = srtprobe
TEMPLATE  = app
CONFIG   += c++11 console

# repo root = scripts/tests/headless-probe/../../..
SRCDIR = $$clean_path($$PWD/../../..)

INCLUDEPATH += $$SRCDIR/src

win32-msvc*: QMAKE_CXXFLAGS += /utf-8 -wd4819
win32-msvc*: CONFIG -= embed_manifest_exe embed_manifest_dll
win32-msvc*: QMAKE_LFLAGS += /MANIFEST:NO

SOURCES += \
    probe.cpp \
    $$SRCDIR/src/ConsoleOutput.cpp \
    $$SRCDIR/src/ConfigStore.cpp \
    $$SRCDIR/src/SecretStore.cpp \
    $$SRCDIR/src/FileScanner.cpp \
    $$SRCDIR/src/RsyncRunner.cpp \
    $$SRCDIR/src/ToolRunner.cpp \
    $$SRCDIR/src/FileTableModel.cpp \
    $$SRCDIR/src/ToolColumnDelegate.cpp \
    $$SRCDIR/src/StatusPillDelegate.cpp \
    $$SRCDIR/src/SettingsDialog.cpp \
    $$SRCDIR/src/SyncPanel.cpp \
    $$SRCDIR/src/ToolPanel.cpp \
    $$SRCDIR/src/AutoSyncScheduler.cpp \
    $$SRCDIR/src/MainWindow.cpp

HEADERS += \
    $$SRCDIR/src/Model.h \
    $$SRCDIR/src/AppVersion.h \
    $$SRCDIR/src/ConsoleOutput.h \
    $$SRCDIR/src/ConfigStore.h \
    $$SRCDIR/src/SecretStore.h \
    $$SRCDIR/src/FileScanner.h \
    $$SRCDIR/src/RsyncRunner.h \
    $$SRCDIR/src/ToolRunner.h \
    $$SRCDIR/src/FileTableModel.h \
    $$SRCDIR/src/ToolColumnDelegate.h \
    $$SRCDIR/src/StatusPillDelegate.h \
    $$SRCDIR/src/SettingsDialog.h \
    $$SRCDIR/src/SyncPanel.h \
    $$SRCDIR/src/ToolPanel.h \
    $$SRCDIR/src/AutoSyncScheduler.h \
    $$SRCDIR/src/MainWindow.h \
    $$SRCDIR/src/ScreenFit.h \
    $$SRCDIR/src/Theme.h

win32:LIBS += -lcrypt32
unix:LIBS += -ldl
