QT += core testlib
CONFIG += console testcase c++17
TEMPLATE = app

SOURCES += \
    test_stationconnectclientpolicy.cpp \
    ../../app/settings/stationconnectclientpolicy.cpp

HEADERS += ../../app/settings/stationconnectclientpolicy.h

INCLUDEPATH += ../../app/settings
