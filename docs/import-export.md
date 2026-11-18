# Import and export

## KEMU archives

*Export…* creates a `.kemu.tar.zst` archive containing the VM configuration, disk images, snapshots, TPM state, and UEFI variables—suitable for backups or transferring VMs between KEMU installations. Linked-clone disks are exported as complete images without snapshots.

To restore an archive, choose *Import Virtual Machine…*, double-click the file in your file manager, or run `kemu <file>`.

- Shared-folder and ISO paths are omitted because they are specific to the source host.
- Encrypted VMs remain encrypted; passwords are not included in the archive.
- Imported VMs retain their UUID unless the source VM still exists. MAC addresses are preserved or regenerated according to *Settings → Virtual Machines*.

## OVA

KEMU imports and exports OVA files to exchange VMs with other virtualisation tools.

**Import:**

- Firmware (BIOS/UEFI, Secure Boot) and boot-controller settings—including common vendor extensions—are retained when specified in the descriptor to preserve the guest's expected hardware: UEFI or SATA selects Q35, NVMe disks remain on NVMe, and IDE, SCSI, or unknown controllers map to SATA/IDE (switching from the PC chipset to Q35 if disk slots run out).
- Network adapters without a matching QEMU model (including vendor-specific models and E1000e) map to E1000. OS information is retained, and legacy OVF 0.9 descriptors are supported.
- Appliance licence terms (`EulaSection`) are displayed before import and must be accepted.
- Manifests (`.mf`) are verified and halt the import on checksum mismatches; signatures (`.cert`) are not verified.
- Compressed (`ovf:compression`) or chunked disk files are not supported.
- Each imported OVA receives a new system UUID.

**Export:**

- The descriptor records hardware details in a form recognised by common tools: firmware and Secure Boot (via vendor extensions), per-disk controllers (AHCI on Q35, IDE on PC chipsets, NVMe), network adapter model, operating system, and VM comment. Because VirtIO disks have no direct OVF equivalent, they are exported using AHCI.
- Some tools read firmware, Secure Boot, and MAC addresses only from proprietary machine metadata rather than standard OVF; a UEFI guest may therefore start in BIOS mode until EFI is enabled in the destination tool's settings.
- TPM state, UEFI variables, system UUID, SMBIOS information, and snapshots are not exported (VMDK does not preserve KEMU snapshots). Use KEMU's native export format for a complete transfer between KEMU installations.
- Highly customised or uncommon configurations from other tools may not import correctly.
