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
- Linked clones
- Encrypted system disk and TPM state (LUKS)
- Expand disks
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

- Additional disks are always qcow2 images inside the VM directory; host
  block devices and images elsewhere on the host cannot be attached directly.
- Disks cannot be added or removed while the VM is running.
- Pointer capture in the D-Bus viewer requires a Wayland session. Under X11 or
  XWayland the capture action is disabled; guests with an absolute pointing
  device (tablet) are unaffected and work everywhere.
- Virtual network NAT is IPv4-only and outbound ICMP echo (ping) from guests is
  not translated by the NAT (a limitation of the embedded libslirp stack).

## Gallery

![image](screenshots/fullview.png) ![image](screenshots/newvm.png)
![image](screenshots/vmsettings.png) ![image](screenshots/snapshotdialog.png)

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
- **Optional:** `swtpm` (for TPM 2.0 emulation) and its `swtpm_ioctl` (for
  encrypted VMs with a TPM)
- **Optional:** `virtiofsd` (for shared folders with Windows guests)

### Arch Linux

```sh
sudo pacman -S qt6-base qt6-declarative extra-cmake-modules \
  kconfig kcoreaddons ki18n kxmlgui kwidgetsaddons kdbusaddons \
  kiconthemes kwallet qemu-full edk2-ovmf libarchive libslirp libisofs \
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
  libkf6texttemplate-dev libkf6wallet-dev qemu-system extra-cmake-modules \
  libegl1-mesa-dev libarchive-dev libslirp-dev libisofs-dev libudev-dev \
  libkf6dbusaddons-dev libkf6dbusaddons-dev build-essential cmake \
  libwayland-dev wayland-protocols qt6-base-private-dev \
  build-essential cmake
```

Optional:

```sh
sudo apt install ovmf swtpm swtpm-tools virtiofsd
```

### Fedora 40+

```sh
sudo dnf install qt6-qtbase-devel qt6-qtdeclarative-devel qt6-qtsvg-devel \
  qt6-qtmultimedia-devel kf6-kconfig-devel kf6-kcoreaddons-devel kf6-ki18n-devel \
  kf6-kio-devel kf6-kcrash-devel kf6-kxmlgui-devel kf6-kwidgetsaddons-devel \
  kf6-kiconthemes-devel kf6-ktexttemplate-devel kf6-kdbusaddons-devel \
  kf6-kwallet-devel extra-cmake-modules qemu-system-x86 qemu-img edk2-ovmf \
  swtpm swtpm-tools mesa-libEGL-devel libarchive-devel libslirp-devel \
  libisofs-devel systemd-devel gcc-c++ make cmake wayland-devel \
  wayland-protocols-devel qt6-qtbase-private-devel systemd-devel \
  gcc-c++ make cmake
```

Optional:

```sh
sudo dnf install edk2-ovmf swtpm swtpm-tools virtiofsd
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
| Downloaded templates        | `~/.local/share/kemu/ghns/`                      |
| Configuration               | `~/.config/kemurc`                               |

Each VM is stored as a subdirectory containing `config.json`, `disk.qcow2`,
`OVMF_VARS.fd` (optional), and `qemu.sh`. Additional disks are stored next to
the system disk as `disk-2.qcow2`, `disk-3.qcow2`, and so on.

## OS Templates

Templates are managed under *Settings → Templates*: create new ones, duplicate
or customize existing ones (OS templates as JSON), reset customizations, hide
OS templates you do not need, and share OS templates as packages
(`.kemu-template.tar.gz` with `template.json` and the cloud-init templates it
uses). Changes take effect immediately.

The *Cloud-Init Templates* tab works the same way for the `.yaml.tpl` files,
with a list of the available placeholders and a preview rendered from the
deployment defaults. Templates named `network-*` provide the network
configuration, all others the user data. A user-data template other than the
one of the OS template can be chosen in the cloud image configuration of a new
VM.

OS presets and Quick Deploy sources come from the template database:
`data/templates.json` holds the global QEMU defaults, `data/templates.d/` one
JSON file per template. Downloaded templates (`~/.local/share/kemu/ghns/`,
one package per subdirectory) come on top of the shipped ones, and the user's
own templates in `~/.local/share/kemu/data/templates.d/` on top of both:

- A file replaces the shipped file of the same name; an empty file (or a
  symlink to `/dev/null`) hides it.
- A template with an existing `id` replaces the shipped one completely.
- `sort_order` sets the position within the OS group (shipped: 100–199,
  default: 500, "Other": 1000).
- `cloud_init_template` and `network_template` name a `.yaml.tpl` file, looked
  up in the user's `cloud-init-templates/` first, then among the downloaded
  ones (including the template packages), then among the shipped ones.
- Templates listed in `~/.local/share/kemu/data/hidden-templates.json` are not
  offered for new VMs.

Minimal example (`mydistro.json`):

```json
{
  "version": 1,
  "templates": [
    {
      "id": "mydistro",
      "display_name": "My Distro",
      "os_family": "linux",
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

Valid `os_family` values are `linux`, `windows`, `bsd`, `dos` and `other`.
Templates are read at startup; malformed files are skipped with a warning (see
[Debug Logging](#debug-logging)).

## Secure Boot Firmware Package Names

| Distro            | Package            | Typical OVMF Path                             |
| ----------------- | ------------------ | --------------------------------------------- |
| **Arch Linux**    | `edk2-ovmf`        | `/usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd` |
| **Debian/Ubuntu** | `ovmf`             | `/usr/share/OVMF/OVMF_CODE_4M.secboot.fd`     |
| **Fedora**        | `edk2-ovmf`        | `/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd`   |
| **openSUSE**      | `qemu-ovmf-x86_64` | `/usr/share/qemu/ovmf-x86_64-smm-ms-code.bin` |

A firmware directory set under Settings -> UEFI Firmware is searched first.
Each UEFI VM keeps its own copy of the matching VARS template as
`OVMF_VARS.fd`, which holds its NVRAM. KEMU creates it when the VM is created
or UEFI is enabled, and on start if it is missing; an existing file is never
replaced.

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

## Linked Clones

A linked clone is created in seconds and takes almost no space: Instead of
copying the disks it stores only its own changes on top of the disks of the
source VM (qcow2 overlays). This suits e.g. throwaway test machines. Select
"Create linked clone" in the Clone VM dialog.

- The source VM is frozen while linked clones depend on it: It cannot be
  started, edited, renamed, deleted, compacted or reverted to a snapshot. It
  can still be exported and further clones of it can be made. It is released
  as soon as its last linked clone is deleted or made independent.
- The source must be shut down.
- A linked clone cannot be the source of another linked clone; it can be cloned
  in full, though.
- "Make Independent" copies the shared data into the clone, which then needs
  as much space as a full clone. Snapshots of the clone are lost in the process.
- Export, OVA export and full clones of a linked clone always contain complete
  disks.
- Each overlay refers to its source by a relative path (`../<source>/disk.qcow2`),
  so the VM storage location can be moved as a whole.

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

## Encrypted Disks

A new virtual machine can store its disks encrypted: select "Encrypt the
disks" in the New Virtual Machine dialog and choose a password. The disks are
qcow2 images with built-in LUKS encryption. Warning: Without the password the
content cannot be recovered.

- The password can be stored in KWallet (folder "KEMU"). Otherwise KEMU asks
  for it. Within the VM settings you can set or remove encryption later.
  Changing the password only replaces the LUKS key slots and does not rewrite
  the disk(s).
- The TPM state is encrypted with the same password (`swtpm --key`). Moving an
  existing state over needs `swtpm_ioctl`, which comes with swtpm (Debian and
  Ubuntu: package `swtpm-tools`).
- Encrypting an existing VM copies the system disk, so it needs free space of
  about its current size. Its snapshots are deleted also on the additional
  disks. The unencrypted original is deleted.
- Linked clones and VMs with a saved state cannot be encrypted.
- Additional disks stay unencrypted; encrypt them inside the guest if needed.
  KEMU hands the password to QEMU and qemu-img through a pipe - never on the
  command line. The exported `qemu.sh` asks for it or reads it from the
  `KEMU_DISK_PASSWORD` environment variable.
- Save State is not available
- Not encrypted are the UEFI variable store and the cloud-init seed image,
  which holds the user data of a deployment.
- Not yet available for encrypted VMs: snapshots, compacting, cloning and
  export.

## QEMU Guest Agent

Every VM with a PCI bus has a channel for the QEMU guest agent
(Advanced -> "Connect the QEMU guest agent", on by default). Inside the
guest, the agent comes with the package `qemu-guest-agent` on Linux and
with the virtio-win guest tools on Windows. While a VM runs, its details
show whether the agent is connected and which version it has.

- KEMU talks to the agent through a socketpair, like to the QEMU monitor:
  there is no socket file other processes could connect to. An exported
  `qemu.sh` lets QEMU listen on `qemu-ga.sock` in the VM folder instead.
- The agent can also run programs and read files in the guest
  (`guest-exec`, `guest-file-*`). KEMU does not use these commands.
- Machines without a PCI bus (ISA PC) have no channel.

With the agent connected, the details of a running VM also show the
guest's host name, operating system and IP addresses (refreshed every 30
seconds; a click copies an address) and the VM list shows the first
address next to the uptime. "Shut Down" then goes through the agent, which
works even if the guest ignores the ACPI power button. Without an agent
KEMU sends the ACPI signal.

## Additional Disks

The system disk is all most VMs need. Further disks can be added in the VM
settings under Hardware -> Additional Disks, either as new, empty images or by
copying an existing image (qcow2, raw, VMDK, VDI, VHD/VHDX) into the VM
directory. The original image is not modified.

- Each disk has its own controller: VirtIO, NVMe or SATA/IDE. New disks default
  to the controller of the system disk, for which the guest already has a
  driver.
- SATA/IDE slots are shared with the CD/DVD drive: Q35 (AHCI) offers six, the
  PC chipsets four. VirtIO and NVMe need a PCI bus.
- Every disk gets a stable serial number (`KEMU-DISK-2`, ...), so Linux guests
  find it under `/dev/disk/by-id/`.
- Snapshots always cover all disks. A disk added after a snapshot was taken
  keeps its content when the VM is reverted to that snapshot. Once a disk has
  been removed, snapshots with a saved running state can only be restored while
  the VM is shut down.
- QEMU cannot migrate its NVMe controller: With a NVMe disk attached, neither
  Save State nor snapshots of the running VM are available.
- Compacting, cloning, export and OVA export include all disks.
- When removing a disk its image can be deleted or moved to another location.

## System Identity

Every new VM gets a random system UUID, which the guest reads from its
firmware (`/sys/class/dmi/id/product_uuid` on Linux,
`Get-CimInstance Win32_ComputerSystemProduct` on Windows). Under
Advanced -> System Identity the UUID can be changed and the SMBIOS system
information (manufacturer, product, version, serial number, SKU, family) can
be set. Empty fields keep QEMU's defaults.

- VMs created with older KEMU versions have no UUID, and QEMU reports all
  zeros. KEMU does not assign one on its own, because a changed identity can
  affect software licensing and Windows activation in an installed guest.
- A clone gets a UUID of its own unless the source has none; the SMBIOS
  strings are copied. An imported archive keeps its UUID unless the source VM
  still exists, an OVA import gets a new one.
- SMBIOS strings are limited to printable ASCII and 64 characters.

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

- The import keeps the guest's firmware (BIOS/UEFI, Secure Boot) and boot
  controller where the descriptor states them (common vendor extensions), so
  the guest finds the hardware it was installed on: UEFI or a SATA controller
  selects Q35, NVMe disks stay on NVMe. IDE, SCSI and unknown controllers
  become SATA/IDE. Network adapters without a QEMU counterpart (vendor
  specific models, E1000e) become E1000. The operating system is taken over as well.
  Descriptors in the old OVF 0.9 layout are read too.
- If the PC chipset runs out of disk slots, the import switches to Q35.
- License terms an appliance ships with (`EulaSection`) are shown before the
  import starts and have to be accepted.
- A manifest (`.mf`) in the archive is verified; a checksum mismatch stops the
  import. Signatures (`.cert`) are not checked.
- Compressed (`ovf:compression`) or chunked disk files are not supported.
- The export describes the hardware the way common tools read it: firmware
  and Secure Boot (vendor extension), controller per disk (AHCI on Q35, IDE on
  the PC chipsets, NVMe), the network adapter model, the operating system and
  the VM's comment. VirtIO disks have no OVF equivalent and are
  exported on AHCI.
- Some tools read the firmware (UEFI), Secure Boot and MAC address only from
  their own machine section, never from a plain OVF. A UEFI machine imported
  there may start with BIOS; enable EFI in its settings afterwards.
- TPM state, the UEFI variable store, the system UUID and the SMBIOS system
  information are not exported. For a faithful copy between KEMU
  installations, use the native export.
- Snapshots are not preserved during OVA export (as VMDK does not support them).
- The shared folder path and ISO path are stripped from native exports because
  they are system-specific.
- OVF parsing covers OVAs of common hypervisors, but it may fail to handle
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
