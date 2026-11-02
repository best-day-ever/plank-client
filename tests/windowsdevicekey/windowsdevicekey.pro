QT += core network gui qml quick quickcontrols2 testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app
SOURCES += test_windowsdevicekey.cpp \
    ../../app/backend/brokerdevicekey.cpp \
    ../../app/backend/brokersessionstore.cpp \
    ../../app/backend/plankpasskey.cpp \
    ../../app/backend/plankenrollment.cpp \
    ../../app/backend/plankbrokerclient.cpp
HEADERS += ../../app/backend/brokerdevicekey.h ../../app/backend/brokersessionstore.h
RESOURCES += ui.qrc
INCLUDEPATH += ../../app ../../app/backend
win32 {
    INCLUDEPATH += ../../libs/windows/include ../../libs/windows/include/x64
    LIBS += -L$$PWD/../../libs/windows/lib/x64 -llibcrypto -lncrypt -lbcrypt -lcrypt32 -ladvapi32
} else {
    CONFIG += link_pkgconfig
    PKGCONFIG += openssl
    macx: LIBS += -framework Security -framework CoreFoundation
}
