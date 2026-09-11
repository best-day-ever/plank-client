# Support debug and release builds from command line for CI
CONFIG += debug_and_release

# Ensure symbols are always generated
CONFIG += force_debug_info

# SDK27 Clang recognizes __yield but requires its ACLE declaration. Qt6.10.2
# qYieldCpu uses the intrinsic without including this header itself.
macx:contains(QMAKE_APPLE_DEVICE_ARCHS, arm64) {
    QMAKE_CFLAGS += -include arm_acle.h
    QMAKE_CXXFLAGS += -include arm_acle.h
    QMAKE_OBJECTIVE_CFLAGS += -include arm_acle.h
    QMAKE_OBJECTIVE_CXXFLAGS += -include arm_acle.h
}

# Disable asserts on release builds
CONFIG(release, debug|release) {
    DEFINES += NDEBUG
}

# Enable CFG, EHCont, and CET
*-msvc {
    QMAKE_CFLAGS += -guard:cf -guard:ehcont
    QMAKE_CXXFLAGS += -guard:cf -guard:ehcont
    QMAKE_LFLAGS += -guard:cf -guard:ehcont

    contains(QT_ARCH, x86_64) {
        QMAKE_LFLAGS += -cetcompat
    }
}

# Enable ASan for Linux or macOS
#CONFIG += sanitizer sanitize_address

# Enable ASan for Windows
#QMAKE_CFLAGS += -fsanitize=address
#QMAKE_CXXFLAGS += -fsanitize=address
#QMAKE_LFLAGS += -incremental:no

# Propagate environment variable flags
QMAKE_CFLAGS   += $$(CFLAGS)
QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_LFLAGS   += $$(LDFLAGS)
