QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += test_fernwehupdater.cpp
HEADERS += ../../app/backend/fernwehupdatepolicy.h
INCLUDEPATH += ../../app/backend
