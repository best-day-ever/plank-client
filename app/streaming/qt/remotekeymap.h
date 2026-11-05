#pragma once
#include <QCoreApplication>
#include <QKeyEvent>
#ifdef Q_OS_MACOS
#include "mackeymap.h"
using QtRemoteKey = QtMacRemoteKey;
inline QtRemoteKey qtRemoteKey(quint32 code) { return qtMacRemoteKey(code); }
inline quint32 qtPhysicalKey(const QKeyEvent* event) { return event->nativeVirtualKey(); }
inline Qt::KeyboardModifiers qtPhysicalModifiers(Qt::KeyboardModifiers mods)
{
    if (QCoreApplication::testAttribute(Qt::AA_MacDontSwapCtrlAndMeta)) return mods;
    const bool command = mods.testFlag(Qt::ControlModifier);
    const bool control = mods.testFlag(Qt::MetaModifier);
    mods &= ~(Qt::ControlModifier | Qt::MetaModifier);
    if (command) mods |= Qt::MetaModifier;
    if (control) mods |= Qt::ControlModifier;
    return mods;
}
#else
#include "linuxkeymap.h"
using QtRemoteKey = QtLinuxRemoteKey;
inline QtRemoteKey qtRemoteKey(quint32 code) { return qtLinuxRemoteKey(code); }
inline quint32 qtPhysicalKey(const QKeyEvent* event) { return event->nativeScanCode(); }
inline Qt::KeyboardModifiers qtPhysicalModifiers(Qt::KeyboardModifiers mods) { return mods; }
#endif
