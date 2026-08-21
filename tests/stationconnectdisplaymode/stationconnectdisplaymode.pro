QT += core testlib
CONFIG += console testcase c++11
TEMPLATE = app

SOURCES += \
    test_stationconnectdisplaymode.cpp \
    ../../app/streaming/stationconnectdisplaymode.cpp

HEADERS += ../../app/streaming/stationconnectdisplaymode.h
INCLUDEPATH += ../../app/streaming
