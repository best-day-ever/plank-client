# Windows tablet input

Fernweh uses two complementary tablet paths on Windows:

- For USB Wacom devices, SDL3 HIDAPI enumerates the physical tablet's Windows
  digitizer, vendor and consumer HID collections and reconstructs their report descriptors from Windows HID
  preparsed data. Fernweh forwards those descriptors and complete reports to
  the Linux host's existing UHID protocol. This retains device-specific pen,
  eraser, pad, wheel and touch reports where the installed Windows driver
  exposes those collections. Host feature and output requests go back to the
  physical tablet on a bounded asynchronous worker. Protected generic mouse/keyboard
  compatibility collections keep using ordinary SDL input. Feature requests
  route across collections belonging to the same physical USB interface.
- Windows Ink pen events provide the automatic fallback if a tablet is not
  connected by USB, the Windows driver denies HID access, or raw attachment
  cannot complete. Enable **Use Windows Ink** in the Wacom driver for this
  fallback. It carries position, pressure, tilt, eraser and pen buttons;
  ExpressKeys, touch and device-specific wheels require the raw path. The
  first-generation Intuos Pro PTH-451/651/851 uses this fallback because its
  Linux driver requires USB interface metadata UHID cannot reproduce.

The normalized pen path pauses as soon as raw attachment succeeds, preventing
simultaneous forwarding. SDL's synthetic pen mouse events are ignored while
one of these tablet transports is available; a physical mouse remains usable.
Leaving the stream, releasing input capture, reconnecting, or losing focus
suspends raw forwarding and releases remote contacts. Returning to the focused
stream reacquires the device. Disconnecting a tablet detaches its host device;
reconnecting it is discovered automatically. A different physical tablet with
identical VID/PID is never combined with the first one.

To force Windows Ink for troubleshooting, launch Fernweh with the environment
variable `PLANK_WINDOWS_PEN_INPUT=1`. Raw HID uses only Wacom USB devices and
requires no replacement driver, administrator access, or driver installation.
Some Windows drivers reserve collections exclusively; Fernweh leaves those
alone and uses Windows Ink rather than replacing or disabling their driver.

## Qualification

Automated tests validate reconstructed-descriptor report sizes (including
numbered and unnumbered reports, padding, global push/pop, malformed and
oversized layouts), first-generation routing, bounded host-control parsing,
bounded lifecycle barriers, unnumbered control-report ID slots, longest-report
GET buffers, and stale asynchronous completion rejection after reconnect. The existing
`macpen` suite exercises the shared SDL pen translation used by Windows.

These checks do not qualify a physical tablet. Before declaring a device fully
supported, test its actual model and installed driver on Windows against a
Linux host: pressure range, tilt in both directions, eraser, all pen buttons,
ExpressKeys, wheels/rings, touch interfaces, USB unplug/replug, focus changes,
input release, and stream reconnection. Windows provides reconstructed
per-collection descriptors, not the original byte-for-byte USB descriptor;
Linux hid-wacom acceptance must therefore be checked for each model. Art Pen
barrel rotation is outside the existing normalized wire format and requires
raw HID. Keep these hardware results distinct from successful compilation or
parser tests.
