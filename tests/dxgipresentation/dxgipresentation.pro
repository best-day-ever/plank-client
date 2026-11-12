QT += core testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = dxgipresentation
DEFINES += NOMINMAX
SOURCES += test_dxgipresentation.cpp
INCLUDEPATH += ../../app
