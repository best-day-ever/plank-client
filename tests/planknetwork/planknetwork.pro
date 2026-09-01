QT += core testlib
CONFIG += console testcase c++11
TEMPLATE = app

SOURCES += test_planknetwork.cpp

HEADERS += \
    ../../app/backend/planknetwork.h

INCLUDEPATH += \
    ../../app/backend
