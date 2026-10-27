# Import and export

## KEMU archives

*Export…* writes a `.kemu.tar.zst` archive with the VM's configuration, its
disks with their snapshots, the TPM state and the UEFI variables – a faithful
copy for another KEMU installation or a backup. The disks of a linked clone are
exported complete, without snapshots. *Import Virtual Machine…*, a double click
in the file manager or `kemu <file>` reads it back.

- The shared folder path and the ISO path are left out, because they belong to
  the host.
- Encrypted VMs stay encrypted; the password is not part of the archive.
- An imported VM keeps its UUID unless the source VM still exists. Its MAC
  address is kept or regenerated as set under *Settings → Virtual Machines*.

## OVA

OVA files are read and written for exchange with other virtualization tools.

**Import:**

- Firmware (BIOS/UEFI, Secure Boot) and boot controller are taken over where
  the descriptor states them (common vendor extensions included), so the guest
  finds the hardware it was installed on: UEFI or a SATA controller selects
  Q35, NVMe disks stay on NVMe. IDE, SCSI and unknown controllers become
  SATA/IDE. If the PC chipset runs out of disk slots, the import switches to
  Q35.
- Network adapters without a QEMU counterpart (vendor-specific models, E1000e)
  become E1000. The operating system is taken over as well. Descriptors in the
  old OVF 0.9 layout are read too.
- License terms of an appliance (`EulaSection`) are shown before the import
  and have to be accepted.
- A manifest (`.mf`) is verified; a checksum mismatch stops the import.
  Signatures (`.cert`) are not checked.
- Compressed (`ovf:compression`) or chunked disk files are not supported.
- An OVA import always gets a new system UUID.

**Export:**

- The descriptor states the hardware the way common tools read it: firmware
  and Secure Boot (vendor extension), the controller per disk (AHCI on Q35, IDE
  on the PC chipsets, NVMe), the network adapter model, the operating system
  and the VM's comment. VirtIO disks have no OVF equivalent and are exported on
  AHCI.
- Some tools read firmware, Secure Boot and MAC address only from their own
  machine section, never from a plain OVF. A UEFI machine may then start with
  BIOS; enable EFI in its settings there.
- TPM state, UEFI variables, system UUID, SMBIOS information and snapshots are
  not exported (VMDK has no snapshots). For a faithful copy between KEMU
  installations use the native export.
- Highly customized or exotic configurations of other tools may not be read
  correctly.
