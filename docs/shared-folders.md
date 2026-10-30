# Shared folders

| Guest         | Protocol    | Host side   | Guest side              |
| ------------- | ----------- | ----------- | ----------------------- |
| **Linux/BSD** | 9p (VirtFS) | built in    | Kernel support          |
| **Windows**   | virtio-fs   | `virtiofsd` | VirtIO drivers + WinFsp |

Configure shared folders in the VM settings. The *Shared Name* is the mount
tag used by the guest. The examples below use `exchange` as the tag and
`/mnt/shared` as the mount point.

## Linux

```sh
sudo mount -t 9p -o trans=virtio,version=9p2000.L exchange /mnt/shared
```

`/etc/fstab`:

```text
exchange /mnt/shared 9p trans=virtio,version=9p2000.L,rw,nofail 0 0
```

## FreeBSD 13+

```sh
sudo mount -t virtfs -o trans=virtio exchange /mnt/shared
```

`/etc/fstab`:

```text
exchange /mnt/shared virtfs trans=virtio,rw 0 0
```

OpenBSD and NetBSD provide only partial 9p support; verify compatibility
before relying on it.

## Mounting at login

As an alternative to `fstab`, an autostart entry can mount the share when
the desktop session starts. For example, create
`~/.config/autostart/mount-exchange.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Mount exchange
Exec=sudo mount -t 9p -o trans=virtio,version=9p2000.L exchange /home/myuser/exchange
```

This requires a sudo rule allowing the mount command to run without a
password.

## Windows

KEMU starts `virtiofsd` on the host automatically. In the Windows guest,
install the following components in order:

1. Install [WinFsp](https://winfsp.dev/rel/).
2. Install the VirtIO guest tools (`virtio-win-gt-x64.msi`).
3. Reboot.

If the drive does not appear, check the service from PowerShell:

```powershell
Get-Service VirtioFsSvc                    # running?
Start-Service VirtioFsSvc                  # start it
sc.exe config VirtioFsSvc start=auto       # start it at boot
```
