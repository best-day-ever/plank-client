QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += test_stationconnecttoolbarlogic.cpp

HEADERS += ../../app/streaming/stationconnecttoolbarlogic.h
HEADERS += ../../app/streaming/input/stationconnectpointerlogic.h
INCLUDEPATH += ../../app/streaming
