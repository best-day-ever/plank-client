#pragma once
#include "clipboardsync.h"
// Compatibility for existing Mac component callers.
using MacClipboardSync = ClipboardSync;
#ifdef Q_OS_MACOS
char* macReadGeneralPasteboardTextForSdl();
#endif
