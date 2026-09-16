QT += core testlib
CONFIG += testcase c++17
TEMPLATE = app

SOURCES += \
    test_plankpresentation.cpp \
    ../../app/streaming/plankpresentation.cpp

HEADERS += ../../app/streaming/plankpresentation.h \
    ../../app/streaming/input/plankmousemotion.h \
    ../../app/streaming/input/plankpointerlogic.h

INCLUDEPATH += ../../app

PKGCONFIG += sdl3
CONFIG += link_pkgconfig
