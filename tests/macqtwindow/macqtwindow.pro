!macx: error("This fixture requires a native macOS window and Metal")
QT += testlib
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = macqtwindow
include(../../app/streaming/qt/qtsession.pri)
include(../../app/streaming/qt/qtvideo.pri)
OBJECTIVE_SOURCES += test_macqtwindow.mm
