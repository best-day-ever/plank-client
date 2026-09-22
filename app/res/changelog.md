## 1.0.152

### Client

- Match the primary client monitor when ordering a Linux Host's virtual displays.
- Keep the existing image proportions when host and client monitor sizes differ.
- Allow manual two-display bookmarks when the client has a different monitor layout.

### Host

- Linux: place the first virtual connector on the client's primary side for applications that choose the first monitor.

## 1.0.151

### Client

- Offer Take Over or Cancel when another client is connected to the same Mac account.
- Use the new client's display size after an approved takeover.
- Preserve valid packets when the connection adjusts its network packet size.
- Ubuntu: include the image plugin needed to display dialog icons.

### Host

- macOS: transfer an active session to another client after explicit confirmation, including while locked.
- macOS: avoid a temporary loss of connectivity when switching between the login screen and desktop.
- Preserve valid packets during network packet-size recovery on Linux and macOS.
- Reduce diagnostic logging overhead during streaming.

## 1.0.146

### Client

- Click the version number to see what's new, even when offline.

### Host

- macOS: retry failed desktop-service startup after login or user switching.

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
