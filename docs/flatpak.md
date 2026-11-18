# KEMU in the Flatpak

Because sandboxed applications cannot run host executables directly, the Flatpak bundles QEMU, `swtpm`, and `virtiofsd` (`KEMU_SELF_CONTAINED`). KEMU detects the sandbox and adapts its behaviour accordingly. For testing, individual features can also be disabled via `--disable-capability <name>`.

## Permissions

| Permission                                               | Purpose                                           |
| -------------------------------------------------------- | ------------------------------------------------- |
| `--socket=wayland`, `--share=ipc`                        | Native Wayland viewer, SDL window of `qemu.sh`    |
| `--device=dri`                                           | 3D acceleration (`virglrenderer`)                 |
| `--device=kvm`                                           | Hardware virtualization                           |
| `--share=network`                                        | User-mode networking and NAT for virtual networks |
| `--socket=pulseaudio`, `--filesystem=xdg-run/pipewire-0` | Guest audio                                       |
| `--filesystem=xdg-config/kdeglobals:ro`                  | KDE colour scheme and fonts                       |
| `--talk-name=org.kde.kwalletd6`                          | Encrypted disk passwords in KWallet               |
| `--talk-name=org.kde.StatusNotifierWatcher`              | System tray icon                                  |

Notifications and application menu entries use desktop portals and require no additional permissions.

## Granting more

Some permissions are intentionally omitted to preserve sandbox isolation; grant additional access only when needed:

- **USB passthrough:** Grant device access via `flatpak override --user --device=all org.kde.kemu` in addition to the udev rule described in the README.
- **ISO images and shared folders outside the application data directory:** Select files through the file dialog (desktop portal), or grant directory access—for example, via `flatpak override --user --filesystem=xdg-download org.kde.kemu`.
- **Shutting down the host with a guest:** Grant `flatpak override --user --talk-name=org.kde.Shutdown org.kde.kemu` on Plasma, or `--system-talk-name=org.freedesktop.login1` for `logind` access. Without either permission, this option is disabled.

## kemu-agent for guests

The Flatpak is built without the `kemu-agent` guest bundle (`-DKEMU_AGENT_BUNDLE=OFF` in `org.kde.kemu.yml`) because building the bundle requires a container engine and network access, neither of which `flatpak-builder` provides. To include bundles in the Flatpak, build them externally (or use prebuilt release archives), add each to the `kemu` module's `sources`, and pass their paths in `/run/build/kemu/` (where `flatpak-builder` places sources) via `-DKEMU_AGENT_BUNDLES`:

```yaml
config-opts:
  - -DKEMU_AGENT_BUNDLE=OFF
  - -DKEMU_AGENT_BUNDLES=/run/build/kemu/kemu-agent-0.3.0-linux-x86_64.tar.gz
sources:
  - type: file
    path: build/agent/bundle/kemu-agent-0.3.0-linux-x86_64.tar.gz
    only-arches: [x86_64]
```

For a released bundle, specify `url:` and `sha256:` instead of `path:`. Bundles are installed to `/app/share/kemu/agent/<arch>/`, where KEMU discovers them via the Flatpak's data directories.

## Differences

- **Executables:** *Settings → System → Executables* is read-only; bundled binaries cannot be replaced with host ones, though paths configured in a native installation remain preserved for later use.
- **Firmware:** The bundled QEMU includes UEFI firmware, and custom directories can still be set under *Settings → System → UEFI Firmware*.
- **Start scripts:** `qemu.sh` and *Show QEMU Command…* reference sandbox paths (`/app/bin/…`), so commands must be invoked via `flatpak run --command=…` rather than directly on the host.
- **File locations:** Data and configuration live under `~/.var/app/org.kde.kemu/`. VMs from a native installation are not detected automatically; select their directory under *Settings → Virtual Machines → Storage location*.
- **Application menu entries:** The desktop portal prompts for confirmation both when creating an entry and when renaming the VM.
- **Dolphin action:** *New Virtual Machine from This Image…* is unavailable by default because Flatpak does not export service menus. To add it manually, save the following as `~/.local/share/kio/servicemenus/kemu-newvm.desktop`:

  ```ini
  [Desktop Entry]
  Type=Service
  MimeType=application/vnd.efi.iso;application/x-cd-image;
  X-KDE-Protocols=file
  X-KDE-MaxNumberOfUrls=1
  Actions=newVm;

  [Desktop Action newVm]
  Name=New Virtual Machine from This Image…
  Icon=org.kde.kemu
  Exec=flatpak run org.kde.kemu --new-vm %f
  ```

- **Notification settings:** System Settings does not list KEMU's Flatpak events; configure them in KEMU under *Settings → Configure Notifications…*.
- **Command line:** `flatpak run org.kde.kemu --start <vm>`.
