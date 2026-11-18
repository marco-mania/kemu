# Display

KEMU provides two display backends per VM (*Edit… → Hardware → Display*):

- **SDL:** Opens QEMU's own window with optional OpenGL-based 3D acceleration and routes audio directly to the host via PipeWire.
- **D-Bus:** Displays the VM in KEMU's integrated viewer with 3D acceleration, bidirectional audio, clipboard sharing, and pointer capture.

While stable, the D-Bus viewer remains under active development and its behaviour may change between releases.

## How the D-Bus viewer works

KEMU launches QEMU with `-display dbus,p2p=on` and passes one end of a Unix `socketpair` via QMP's `add_client` command, establishing a direct peer-to-peer D-Bus connection without a message bus or socket file. Video and input require no special guest drivers.

- **Hardware-accelerated graphics:** With Virtio-GPU and 3D acceleration enabled, frames arrive as DMA-BUF descriptors and import directly into the viewer's OpenGL context, enabling GPU-to-GPU transfers without copying through system memory.
- **Bidirectional audio:** Playback and microphone audio travel over D-Bus via QEMU's `audiodev=dbus`, handled on the host by Qt Multimedia (PipeWire).
- **Clipboard:** Select off, guest → host, host → guest, or bidirectional sharing under the viewer's *Devices* menu (requires `kemu-agent` in the guest; see below):
  - *Filter Passwords* (enabled by default) keeps entries marked as secret by password managers such as KeePassXC on their originating side in both directions; when disabled, they transfer like regular text.
  - *Share Selection (Middle Click)* additionally shares the primary selection.
  - *Share Files* (disabled by default) allows copying and pasting files and folders between host and guest up to *File Size Limit* per copy (256 MiB by default). When active, the guest can write files to KEMU's host cache directory; they are never executable and are removed when the viewer closes.
  - Settings can be configured before `kemu-agent` starts and take effect once it connects, with the menu's first line indicating what KEMU is waiting for. Features unsupported by the running agent are greyed out with an explanatory tooltip (for example, *Share Files* with an agent older than 0.3). The entire menu is unavailable on non-PCI machine types (which lack the agent port) and for Windows, BSD, or DOS guests.
- **Pointer capture:** Locks the pointer inside the viewer for guests using relative mouse input (requires compositor support for the pointer-constraints and relative-pointer protocols; unnecessary for guests using a tablet pointer).
- **Status bar:** Displays CD/DVD, floppy, and USB devices; network status (click to connect or disconnect the cable); `kemu-agent` status; guest-agent status (click to copy the guest's address); clipboard sharing; microphone and audio output; and pointer capture.
- **Headless operation:** Closing the viewer leaves the VM running; reopen it via *Show Display* in the VM or system tray menu.

*Settings → Display* controls the default resolution and when the screensaver is inhibited: whenever a viewer is open, while it is in fullscreen mode, or while it captures the pointer.

## kemu-agent

`kemu-agent` is KEMU's guest agent for Linux (`agent/` in this repository). Running in the user's graphical session, it:

- Shares the clipboard—and optionally the primary selection—in every format an application offers (text, HTML, images, and application formats)
- Reports the guest's monitor layout so the pointer lands accurately on every display regardless of per-monitor scaling

It works on Wayland (KDE Plasma, wlroots compositors such as Sway and Hyprland, GNOME via Xwayland) and on X11 desktops. It replaces `spice-vdagent`, which KEMU no longer supports, while `qemu-guest-agent` continues to run alongside it.

KEMU attaches a virtio-serial port named `org.kde.kemu.agent.0` to every VM using the integrated viewer and a PCI bus; existing VMs receive the port automatically on their next start. The KEMU icon in the viewer's status bar reflects the agent's state—dimmed when disconnected, with a warning emblem when it lacks access to the guest's clipboard—and its tooltip lists the agent version, guest session, and supported capabilities.

Current version limits:

- Clipboard content is fetched as soon as either side offers it, up to 32 MiB per copy; larger content remains on the originating side.
- Files and folders transfer only with *Share Files* enabled (see above). They are copied when placed on the clipboard rather than when pasted, so each copy is subject to the size limit; symbolic links are skipped, and cut operations behave like copy.
- Windows and BSD guests have no agent yet and therefore no clipboard sharing. KEMU checks the operating system configured for the VM, though if an agent connects anyway, it is used.

The protocol between KEMU and the agent is specified in [kemu-agent-protocol.md](kemu-agent-protocol.md).
