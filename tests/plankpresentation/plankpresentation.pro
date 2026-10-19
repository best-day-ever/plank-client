QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
win32 {
    INCLUDEPATH += $$PWD/../../libs/windows/include $$PWD/../../libs/windows/include/x64
    LIBS += $$PWD/../../libs/windows/lib/x64/SDL3.lib
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += sdl3
}

SOURCES += \
    test_plankpresentation.cpp \
    ../../app/streaming/plankpresentation.cpp

HEADERS += ../../app/streaming/plankpresentation.h \
    ../../app/streaming/input/plankmousemotion.h \
    ../../app/streaming/input/plankpointerlogic.h

INCLUDEPATH += ../../app
