QT += core network testlib
CONFIG += console testcase c++17
CONFIG -= app_bundle
TEMPLATE = app

SOURCES += \
    test_plankbroker.cpp \
    ../../app/backend/plankbrokerclient.cpp \
    ../../app/backend/outputtopology.cpp

HEADERS += \
    ../../app/backend/plankbroker.h \
    ../../app/backend/plankbrokerclient.h \
    ../../app/backend/plankhttp.h \
    ../../app/backend/macpreviewlaunch.h \
    ../../app/backend/outputtopology.h

INCLUDEPATH += \
    ../../app/backend \
    ../../moonlight-common-c/moonlight-common-c/src

# Qt's OpenSSL TLS backend (selected in initTestCase, as in main.cpp) dlopens
# libssl/libcrypto through the executable's rpaths. Give the test the same
# bundled-OpenSSL rpath the Client has.
macx {
    QMAKE_RPATHDIR += $$system(pkg-config --variable=libdir openssl)
}
