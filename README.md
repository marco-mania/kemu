# KEMU – KDE QEMU Virtual Machine Manager

KEMU is a desktop frontend for QEMU that runs virtual machines directly—without libvirt, background daemons, or root privileges. Built with Qt 6 Widgets and KDE Frameworks 6, it integrates tightly with the Plasma desktop.

**Development status:** KEMU is under active development.

KEMU requires a native Wayland session on the host; X11 and XWayland cannot be used as host display backends (guest operating systems can still use X11).

![image](screenshots/fullview.png) ![image](screenshots/newvm.png)
![image](screenshots/vmsettings.png) ![image](screenshots/vmsettings2.png)
![image](screenshots/snapshotdialog.png)

## Features

**Virtual machines and disks**

- Create VMs from OS templates: install from ISO images or deploy cloud images automatically via cloud-init
- Add disks, expand or compact disk images, and reclaim unused space
- Snapshots, saved states, and linked clones
- LUKS encryption for the system disk and TPM state, with passwords stored in KWallet
- TPM 2.0 (`swtpm`), UEFI with Secure Boot, and editable system identity (UUID, SMBIOS)

**Display and devices**

- Integrated QEMU D-Bus viewer with 3D acceleration, bidirectional audio, clipboard sharing, and pointer capture, or QEMU's native SDL window
- Experimental second monitor in a separate window
- Hot-swapping of ISO and floppy images; USB passthrough
- Shared folders via 9p (Linux/BSD) and virtio-fs (Windows)

**Networking**

- Per-VM user-mode networking, or virtual networks connecting VMs through an in-process switch with DHCP, DNS, and optional NAT—no root privileges, bridges, or helper processes required

**Desktop integration**

- System tray icon and desktop notifications
- Launch VMs directly from the application menu like native applications
- Command-line startup via `kemu --start <vm>`
- File associations for KEMU archives, OVA files, and template packages, plus a Dolphin action to create a VM from a disc image
- QEMU guest agent integration: displays guest hostname, IP address, and OS; handles graceful shutdown, space reclamation, and clock synchronisation after host sleep
- Pauses VMs on host sleep and prevents the session from ending while VMs are running

**Portability**

- Self-contained VM directories with `config.json`, disk images, and a `qemu.sh` start script using relative paths
- Import and export `.kemu.tar.zst` archives, or exchange OVA files with other tools

## Installation

### Flatpak

The Flatpak bundles QEMU, swtpm, and virtiofsd. To build and install it from source:

```sh
flatpak install flathub org.kde.Platform//6.11 org.kde.Sdk//6.11 \
    org.freedesktop.Sdk.Extension.rust-stable//25.08
flatpak-builder --user --install --force-clean build-dir org.kde.kemu.yml
```

For sandbox-specific behaviour and additional permissions, see [docs/flatpak.md](docs/flatpak.md).

### From source

**Requirements:**

- Qt ≥ 6.8 (Core, Gui, Widgets, Network, Svg, DBus, Multimedia, OpenGL) and its private headers
- Qt Wayland platform plugin (required at runtime)
- KDE Frameworks ≥ 6.13: CoreAddons, Config, ConfigWidgets, Crash, DBusAddons, I18n, IconThemes, JobWidgets, KIO, Notifications, NotifyConfig, StatusNotifierItem, TextTemplate, Wallet, WidgetsAddons, WindowSystem, XmlGui
- QEMU ≥ 7.2 (with Wayland-enabled SDL if using the SDL display backend)
- libarchive, libslirp ≥ 4.7, libisofs, libudev, EGL, Wayland (client library, protocols, `wayland-scanner`)

**Optional runtime dependencies:**

- `edk2-ovmf` / `ovmf` – UEFI and Secure Boot
- `swtpm` with `swtpm_ioctl` – TPM 2.0 and encrypted VMs with a TPM
- `virtiofsd` – shared folders for Windows guests

KEMU detects these programs at startup and disables features whose dependencies are missing. Results appear under *Settings → System → Executables*.

#### Arch Linux

```sh
sudo pacman -S cmake extra-cmake-modules qt6-base qt6-svg qt6-multimedia \
  kcoreaddons kconfig kconfigwidgets kcrash kdbusaddons ki18n kiconthemes \
  kjobwidgets kio knotifications knotifyconfig kstatusnotifieritem \
  ktexttemplate kwallet kwidgetsaddons kwindowsystem kxmlgui \
  libarchive libslirp libisofs wayland wayland-protocols qt6-wayland qemu-full
sudo pacman -S edk2-ovmf swtpm virtiofsd   # optional
```

#### Debian 13+ / (K)Ubuntu 26.04+ / KDE neon

```sh
sudo apt install build-essential cmake extra-cmake-modules \
  qt6-base-dev qt6-base-private-dev qt6-wayland-dev qt6-svg-dev qt6-multimedia-dev \
  libkf6coreaddons-dev libkf6config-dev libkf6configwidgets-dev libkf6crash-dev \
  libkf6dbusaddons-dev libkf6i18n-dev libkf6iconthemes-dev libkf6jobwidgets-dev \
  libkf6kio-dev libkf6notifications-dev libkf6notifyconfig-dev \
  libkf6statusnotifieritem-dev libkf6texttemplate-dev libkf6wallet-dev \
  libkf6widgetsaddons-dev libkf6windowsystem-dev libkf6xmlgui-dev \
  libarchive-dev libslirp-dev libisofs-dev libudev-dev libegl1-mesa-dev \
  libwayland-dev wayland-protocols qemu-system
sudo apt install ovmf swtpm swtpm-tools virtiofsd   # optional
```

#### Fedora 40+

```sh
sudo dnf install gcc-c++ cmake extra-cmake-modules \
  qt6-qtbase-devel qt6-qtbase-private-devel qt6-qtwayland qt6-qtsvg-devel qt6-qtmultimedia-devel \
  kf6-kcoreaddons-devel kf6-kconfig-devel kf6-kconfigwidgets-devel kf6-kcrash-devel \
  kf6-kdbusaddons-devel kf6-ki18n-devel kf6-kiconthemes-devel kf6-kjobwidgets-devel \
  kf6-kio-devel kf6-knotifications-devel kf6-knotifyconfig-devel \
  kf6-kstatusnotifieritem-devel kf6-ktexttemplate-devel kf6-kwallet-devel \
  kf6-kwidgetsaddons-devel kf6-kwindowsystem-devel kf6-kxmlgui-devel \
  libarchive-devel libslirp-devel libisofs-devel systemd-devel mesa-libEGL-devel \
  wayland-devel wayland-protocols-devel qemu-system-x86 qemu-img
sudo dnf install edk2-ovmf swtpm swtpm-tools virtiofsd   # optional
```

#### Build, install, uninstall

The source tree contains two components: KEMU (host) and `kemu-agent` in `agent/` (Linux guests, providing clipboard sharing and monitor layout; see [docs/display.md](docs/display.md)). Three build configurations are available:

| You want                 | Build it on | Configure                                | What `install` installs |
| ------------------------ | ----------- | ---------------------------------------- | ----------------------- |
| 1. KEMU only (default)   | the host    | `cmake -B build`                         | KEMU                    |
| 2. kemu-agent only       | the guest   | `cmake -S agent -B build-agent`          | kemu-agent              |
| 3. Both, in one build    | the host    | `cmake -B build -DKEMU_BUILD_AGENT=ON`   | KEMU only               |

##### 1. KEMU only

Build KEMU and run it from the build directory:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/bin/kemu
```

Install for the current user (no root required; ensure `~/.local/bin` is in `$PATH`):

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=~/.local
cmake --build build --target install
```

Install system-wide (typically `/usr/local`) or uninstall:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
sudo cmake --build build --target install
sudo cmake --build build --target uninstall
```

Because pointer capture relies on a private Qt API to access the viewer window's Wayland surface, KEMU must be rebuilt after upgrading to a new Qt minor version.

##### 2. kemu-agent only

Builds only the agent inside a guest. It requires neither KEMU's dependencies nor KDE libraries—only Qt ≥ 6.5 (Core, Gui, DBus), libwayland-client ≥ 1.20, Extra CMake Modules, `wayland-scanner`, and wayland-protocols. See [agent/README.md](agent/README.md) for details and installed files.

```sh
cmake -S agent -B build-agent -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-agent
sudo cmake --install build-agent
sudo cmake --build build-agent --target uninstall    # to remove it again
```

Install under `/usr` rather than your home directory, as it includes a udev rule and a systemd user unit. Afterwards, reload udev (`sudo udevadm control --reload && sudo udevadm trigger`) or reboot the guest. Guests without build tools can use the portable bundle from option 3 instead, which includes its own `install.sh`.

##### 3. KEMU and the agent in one build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release -DKEMU_BUILD_AGENT=ON
cmake --build build
sudo cmake --build build --target install            # installs KEMU only
```

The agent is built alongside KEMU and tested via `ctest`, while `install` omits it—it belongs in the guest, and installing it on the host would enable its user unit for every host user.

The same build tree can also package the agent as a *portable bundle*: a tarball containing the agent and its few runtime libraries, built in a container (Podman or Docker) against an old glibc and a static Qt. It runs on guests with glibc 2.28 or newer without requiring Qt or development packages:

```sh
cmake --build build --target agent-bundle
# build/agent/bundle/kemu-agent-<version>-linux-x86_64.tar.gz
```

In the guest, unpack the archive and run `sudo sh install.sh` to install to `/opt/kemu-agent` and `/etc` (`/opt/kemu-agent/uninstall.sh` removes it). See [agent/README.md](agent/README.md#portable-bundle) for options.

### Automated tests

The source tree includes regression tests for VM metadata, storage, templates, and the D-Bus viewer—and, with `-DKEMU_BUILD_AGENT=ON`, for the guest agent. See [docs/testing.md](docs/testing.md) for build instructions and coverage details.

## Using KEMU

### Creating virtual machines

*New Virtual Machine…* lists the operating systems available in the template database. Each template provides suitable hardware defaults and, where offered by the distribution, a cloud image that KEMU can download and set up unattended via cloud-init using the user account, SSH keys, locale, and keyboard settings from *Settings → Deployment*.

Alternatively, install from an ISO image or start with a blank disk. In Dolphin, selecting *New Virtual Machine from This Image…* on a disc image opens the dialog with that image preselected.

Manage templates under *Settings → Templates* (format documented in [docs/templates.md](docs/templates.md)).

### Display

Each VM uses one of two display backends:

| Backend   | Window              | 3D acceleration | Audio              | Clipboard |
| --------- | ------------------- | --------------- | ------------------ | --------- |
| **D-Bus** | KEMU's viewer       | Yes (Virtio)    | Through the viewer | Yes       |
| **SDL**   | QEMU's own window   | Yes (Virtio)    | System (PipeWire)  | No        |

Closing the viewer leaves the VM running headless; select *Show Display* to reopen it. Clipboard sharing requires `kemu-agent` in Linux guests; see [docs/display.md](docs/display.md).

### Multiple monitors (experimental)

VMs using VirtIO graphics and the integrated viewer support two monitors (*Edit… → Hardware → Display → Monitors*). Display 2 opens in a separate window; closing it disconnects the monitor in the guest OS, and *View → Displays → Display 2* reconnects it.

With `kemu-agent` in a Linux guest, KEMU knows the guest's monitor arrangement—including per-display scaling factors—and places the pointer accurately. Without the agent, KEMU infers the layout from the guest cursor or uses *View → Displays → Display 2 Position*.

### Networking

| Mode                | VM-to-VM | DHCP     | NAT      | Port forwarding |
| ------------------- | -------- | -------- | -------- | --------------- |
| **User mode**       | No       | Yes      | Yes      | Yes             |
| **Virtual network** | Yes      | Optional | Optional | Yes             |

Both modes run in userspace without root privileges. Configure virtual networks under *Settings → Virtual Networks*; see [docs/networking.md](docs/networking.md).

### Shared folders

KEMU shares host folders via 9p (Linux and BSD guests) or virtio-fs (Windows guests, requiring `virtiofsd` on the host plus VirtIO drivers and WinFsp in the guest). The *Shared Name* serves as the mount tag inside the guest—for example, on Linux:

```sh
sudo mount -t 9p -o trans=virtio,version=9p2000.L exchange /mnt/shared
```

For other guest systems, `fstab` examples, and Windows setup instructions, see [docs/shared-folders.md](docs/shared-folders.md).

### Disks and snapshots

- The system disk is a `qcow2` image; add empty or copied disks under *Edit… → Hardware → Additional Disks*.
- Expand disks in VM settings. When a guest trims its filesystems, unused blocks can be reclaimed on the host: *Disk Space → Free Unused Blocks* triggers trimming via the guest agent, while *Compact System Disk* rewrites the image without unused blocks or snapshots.
- Snapshots cover all disks, with or without the running state.
- A **linked clone** stores only its own changes on top of the source's disks; the source remains frozen while clones depend on it.
- The **system disk and TPM state can be encrypted** (LUKS) when creating a VM or later in its settings. Data cannot be recovered without the password.

Details and limits: [docs/storage.md](docs/storage.md).

### QEMU guest agent

Any VM with a PCI bus can connect to the QEMU guest agent (*Edit… → Advanced → Connect the QEMU guest agent*, enabled by default). Install `qemu-guest-agent` in Linux guests or the `virtio-win` guest tools in Windows guests; cloud-init deployments install the agent where possible.

When the agent is responding:

- The line below the VM status displays the guest's hostname, first IP address (click to copy), and OS; the VM list also shows the address next to uptime.
- *Shut Down* uses the agent even when the guest ignores the ACPI power button.
- *Free Unused Blocks* trims the guest's filesystems.
- The guest clock synchronises after resuming, host sleep, or reverting to a live snapshot.

KEMU communicates with the agent over a `socketpair` without creating a socket file accessible to other processes, and never uses guest-agent commands to execute programs or read files inside the guest.

### Desktop integration

- **System tray:** Enable under *Settings → Appearance → Enable system tray icon*. Clicking toggles the main window; the tray menu lists running VMs and starts stopped ones. Closing the main window keeps KEMU and its VMs running (*Quit* exits the application). *Start minimized to tray* launches KEMU with only the tray icon visible.
- **Notifications:** Cover VM shutdowns, unexpected stops, saved states, resuming after host sleep, deployment results, reclaimed disk space, linked clones becoming independent, and pending host shutdowns (*Settings → Configure Notifications…*). Long-running tasks (import, export, cloning, compaction) report progress through Plasma's job interface.
- **Application menu:** *VM → Add to Application Menu* creates a launcher (which can also be pinned to the task manager) that updates when the VM is renamed and is removed when the VM is deleted.
- **Command line:** `kemu --start <vm> [--headless]` starts a VM by name or UUID without showing the main window (keeping it in the tray or minimised) and exits when that VM stops, unless the main window was opened or another VM started in the meantime. If KEMU is already running, that instance handles the request. `kemu --new-vm <image>` opens the new VM dialog for a disc image; `kemu <file>` imports archives, OVA files, and template packages.
- **File associations:** Opening `.kemu.tar.zst` archives or OVA files imports them; `.kemu-template.tar.gz` packages open the Templates settings page.

### Shutting down the host with a guest

KEMU can shut down the host after a guest powers itself off—for example, following a lengthy update. Configure this per VM under *Edit… → System → When the guest shuts down*, or for the current run via *Shut Down Host with Guest* in the VM menu or viewer.

Only a guest-initiated shutdown qualifies, provided no other VM is running or busy, and a 30-second countdown allows you to cancel. On Plasma, KEMU ends the session normally; elsewhere, it asks logind to shut down the host (polkit may request a password).

### USB passthrough

USB devices can be attached to running VMs once QEMU has read/write access to `/dev/bus/usb/BUS/DEVICE`, such as via `kvm` group membership and a udev rule:

```udev
# /etc/udev/rules.d/90-qemu-usb.rules
SUBSYSTEM=="usb", MODE="0664", GROUP="kvm"
```

Reload udev rules with `sudo udevadm control --reload-rules && sudo udevadm trigger`.

**Security warning:** This rule grants `kvm` group members access to all USB devices, including keyboards and storage devices. Prefer a rule matching only the devices you intend to pass through:

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="0951", ATTR{idProduct}=="1666", MODE="0664", GROUP="kvm"
```

### System identity

Every new VM receives a random system UUID, editable alongside SMBIOS information under *Edit… → Advanced → System Identity*.

VMs created by older KEMU versions may lack a UUID. KEMU does not assign one automatically because changing a VM's identity can affect software licences and Windows activation. Clones receive their own UUID (unless the source has none), imported archives retain theirs unless the source VM still exists, and OVA imports receive a new UUID.

### Import and export

*Export…* creates a `.kemu.tar.zst` archive containing everything needed to move or back up a VM between KEMU installations. KEMU also imports and exports OVA files for use with other tools, subject to hardware compatibility limitations (see [docs/import-export.md](docs/import-export.md)).

## Troubleshooting

### Debug logging

| Category                   | Covers                                                                |
| -------------------------- | --------------------------------------------------------------------- |
| `org.kde.kemu`             | Application, VM lifecycle, networking, QMP, guest agent, snapshots    |
| `org.kde.kemu.dbusdisplay` | Integrated D-Bus viewer (frame-level traffic)                         |

By default, only warnings are logged. To collect detailed output for a bug report:

```sh
QT_LOGGING_RULES="org.kde.kemu*=true" kemu 2>&1 | tee kemu.log
```

Categories can also be enabled via `kdebugsettings` if installed.

### File locations

| Purpose                  | Path                                             |
| ------------------------ | ------------------------------------------------ |
| Virtual machines         | `~/.local/share/kemu/vms/`                       |
| Own OS templates         | `~/.local/share/kemu/data/templates.d/`          |
| Own cloud-init templates | `~/.local/share/kemu/data/cloud-init-templates/` |
| Downloaded templates     | `~/.local/share/kemu/ghns/`                      |
| Configuration            | `~/.config/kemurc`                               |

In the Flatpak, these files live under `~/.var/app/org.kde.kemu/`. Each VM directory contains `config.json`, `disk.qcow2` (additional disks are named `disk-2.qcow2`, …), `OVMF_VARS.fd` for UEFI VMs, and `qemu.sh`. Change the storage location under *Settings → Virtual Machines*.

### UEFI firmware

KEMU automatically locates OVMF firmware on common distributions, searching any directory specified under *Settings → System → UEFI Firmware* first.

| Distribution  | Package            | Typical Secure Boot firmware                  |
| ------------- | ------------------ | --------------------------------------------- |
| Arch Linux    | `edk2-ovmf`        | `/usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd` |
| Debian/Ubuntu | `ovmf`             | `/usr/share/OVMF/OVMF_CODE_4M.secboot.fd`     |
| Fedora        | `edk2-ovmf`        | `/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd`   |
| openSUSE      | `qemu-ovmf-x86_64` | `/usr/share/qemu/ovmf-x86_64-smm-ms-code.bin` |

Each UEFI VM stores its NVRAM in a separate `OVMF_VARS.fd`, created from a matching template when needed without overwriting existing files.

### Screenshots

In the D-Bus viewer, *Machine → Save Screenshot…* captures the guest display at its native resolution, including Virtio-GL (using the optional Wayland zero-copy path when the DMA-BUF can also be imported through EGL). The main window's screenshot action uses the viewer when connected and falls back to QEMU's `screendump` for 2D VMs without a viewer or with SDL. For a headless 3D VM, open the D-Bus viewer before taking a screenshot.

### QEMU sandbox

*Settings → System → QEMU Sandbox* enables QEMU's seccomp filter (`-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,resourcecontrol=deny`), restricting process spawning and privilege changes if a guest compromises QEMU. Features that spawn helper processes (`-netdev tap,script=…`, `smb=`) cannot be used with these restrictions, and a blocked system call terminates QEMU with `SIGSYS`. A `-sandbox` option in a VM's extra arguments always takes precedence.

## Known limitations

- Additional disks are `qcow2` images inside the VM directory; host block devices cannot be attached directly, and disks cannot be added or removed while the VM runs.
- Pointer capture requires a Wayland session; guests with a tablet pointer do not require pointer capture.
- NAT in virtual networks is IPv4-only and does not translate ping.
- Encrypted VMs cannot save their state, and snapshots of them are taken while shut down.

## Credits and trademarks

KEMU builds on QEMU (Fabrice Bellard and the QEMU Project developers), swtpm and libtpms, virtiofsd, libslirp, libisofs, libarchive, EDK II/OVMF, and cloud-init. Operating system icons come from the [Papirus Icon Theme](https://github.com/PapirusDevelopmentTeam/papirus-icon-theme) (GPL-3.0); the QEMU logo is by Benoît Canet (CC BY 3.0). See *Help → About KEMU* for the full list.

Trademarks, logos, and brand names belong to their respective owners. Third-party logos in KEMU are used solely to identify guest operating systems. KEMU is an independent open-source project and is not affiliated with, endorsed by, or sponsored by any trademark holder.

## License

KEMU is licensed under GPL-3.0-or-later. Its documentation (this README and `docs/`) is licensed under CC-BY-SA-4.0.
