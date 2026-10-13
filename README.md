<!--
SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>

SPDX-License-Identifier: GPL-3.0-or-later
-->

# KEMU – KDE QEMU Virtual Machine Manager

A traditional, direct-to-QEMU desktop frontend for Linux, built with Qt 6
Widgets and KDE Frameworks 6 for deep desktop integration.

**Disclaimer:** This project is in early-stage development.

## Features

- Create, edit, and delete QEMU virtual machines
- Integrated D-Bus viewer with audio and clipboard sharing
- Automated deployment utilizing Cloud-Init
- Snapshot support (QCOW2)
- Hot-swapping of ISO and floppy images
- Networking: User-mode networking or in-process virtual networks
  (in-memory L2 switch with built-in DHCP and optional libslirp NAT)
- Shared folders via 9p/VirtFS (Linux/BSD) and virtio-fs (Windows)
- Portable VM storage: VMs are stored as directories containing `config.json`,
  `disk.qcow2`, and a `qemu.sh` start script using relative paths
- Export and import VMs as compressed archives (`.kemu.tar.zst`) for seamless
  backup or migration
- OVA export and import (limited) for cross-platform interoperability

## Limitations

- Currently limited to a single storage drive per virtual machine.
- Pointer capture in the D-Bus viewer requires a Wayland session. Under X11 or
  XWayland the capture action is disabled; guests with an absolute pointing
  device (tablet) are unaffected and work everywhere.
- Virtual network NAT is IPv4-only, and outbound ICMP echo (ping) from guests is
  not translated by the NAT (a limitation of the embedded libslirp stack).

## Gallery

![image](screenshots/fullview.png) ![image](screenshots/mainwindow.png)
![image](screenshots/newvm.png) ![image](screenshots/vmsettings.png)
![image](screenshots/vmsettings2.png) ![image](screenshots/snapshotdialog.png)

## Dependencies

- Qt 6
- KDE Frameworks 6
- QEMU >= 7.2
- Wayland (client library, protocols and `wayland-scanner`; build-time and
  runtime)
- Qt 6 private headers (`Qt6::GuiPrivate`, for the Wayland surface of a window)
- libarchive
- libslirp >= 4.7 (NAT and DNS proxy for virtual networks)
- libisofs (cloud-init seed ISOs)
- libudev (for USB device passthrough)
- **Optional:** `edk2-ovmf` (for UEFI boot)
- **Optional:** `swtpm` (for TPM 2.0 emulation)
- **Optional:** `virtiofsd` (for shared folders with Windows guests)

### Arch Linux

```sh
sudo pacman -S qt6-base qt6-declarative extra-cmake-modules \
  kconfig kcoreaddons ki18n kxmlgui kwidgetsaddons kdbusaddons \
  kiconthemes qemu-full edk2-ovmf libarchive libslirp libisofs \
  wayland wayland-protocols
```

Optional:

```sh
sudo pacman -S swtpm virtiofsd
```

### Debian 13+ / (K)Ubuntu 26.04+ / KDE neon

```sh
sudo apt install qt6-base-dev qt6-declarative-dev qt6-svg-dev qt6-multimedia-dev \
  libkf6config-dev libkf6coreaddons-dev libkf6i18n-dev libkf6kio-dev \
  libkf6crash-dev libkf6xmlgui-dev libkf6widgetsaddons-dev libkf6iconthemes-dev \
  libkf6texttemplate-dev qemu-system extra-cmake-modules libegl1-mesa-dev \
  libarchive-dev libslirp-dev libisofs-dev libudev-dev libkf6dbusaddons-dev \
  libkf6dbusaddons-dev build-essential cmake \
  libwayland-dev wayland-protocols qt6-base-private-dev \
  build-essential cmake
```

Optional:

```sh
sudo apt install ovmf swtpm virtiofsd
```

### Fedora 40+

```sh
sudo dnf install qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel \
  qt6-qtmultimedia-devel kf6-kconfig-devel kf6-kcoreaddons-devel kf6-ki18n-devel \
  kf6-kio-devel kf6-kcrash-devel kf6-kxmlgui-devel kf6-kwidgetsaddons-devel \
  kf6-kiconthemes-devel kf6-ktexttemplate-devel kf6-kdbusaddons-devel \
  extra-cmake-modules qemu-system-x86 qemu-img edk2-ovmf swtpm swtpm-tools \
  mesa-libEGL-devel libarchive-devel libslirp-devel libisofs-devel \
  systemd-devel gcc-c++ make cmake wayland-devel wayland-protocols-devel \
  qt6-qtbase-private-devel systemd-devel \
  gcc-c++ make cmake
```

Optional:

```sh
sudo dnf install edk2-ovmf swtpm virtiofsd
```

## Building and Running

To **compile** and run KEMU:

```sh
cmake -DCMAKE_VERBOSE_MAKEFILE=ON -DCMAKE_BUILD_TYPE=Debug -B build/
cmake --build build/
cd build/bin
./kemu
```

### Local Installation (Recommended for testing)

Installs KEMU to your user's home directory (`~/.local`). This requires no root
privileges.

```sh
cmake -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local -B build/
cmake --build build/ --target install
```

**Note:** Ensure `~/.local/bin` is in your system's `$PATH`.

**Note:** The pointer capture implementation uses one Qt private header
(`QNativeInterface::Private::QWaylandWindow`) to obtain the `wl_surface` of the
viewer window. There is no public API for this. As a consequence, KEMU must be
rebuilt after a Qt minor version upgrade.

### System-wide Installation

Installs KEMU system-wide for all users (usually under `/usr/local/`).

```sh
cmake -DCMAKE_BUILD_TYPE=Release -B build/
sudo cmake --build build/ --target install
```

To **uninstall**:

```sh
sudo make uninstall
```

## Debug Logging

KEMU utilizes Qt logging categories. Two primary categories are defined:

| Category                   | Covers                                                                                      |
| -------------------------- | ------------------------------------------------------------------------------------------- |
| `org.kde.kemu`             | Main application, VM lifecycle, networking (switch, DHCP, NAT uplink), QMP, snapshots, etc. |
| `org.kde.kemu.dbusdisplay` | Integrated D-Bus display viewer (frame-level traffic)                                       |

By default, only warnings and critical messages are printed. To enable debug
output for a single run:

```sh
QT_LOGGING_RULES="org.kde.kemu.debug=true" kemu
QT_LOGGING_RULES="org.kde.kemu*=true" kemu # enables both categories
```

If `kdebugsettings` is installed, KEMU's categories will appear there and can be
toggled via the GUI.

When filing a bug report, please attach the full debug output:

```sh
QT_LOGGING_RULES="org.kde.kemu*=true" kemu 2>&1 | tee kemu.log
```

## File Locations

| Purpose                     | Path                                             |
| --------------------------- | ------------------------------------------------ |
| VM data                     | `~/.local/share/kemu/vms/`                       |
| User OS templates           | `~/.local/share/kemu/data/templates.d/`          |
| User Cloud-Init templates   | `~/.local/share/kemu/data/cloud-init-templates/` |
| Configuration               | `~/.config/kemurc`                               |

Each VM is stored as a subdirectory containing `config.json`, `disk.qcow2`,
`OVMF_VARS.fd` (optional), and `qemu.sh`.

## OS Templates

OS presets and Quick Deploy sources come from the template database:
`data/templates.json` holds the global QEMU defaults, `data/templates.d/` one
JSON file per template. Templates in `~/.local/share/kemu/data/templates.d/`
are added on top:

- A file replaces the shipped file of the same name; an empty file (or a
  symlink to `/dev/null`) hides it.
- A template with an existing `id` replaces the shipped one completely.
- `sort_order` sets the position within the OS group (shipped: 100–199,
  default: 500, "Other": 1000).
- `cloud_init_template` and `network_template` name a `.yaml.tpl` file, looked
  up in the user's `cloud-init-templates/` first.

Minimal example (`mydistro.json`):

```json
{
  "version": 1,
  "templates": [
    {
      "id": "mydistro",
      "display_name": "My Distro",
      "os_type": "linux",
      "cloud_init_template": "debian",
      "network_template": "network-v2",
      "releases": [
        {
          "version": "1.0",
          "cloud_images": [
            { "arch": "x86_64", "url": "https://example.org/mydistro-1.0.qcow2" }
          ]
        }
      ]
    }
  ]
}
```

Valid `os_type` values are `linux`, `windows`, `bsd`, `dos` and `other`.
Templates are read at startup; malformed files are skipped with a warning (see
[Debug Logging](#debug-logging)).

## Secure Boot Firmware Package Names

| Distro            | Package            | Typical OVMF Path                             |
| ----------------- | ------------------ | --------------------------------------------- |
| **Arch Linux**    | `edk2-ovmf`        | `/usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd` |
| **Debian/Ubuntu** | `ovmf`             | `/usr/share/OVMF/OVMF_CODE_4M.secboot.fd`     |
| **Fedora**        | `edk2-ovmf`        | `/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd`   |
| **openSUSE**      | `qemu-ovmf-x86_64` | `/usr/share/qemu/ovmf-x86_64-smm-ms-code.bin` |

## Display Backends

KEMU supports three display backends, which can be selected per VM in the VM
settings:

| Backend   | Window          | 3D Acceleration | Audio Routing     | Clipboard Sharing | External Process |
| --------- | --------------- | --------------- | ----------------- | ----------------- | ---------------- |
| **SDL**   | QEMU built-in   | Yes (Virtio)    | System (PipeWire) | No                | None             |
| **D-Bus** | KEMU integrated | Yes (Virtio)    | Routed via D-Bus  | Yes (built-in)    | None             |

**SDL** is the simplest options: QEMU opens its own window directly, with
optional OpenGL passthrough for hardware-accelerated graphics.
The **D-Bus** backend combines hardware-accelerated rendering inside a viewer
window managed by KEMU itself, featuring built-in audio and clipboard sharing.

### D-Bus Display

**Hint:** Although stable, the D-Bus display backend is under active
development. Expect bugs, missing features, and potential breaking changes
between versions. It is not fully recommended for production use.

The D-Bus display backend embeds the VM's framebuffer directly into a
KEMU-managed Qt window. It uses QEMU's `-display dbus` interface to receive
video, send keyboard and mouse events, and exchange audio and clipboard data.
KEMU starts QEMU with `-display dbus,p2p=on` and hands it one end of a Unix
socket pair via QMP's `add_client` command. This establishes a direct,
peer-to-peer D-Bus connection between KEMU and QEMU.

**Features:**

- **Hardware-accelerated graphics**: When the VM uses Virtio-GPU with 3D
  acceleration, frames are passed as DMA-BUF descriptors over D-Bus and imported
  directly into the viewer's OpenGL context for zero-copy, GPU-to-GPU
  performance.
- **Bidirectional audio**: Playback (host speakers) and capture (host
  microphone) are routed through D-Bus to QEMU's `audiodev=dbus` backend, using
  PipeWire on the host side via Qt Multimedia.
- **Clipboard synchronization**: Toggle between off, guest->host, host->guest,
  or bidirectional synchronization in the viewer's Devices menu. This requires a
  clipboard agent in the guest (SPICE vdagent).
- **Reconnect support**: Closing the viewer window switches the VM to headless
  mode instead of terminating it. You can reconnect later via the VM's context
  menu.

**Requirements:**

- QEMU must support peer-to-peer D-Bus display connections
  (`-display dbus,p2p=on`), available since QEMU 7.0 and covered by the general
  dependency requirement above.
- The guest requires no special drivers for video or input; QEMU handles
  everything on its side. For clipboard features, simply install the standard
  SPICE vdagent in the guest.
  
### Wayland guests and clipboard sharing

When running a Wayland-based Linux guest, clipboard sharing from the guest to
the host can sometimes fail or behave inconsistently.

This issue occurs because many clipboard integration tools rely on X11 protocol
calls (`xclip`/`xsel`), which are not yet (or running within older guests)
natively supported under Wayland without a bridging mechanism.

Create a shell script that continuously syncs the Wayland clipboard to the
X11 clipboard buffer:

```sh
#!/bin/sh
# clip-bridge.sh: Syncs Wayland clipboard buffer to X11 (xclip)
new=$(cat; printf x); new=${new%x}
cur=$(xclip -selection clipboard -o -t UTF8_STRING 2>/dev/null; printf x); cur=${cur%x}
[ "$new" = "$cur" ] && exit 0
printf %s "$new" | xclip -selection clipboard -i -t UTF8_STRING
```

Either run it manually with

```sh
wl-paste --type text --watch /path/to/clip-bridge.sh
```

or create the service file at `~/.config/systemd/user/wl-clip-bridge.service`:

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

and enable and start with:

```sh
systemctl --user daemon-reload
systemctl --user enable --now wl-clip-bridge.service
```

## Networking

KEMU offers two network modes, both running entirely in userspace without
requiring root privileges.

| Mode                  | Backend                                  | VM-to-VM | DHCP              | NAT            | Port Forwarding |
| --------------------- | ---------------------------------------- | -------- | ----------------- | -------------- | --------------- |
| **User Mode (SLIRP)** | QEMU built-in                            | No       | Yes (10.0.2.0/24) | Yes            | Yes             |
| **Virtual Network**   | In-process L2 switch + embedded libslirp | Yes      | Yes (optional)    | Yes (optional) | Yes             |

### User Mode

Each VM gets its own isolated network connection through QEMU's built-in SLIRP
stack. Port forwarding rules can be configured per VM to expose guest services
on the host.

### Virtual Network

Virtual networks connect multiple VMs to a shared private subnet. The entire
stack runs inside the KEMU process:

```text
┌─── KEMU process ────────────────────────────────────────────────┐
│                                                                 │
│   VirtualSwitch (in-memory L2, one port per VM)                 │
│    ├── QEMU VM1  (-netdev stream, own socketpair)               │
│    ├── QEMU VM2  (-netdev stream, own socketpair)               │
│    ├── DhcpServer (optional, built-in)                          │
│    └── SlirpUplink (optional: NAT + DNS proxy)                  │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

Each virtual network runs an in-memory layer-2 switch. Every VM gets its own
port: one end of a socketpair that QEMU inherits as a file descriptor
(`-netdev stream,addr.type=fd`). Nothing is created in the file system, so no
other process can attach to a port, and a port only accepts frames from the VM's
configured MAC address, so guests cannot impersonate each other; VMs with the
same MAC cannot join the same network. The switch floods broadcast and multicast
frames; frames for unknown destinations go to the VMs only, never to the NAT
uplink. Because nothing touches the host kernel or spawns helper processes,
virtual networks work unchanged inside sandboxed (Flatpak) environments.

**Features per network (individually configurable):**

- **DHCP**: A lightweight DHCPv4 server built into KEMU assigns addresses
  automatically. It can be disabled for static IP setups or Cloud-Init
  deployments.
- **NAT**: An embedded libslirp instance terminates guest TCP/UDP connections in
  userspace and re-originates them on the host, including a DNS proxy. When
  disabled, the network is fully air-gapped (the gateway address still answers
  ARP and ping so static setups remain diagnosable) and the built-in DNS server
  only answers for the local zone.
- **Host access**: Off by default. When enabled (NAT only), guests reach
  services listening on the host's `127.0.0.1` via the gateway address.
- **Port forwarding**: Exposes ports of a specific guest VM on the host, bound
  to `127.0.0.1`. Format: `proto/hostPort:guestIp:guestPort`, e.g.
  `tcp/8080:192.168.100.50:80`.

**Requirements:**

- QEMU >= 7.2 (the `stream` network backend used for VM attachment).
- libslirp >= 4.7 (build-time dependency, used for NAT and the DNS proxy).

**Automatic cleanup:** When the last VM detaches from a virtual network, KEMU
stops the network and closes its descriptors. A network leaves nothing behind in
the file system, and no state survives outside the KEMU process.

**Exported VMs:** Start scripts generated for exported VMs connect the VMs of
the same virtual network via QEMU multicast networking. This provides layer-2
connectivity between the exported VMs only - there is no DHCP server and no
internet access, so configure static addresses (manually or via Cloud-Init).

**Debugging:** Virtual network activity (switch frames, DHCP exchanges, NAT
traffic) is logged under the `org.kde.kemu` logging category; see
[Debug Logging](#debug-logging).

## Sandbox Mode

Some of KEMU's features rely on Linux user namespaces, which sandbox
environments block by design. When running in a restricted environment, KEMU
detects this automatically and adjusts its behavior to prevent runtime failures.
Features may be manually disabled for testing purpose with
`--disable-capability ...` cli option.

- **Settings:** The Executables page is read-only since all helper programs are
  bundled within the Flatpak image and cannot be replaced with host binaries.
  Existing configurations are preserved, so they will still be available if you
  later switch to a native installation.
- **Firmware:** UEFI firmware is included with the bundled QEMU build. If a
  specific OVMF variant is needed, KEMU searches the host's firmware
  directories. Setting a custom path under _Settings -> Firmware_ works as
  expected.
- **Start scripts:** The generated `qemu.sh` and the "Show QEMU command" dialog
  reference paths inside the sandbox (e.g., `/app/bin/...`). These are intended
  for inspection and for execution via `flatpak run --command=...`, not for
  running directly on the host.
- **File locations:** VM data and configuration reside under
  `~/.var/app/org.kde.kemu/`, rather than `~/.local/share/kemu`. VMs from a
  previous native installation will not be detected automatically; update
  _Settings -> General -> VM directory_ to point to the old path if you wish to
  continue using them.
- **USB passthrough:** In addition to the udev rule described under
  [USB Passthrough](#usb-passthrough), the sandbox must be granted permission to
  access the device. Recent Flatpak versions prompt for this permission per
  device.

### QEMU Confinement

Whenever the QEMU binary supports it, KEMU starts QEMU with its seccomp sandbox
(`-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny`,
the set libvirt uses). A guest that breaks into QEMU can then neither start
programs nor change privileges. Options that make QEMU spawn helpers, such as
`-netdev tap,script=...` or `smb=` in the extra arguments, do not work under it;
any `-sandbox` option in the extra arguments replaces KEMU's. Exported start
scripts apply the same filter if the QEMU they find supports it.

## Shared Folders

KEMU enables host-guest file exchange via the following protocols:

| Guest OS      | Protocol    | Host Daemon | Guest Software          |
| ------------- | ----------- | ----------- | ----------------------- |
| **Linux/BSD** | 9p (VirtFS) | None        | Native kernel support   |
| **Windows**   | virtio-fs   | `virtiofsd` | VirtIO drivers + WinFsp |

Configuration is done in the VM settings. The _Shared Name_ serves as the mount
tag within the guest.

### Linux & BSD Guests

Mounting via 9p (Example: using the tag `exchange` mounted to `/mnt/shared`).

- **Linux:**

```sh
sudo mount -t 9p -o trans=virtio exchange /mnt/shared
```

_fstab:_ `exchange /mnt/shared 9p trans=virtio,version=9p2000.L,rw,nofail 0 0`

- **FreeBSD (13+):**

```sh
sudo mount -t virtfs -o trans=virtio exchange /mnt/shared
```

_fstab:_ `exchange /mnt/shared virtfs trans=virtio,rw 0 0`

> **Note:** OpenBSD/NetBSD support is limited; please verify compatibility
> beforehand. The virtual network mode note above applies to all Unix guests
> utilizing 9p.

#### Automount

To automatically mount the share upon starting your desktop environment, create
a `mount.desktop` file in `~/.config/autostart/` with the following content:

```ini
[Desktop Entry]
Type=Application
Name=mount
Exec=sudo mount -t 9p -o trans=virtio -oversion=9p2000.L exchange /home/myuser/exchange
```

### Windows Guest

Requires `virtio-fs`. On the host side, KEMU starts `virtiofsd` automatically.

The installation order is critical:

1. **Install WinFsp** ([winfsp.dev](https://winfsp.dev/rel/)).
2. **Install VirtIO Guest Tools** (Run `virtio-win-gt-x64.msi`).
3. **Reboot.**

If the drive doesn't appear, you can fix the service via PowerShell:

```powershell
# Check if the service is running:
Get-Service VirtioFsSvc

# Start it manually if stopped:
Start-Service VirtioFsSvc

# Set it to start automatically on boot:
sc.exe config VirtioFsSvc start=auto
```

## Screenshots

KEMU uses QEMU's `screendump` command to capture VM screenshots via the software
framebuffer. However, VirtIO-GL offloads rendering to the host GPU, bypassing
this framebuffer entirely. To enable screenshots, configure the display to use
`VGA` or `standard VirtIO` instead.

## USB Passthrough

KEMU supports hot-plugging USB devices into running virtual machines. QEMU
requires read/write access to `/dev/bus/usb/BUS/DEVICE`.

To allow access, create a udev rule (recommended):

```udev
# /etc/udev/rules.d/90-qemu-usb.rules
SUBSYSTEM=="usb", MODE="0664", GROUP="kvm"
```

Afterward, add your user to the `kvm` group. Reload the rules using:
`sudo udevadm control --reload-rules && sudo udevadm trigger`

**Security Warning:** This rule grants the `kvm` group full access to all host
USB devices (including your keyboard or private drives). For enhanced security,
restrict access to specific devices using their Vendor/Product IDs, as
demonstrated below:

```udev
# Kingston DataTraveler flash drive (example IDs)
SUBSYSTEM=="usb", ATTR{idVendor}=="0951", ATTR{idProduct}=="1666", MODE="0664", GROUP="kvm"
```

## Host Shutdown with Guest

KEMU can shut down the host once a guest has powered itself off, for example
after a long-running update or when a VM is used as the primary desktop.

- Per VM: Edit -> System -> When the guest shuts down.
- Per run: Shut Down Host with Guest in the VM menu, the context menu, or
  the viewer’s Machine menu. The VM setting is the default for each start.

Only a power-off initiated by the guest counts. Shutting the VM down from KEMU,
the exit policy, crashes, reboots and deployments never trigger it. If other
VMs are still running or busy the host stays on. Otherwise a 30-second
countdown allows cancelling.

On Plasma, the session is ended properly, so applications can save their work
or abort the shutdown. Elsewhere, KEMU asks logind directly; polkit may ask for
a password.

## OVA Import/Export Limitations

- OVA support is limited to single-disk VMs; multi-disk configurations are not
  supported.
- UEFI, TPM, and Secure Boot settings cannot be represented in the OVA format
  and will be lost upon export.
- Snapshots are not preserved during OVA export (as VMDK does not support them).
- The shared folder path and ISO path are stripped from native exports because
  they are system-specific.
- OVF parsing covers common VirtualBox/VMware OVAs, but it may fail to handle
  highly customized or exotic configurations.
- Host shutdown feature requires `--talk-name=org.kde.Shutdown` (Plasma) or
  `--system-talk-name=org.freedesktop.login1`. Without the option is disabled.

## Credits and Trademarks

**Third-Party Assets:** Operating system icons utilized within KEMU are provided
by the
[Papirus Icon Theme](https://github.com/PapirusDevelopmentTeam/papirus-icon-theme),
licensed under the GPL-3.0.

**Trademark Disclaimer:** Any trademarks, logos, and brand names used in this
software are the property of their respective owners. The use of third-party
logos within the KEMU user interface is strictly for identification and
descriptive purposes to represent the guest operating system of a virtual
machine.

KEMU is an independent, open-source project and is not affiliated with, endorsed
by, or sponsored by any mentioned trademark holder.

## License

GPL-3.0
