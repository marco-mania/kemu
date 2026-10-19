/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "const.h"

#include <QDir>
#include <QIcon>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>
#include <QUuid>

#include <KLocalizedString>
#include <qtmetamacros.h>

#include <utility>

using namespace Qt::StringLiterals;

namespace VMSpecs
{

Q_NAMESPACE

enum class DisplayType {
    Sdl,
    Dbus
};

enum class GpuType {
    Vga, // PCI
    VgaISA, // ISA
    CirrusGD5446, // PCI
    CirrusGD5430, // ISA
    Virtio,
    Qxl
};

enum class AudioDevice {
    None,
    Hda, // PCI
    Ac97, // PCI
    Es1370, // PCI
    Sb16, // ISA
    Adlib, // ISA
    PcSpkr
};

enum class DiskInterface {
    Virtio,
    Nvme,
    SataIde // SATA (AHCI) for Q35, IDE for PC
};

enum class UsbController {
    None,
    Xhci,
    Ehci,
    Default
};

enum class CpuModel {
    Host,
    Qemu64,
    Qemu32,
    I486,
    Pentium,
    Pentium2,
    Pentium3,
    Pentium4,
    CoreDuo,
    Core2Duo,
    Nehalem,
    SandyBridge,
    Haswell,
    Skylake,
    Phenom,
    Epyc
};

enum class MachineType {
    Q35,
    Pc,
    PcISA // ISA-only PC
};

enum class NetworkCard {
    None,
    Virtio, // PCI
    E1000, // PCI
    Rtl8139, // PCI
    Pcnet, // PCI
    Rtl8029, // PCI (NE2000 comp.)
    Ne2000, // ISA
    I82551, // PCI (Intel PRO/100)
    I82557b, // PCI (Intel PRO/100)
    I82559er // PCI (Intel PRO/100)
};

enum class NetworkMode {
    UserMode,
    VirtualNetwork
};

enum class PointerDevice {
    Ps2, // relative, always present on pc/q35, no USB required
    UsbMouse, // relative, requires a USB controller
    UsbTablet, // absolute, requires a USB controller
    VirtioTablet // absolute, requires PCI and a virtio input driver in the guest
};

enum class KeyboardDevice {
    Ps2, // always present, no USB required
    Usb // requires a USB controller
};

enum class OsType {
    Linux,
    Windows,
    DOS,
    BSD,
    Other
};

enum class Architecture {
    X86_64, // amd64 / x86-64
    I386, // x86, 32-bit
    Aarch64, // ARM, 64-bit
    Riscv64 // RISC-V, 64-bit
};

enum class ExitAction {
    SaveState, // migrate to file, resume on next start
    Shutdown, // ACPI power button, let the guest shut down
    PowerOff // kill QEMU (last resort)
};

enum class GuestShutdownAction {
    None,
    ShutdownHost // power off the host once the guest has powered itself off
};

Q_ENUM_NS(DisplayType)
Q_ENUM_NS(GpuType)
Q_ENUM_NS(AudioDevice)
Q_ENUM_NS(DiskInterface)
Q_ENUM_NS(UsbController)
Q_ENUM_NS(CpuModel)
Q_ENUM_NS(MachineType)
Q_ENUM_NS(NetworkCard)
Q_ENUM_NS(NetworkMode)
Q_ENUM_NS(PointerDevice)
Q_ENUM_NS(KeyboardDevice)
Q_ENUM_NS(OsType)
Q_ENUM_NS(Architecture)
Q_ENUM_NS(ExitAction)
Q_ENUM_NS(GuestShutdownAction)

struct Defaults {
    bool useKvm = true;
    int ramMb = 4096;
    bool useMemoryBallooning = true;
    bool preallocMemory = false;
    CpuModel cpuModel = CpuModel::Host;
    int cpuCoreCount = 2;
    int diskSizeGb = 40;
    MachineType machineType = MachineType::Q35;
    GpuType gpuType = GpuType::Virtio;
    int gpuMemMb = 64;
    bool gpu3dAcceleration = true;
    AudioDevice audioDevice = AudioDevice::Hda;
    DiskInterface diskInterface = DiskInterface::Virtio;
    NetworkCard networkCard = NetworkCard::Virtio;
    DisplayType displayType = DisplayType::Dbus;
    UsbController usbController = UsbController::Xhci;
    PointerDevice pointerDevice = PointerDevice::UsbTablet;
    KeyboardDevice keyboardDevice = KeyboardDevice::Ps2;
    QSize displayResolution = QSize(1280, 720);
    bool useUefi = true;
    bool useTpm = false;
    bool useSecureBoot = false;
    bool useLocalTime = false;
    bool useAcpi = true;
    OsType osType = OsType::Linux;
    Architecture architecture = Architecture::X86_64;
    NetworkMode networkMode = NetworkMode::UserMode;
    QString macAddress = u"52:54:00:12:34:56"_s;
    ExitAction exitAction = ExitAction::SaveState;
    GuestShutdownAction guestShutdownAction = GuestShutdownAction::None;
};

} // namespace VMSpecs

inline const VMSpecs::Defaults GlobalDefaults;

struct PortForwardRule {
    enum class Protocol {
        Tcp,
        Udp
    };
    Protocol protocol = Protocol::Tcp;
    quint16 hostPort = 0;
    quint16 guestPort = 0;

    bool operator==(const PortForwardRule &other) const
    {
        return protocol == other.protocol && hostPort == other.hostPort && guestPort == other.guestPort;
    }
};

/// An additional qcow2 image inside the VM directory. The system disk is not
/// part of this list.
struct DiskConfig {
    QString fileName; // "disk-<n>.qcow2", relative to the VM directory
    QString label;
    VMSpecs::DiskInterface diskInterface = VMSpecs::DiskInterface::Virtio;
    QString serial;

    bool operator==(const DiskConfig &other) const = default;
};

/// SMBIOS type 1 (system information). Empty fields keep QEMU's defaults.
struct SmbiosSystemInfo {
    QString manufacturer;
    QString product;
    QString version;
    QString serial;
    QString sku;
    QString family;

    [[nodiscard]] bool isEmpty() const
    {
        return manufacturer.isEmpty() && product.isEmpty() && version.isEmpty() && serial.isEmpty() && sku.isEmpty() && family.isEmpty();
    }
    bool operator==(const SmbiosSystemInfo &other) const = default;
};

/// JSON keys, identical to the property names of QEMU's -smbios type=1.
inline constexpr std::pair<QLatin1StringView, QString SmbiosSystemInfo::*> SmbiosSystemFields[] = {
    {"manufacturer"_L1, &SmbiosSystemInfo::manufacturer},
    {"product"_L1, &SmbiosSystemInfo::product},
    {"version"_L1, &SmbiosSystemInfo::version},
    {"serial"_L1, &SmbiosSystemInfo::serial},
    {"sku"_L1, &SmbiosSystemInfo::sku},
    {"family"_L1, &SmbiosSystemInfo::family},
};

struct Sb16Config {
    int irq = 5;
    int dma = 1;
    int dma16 = 5;
    int iobase = 0x220;

    bool isDefault() const
    {
        return irq == 5 && dma == 1 && dma16 == 5 && iobase == 0x220;
    }

    QString toPropertyString() const
    {
        QStringList props;
        if (irq != 5)
            props << u"irq=%1"_s.arg(irq);
        if (dma != 1)
            props << u"dma=%1"_s.arg(dma);
        if (dma16 != 5)
            props << u"dma16=%1"_s.arg(dma16);
        if (iobase != 0x220)
            props << u"iobase=0x%1"_s.arg(iobase, 0, 16);
        return props.join(u',');
    }
};

struct MacUsageInfo {
    QString vmName;
    QString mac;
    VMSpecs::NetworkMode networkMode = VMSpecs::NetworkMode::UserMode;
    QString virtualNetworkName;
    QString dnsHostname; // effective (explicit or derived from the VM name)
};

class VMConfig
{
    Q_GADGET

public:
    VMConfig() = default;

    enum class ImageType {
        CloudImage,
        InstallerIso,
        Manual
    };
    enum class DeployState {
        None,
        Deploying,
        Cloning,
        FirstBoot,
        Complete
    };
    enum class IconSource {
        None,
        SystemTheme,
        IconSet,
        Custom
    };

    // --- Getter ---
    [[nodiscard]] QString name() const
    {
        return m_name;
    }
    [[nodiscard]] QString comment() const
    {
        return m_comment;
    }
    [[nodiscard]] VMSpecs::OsType osType() const
    {
        return m_osType;
    }
    [[nodiscard]] VMSpecs::Architecture architecture() const
    {
        return m_architecture;
    }
    [[nodiscard]] QString distroId() const
    {
        return m_distroId;
    }

    [[nodiscard]] IconSource iconSource() const
    {
        return m_iconSource;
    }
    [[nodiscard]] QString iconIdentifier() const
    {
        return m_iconIdentifier;
    }
    [[nodiscard]] QIcon customIcon() const
    {
        return m_customIcon;
    }

    [[nodiscard]] VMSpecs::CpuModel cpuModel() const
    {
        return m_cpuModel;
    }

    [[nodiscard]] bool useKvm() const
    {
        return m_useKvm;
    }
    [[nodiscard]] int ramMb() const
    {
        return m_ramMb;
    }
    [[nodiscard]] int cpuCoreCount() const
    {
        return m_cpuCoreCount;
    }
    [[nodiscard]] int diskSizeGb() const
    {
        return m_diskSizeGb;
    }
    [[nodiscard]] QString isoPath() const
    {
        return m_isoPath;
    }
    [[nodiscard]] bool bootFromIso() const
    {
        return m_bootFromIso;
    }
    [[nodiscard]] QString floppyPath() const
    {
        return m_floppyPath;
    }
    [[nodiscard]] bool bootFromFloppy() const
    {
        return m_bootFromFloppy;
    }
    [[nodiscard]] QString extraArgs() const
    {
        return m_extraArgs;
    }
    [[nodiscard]] QString diskPath() const
    {
        return m_diskPath;
    }
    [[nodiscard]] QList<DiskConfig> extraDisks() const
    {
        return m_extraDisks;
    }
    /// System disk first, then the extra disks, as absolute paths.
    [[nodiscard]] QStringList diskPaths(const QDir &vmDir) const;
    /// Relative file names of all disks, system disk first.
    [[nodiscard]] QStringList diskFileNames() const;

    [[nodiscard]] VMSpecs::DisplayType displayType() const
    {
        return m_displayType;
    }
    [[nodiscard]] VMSpecs::GpuType gpuType() const
    {
        return m_gpuType;
    }
    [[nodiscard]] bool gpu3dAcceleration() const
    {
        return m_gpu3dAcceleration;
    }
    [[nodiscard]] int gpuMemMb() const
    {
        return m_gpuMemMb;
    }
    [[nodiscard]] QSize displayResolution() const
    {
        return m_displayResolution;
    }
    [[nodiscard]] VMSpecs::AudioDevice audioDevice() const
    {
        return m_audioDevice;
    }
    [[nodiscard]] Sb16Config sb16Config() const
    {
        return m_sb16Config;
    }
    [[nodiscard]] VMSpecs::DiskInterface diskInterface() const
    {
        return m_diskInterface;
    }
    [[nodiscard]] VMSpecs::UsbController usbController() const
    {
        return m_usbController;
    }
    [[nodiscard]] VMSpecs::PointerDevice pointerDevice() const
    {
        return m_pointerDevice;
    }
    [[nodiscard]] VMSpecs::KeyboardDevice keyboardDevice() const
    {
        return m_keyboardDevice;
    }

    [[nodiscard]] VMSpecs::MachineType machineType() const
    {
        return m_machineType;
    }
    [[nodiscard]] bool useUefi() const
    {
        return m_useUefi;
    }
    [[nodiscard]] bool useMemoryBallooning() const
    {
        return m_useMemoryBallooning;
    }
    [[nodiscard]] bool preallocMemory() const
    {
        return m_preallocMemory;
    }
    [[nodiscard]] bool useLocalTime() const
    {
        return m_useLocalTime;
    }
    [[nodiscard]] bool useTpm() const
    {
        return m_useTpm;
    }
    [[nodiscard]] bool useSecureBoot() const
    {
        return m_useSecureBoot;
    }
    [[nodiscard]] bool useAcpi() const
    {
        return m_useAcpi;
    }
    [[nodiscard]] QString sharedPath() const
    {
        return m_sharedPath;
    }
    [[nodiscard]] QString sharedName() const
    {
        return m_sharedName;
    }

    [[nodiscard]] VMSpecs::NetworkCard networkCard() const
    {
        return m_networkCard;
    }
    [[nodiscard]] QString macAddress() const
    {
        return m_macAddress;
    }

    [[nodiscard]] VMSpecs::NetworkMode networkMode() const
    {
        return m_networkMode;
    }
    [[nodiscard]] QString virtualNetworkName() const
    {
        return m_virtualNetworkName;
    }
    /// Explicit DNS hostname for virtual networks; empty = derived from the
    /// VM name.
    [[nodiscard]] QString dnsHostname() const
    {
        return m_dnsHostname;
    }
    /// dnsHostname() if set, otherwise derived from the VM name.
    [[nodiscard]] QString effectiveDnsHostname() const;

    /// Null for VMs created before KEMU stored one: QEMU then reports an
    /// all-zero UUID, and changing that is up to the user.
    [[nodiscard]] QUuid uuid() const
    {
        return m_uuid;
    }
    [[nodiscard]] SmbiosSystemInfo smbiosSystemInfo() const
    {
        return m_smbiosSystemInfo;
    }

    [[nodiscard]] QList<PortForwardRule> portForwards() const
    {
        return m_portForwards;
    }

    [[nodiscard]] ImageType imageType() const
    {
        return m_imageType;
    }
    [[nodiscard]] DeployState deployState() const
    {
        return m_deployState;
    }
    [[nodiscard]] VMSpecs::ExitAction exitAction() const
    {
        return m_exitAction;
    }
    [[nodiscard]] VMSpecs::GuestShutdownAction guestShutdownAction() const
    {
        return m_guestShutdownAction;
    }

    static VMSpecs::MachineType recommendedMachineType(VMSpecs::CpuModel type);
    static int maxCoresForModel(VMSpecs::CpuModel type);
    static int vgaMemMbLowerLimit(const VMSpecs::GpuType &type);
    static int vgaMemMbUpperLimit(const VMSpecs::GpuType &type);
    static QString x86ArchHint(VMSpecs::CpuModel model);

    // --- Setter ---
    void setName(const QString &name)
    {
        m_name = name;
    }
    void setComment(const QString &comment)
    {
        m_comment = comment;
    }
    void setOsType(const VMSpecs::OsType type)
    {
        m_osType = type;
    }
    void setArchitecture(const VMSpecs::Architecture arch)
    {
        m_architecture = arch;
    }
    void setDistroId(const QString &id)
    {
        m_distroId = id;
    }

    void setIconSource(const IconSource iconsrc)
    {
        m_iconSource = iconsrc;
    }
    void setIconIdentifier(const QString &iconId)
    {
        m_iconIdentifier = iconId;
    }
    void setCustomIcon(const QIcon &icon)
    {
        m_customIcon = icon;
    }

    void setCpuModel(VMSpecs::CpuModel model)
    {
        m_cpuModel = model;
    }

    void setUseKvm(const bool kvm)
    {
        m_useKvm = kvm;
    }
    void setRamMb(const int mb)
    {
        m_ramMb = mb;
    }
    void setMemoryBalloning(const bool ballooning)
    {
        m_useMemoryBallooning = ballooning;
    }
    void setPreallocMemory(const bool prealloc)
    {
        m_preallocMemory = prealloc;
    }
    void setCpuCoreCount(const int count)
    {
        m_cpuCoreCount = count;
    }
    void setDiskSizeGb(const int gb)
    {
        m_diskSizeGb = gb;
    }
    void setISOPath(const QString &path)
    {
        m_isoPath = path;
    }
    void setBootFromIso(const bool bfi)
    {
        m_bootFromIso = bfi;
    }
    void clearISOPath()
    {
        m_isoPath.clear();
    }
    void setFloppyPath(const QString &path)
    {
        m_floppyPath = path;
    }
    void setBootFromFloppy(const bool bff)
    {
        m_bootFromFloppy = bff;
    }
    void clearFloppyPath()
    {
        m_floppyPath.clear();
    }
    void setExtraArgs(const QString &args)
    {
        m_extraArgs = args;
    }
    void setDiskPath(const QString &path)
    {
        m_diskPath = path;
    }
    void setExtraDisks(const QList<DiskConfig> &disks)
    {
        m_extraDisks = disks;
    }
    void addExtraDisk(const DiskConfig &disk)
    {
        m_extraDisks.append(disk);
    }

    void setDisplayType(const VMSpecs::DisplayType type)
    {
        m_displayType = type;
    }
    void setGpuType(const VMSpecs::GpuType type)
    {
        m_gpuType = type;
    }
    void setGpu3dAcceleration(bool enabled)
    {
        m_gpu3dAcceleration = enabled;
    }
    void setGpuMemMb(const int mb)
    {
        m_gpuMemMb = mb;
    }
    void setDisplayResolution(const QSize &res)
    {
        m_displayResolution = res;
    }
    void setAudioDevice(const VMSpecs::AudioDevice device)
    {
        m_audioDevice = device;
    }
    void setSb16Config(const Sb16Config &cfg)
    {
        m_sb16Config = cfg;
    }
    void setDiskInterface(const VMSpecs::DiskInterface interface)
    {
        m_diskInterface = interface;
    }
    void setUsbController(const VMSpecs::UsbController controller)
    {
        m_usbController = controller;
    }
    void setPointerDevice(const VMSpecs::PointerDevice dev)
    {
        m_pointerDevice = dev;
    }
    void setKeyboardDevice(const VMSpecs::KeyboardDevice dev)
    {
        m_keyboardDevice = dev;
    }

    void setMachineType(VMSpecs::MachineType type)
    {
        m_machineType = type;
    }
    void setUseUefi(const bool uefi)
    {
        m_useUefi = uefi;
    }
    void setUseBalloon(const bool balloon)
    {
        m_useMemoryBallooning = balloon;
    }
    void setUsePrealloc(const bool prealloc)
    {
        m_preallocMemory = prealloc;
    }
    void setUseLocalTime(const bool localTime)
    {
        m_useLocalTime = localTime;
    }
    void setUseTpm(const bool tpm)
    {
        m_useTpm = tpm;
    }
    void setUseSecureBoot(const bool sb)
    {
        m_useSecureBoot = sb;
    }
    void setUseAcpi(const bool acpi)
    {
        m_useAcpi = acpi;
    }
    void setSharedPath(const QString &path)
    {
        m_sharedPath = path;
    }
    void setSharedName(const QString &name)
    {
        m_sharedName = name;
    }

    void setNetworkCard(const VMSpecs::NetworkCard card)
    {
        m_networkCard = card;
    }
    void setMacAddress(const QString &mac)
    {
        m_macAddress = mac;
    }
    void ensureMacAddress(const QStringList &usedMacs);

    void setNetworkMode(VMSpecs::NetworkMode mode)
    {
        m_networkMode = mode;
    }
    void setVirtualNetworkName(const QString &name)
    {
        m_virtualNetworkName = name;
    }
    void setDnsHostname(const QString &hostname)
    {
        m_dnsHostname = hostname;
    }

    void setUuid(const QUuid &uuid)
    {
        m_uuid = uuid;
    }
    /// Assigns a random UUID not contained in @p usedUuids.
    void regenerateUuid(const QList<QUuid> &usedUuids);
    void setSmbiosSystemInfo(const SmbiosSystemInfo &info)
    {
        m_smbiosSystemInfo = info;
    }

    void setPortForwards(const QList<PortForwardRule> &rules)
    {
        m_portForwards = rules;
    }
    void addPortForward(const PortForwardRule &rule)
    {
        m_portForwards.append(rule);
    }
    void removePortForward(int index)
    {
        m_portForwards.removeAt(index);
    }
    void clearPortForwards()
    {
        m_portForwards.clear();
    }

    void setImageType(ImageType type)
    {
        m_imageType = type;
    }
    void setDeployState(DeployState state)
    {
        m_deployState = state;
    }

    void setExitAction(const VMSpecs::ExitAction action)
    {
        m_exitAction = action;
    }
    void setGuestShutdownAction(const VMSpecs::GuestShutdownAction action)
    {
        m_guestShutdownAction = action;
    }

    // --- Bus Type Helpers ---
    [[nodiscard]] static bool isIsaDevice(VMSpecs::GpuType type);
    [[nodiscard]] static bool isIsaDevice(VMSpecs::AudioDevice device);
    [[nodiscard]] static bool isIsaDevice(VMSpecs::NetworkCard card);
    [[nodiscard]] static bool supportsIsaBus(VMSpecs::MachineType type);
    [[nodiscard]] static bool supportsPciBus(VMSpecs::MachineType type);
    [[nodiscard]] static bool isCpuCompatible(VMSpecs::CpuModel cpu, VMSpecs::MachineType machine);

    // --- Icon Helpers ---
    [[nodiscard]] QIcon icon(int size) const;
    [[nodiscard]] QString iconSetPath() const; // Get the filesystem path for IconSet icons
    [[nodiscard]] static QString customIconFileName();
    bool saveCustomIcon(const QDir &dir) const;
    bool loadCustomIcon(const QDir &dir);
    bool removeCustomIcon(const QDir &dir);

    // --- Static Icon Helper ---
    enum class VmVisualState {
        Stopped,
        Running,
        Saved
    };
    [[nodiscard]] static QIcon styledVmIcon(const QIcon &baseIcon, VmVisualState state, bool isHeadless, int size = KEMUConst::ListViewIconSize);
    [[nodiscard]] static QString osIconDir();
    [[nodiscard]] static QString defaultIconForType(VMSpecs::OsType type);
    [[nodiscard]] static QString defaultIconFor(VMSpecs::OsType type, const QString &variant);

    [[nodiscard]] static bool isAllowedCharInVmName(const QChar c);
    [[nodiscard]] static bool isValidVmName(QStringView name, QString *error = nullptr);

    // --- Extra disks ---
    [[nodiscard]] static bool isValidExtraDiskFileName(QStringView fileName);
    /// Lowest free "disk-<n>.qcow2" not listed in @p taken.
    [[nodiscard]] static QString nextExtraDiskFileName(const QStringList &taken);
    [[nodiscard]] static QString extraDiskDriveId(const QString &fileName);
    [[nodiscard]] static QString defaultDiskSerial(const QString &fileName);
    [[nodiscard]] static int ideSlotCapacity(VMSpecs::MachineType type);
    /// Empty if the disk layout fits the machine, otherwise a user-facing
    /// reason. The seed ISO of a pending cloud-init deployment takes an IDE
    /// slot of its own.
    [[nodiscard]] QString storageLayoutError(bool seedIsoPresent = false) const;

    // --- Machine identity ---
    /// Printable ASCII, at most KEMUConst::MaxSmbiosStringLength characters.
    /// Empty means unset and is valid.
    [[nodiscard]] static bool isValidSmbiosString(const QString &value);
    [[nodiscard]] static bool supportsSmbios(VMSpecs::Architecture arch);

    [[nodiscard]] static QString toQemuSystemBinary(VMSpecs::Architecture arch);

    /// True if this build can actually provide the given display type.
    [[nodiscard]] static bool isDisplayTypeAvailable(VMSpecs::DisplayType type);

    // --- QEMU Strings ---
    [[nodiscard]] static QString toQemuString(VMSpecs::CpuModel model);
    [[nodiscard]] static QString toQemuString(VMSpecs::NetworkCard card);
    [[nodiscard]] static QString toQemuString(VMSpecs::DiskInterface interface);
    [[nodiscard]] static QString toQemuString(VMSpecs::DisplayType type);
    [[nodiscard]] static QString toQemuString(VMSpecs::AudioDevice device);

    // --- UI Strings (i18n) ---
    [[nodiscard]] static QString toUIString(VMSpecs::CpuModel model);
    [[nodiscard]] static QString toUIString(VMSpecs::NetworkCard card);
    [[nodiscard]] static QString toUIString(VMSpecs::AudioDevice device);
    [[nodiscard]] static QString toUIString(VMSpecs::DiskInterface interface);
    [[nodiscard]] static QString toUIString(VMSpecs::GpuType type);
    [[nodiscard]] static QString toUIString(VMSpecs::DisplayType type);
    [[nodiscard]] static QString toUIString(VMSpecs::UsbController controller);
    [[nodiscard]] static QString toUIString(VMSpecs::PointerDevice dev);
    [[nodiscard]] static QString toUIString(VMSpecs::KeyboardDevice dev);
    [[nodiscard]] static QString toUIString(VMSpecs::MachineType type);
    [[nodiscard]] static QString toUIString(VMSpecs::OsType type);
    [[nodiscard]] static QString toUIString(VMSpecs::Architecture arch);
    [[nodiscard]] static QString toUIString(VMSpecs::NetworkMode mode);
    [[nodiscard]] static QString toUIString(VMSpecs::ExitAction action);
    [[nodiscard]] static QString toUIString(VMSpecs::GuestShutdownAction action);
    [[nodiscard]] static QString firmwareToUIString(bool useUefi, bool useSecureBoot);

    // --- Common Strings ---
    [[nodiscard]] static QString toString(VMSpecs::CpuModel model);
    [[nodiscard]] static QString toString(VMSpecs::DisplayType type);
    [[nodiscard]] static QString toString(VMSpecs::MachineType type);
    [[nodiscard]] static QString toString(VMSpecs::GpuType type);
    [[nodiscard]] static QString toString(VMSpecs::AudioDevice type);
    [[nodiscard]] static QString toString(VMSpecs::NetworkCard type);
    [[nodiscard]] static QString toString(VMSpecs::DiskInterface type);
    [[nodiscard]] static QString toString(VMSpecs::UsbController type);
    [[nodiscard]] static QString toString(VMSpecs::PointerDevice dev);
    [[nodiscard]] static QString toString(VMSpecs::KeyboardDevice dev);
    [[nodiscard]] static QString toString(PortForwardRule::Protocol proto);
    [[nodiscard]] static QString toString(VMSpecs::OsType type);
    [[nodiscard]] static QString toString(VMSpecs::Architecture arch);
    [[nodiscard]] static QString toString(VMSpecs::NetworkMode mode);
    [[nodiscard]] static QString toString(VMSpecs::ExitAction action);
    [[nodiscard]] static QString toString(VMSpecs::GuestShutdownAction action);
    [[nodiscard]] static QString toString(ImageType type);
    [[nodiscard]] static QString toString(DeployState state);
    [[nodiscard]] static QString toString(IconSource src);

    // --- Parse Strings ---
    [[nodiscard]] static VMSpecs::CpuModel parseCpuModel(const QString &str);
    [[nodiscard]] static VMSpecs::DisplayType parseDisplayType(const QString &str);
    [[nodiscard]] static VMSpecs::MachineType parseMachineType(const QString &str);
    [[nodiscard]] static VMSpecs::GpuType parseGpuType(const QString &str);
    [[nodiscard]] static VMSpecs::AudioDevice parseAudioDevice(const QString &str);
    [[nodiscard]] static VMSpecs::NetworkCard parseNetworkCard(const QString &str);
    [[nodiscard]] static VMSpecs::DiskInterface parseDiskInterface(const QString &str);
    [[nodiscard]] static VMSpecs::UsbController parseUSBController(const QString &str);
    [[nodiscard]] static VMSpecs::PointerDevice parsePointerDevice(const QString &str);
    [[nodiscard]] static VMSpecs::KeyboardDevice parseKeyboardDevice(const QString &str);
    [[nodiscard]] static PortForwardRule::Protocol parseProtocol(const QString &str);
    [[nodiscard]] static VMSpecs::OsType parseOsType(const QString &str);
    [[nodiscard]] static VMSpecs::Architecture parseArchitecture(const QString &str);
    [[nodiscard]] static VMSpecs::NetworkMode parseNetworkMode(const QString &str);
    [[nodiscard]] static VMSpecs::ExitAction parseExitAction(const QString &str);
    [[nodiscard]] static VMSpecs::GuestShutdownAction parseGuestShutdownAction(const QString &str);
    [[nodiscard]] static ImageType parseImageType(const QString &str);
    [[nodiscard]] static DeployState parseDeployState(const QString &str);
    [[nodiscard]] static IconSource parseIconSource(const QString &str);

    static QPair<VMSpecs::OsType, QString> guessOSFromText(const QString &string, bool *found = nullptr);

    // --- JSON Serialization ---
    [[nodiscard]] QJsonObject toJson() const;
    static VMConfig fromJson(const QJsonObject &json);

    void normalizePaths(const QDir &baseDir);

    /// Drops settings that an imported archive must not control: host paths
    /// and raw QEMU arguments. Returns the labels of the dropped settings.
    QStringList dropUntrustedSettings(const QDir &baseDir);

private:
    QString m_name;
    QString m_comment;
    VMSpecs::OsType m_osType = GlobalDefaults.osType;
    VMSpecs::Architecture m_architecture = GlobalDefaults.architecture;
    QString m_distroId;

    IconSource m_iconSource = IconSource::None;
    QString m_iconIdentifier;
    QIcon m_customIcon = {};

    VMSpecs::CpuModel m_cpuModel = GlobalDefaults.cpuModel;
    bool m_useKvm = true;

    int m_ramMb = GlobalDefaults.ramMb;
    bool m_useMemoryBallooning = GlobalDefaults.useMemoryBallooning;
    bool m_preallocMemory = GlobalDefaults.preallocMemory;
    int m_cpuCoreCount = GlobalDefaults.cpuCoreCount;
    int m_diskSizeGb = GlobalDefaults.diskSizeGb;
    QString m_isoPath;
    bool m_bootFromIso = false;
    QString m_floppyPath;
    bool m_bootFromFloppy = false;
    QString m_extraArgs;
    QString m_diskPath;
    QList<DiskConfig> m_extraDisks;

    VMSpecs::DisplayType m_displayType = GlobalDefaults.displayType;
    VMSpecs::GpuType m_gpuType = GlobalDefaults.gpuType;
    bool m_gpu3dAcceleration = GlobalDefaults.gpu3dAcceleration;
    int m_gpuMemMb = GlobalDefaults.gpuMemMb;
    QSize m_displayResolution = GlobalDefaults.displayResolution;
    VMSpecs::AudioDevice m_audioDevice = GlobalDefaults.audioDevice;
    Sb16Config m_sb16Config;
    VMSpecs::DiskInterface m_diskInterface = GlobalDefaults.diskInterface;
    VMSpecs::UsbController m_usbController = GlobalDefaults.usbController;
    VMSpecs::PointerDevice m_pointerDevice = GlobalDefaults.pointerDevice;
    VMSpecs::KeyboardDevice m_keyboardDevice = GlobalDefaults.keyboardDevice;

    VMSpecs::MachineType m_machineType = GlobalDefaults.machineType;
    bool m_useUefi = GlobalDefaults.useUefi;
    bool m_useLocalTime = GlobalDefaults.useLocalTime;
    bool m_useTpm = GlobalDefaults.useTpm;
    bool m_useSecureBoot = GlobalDefaults.useSecureBoot;
    bool m_useAcpi = GlobalDefaults.useAcpi;
    QString m_sharedPath;
    QString m_sharedName;

    VMSpecs::NetworkCard m_networkCard = GlobalDefaults.networkCard;
    VMSpecs::NetworkMode m_networkMode = GlobalDefaults.networkMode;
    QString m_macAddress = GlobalDefaults.macAddress;
    QString m_virtualNetworkName;
    QString m_dnsHostname;

    QUuid m_uuid;
    SmbiosSystemInfo m_smbiosSystemInfo;

    QList<PortForwardRule> m_portForwards;

    ImageType m_imageType = ImageType::Manual;
    DeployState m_deployState = DeployState::None;

    VMSpecs::ExitAction m_exitAction = GlobalDefaults.exitAction;
    VMSpecs::GuestShutdownAction m_guestShutdownAction = GlobalDefaults.guestShutdownAction;
};
