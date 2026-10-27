# Disks, snapshots and encryption

## Additional disks

The system disk is all most VMs need. Further disks are added under
*Edit… → Hardware → Additional Disks*, either as new, empty images or by
copying an existing image (qcow2, raw, VMDK, VDI, VHD/VHDX) into the VM
directory. The original image is not modified.

- Each disk has its own controller: VirtIO, NVMe or SATA/IDE. New disks default
  to the controller of the system disk, for which the guest already has a
  driver.
- SATA/IDE slots are shared with the CD/DVD drive: Q35 (AHCI) offers six, the
  PC chipsets four. VirtIO and NVMe need a PCI bus.
- Every disk gets a stable serial number (`KEMU-DISK-2`, …), so Linux guests
  find it under `/dev/disk/by-id/`.
- When a disk is removed, its image can be deleted or moved elsewhere.
- Disks are qcow2 images inside the VM directory; host block devices cannot be
  attached. Disks cannot be added or removed while the VM runs.

## Expanding, compacting, freeing blocks

- **Expand:** Enter a larger size in the VM settings (system disk) or in the
  properties of an additional disk and confirm with the expand button. The
  partitions inside the guest have to be grown there afterwards.
- **Free blocks:** Blocks the guest frees are released in the images
  (`discard=unmap,detect-zeroes=unmap`), so the images shrink on the host
  whenever the guest trims its file systems – Linux weekly through
  `fstrim.timer`, Windows through "Optimize Drives". With the guest agent,
  *VM → Disk Space → Free Unused Blocks* trims at once. Blocks still used by
  snapshots stay allocated.
- **Compact:** *VM → Disk Space → Compact System Disk* (or the button in the VM
  settings) rewrites the system disk without unused blocks and without
  snapshots. Additional disks are compacted from their properties. With the
  above it is rarely needed for space anymore.

## Snapshots

- Snapshots always cover all disks, either of the shut-down VM or with the
  complete running state.
- A disk added after a snapshot keeps its content when the VM is reverted to
  that snapshot. After a disk was removed, snapshots with a running state can
  only be restored while the VM is shut down.
- QEMU cannot migrate its NVMe controller: with an NVMe disk attached, neither
  Save State nor snapshots of the running VM are available.

## Linked clones

A linked clone is created in seconds and takes almost no space: it stores only
its own changes on top of the disks of the source VM (qcow2 overlays) – good
for throwaway test machines. Select *Create linked clone* in the Clone dialog.

- The source is frozen while linked clones depend on it: it cannot be started,
  edited, renamed, deleted, compacted or reverted to a snapshot. It can still
  be exported and cloned. It is released when its last linked clone is deleted
  or made independent.
- The source must be shut down. A linked clone cannot be the source of another
  linked clone, but can be cloned in full.
- *Make Independent* copies the shared data into the clone, which then needs
  as much space as a full clone. Its snapshots are lost.
- Export, OVA export and full clones of a linked clone contain complete disks.
- Overlays refer to their source by a relative path
  (`../<source>/disk.qcow2`), so the VM storage can be moved as a whole.

## Encryption

The system disk can be encrypted with a password: in the New Virtual Machine
dialog (*Encrypt system disk*) or later with the lock button next to the disk
size in the VM settings. The disk is a qcow2 image with built-in LUKS
encryption. **Without the password the data cannot be recovered.**

- The password can be kept in KWallet (folder "KEMU"); otherwise KEMU asks for
  it at every start. Next to the disk size, the VM settings also offer to
  change the password, store it in or remove it from KWallet, and decrypt the
  disk again. Changing the password only replaces the LUKS key slots.
- The TPM state is encrypted with the same password (`swtpm --key`). Moving an
  existing state over needs `swtpm_ioctl` (Debian/Ubuntu/Fedora: package
  `swtpm-tools`).
- Encrypting an existing VM copies the system disk and needs free space of
  about its size. Its snapshots are deleted, on the additional disks as well.
  The unencrypted original is deleted afterwards.
- Linked clones and VMs with a saved state cannot be encrypted.
- Not encrypted are additional disks (encrypt them inside the guest if needed),
  the UEFI variable store and the cloud-init seed image with the user data of
  a deployment.
- Encrypted VMs cannot save their state; snapshots are taken while they are
  shut down.
- Clones and exports stay encrypted. A full clone gets its own LUKS header and
  KWallet entry.
- KEMU hands the password to QEMU and qemu-img through a pipe, never on the
  command line. The exported `qemu.sh` asks for it or reads
  `KEMU_DISK_PASSWORD`.
- Freed blocks are released in encrypted images too. The image then shows
  which areas are unused, as with TRIM on any LUKS volume; their content stays
  unreadable.
