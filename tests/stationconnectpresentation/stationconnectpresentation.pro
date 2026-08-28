QT += core testlib
CONFIG += testcase c++17
TEMPLATE = app

SOURCES += \
    test_stationconnectpresentation.cpp \
    ../../app/streaming/stationconnectpresentation.cpp

HEADERS += ../../app/streaming/stationconnectpresentation.h

INCLUDEPATH += ../../app

PKGCONFIG += sdl3
CONFIG += link_pkgconfig
