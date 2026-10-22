QT += core testlib
QT -= gui
CONFIG += console testcase c++17
TEMPLATE = app
isEmpty(PLANK_PROTOCOL_INCLUDE): PLANK_PROTOCOL_INCLUDE = $$PWD/../../../../protocol/plank-transport/include
INCLUDEPATH += $$PLANK_PROTOCOL_INCLUDE ../../app/streaming
SOURCES += test_windowsfileclipboard.cpp
win32 {
    LIBS += -ladvapi32 -lshell32 -luser32
}
