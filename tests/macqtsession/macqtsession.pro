!macx: error("This fixture checks the macOS Qt session adapter")
QT += testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = macqtsession
include(../../app/streaming/qt/qtsession.pri)
SOURCES += test_macqtsession.cpp
