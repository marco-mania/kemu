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
  sharing from the viewer's *Devices* menu. The guest requires the SPICE
  vdagent.
- **Pointer capture:** Locks the pointer to the viewer for guests using
  relative mouse input. The compositor must support the pointer constraints
  and relative pointer protocols. Guests using a tablet pointer do not need
  capture.
- **Status bar:** Shows CD/DVD, floppy and USB devices; network status (click
  to connect or disconnect the cable); guest-agent status (click to copy the
  guest's address); clipboard sharing; microphone and audio output; and
  pointer capture.
- **Headless operation:** Closing the viewer leaves the VM running. Reopen
  it with *Show Display* from the VM or system tray menu.

*Settings → Display* controls the default resolution and when the screen
saver is inhibited: whenever a viewer is open, while it is in fullscreen
mode, or while it captures the pointer.

## Clipboard with Wayland guests

Clipboard sharing from a Wayland guest to the host may fail because the
agent often relies on X11 (`xclip`/`xsel`). Some guest environments do not
synchronise their native Wayland clipboard with X11. As a workaround, run a
small script inside the guest to copy Wayland clipboard contents to X11:

```sh
#!/bin/sh
# clip-bridge.sh: copy the Wayland clipboard to the X11 clipboard
new=$(cat; printf x); new=${new%x}
cur=$(xclip -selection clipboard -o -t UTF8_STRING 2>/dev/null; printf x); cur=${cur%x}
[ "$new" = "$cur" ] && exit 0
printf %s "$new" | xclip -selection clipboard -i -t UTF8_STRING
```

Run it with `wl-paste --type text --watch /path/to/clip-bridge.sh`, or set it
up as a user service at `~/.config/systemd/user/wl-clip-bridge.service`:

```ini
[Unit]
Description=Wayland to X11 Clipboard Bridge
After=graphical-session.target

[Service]
ExecStart=/usr/bin/wl-paste --type text --watch /path/to/clip-bridge.sh
Restart=on-failure

[Install]
WantedBy=graphical-session.target
```

```sh
systemctl --user daemon-reload
systemctl --user enable --now wl-clip-bridge.service
```
