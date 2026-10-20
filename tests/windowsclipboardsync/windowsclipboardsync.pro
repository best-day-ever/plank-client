QT += core gui testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
INCLUDEPATH += ../../../../protocol/plank-transport/include ../../app/streaming ../../moonlight-common-c/moonlight-common-c/src ../../libs/windows/include ../../libs/windows/include/x64
LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL3 -luser32
SOURCES += test_windowsclipboardsync.cpp ../../app/streaming/clipboardsync.cpp ../../app/streaming/windowsclipboard.cpp
HEADERS += ../../app/streaming/clipboardsync.h ../../app/streaming/clipboardplatform.h
