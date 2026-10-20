# Windows file clipboard checks

This suite exercises the shared PCF2 Windows filename policy on every Qt 6
platform. On Windows it also compiles the actual Windows file clipboard engine,
using an in-memory replacement for the native transport ABI. A small fake Host
requests receiver-selected ranges, reconstructs outgoing content and checks hashes
before releasing the withheld paste. Outgoing cases cover nested folders, multiple
top-level selections, Unicode filenames, an empty file, multichunk content, Host
cancellation with source-handle release, focus cancellation followed immediately
by a new paste, stale ready-event rejection, duplicate-name preflight, and
direction/disabled-mode restrictions. Native recovery cases also exercise a Host
CANCEL while the data lane is silent and publication retry after another process
releases its native clipboard lock. A crossed-offer case starts a Host offer
before ACKing the Client manifest, proves Host publication wins first, then
checks the retried Client offer still transfers its original saved local
selection despite CF_HDROP having been replaced by the Host files.
Native cases also check
frozen source handles, hard-link rejection, hashed incoming file publication as
CF_HDROP, integrity failure without publication, rejection of reserved device
names, preservation of local copies and clipboard ownership when stream focus
is absent, and cleanup when the session stops.

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
are deliberately rejected. Malformed and out-of-order incoming manifests must
fail promptly while permitting a subsequent valid offer on the same worker. Transfers preserve content and modification times;
Unix executable/readonly modes do not map to Windows access control. Received
files use a private user/SYSTEM ACL and remain available until the stream ends.
