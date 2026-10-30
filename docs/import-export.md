# Import and export

## KEMU archives

*Export…* creates a `.kemu.tar.zst` archive containing the VM configuration,
disk images and snapshots, TPM state and UEFI variables. It can be used as a
backup or transferred to another KEMU installation. Linked-clone disks are
exported as complete images, without snapshots.

To restore an archive, choose *Import Virtual Machine…*, double-click it in
the file manager or run `kemu <file>`.

- Shared-folder and ISO paths are omitted because they are specific to the
  source host.
- Encrypted VMs remain encrypted; passwords are not included in the archive.
- Imported VMs retain their UUID unless the source VM still exists. MAC
  addresses are preserved or regenerated according to *Settings → Virtual
  Machines*.

## OVA

KEMU can import and export OVA files to exchange VMs with other virtualisation
tools.

**Import:**

- Firmware (BIOS/UEFI, Secure Boot) and boot-controller settings are retained
  when specified in the descriptor, including common vendor extensions. This
  helps preserve the guest's expected hardware: UEFI or SATA selects Q35,
  while NVMe disks remain on NVMe. IDE, SCSI and unknown controllers are mapped
  to SATA/IDE. If the PC chipset runs out of disk slots, KEMU switches to Q35.
- Network adapters without a matching QEMU model (including vendor-specific
  models and E1000e) are mapped to E1000. OS information is retained, and
  legacy OVF 0.9 descriptors are supported.
- Appliance licence terms (`EulaSection`) are displayed before import and
  must be accepted.
- Manifests (`.mf`) are verified, and a checksum mismatch stops the import.
  Signatures (`.cert`) are not verified.
- Compressed (`ovf:compression`) or chunked disk files are not supported.
- Each imported OVA receives a new system UUID.

**Export:**

- The descriptor records hardware information in a form recognised by common
  tools: firmware and Secure Boot (via vendor extensions), per-disk controllers
  (AHCI on Q35, IDE on PC chipsets, NVMe), network adapter model, operating
  system and VM comment. VirtIO disks have no direct OVF equivalent and are
  exported using AHCI.
- Some tools read firmware, Secure Boot and MAC addresses only from their
  proprietary machine metadata, not from standard OVF. A UEFI guest may
  therefore start in BIOS mode; enable EFI in the destination tool's settings.
- TPM state, UEFI variables, system UUID, SMBIOS information and snapshots
  are not exported (VMDK does not preserve KEMU snapshots). Use KEMU's native
  export format for a complete transfer between KEMU installations.
- Highly customised or uncommon configurations from other tools may not be
  imported correctly.
