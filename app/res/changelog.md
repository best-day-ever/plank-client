## This build

### Client

- Click the version number to see what's new, even when offline.

## 1.0.143

### Client

- Fix mouse positioning at the right and bottom edges of the remote screen.

### Host

- Linux: deliver mouse clicks immediately and improve high-bitrate sending.
- macOS: keep the connection open when the screen locks.
- macOS: let administrators read the Host's system logs.

## 1.0.137

### Client

- Copy and paste plain text between Mac Clients and Mac Hosts.
- Improve clipboard transfers when the connection is busy.

### Host

- macOS: add plain-text clipboard sharing with Mac Clients.
- macOS and Linux: limit shared text to 512 KiB and reject invalid text safely.

## 1.0.135

### Client

- macOS: send Command-Tab and Command-Space to the Host, including after switching Spaces.
- macOS: request keyboard-capture permission before starting a session.
- macOS: improve multi-monitor and Wacom support; support macOS 15 and newer.
- Add network round-trip time to the toolbar and reduce toolbar flicker on macOS.
- Copy and paste plain text between Mac Clients and Linux Hosts.

### Host

- Linux: add plain-text clipboard sharing with Mac Clients.
- Improve mouse-button ordering and keyboard release handling.
