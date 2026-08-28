QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += test_wacomtransportpolicy.cpp

HEADERS += ../../app/streaming/input/linuxrawwacom.h
INCLUDEPATH += ../../app/streaming/input
