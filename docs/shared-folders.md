# Shared folders

| Guest         | Protocol    | Host side   | Guest side              |
| ------------- | ----------- | ----------- | ----------------------- |
| **Linux/BSD** | 9p (VirtFS) | Built-in    | Kernel support          |
| **Windows**   | virtio-fs   | `virtiofsd` | VirtIO drivers + WinFsp |

Configure shared folders in the VM settings; the *Shared Name* serves as the mount tag inside the guest. The examples below use `exchange` as the mount tag and `/mnt/shared` as the mount point.

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

OpenBSD and NetBSD provide only partial 9p support; verify compatibility before relying on it.

## Mounting at login

As an alternative to `/etc/fstab`, an autostart entry can mount the share when your desktop session starts—for example, in `~/.config/autostart/mount-exchange.desktop`:

```ini
[Desktop Entry]
Type=Application
Name=Mount exchange
Exec=sudo mount -t 9p -o trans=virtio,version=9p2000.L exchange /home/myuser/exchange
```

This requires a `sudo` rule allowing the `mount` command to run without a password.

## Windows

KEMU starts `virtiofsd` on the host automatically. Inside the Windows guest, install these components in order:

1. [WinFsp](https://winfsp.dev/rel/)
2. VirtIO guest tools (`virtio-win-gt-x64.msi`)
3. Reboot the guest.

If the drive does not appear, check and enable the service via PowerShell:

```powershell
Get-Service VirtioFsSvc                    # check status
Start-Service VirtioFsSvc                  # start service
sc.exe config VirtioFsSvc start=auto       # enable at boot
```
