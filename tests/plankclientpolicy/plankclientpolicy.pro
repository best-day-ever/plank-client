QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += \
    test_plankclientpolicy.cpp \
    ../../app/settings/plankclientpolicy.cpp

HEADERS += ../../app/settings/plankclientpolicy.h

INCLUDEPATH += ../../app/settings
