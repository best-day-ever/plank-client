QT += core gui testlib
CONFIG += console testcase c++17 link_pkgconfig
TEMPLATE = app

PKGCONFIG += sdl3
LIBS += -framework AppKit

INCLUDEPATH += \
    ../../app/streaming \
    ../../moonlight-common-c/moonlight-common-c/src

SOURCES += \
    test_macclipboardsync.mm \
    ../../app/streaming/macclipboardsync.mm

HEADERS += \
    ../../app/streaming/macclipboardsync.h \
    ../../app/streaming/plankclipboard.h
