!linux:!macx: error("The Qt session supports Linux and macOS")
QT += core gui qml quick quickcontrols2
CONFIG += c++17
INCLUDEPATH += $$PWD
SOURCES += $$PWD/sessionwindow.cpp $$PWD/cursoritem.cpp
linux: SOURCES += $$PWD/sessionshell.cpp
macx {
    OBJECTIVE_SOURCES += $$PWD/metalsessionshell.mm
    HEADERS += $$PWD/metalsessionshell.h
    LIBS += -framework Metal -framework QuartzCore
}
HEADERS += $$PWD/sessionwindow.h $$PWD/sessiongeometry.h $$PWD/sessionshell.h $$PWD/cursoritem.h $$PWD/sessionshortcut.h
RESOURCES += $$PWD/qtsession.qrc
