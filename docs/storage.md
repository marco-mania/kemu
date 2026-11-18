# Disks, snapshots, and encryption

## Additional disks

Most VMs require only a system disk. Add extra disks under *Edit… → Hardware → Additional Disks*, either as empty images or by copying an existing image (`qcow2`, `raw`, VMDK, VDI, or VHD/VHDX) into the VM directory, leaving the original image unchanged.

- Each disk uses a VirtIO, NVMe, or SATA/IDE controller; new disks default to the system disk's controller, for which the guest should already have a driver.
- SATA/IDE slots are shared with the CD/DVD drive (six slots on Q35/AHCI; four on PC chipsets), whereas VirtIO and NVMe require a PCI bus.
- Each disk receives a stable serial number (`KEMU-DISK-2`, …) so Linux guests can identify it under `/dev/disk/by-id/`.
- When removing a disk, you can delete its image or move it elsewhere.
- Disks are stored as `qcow2` images inside the VM directory; host block devices cannot be attached directly, and disks cannot be added or removed while a VM is running.

## Expanding, compacting, freeing blocks

- **Expand:** Set a larger size in the VM settings (for the system disk) or in an additional disk's properties, then confirm via the expand button. Afterwards, resize the partitions and filesystems inside the guest.
- **Free blocks:** Freed blocks are reclaimed in disk images (`discard=unmap,detect-zeroes=unmap`) when the guest trims its filesystems (typically via `fstrim.timer` on Linux or *Optimize Drives* on Windows). With the QEMU guest agent running, *VM → Disk Space → Free Unused Blocks* triggers trimming immediately. Blocks referenced by snapshots remain allocated.
- **Compact:** *VM → Disk Space → Compact System Disk* (also accessible in VM settings) rewrites the system disk without unused blocks or snapshots; compact additional disks via their properties. When trimming is enabled, compaction is rarely needed solely to reclaim space.

## Snapshots

- Snapshots always cover all disks and can capture either a shut-down VM or its full running state.
- Disks added after a snapshot retain their contents when reverting to that snapshot; if a disk was removed, live snapshots (containing a running state) can be restored only while the VM is shut down.
- Because QEMU cannot migrate its NVMe controller, attaching an NVMe disk disables both Save State and live snapshots.

## Linked clones

A linked clone is created in seconds and initially uses minimal disk space by storing only differences relative to the source VM's disks (`qcow2` overlays), making it ideal for temporary test environments. Select *Create linked clone* in the Clone dialog.

- While linked clones depend on a source VM, the source is frozen: it cannot be started, edited, renamed, deleted, compacted, or reverted to a snapshot (though it can still be exported or cloned). These restrictions lift once its last linked clone is deleted or made independent.
- The source VM must be shut down. A linked clone cannot serve as the source for another linked clone, though it can be fully cloned.
- *Make Independent* copies shared backing data into the clone—matching a full clone's disk usage—and discards its snapshots.
- Exports, OVA exports, and full clones of a linked clone produce self-contained, complete disks.
- Overlays reference their source via relative paths (`../<source>/disk.qcow2`), allowing the VM storage tree to be moved as a unit.

## Encryption

Encrypt the system disk with a password during VM creation (*Encrypt system disk*) or later via the lock button next to the disk size in VM settings. KEMU uses `qcow2` images with built-in LUKS encryption. **Data cannot be recovered without the password.**

- Passwords can be saved in KWallet (in the `KEMU` folder); otherwise, KEMU prompts for the password on every boot. In VM settings, you can also change the password (which updates only the LUKS key slots), add or remove it from KWallet, or decrypt the disk.
- The TPM state is encrypted with the same password (`swtpm --key`); migrating an existing TPM state requires `swtpm_ioctl` (`swtpm-tools` on Debian, Ubuntu, and Fedora).
- Encrypting an existing VM copies its system disk—requiring roughly the same amount of free space—deletes all existing snapshots (including on additional disks), and removes the unencrypted original afterwards.
- Linked clones and VMs with a saved state cannot be encrypted.
- Additional disks, UEFI variables, and cloud-init seed images (which contain deployment user data) are not encrypted; encrypt additional disks inside the guest if required.
- Encrypted VMs cannot save their running state, and snapshots can be taken only while shut down.
- Clones and exports remain encrypted; full clones receive their own LUKS header and KWallet entry.
- KEMU passes the password to QEMU and `qemu-img` over a pipe—never on the command line—while exported `qemu.sh` scripts prompt for it or read `KEMU_DISK_PASSWORD`.
- Freed blocks can also be reclaimed in encrypted images; as with TRIM on any LUKS volume, this exposes which blocks are unused without revealing their contents.
