QT += core network gui qml quick quickcontrols2 testlib
CONFIG += console testcase c++17
TEMPLATE = app
SOURCES += test_internetconnectivity.cpp \
    ../../app/backend/internetconnectivity.cpp \
    ../../app/settings/plankclientpolicy.cpp
HEADERS += ../../app/backend/internetconnectivity.h
RESOURCES += ../../app/qml.qrc
