# Display

KEMU shows every VM in its integrated D-Bus viewer, with 3D acceleration, bidirectional audio, clipboard sharing, and pointer capture. While stable, the viewer remains under active development and its behaviour may change between releases.

Only the exported `qemu.sh` script, which runs without KEMU, opens QEMU's own SDL window instead, with audio routed directly to the host via PipeWire.

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

*Settings → Display* sets the initial resolution of all VMs, which applies until the guest's graphics driver runs and the viewer window takes over, and when the screensaver is inhibited: whenever a viewer is open, while it is in fullscreen mode, or while it captures the pointer.

## Graphics: acceleration and video memory

*Edit… → Hardware → Graphics* sets the adapter, its *Acceleration* and its *Video memory*, in this order, as the acceleration decides what the video memory means. Options the adapter or the host cannot provide are greyed out, and their tooltip says why.

- **3D:** QEMU starts with `virtio-vga-gl`, and the guest's OpenGL runs on the host GPU through `virglrenderer`. Requires VirtIO graphics. The host GPU provides the guest's video memory as needed, so *Video memory* shows *Automatic*.
- **Direct GPU memory access**, below *3D*: enables host blobs (`blob=on,hostmem=…`). Instead of having every change copied, the guest maps GPU memory of the host directly. Linux guests get OpenGL 4.6 instead of 4.3, and applications that stream a lot of data to the GPU need less CPU time. So far only Mesa in Linux guests makes use of it. It requires QEMU 9.2 or later built with `virglrenderer` 1.0 or later (the Flatpak ships both), and Linux 6.13 or later on the host, which QEMU needs to map GPU memory into a VM. *Settings → System → Capabilities* shows whether the host meets them.
- **Vulkan**, below *Direct GPU memory access* and requiring it: adds Venus (`venus=on`), so the guest can use Vulkan, rendered on the host GPU. Linux guests use Mesa's Venus driver, which needs a guest kernel 5.16 or later; `vulkaninfo` lists it as *Virtio-GPU Venus*. For Windows guests, only experimental community drivers exist, such as vgpu3d; they suggest 4 GiB of video memory. Venus runs in virglrenderer's render server, `virgl_render_server`, so virglrenderer has to be built with Venus; Debian and Ubuntu ship the server in the `virgl-server` package, and the Flatpak includes it. virglrenderer starts the server from `RENDER_SERVER_EXEC_PATH` if that is set, otherwise from the path it was built with; KEMU looks below `libexec/` and `lib/` next to QEMU, in `/usr` and in `/usr/local`, so where a distribution installs it elsewhere, set the variable before starting KEMU. Since the server is a program of its own, VMs with Vulkan run without QEMU's seccomp sandbox (*Settings → System → QEMU Sandbox*). The host also needs a Vulkan driver for its GPU, which KEMU does not check.

*Video memory* takes any size within its range; the arrows step through powers of two, and the button next to the field lists them. The help button beside it explains what the size means at the moment.

| Adapter / acceleration             | Range         | Note                                    |
| ---------------------------------- | ------------- | --------------------------------------- |
| Standard VGA                       | 4–256 MiB     | QEMU rounds up to a power of two        |
| ISA VGA                            | 1–16 MiB      | QEMU rounds up to a power of two        |
| Cirrus VGA                         | 4, 8, 16 MiB  | Other sizes snap to these               |
| QXL                                | 8–256 MiB     | QEMU rounds up to a power of two        |
| VirtIO                             | 16–2048 MiB   | Limits the host memory of 2D resources  |
| VirtIO, 3D                         | Automatic     | Provided by the host GPU as needed      |
| VirtIO, direct GPU memory access   | 256 MiB–8 GiB | Powers of two only; 1 GiB by default    |

With direct GPU memory access, the size is how much host GPU memory the guest maps at once (`hostmem`). It takes guest address space only; the host backs it where the guest maps memory. Host blobs need no shared memory for the guest RAM, so memory preallocation behaves as without them. Exported `qemu.sh` scripts never use them, nor Vulkan, so they keep running on older QEMUs.

On hosts with an Intel CPU, KVM by default ignores the memory types the guest sets up and maps its memory write-back. GPU memory the host itself maps write-combined—with Meteor Lake and xe graphics, and with dedicated GPUs—then shows the guest stale data, and Vulkan hangs (in the guest: `MESA-VIRTIO: debug: stuck in fence wait`). For VMs with Vulkan, KEMU therefore starts QEMU with `-accel kvm,honor-guest-pat=auto` where QEMU offers it (11.0 or later); it takes effect with Linux 6.16 or later on the host. VMs with OpenGL alone keep KVM's default: with the option, Firefox's GPU compositor (WebRender) stutters on videos, presumably because it reads from write-combined memory. In VMs with Vulkan it does so on such hosts as well; setting `gfx.webrender.software` to `true` in Firefox's `about:config` makes playback smooth again, and Chromium is not affected. AMD CPUs always honor the guest's memory types. *Settings → System → Capabilities* shows the state under *Guest memory types*.

## kemu-agent

`kemu-agent` is KEMU's guest agent for Linux (`agent/` in this repository). Running in the user's graphical session, it:

- Shares the clipboard—and optionally the primary selection—in every format an application offers (text, HTML, images, and application formats)
- Reports the guest's monitor layout so the pointer lands accurately on every display regardless of per-monitor scaling

It works on Wayland (KDE Plasma, wlroots compositors such as Sway and Hyprland, GNOME via Xwayland) and on X11 desktops. It replaces `spice-vdagent`, which KEMU no longer supports, while `qemu-guest-agent` continues to run alongside it.

KEMU attaches a virtio-serial port named `org.kde.kemu.agent.0` to every VM using the integrated viewer and a PCI bus; existing VMs receive the port automatically on their next start. The KEMU icon in the viewer's status bar reflects the agent's state—dimmed when disconnected, with a warning emblem when it lacks access to the guest's clipboard—and its tooltip lists the agent version, guest session, and supported capabilities.

### Installing it in a guest

*Insert kemu-agent CD Image…*—in the viewer's *Devices* menu, behind the kemu-agent icon in its status bar, and under *VM → Removable Media*—places a CD image with the agent in the VM's CD drive, as other hypervisors do with their guest tools. In the guest, open a terminal and run:

```sh
sudo sh /run/media/$USER/KEMU_AGENT/install.sh
```

On Debian and Ubuntu, the disc is mounted at `/media/$USER/KEMU_AGENT` instead; if the desktop has not mounted it, run `sudo mount -o ro /dev/sr0 /mnt && sudo sh /mnt/install.sh`. The installer places the agent in `/opt/kemu-agent` and starts it in running sessions; a newer CD image updates it, and `sudo sh /opt/kemu-agent/uninstall.sh` removes it. The command has to be typed, since there is no shared clipboard until the agent runs; a `README.txt` on the disc repeats these steps. After inserting the disc, KEMU shows them as well and, if an agent is already running, compares its version with the one on the disc.

KEMU builds the CD image from kemu-agent bundles (see `agent/README.md`, "Portable bundle"): those installed to `share/kemu/agent/<arch>/`—which KEMU's build does by default (see the README, "Build, install, uninstall")—and one the user chose. If none matches the guest's architecture, KEMU explains where bundles come from and offers to choose a bundle tarball, remembering the choice. The image holds the newest bundle for each architecture plus an `install.sh` that runs the one matching the guest's `uname -m`. It is kept at `~/.local/share/kemu/agent-medium/kemu-agent.iso` and rebuilt only when the bundles change; a disc already in the drive is replaced only after confirmation. The entry is disabled for Windows, BSD, and DOS guests.

Current version limits:

- Clipboard content is fetched as soon as either side offers it, up to 32 MiB per copy; larger content remains on the originating side.
- Files and folders transfer only with *Share Files* enabled (see above). They are copied when placed on the clipboard rather than when pasted, so each copy is subject to the size limit; symbolic links are skipped, and cut operations behave like copy.
- Windows and BSD guests have no agent yet and therefore no clipboard sharing. KEMU checks the operating system configured for the VM, though if an agent connects anyway, it is used.

The protocol between KEMU and the agent is specified in [kemu-agent-protocol.md](kemu-agent-protocol.md).
