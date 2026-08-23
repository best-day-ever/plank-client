QT += core testlib
CONFIG += console testcase c++11
TEMPLATE = app

SOURCES += test_stationconnectnetwork.cpp

HEADERS += \
    ../../app/backend/stationconnectnetwork.h \
    ../../app/streaming/stationconnectpacketsize.h

INCLUDEPATH += \
    ../../app/backend \
    ../../app/streaming
