# Display

KEMU renders every VM in its integrated D-Bus viewer with 3D acceleration, bidirectional audio, clipboard sharing, and pointer capture. While stable, the viewer remains under active development and its behaviour may change between releases.

Only the exported `qemu.sh` script—which runs standalone without KEMU—uses QEMU's own SDL window instead, routing audio directly to the host via PipeWire.

## How the D-Bus viewer works

KEMU launches QEMU with `-display dbus,p2p=on` and passes one end of a Unix `socketpair` via QMP's `add_client` command, establishing a direct peer-to-peer D-Bus connection without a message bus or socket file. Video and input require no special guest drivers.

- **Hardware-accelerated graphics:** With Virtio-GPU and 3D acceleration enabled, frames arrive as DMA-BUF descriptors and import directly into the viewer's OpenGL context, enabling zero-copy GPU-to-GPU transfers.
- **Bidirectional audio:** Playback and microphone audio travel over D-Bus via QEMU's `audiodev=dbus`, handled on the host by Qt Multimedia (PipeWire). Because the viewer keeps its playback buffer short, KEMU asks PipeWire for periods of at most 512 frames at 48 kHz (`PIPEWIRE_LATENCY=512/48000`, unless already set); longer periods, which audio players on the host may request, broke up the guest's sound. While a VM plays sound, the host's audio graph therefore runs in these shorter periods.
- **Clipboard:** Select off, guest → host, host → guest, or bidirectional sharing under the viewer's *Devices* menu (requires `kemu-agent` in the guest; see below):
  - *Filter Passwords* (enabled by default) keeps entries marked as secret by password managers such as KeePassXC on their originating side in both directions; when disabled, they transfer like regular text.
  - *Share Selection (Middle Click)* additionally shares the primary selection.
  - *Share Files* (disabled by default) allows copying and pasting files and folders between host and guest up to *File Size Limit* per copy (256 MiB by default). When active, the guest can write files to KEMU's host cache directory; they are never executable and are removed when the viewer closes.
  - Until `kemu-agent` runs in the guest—typically because it is not installed yet—all menu items are greyed out, while the `kemu-agent` icon in the status bar indicates that the agent is not running and offers *Insert kemu-agent CD Image…* (see below). Configured settings are retained and take effect once the agent connects. Features unsupported by the running agent are greyed out with an explanatory tooltip (for example, *Share Files* with an agent older than 0.3). The entire menu is unavailable on non-PCI machine types (which lack the agent port) and for Windows, BSD, or DOS guests.
- **Pointer capture:** Locks the pointer inside the viewer for guests using relative mouse input (requires compositor support for the pointer-constraints and relative-pointer protocols; unnecessary for guests using a tablet pointer).
- **Status bar:** Displays 3D acceleration (see *Graphics* below); CD/DVD, floppy, and USB devices; network status (click to connect or disconnect the cable); `kemu-agent` status; guest-agent status (click to copy the guest's address); clipboard sharing; microphone and audio output; and pointer capture.
- **Headless operation:** Closing the viewer leaves the VM running; reopen it via *Show Display* in the VM or system tray menu.

*Settings → Display* configures the initial resolution for all VMs (active until the guest's graphics driver loads and the viewer window takes over) and specifies when the screensaver is inhibited: whenever a viewer is open, in fullscreen mode, or capturing the pointer.

## Graphics: acceleration and video memory

*Edit… → Hardware* provides two settings groups. Graphics configures the adapter, Video memory, and monitor count. Below it, the 3D acceleration group is enabled via its header checkbox and sets the guest's OpenGL driver and optional Vulkan support, automatically adjusting required dependencies under Advanced. Each option includes a help button detailing its functionality, requirements, and limitations. Unsupported options are greyed out, with a status message at the bottom explaining why. When Native or Vulkan forces direct GPU memory access, a note appears directly below that setting.

- **3D acceleration** (the checkbox in the group title): Launches QEMU with `virtio-vga-gl`, running guest OpenGL on the host GPU via `virglrenderer` (requires VirtIO graphics). The host GPU allocates video memory dynamically, so *Video memory* displays *Automatic*. Switching it off keeps the choices below for when it is switched on again.
- **OpenGL driver in the guest**, one of two:
  - **Universal (virgl)**, the default: The guest's Mesa driver `virgl` translates every OpenGL call for the host GPU. It works with any host GPU and any QEMU with 3D acceleration.
  - **Native (native context)**: Enables a DRM native context (`drm_native_context=on`). Instead of virgl, Linux guests then run the Mesa driver of the host GPU itself (for example `iris` or `radeonsi`), for near-native performance. This requires QEMU ≥ 11.0 and a host GPU supported by `virglrenderer`: Intel with `i915` (`virglrenderer` ≥ 1.3.0), AMD (≥ 1.1.0), or Qualcomm. The guest needs Linux ≥ 6.14 and Mesa ≥ 26.1 for Intel or ≥ 25.0 for AMD, built with native context support; for Intel that takes `-Dintel-virtio-experimental=true`, which Debian and Ubuntu do not set so far. KEMU can only check QEMU; if `virglrenderer` or the guest lacks support, the guest keeps using virgl (`glxinfo -B` names the driver in use, and so does the viewer's status bar with `kemu-agent`). Unlike Vulkan, native contexts run inside QEMU and keep the seccomp sandbox. Not offered for Windows guests, which have no Mesa driver to run.
- **Vulkan (Venus)**: Enables Venus (`venus=on`) to run guest Vulkan on the host GPU, on top of either OpenGL driver. Linux guests use Mesa's Venus driver (requiring guest kernel ≥ 5.16; listed as *Virtio-GPU Venus* in `vulkaninfo`), whereas Windows guests have only experimental community drivers such as `vgpu3d` (which recommends a *Memory limit* of 4 GiB). Venus runs in `virglrenderer`'s render server (`virgl_render_server`), so `virglrenderer` must be built with Venus support (packaged as `virgl-server` on Debian/Ubuntu and included in the Flatpak). `virglrenderer` launches the server from `RENDER_SERVER_EXEC_PATH` if set, or its compile-time path otherwise; KEMU searches `libexec/` and `lib/` alongside QEMU, in `/usr`, and in `/usr/local`—set `RENDER_SERVER_EXEC_PATH` before starting KEMU if installed elsewhere. Because the render server runs as a separate process, Vulkan VMs disable QEMU's seccomp sandbox (*Settings → System → QEMU Sandbox*); the line below the option says so while the sandbox is on. The host also needs a Vulkan GPU driver, which KEMU does not check.
- **Advanced → Direct GPU memory access**: Enables host blobs (`blob=on,hostmem=…`), allowing the guest to map host GPU memory directly rather than copying changes. Linux guests gain OpenGL 4.6 instead of 4.3, and GPU-streaming applications use less CPU. Currently supported only by Mesa in Linux guests, this requires QEMU ≥ 9.2 built with `virglrenderer` ≥ 1.0 (both bundled in the Flatpak) and host Linux ≥ 6.13 (needed by QEMU to map GPU memory into a VM). Check host support under *Settings → System → Capabilities*. QEMU accepts Native and Vulkan only together with it, so choosing either switches it on and locks it (*Required by …*); with Universal alone it is a choice of its own, which returns once neither needs it. *Memory limit* below it sets the size of the memory window (see below). *Advanced* opens by itself for VMs whose settings there differ from the defaults.

In the viewer, a status bar icon appears for VMs with 3D acceleration. Its tooltip lists what KEMU started QEMU with and, once `kemu-agent` reports it, which OpenGL driver the guest uses, in the terms of the edit dialog: Universal or Native (each with the host GPU's renderer), Zink on Vulkan, or software rendering (`llvmpipe`). A warning emblem marks software rendering despite 3D acceleration, Native the host does not offer or the guest does not use, and Vulkan the host does not offer.

*Video memory* and *Memory limit* accept any value within their range; the arrows step through powers of two, and the adjacent button lists them.

| Field                        | Range         | Note                                  |
| ---------------------------- | ------------- | ------------------------------------- |
| Video memory, Standard VGA   | 4–256 MiB     | QEMU rounds up to a power of two      |
| Video memory, ISA VGA        | 1–16 MiB      | QEMU rounds up to a power of two      |
| Video memory, Cirrus VGA     | 4, 8, 16 MiB  | Other sizes snap to these             |
| Video memory, QXL            | 8–256 MiB     | QEMU rounds up to a power of two      |
| Video memory, VirtIO         | 16–2048 MiB   | Limits host memory for 2D resources   |
| Video memory, VirtIO with 3D | Automatic     | Allocated dynamically by the host GPU |
| Memory limit                 | 256 MiB–8 GiB | Powers of two only; 1 GiB by default  |

*Memory limit* specifies how much host GPU memory the guest can map at once (`hostmem`). This reserves guest address space only; the host backs pages on demand where mapped. Host blobs require no shared memory for guest RAM, so memory preallocation behaves normally. Exported `qemu.sh` scripts never enable host blobs, Vulkan, or native contexts so they remain compatible with older QEMU releases.

On Intel CPU hosts, KVM ignores guest memory types by default and maps guest memory write-back. When the host itself maps GPU memory write-combined—such as on Meteor Lake, `xe` graphics, and dedicated GPUs—the guest reads stale data and Vulkan hangs (`MESA-VIRTIO: debug: stuck in fence wait` in the guest). For Vulkan VMs, KEMU therefore passes `-accel kvm,honor-guest-pat=auto` when QEMU supports it (11.0 or later), taking effect on host Linux ≥ 6.16. Native contexts map GPU memory the same way and get the option too. OpenGL-only VMs retain KVM's default because enabling the option causes Firefox's GPU compositor (WebRender) to stutter during video playback, presumably from reading write-combined memory. In Vulkan VMs on affected Intel hosts, Firefox exhibits the same stutter; set `gfx.webrender.software` to `true` in `about:config` for smooth playback (Chromium is unaffected). AMD CPUs always honour guest memory types. Check host status under *Settings → System → Capabilities* (*Guest memory types*).

## kemu-agent

`kemu-agent` is KEMU's guest agent for Linux (`agent/` in this repository). Running inside the user's graphical session, it:

- Shares the clipboard—and optionally the primary selection—in every format an application offers (text, HTML, images, and application formats)
- Reports the guest's monitor layout so the pointer lands accurately on every display regardless of per-monitor scaling

It works on Wayland (KDE Plasma, wlroots compositors such as Sway and Hyprland, GNOME via Xwayland) and on X11 desktops. It replaces `spice-vdagent`, which KEMU no longer supports, while `qemu-guest-agent` continues to run alongside it.

KEMU attaches a virtio-serial port named `org.kde.kemu.agent.0` to every VM using the integrated viewer and a PCI bus; existing VMs receive the port automatically on their next start. The KEMU icon in the viewer's status bar reflects the agent's state—dimmed when disconnected, with a warning emblem when it lacks access to the guest's clipboard—and its tooltip lists the agent version, guest session, and supported capabilities.

### Installing it in a guest

*Insert kemu-agent CD Image…*—available in the viewer's *Devices* menu, behind the `kemu-agent` status bar icon, and under *VM → Removable Media*—inserts an ISO containing the agent into the VM's CD drive, much like guest tools in other hypervisors. Open a terminal in the guest and run:

```sh
sudo sh /run/media/$USER/KEMU_AGENT/install.sh
```

On Debian and Ubuntu, the disc mounts at `/media/$USER/KEMU_AGENT` instead; if it is not mounted automatically, run `sudo mount -o ro /dev/sr0 /mnt && sudo sh /mnt/install.sh`. The installer places the agent in `/opt/kemu-agent` and starts it in running sessions; inserting a newer CD image updates an existing installation, and `sudo sh /opt/kemu-agent/uninstall.sh` removes it. Because clipboard sharing is unavailable until the agent runs, the command must be typed manually (`README.txt` on the disc repeats these instructions). After inserting the disc, KEMU also displays the instructions and, if an agent is already running, compares its version against the disc.

KEMU builds the ISO from `kemu-agent` bundles (see `agent/README.md`, "Portable bundle")—both those installed under `share/kemu/agent/<arch>/` (a build of their own, or installed together with KEMU; see the README, "kemu-agent bundle for guests") and any bundle selected by the user. If no bundle matches the guest's architecture, KEMU explains where bundles come from and prompts you to choose a bundle tarball, remembering the selection. The image includes the newest bundle per architecture alongside an `install.sh` script that selects the bundle matching `uname -m`. It is cached at `~/.local/share/kemu/agent-medium/kemu-agent.iso`, rebuilt only when bundles change, and replaces an already-inserted disc only after confirmation. The menu item is disabled for Windows, BSD, and DOS guests.

Current version limits:

- Clipboard content is fetched as soon as either side offers it, up to 32 MiB per copy; larger content remains on the originating side.
- Files and folders transfer only with *Share Files* enabled (see above). They are copied when placed on the clipboard rather than when pasted, so each copy is subject to the size limit; symbolic links are skipped, and cut operations behave like copy.
- Windows and BSD guests have no agent yet and therefore no clipboard sharing. KEMU checks the operating system configured for the VM, though if an agent connects anyway, it is used.

The protocol between KEMU and the agent is specified in [kemu-agent-protocol.md](kemu-agent-protocol.md).
