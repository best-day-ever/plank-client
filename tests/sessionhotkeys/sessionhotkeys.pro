QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
win32 {
    INCLUDEPATH += ../../libs/windows/include ../../libs/windows/include/x64
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -lSDL3
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl3
}
SOURCES += test_sessionhotkeys.cpp
HEADERS += ../../app/streaming/planksessionhotkeys.h
