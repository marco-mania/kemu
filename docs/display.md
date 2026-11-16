# Display

KEMU provides two display backends for each VM (*Edit… → Hardware → Display*):

- **SDL:** QEMU opens its own window, with optional OpenGL-based 3D
  acceleration. Audio is sent directly to the host through PipeWire.
- **D-Bus:** KEMU displays the VM in an integrated viewer with 3D
  acceleration, audio, clipboard sharing and pointer capture.

The D-Bus viewer is stable but remains under active development. Its
behaviour may change between releases.

## How the D-Bus viewer works

KEMU starts QEMU with `-display dbus,p2p=on` and passes one end of a Unix
socketpair through QMP's `add_client` command. This establishes a direct
peer-to-peer D-Bus connection between KEMU and QEMU, without a message bus or
socket file. No special guest drivers are required for video or input.

- **Hardware-accelerated graphics:** With Virtio-GPU and 3D acceleration,
  frames arrive as DMA-BUF descriptors and are imported into the viewer's
  OpenGL context, enabling GPU-to-GPU transfers without copying through
  system memory.
- **Bidirectional audio:** Playback and microphone audio travel over D-Bus
  through QEMU's `audiodev=dbus`, with Qt Multimedia (PipeWire) handling audio
  on the host.
- **Clipboard:** Choose off, guest → host, host → guest or bidirectional
  sharing from the viewer's *Devices* menu. The guest requires kemu-agent
  (see below). *Filter Passwords* (on by default) keeps entries that password
  managers such as KeePassXC mark as secret on their side, in both
  directions; switched off, they cross like any other text. *Share Selection
  (Middle Click)* shares the primary selection as well.
  *Share Files* (off by default) lets files and folders copied on one side be
  pasted on the other, up to the *File Size Limit* per copy (256 MiB by
  default). With it, the guest can place files in KEMU's cache directory on
  the host; they are never executable and are removed when the viewer closes.
  The settings can be made before kemu-agent runs and take effect once it
  does; the menu's first line then says what KEMU is waiting for. What the
  running agent cannot do is greyed out, with the reason as a tooltip, for
  example *Share Files* with an agent older than 0.3. Without the agent port
  (machine types without PCI) and for Windows, BSD and DOS guests the whole
  menu is unavailable.
- **Pointer capture:** Locks the pointer to the viewer for guests using
  relative mouse input. The compositor must support the pointer constraints
  and relative pointer protocols. Guests using a tablet pointer do not need
  capture.
- **Status bar:** Shows CD/DVD, floppy and USB devices; network status (click
  to connect or disconnect the cable); kemu-agent status; guest-agent status
  (click to copy the guest's address); clipboard sharing; microphone and audio output; and
  pointer capture.
- **Headless operation:** Closing the viewer leaves the VM running. Reopen
  it with *Show Display* from the VM or system tray menu.

*Settings → Display* controls the default resolution and when the screen
saver is inhibited: whenever a viewer is open, while it is in fullscreen
mode, or while it captures the pointer.

## kemu-agent

kemu-agent is KEMU's guest agent for Linux. Its source is in the `agent/`
directory of this repository. It runs in the user's graphical session and

- shares the clipboard, and optionally the primary selection, in every format
  an application offers: text, HTML, images and application formats;
- reports how the guest arranged its monitors, so the pointer lands on the
  right spot of every monitor, whatever its scaling factor.

It works on Wayland (KDE Plasma, wlroots compositors such as Sway and
Hyprland, GNOME through Xwayland) and on X11 desktops. It replaces
spice-vdagent, which KEMU no longer supports; the QEMU guest agent stays and
runs beside it.

KEMU gives every VM with the integrated viewer and a PCI bus a virtio-serial
port named `org.kde.kemu.agent.0` for the agent. Existing VMs need no change:
the port appears at their next start. The KEMU icon in the viewer's status
bar shows whether the agent runs: dimmed while it does not, with a warning
emblem when it has no access to the guest's clipboard. Its tooltip names the
agent version, the guest session and what the agent can do there.

Some limits of the current version:

- Clipboard content is copied as soon as one side offers it, up to 32 MiB per
  copy. Larger content stays where it was copied.
- Files and folders travel only with *Share Files* switched on (see above).
  They are copied when they are copied to the clipboard, not when they are
  pasted, so each copy is limited in size; symbolic links are left out, and
  cut works like copy.
- Windows and BSD guests have no agent yet, so they have no clipboard
  sharing. KEMU goes by the operating system set for the VM; should an agent
  connect after all, it is used.

The protocol between KEMU and the agent is specified in
[kemu-agent-protocol.md](kemu-agent-protocol.md).
