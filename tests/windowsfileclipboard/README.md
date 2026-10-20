# Windows file clipboard checks

This suite exercises the shared PCF2 Windows filename policy on every Qt 6
platform. On Windows it also compiles the actual Windows file clipboard engine,
using an in-memory replacement for the native transport ABI. Native cases check
frozen source handles, hard-link rejection, hashed incoming file publication as
CF_HDROP, integrity failure without publication, rejection of reserved device
names, and cleanup when the session stops.

Use an empty build directory and a Qt 6 C++17 toolchain:

```sh
qmake <client>/tests/windowsfileclipboard/windowsfileclipboard.pro \
  PLANK_PROTOCOL_INCLUDE=<plank>/protocol/plank-transport/include \
  CONFIG+=release CONFIG-=app_bundle
make -j2
./windowsfileclipboard
```

On Windows use `nmake` or `jom` matching the Qt toolchain and run
`windowsfileclipboard.exe`. The suite links user32, shell32 and advapi32; it
needs no live Host, credentials, SDL, or native transport library. The native
round-trip test temporarily replaces the interactive Windows user's clipboard.
Run it on the authorized test account with no important clipboard content.

Physical acceptance still requires Explorer and the remote desktop file manager
in both directions: a file, nested folder, empty file, Unicode filename,
large multichunk file, collision rejection, interrupted session, and new local
copy after a Host transfer. Network shares, reparse points/junctions, symlinks,
hard-linked source files, alternate streams, and Windows device-name aliases
are deliberately rejected. Transfers preserve content and modification times;
Unix executable/readonly modes do not map to Windows access control. Received
files use a private user/SYSTEM ACL and remain available until the stream ends.
