QT += core gui network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += test_betadiagnostics.cpp ../../app/backend/betadiagnostics.cpp
HEADERS += ../../app/backend/betadiagnostics.h
INCLUDEPATH += ../../app
win32: LIBS += -ldxgi -luser32
