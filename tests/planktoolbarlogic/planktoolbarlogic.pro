QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += test_planktoolbarlogic.cpp

HEADERS += ../../app/streaming/planktoolbarlogic.h
HEADERS += ../../app/streaming/input/plankpointerlogic.h
HEADERS += ../../app/streaming/videopacketlosswindow.h
INCLUDEPATH += ../../app/streaming
