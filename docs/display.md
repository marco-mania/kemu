# Display

KEMU offers two display backends per VM (*Edit… → Hardware → Display*):

- **SDL:** QEMU opens its own window, optionally with OpenGL for 3D
  acceleration. Audio goes to the host directly through PipeWire.
- **D-Bus:** KEMU shows the VM in its own viewer window, with 3D acceleration,
  audio, clipboard sharing and pointer capture.

The D-Bus viewer is stable, but still under active development; expect
changes between versions.

## How the D-Bus viewer works

KEMU starts QEMU with `-display dbus,p2p=on` and hands it one end of a Unix
socketpair through QMP's `add_client` command. This gives a direct
peer-to-peer D-Bus connection between KEMU and QEMU, without any bus or socket
file in between. The guest needs no special driver for video or input.

- **Hardware-accelerated graphics:** With Virtio-GPU and 3D acceleration,
  frames arrive as DMA-BUF descriptors and are imported into the viewer's
  OpenGL context – zero-copy, from GPU to GPU.
- **Audio in both directions:** Playback and microphone are routed through
  D-Bus to QEMU's `audiodev=dbus`, on the host side through Qt Multimedia
  (PipeWire).
- **Clipboard:** Off, guest → host, host → guest or both, in the viewer's
  *Devices* menu. The guest needs the SPICE vdagent.
- **Pointer capture:** Locks the pointer to the viewer for guests with a
  relative mouse. The compositor must support pointer constraints and
  relative pointer protocols. Guests with a tablet pointer do not need capture.
- **Status bar:** Shows CD/DVD, floppy, USB devices, network (click to plug or
  unplug the cable), guest agent (click to copy the guest's address),
  clipboard sharing, microphone, audio output and pointer capture.
- **Headless:** Closing the viewer keeps the VM running; *Show Display* in the
  VM menu or the tray menu opens it again.

*Settings → Display* sets the default resolution and when the screen saver is
held off: whenever a viewer is open, while it is fullscreen, or while it
captures the pointer.

## Clipboard with Wayland guests

From a Wayland guest to the host, clipboard sharing can fail: the guest's
clipboard agent often relies on X11 (`xclip`/`xsel`), which older guests do not
bridge to Wayland. A small script in the guest copies the Wayland clipboard to
the X11 one:

```sh
#!/bin/sh
# clip-bridge.sh: copy the Wayland clipboard to the X11 clipboard
new=$(cat; printf x); new=${new%x}
cur=$(xclip -selection clipboard -o -t UTF8_STRING 2>/dev/null; printf x); cur=${cur%x}
[ "$new" = "$cur" ] && exit 0
printf %s "$new" | xclip -selection clipboard -i -t UTF8_STRING
```

Run it with `wl-paste --type text --watch /path/to/clip-bridge.sh`, or as a
user service in `~/.config/systemd/user/wl-clip-bridge.service`:

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
