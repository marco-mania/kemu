# Disks, snapshots and encryption

## Additional disks

Most VMs only need a system disk. Add further disks under *Edit… → Hardware →
Additional Disks*, either as empty images or by copying an existing image
(qcow2, raw, VMDK, VDI or VHD/VHDX) into the VM directory. The original image
is left unchanged.

- Each disk uses a VirtIO, NVMe or SATA/IDE controller. New disks default to
  the system disk's controller, for which the guest should already have a
  driver.
- SATA/IDE slots are shared with the CD/DVD drive. Q35 (AHCI) provides six
  slots; PC chipsets provide four. VirtIO and NVMe require a PCI bus.
- Each disk receives a stable serial number (`KEMU-DISK-2`, …), allowing
  Linux guests to identify it under `/dev/disk/by-id/`.
- When a disk is removed, its image can be deleted or moved elsewhere.
- Disks are qcow2 images stored in the VM directory; host block devices
  cannot be attached. Disks cannot be added or removed while a VM is running.

## Expanding, compacting, freeing blocks

- **Expand:** Specify a larger size in the VM settings (system disk) or in
  the properties of an additional disk, then confirm with the expand button.
  Afterwards, extend the partitions and file systems inside the guest.
- **Free blocks:** Freed blocks can be reclaimed in the disk images
  (`discard=unmap,detect-zeroes=unmap`) when the guest trims its file systems.
  Linux commonly uses `fstrim.timer`; Windows provides "Optimize Drives".
  With the guest agent, *VM → Disk Space → Free Unused Blocks* triggers trimming
  immediately. Blocks referenced by snapshots remain allocated.
- **Compact:** *VM → Disk Space → Compact System Disk* (also available in the
  VM settings) rewrites the system disk without unused blocks or snapshots.
  Compact additional disks from their properties. With trimming enabled,
  compaction is rarely needed solely to reclaim space.

## Snapshots

- Snapshots always include all disks. They can capture a powered-off VM or
  include its full running state.
- Disks added after a snapshot retain their contents when the VM is
  reverted to that snapshot. If a disk has been removed, snapshots containing
  a running state can only be restored while the VM is shut down.
- QEMU cannot migrate its NVMe controller: with an NVMe disk attached, neither
  Save State nor snapshots of the running VM are available.

## Linked clones

A linked clone can be created in seconds and initially uses very little
space. It stores only changes relative to the source VM's disks (qcow2
overlays), making it useful for temporary test environments. Select *Create
linked clone* in the Clone dialog.

- The source VM is frozen while linked clones depend on it: it cannot be
  started, edited, renamed, deleted, compacted or reverted to a snapshot. It
  can still be exported or cloned. The restrictions are lifted when its last
  linked clone is deleted or made independent.
- The source VM must be shut down. A linked clone cannot itself be the source
  of another linked clone, but it can be fully cloned.
- *Make Independent* copies shared data into the clone, making its space
  requirements comparable to a full clone. Its snapshots are discarded.
- Export, OVA export and full clones of a linked clone contain complete disks.
- Overlays reference their source using relative paths
  (`../<source>/disk.qcow2`), so the VM storage tree can be moved as a unit.

## Encryption

You can encrypt the system disk with a password when creating a VM (*Encrypt
system disk*) or later using the lock button next to the disk size in the VM
settings. KEMU uses a qcow2 image with built-in LUKS encryption.
**Without the password, the data cannot be recovered.**

- The password can be stored in KWallet (folder "KEMU"); otherwise KEMU
  prompts for it at each start. The VM settings also let you change the
  password, add or remove it from KWallet, or decrypt the disk. Changing the
  password replaces only the LUKS key slots.
- The TPM state is encrypted using the same password (`swtpm --key`).
  Migrating an existing TPM state requires `swtpm_ioctl` (package
  `swtpm-tools` on Debian, Ubuntu and Fedora).
- Encrypting an existing VM copies its system disk and requires roughly the
  same amount of free space. Existing snapshots, including those on additional
  disks, are deleted. The unencrypted original is removed afterwards.
- Linked clones and VMs with a saved state cannot be encrypted.
- Additional disks, UEFI variables and cloud-init seed images containing
  deployment user data are not encrypted. Encrypt additional disks inside the
  guest if needed.
- Encrypted VMs cannot save their state; snapshots are taken while they are
  shut down.
- Clones and exports remain encrypted. Full clones receive their own LUKS
  header and KWallet entry.
- KEMU passes the password to QEMU and qemu-img through a pipe, never on the
  command line. The exported `qemu.sh` prompts for it or reads
  `KEMU_DISK_PASSWORD`.
- Freed blocks can also be reclaimed in encrypted images. As with TRIM on
  other LUKS volumes, this reveals which areas are unused, but not their
  contents.
