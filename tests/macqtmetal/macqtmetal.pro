!macx: error("This test requires macOS and Metal")
QT += core gui quick
CONFIG += console c++17
CONFIG -= app_bundle
TEMPLATE = app
TARGET = macqtmetal
OBJECTIVE_SOURCES += test_macqtmetal.mm
QMAKE_CXXFLAGS += -fobjc-arc
LIBS += -framework Metal -framework QuartzCore
