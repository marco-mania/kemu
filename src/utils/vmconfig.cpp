/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmconfig.h"

#include "const.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

int VMConfig::maxCoresForModel(VMSpecs::CpuModel type)
{
    switch (type) {
        // Pure Single-Core Era (DOS, Win 3.11, Win 95/98)
    case VMSpecs::CpuModel::I486:
    case VMSpecs::CpuModel::Pentium:
    case VMSpecs::CpuModel::Pentium2:
    case VMSpecs::CpuModel::Pentium3:
    case VMSpecs::CpuModel::Pentium4:
        return 1;

        // Early Dual-Core Era
    case VMSpecs::CpuModel::CoreDuo:
    case VMSpecs::CpuModel::Core2Duo:
        return 2;

        // Modern Multicore CPUs
    case VMSpecs::CpuModel::Nehalem:
    case VMSpecs::CpuModel::Phenom:
        return 4; // Realistic limit for that period

    case VMSpecs::CpuModel::SandyBridge:
    case VMSpecs::CpuModel::Haswell:
        return 8;

        // Generic or Host CPUs are allowed to use all host resources
    case VMSpecs::CpuModel::Host:
    case VMSpecs::CpuModel::Qemu32:
    case VMSpecs::CpuModel::Qemu64:
    case VMSpecs::CpuModel::Skylake:
    case VMSpecs::CpuModel::Epyc:
    default:
        // Returns the physical/logical threads of the actual host PC
        // (Fallback to 8 if idealThreadCount returns -1)
        int hostThreads = QThread::idealThreadCount();
        return hostThreads > 0 ? hostThreads : 8;
    }
}

VMSpecs::MachineType VMConfig::recommendedMachineType(VMSpecs::CpuModel type)
{
    switch (type) {
    // Legacy CPUs generally require the old i440FX chipset (classic PCI bus)
    // for legacy operating systems such as DOS, Windows 95/98/ME.
    case VMSpecs::CpuModel::I486:
    case VMSpecs::CpuModel::Pentium:
    case VMSpecs::CpuModel::Pentium2:
    case VMSpecs::CpuModel::Pentium3:
    case VMSpecs::CpuModel::Pentium4:
    case VMSpecs::CpuModel::Qemu32: // for old systems
        return VMSpecs::MachineType::Pc;

        // All modern CPUs from the Core era onwards benefit from the Q35 chipset (PCIe bus)
    case VMSpecs::CpuModel::Host:
    case VMSpecs::CpuModel::Qemu64:
    case VMSpecs::CpuModel::CoreDuo:
    case VMSpecs::CpuModel::Core2Duo:
    case VMSpecs::CpuModel::Nehalem:
    case VMSpecs::CpuModel::SandyBridge:
    case VMSpecs::CpuModel::Haswell:
    case VMSpecs::CpuModel::Skylake:
    case VMSpecs::CpuModel::Phenom:
    case VMSpecs::CpuModel::Epyc:
    default:
        return VMSpecs::MachineType::Q35;
    }
}

int VMConfig::vgaMemMbLowerLimit(const VMSpecs::GpuType& type)
{
    switch (type) {
    case VMSpecs::GpuType::CirrusGD5430:
    case VMSpecs::GpuType::CirrusGD5446:
        return 4;
    default:
        return 1;
    }
}

int VMConfig::vgaMemMbUpperLimit(const VMSpecs::GpuType& type)
{
    switch (type) {
    case VMSpecs::GpuType::VgaISA:
        return 2;
    case VMSpecs::GpuType::CirrusGD5430:
        return 16;
    case VMSpecs::GpuType::CirrusGD5446:
        return 16;
        break;
    case VMSpecs::GpuType::Virtio:
        return 2048;
        break;
    default:
        return 256;
    }
}

void VMConfig::ensureMacAddress(const QStringList& usedMacs)
{
    if (!m_macAddress.isEmpty())
        return;
    if (m_networkCard == VMSpecs::NetworkCard::None)
        return;
    m_macAddress = Utils::generateRandomMac(usedMacs);
}

// --- Bus Type Helpers ---

bool VMConfig::isIsaDevice(VMSpecs::GpuType type)
{
    return type == VMSpecs::GpuType::VgaISA
        || type == VMSpecs::GpuType::CirrusGD5430;
}

bool VMConfig::isIsaDevice(VMSpecs::AudioDevice device)
{
    return device == VMSpecs::AudioDevice::Sb16
        || device == VMSpecs::AudioDevice::Adlib
        || device == VMSpecs::AudioDevice::PcSpkr;
}

bool VMConfig::isIsaDevice(VMSpecs::NetworkCard card)
{
    return card == VMSpecs::NetworkCard::Ne2000;
}

bool VMConfig::supportsIsaBus(VMSpecs::MachineType type)
{
    return type == VMSpecs::MachineType::Pc
        || type == VMSpecs::MachineType::PcISA;
}

bool VMConfig::supportsPciBus(VMSpecs::MachineType type)
{
    return type == VMSpecs::MachineType::Q35
        || type == VMSpecs::MachineType::Pc;
}

bool VMConfig::isCpuCompatible(VMSpecs::CpuModel cpu, VMSpecs::MachineType machine)
{
    // Ultra legacy CPUs suitable for pure ISA (isapc)
    const bool isUltraLegacy = (cpu == VMSpecs::CpuModel::I486 || cpu == VMSpecs::CpuModel::Pentium || cpu == VMSpecs::CpuModel::Qemu32);

    // Pre-PCIe CPUs
    const bool isPreCore = isUltraLegacy || cpu == VMSpecs::CpuModel::Pentium2 || cpu == VMSpecs::CpuModel::Pentium3 || cpu == VMSpecs::CpuModel::Pentium4;

    if (machine == VMSpecs::MachineType::Q35) {
        return !isPreCore;
    } else if (machine == VMSpecs::MachineType::PcISA) {
        return isUltraLegacy;
    }

    // Standard PC (i440FX) fallback
    return true;
}

// --- Icon Helpers ---

QIcon VMConfig::icon(int size) const
{
    Q_UNUSED(size)

    if (m_iconSource == IconSource::Custom && !m_customIcon.isNull())
        return m_customIcon;

    if (m_iconSource == IconSource::IconSet) {
        const QString dir = osIconDir();
        if (!dir.isEmpty()) {
            const QString fullPath = dir + u"/"_s + m_iconIdentifier;
            if (QFile::exists(fullPath))
                return QIcon(fullPath);
        }
    }

    if (m_iconSource == IconSource::SystemTheme && !m_iconIdentifier.isEmpty())
        return QIcon::fromTheme(m_iconIdentifier);

    return QIcon::fromTheme(KEMUConst::VMDefaultIcon);
}

QString VMConfig::iconSetPath() const
{
    if (m_iconSource != IconSource::IconSet || m_iconIdentifier.isEmpty())
        return { };
    QString dir = osIconDir();
    if (dir.isEmpty())
        return { };
    QString fullPath = dir + u"/"_s + m_iconIdentifier;
    return QFile::exists(fullPath) ? fullPath : QString();
}

bool VMConfig::saveCustomIcon(const QDir& dir) const
{
    if (m_iconSource != IconSource::Custom || m_customIcon.isNull()) {
        return false;
    }

    const QString iconPath = dir.absoluteFilePath(KEMUConst::CustomIconFileNameBase + u'.' + QString(KEMUConst::CustomIconFileFormat).toLower());
    QFile iconFile(iconPath);

    if (!iconFile.open(QIODevice::WriteOnly)) {
        return false;
    }

    return m_customIcon.pixmap(KEMUConst::CustomIconStoreSize, KEMUConst::CustomIconStoreSize).save(&iconFile, KEMUConst::CustomIconFileFormat.constData());
}

bool VMConfig::loadCustomIcon(const QDir& dir)
{
    if (m_iconSource != IconSource::Custom) {
        return false;
    }

    QString iconPath;
    if (!m_iconIdentifier.isEmpty())
        iconPath = dir.absoluteFilePath(m_iconIdentifier);
    else
        iconPath = dir.absoluteFilePath(KEMUConst::CustomIconFileNameBase + u'.' + QString(KEMUConst::CustomIconFileFormat).toLower());

    if (!QFile::exists(iconPath)) {
        return false;
    }

    m_customIcon = QIcon(iconPath);
    if (m_customIcon.isNull()) {
        return false;
    }

    m_iconSource = IconSource::Custom;
    return true;
}

bool VMConfig::removeCustomIcon(const QDir& dir)
{
    QString iconPath;
    if (!m_iconIdentifier.isEmpty())
        iconPath = dir.absoluteFilePath(m_iconIdentifier);
    else
        iconPath = dir.absoluteFilePath(KEMUConst::CustomIconFileNameBase + u'.' + QString(KEMUConst::CustomIconFileFormat).toLower());

    m_iconSource = IconSource::SystemTheme;
    m_iconIdentifier = KEMUConst::VMDefaultIcon;

    return QFile::remove(iconPath);
}

// --- Static Icon Helper ---

QIcon VMConfig::styledVmIcon(const QIcon& baseIcon, bool isRunning,
    bool isHeadless, int size)
{
    const qreal dpr = qApp->devicePixelRatio();
    const int margin = 4;
    const int totalSize = size + margin * 2;

    // Allocate at device-pixel resolution, but mark with DPR
    // so QPainter works in logical coordinates (0 … totalSize)
    QPixmap result(qRound(totalSize * dpr), qRound(totalSize * dpr));
    result.setDevicePixelRatio(dpr);
    result.fill(Qt::transparent);

    // Request base icon at logical size with DPR hint –
    // Qt returns a high-res pixmap with DPR already set
    QPixmap basePix = baseIcon.pixmap(QSize(size, size), dpr);
    if (basePix.isNull())
        return baseIcon;

    QPainter painter(&result);
    painter.setRenderHint(QPainter::Antialiasing);

    // ---- All coordinates below are LOGICAL ----

    if (isRunning) {
        QRadialGradient glow(totalSize / 2.0, totalSize / 2.0, totalSize / 2.0);
        glow.setColorAt(0.0, QColor(76, 175, 80, 100));
        glow.setColorAt(0.6, QColor(76, 175, 80, 50));
        glow.setColorAt(1.0, QColor(76, 175, 80, 0));
        painter.setBrush(glow);
        painter.setPen(Qt::NoPen);
        painter.drawEllipse(0, 0, totalSize, totalSize);

        painter.drawPixmap(QRect(margin, margin, size, size), basePix);

        int emblemSize = size / 2;
        int ex = totalSize - emblemSize - 1;
        int ey = totalSize - emblemSize - 1;

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 200));
        painter.drawEllipse(ex - 1, ey - 1, emblemSize + 2, emblemSize + 2);

        painter.setBrush(isHeadless ? QColor(33, 150, 243) : QColor(76, 175, 80));
        painter.drawEllipse(ex, ey, emblemSize, emblemSize);

        painter.setBrush(Qt::white);
        int m = emblemSize / 4;
        QPolygon tri;
        tri << QPoint(ex + m + 2, ey + m)
            << QPoint(ex + emblemSize - m, ey + emblemSize / 2)
            << QPoint(ex + m + 2, ey + emblemSize - m);
        painter.drawPolygon(tri);
    } else {
        QImage img = basePix.toImage();
        for (int y = 0; y < img.height(); ++y) {
            for (int x = 0; x < img.width(); ++x) {
                QColor c = img.pixelColor(x, y);
                int gray = qGray(c.red(), c.green(), c.blue());
                img.setPixelColor(x, y,
                    QColor(gray, gray, gray, c.alpha() * 0.5));
            }
        }
        // Map full device-pixel image into logical rect
        painter.drawImage(QRectF(margin, margin, size, size),
            img, QRectF(0, 0, img.width(), img.height()));

        int emblemSize = size / 2;
        int ex = totalSize - emblemSize - 1;
        int ey = totalSize - emblemSize - 1;

        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 200));
        painter.drawEllipse(ex - 1, ey - 1, emblemSize + 2, emblemSize + 2);

        painter.setBrush(QColor(158, 158, 158));
        painter.drawEllipse(ex, ey, emblemSize, emblemSize);

        painter.setBrush(Qt::white);
        int sm = emblemSize / 3;
        painter.drawRect(ex + sm, ey + sm,
            emblemSize - 2 * sm, emblemSize - 2 * sm);
    }

    painter.end();
    return QIcon(result);
}

QString VMConfig::osIconDir()
{
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation, KEMUConst::OsIconDirPath, QStandardPaths::LocateDirectory);
}

QString VMConfig::defaultIconForType(VMSpecs::OsType type)
{
    QString filename;
    switch (type) {
    case VMSpecs::OsType::Linux:
        filename = u"papirus-icons/tux.svg"_s;
        break;
    case VMSpecs::OsType::Windows:
        filename = u"papirus-icons/distributor-logo-windows.svg"_s;
        break;
    case VMSpecs::OsType::DOS:
        filename = u"kemu-icons/dos.svg"_s;
        break;
    case VMSpecs::OsType::BSD:
        filename = u"misc-icons/beastie.svg"_s;
        break;
    case VMSpecs::OsType::Other:
        filename = u"misc-icons/generic-vm.svg"_s;
        break;
    }

    QString dir = osIconDir();
    if (dir.isEmpty()) {
        return { };
    }
    return dir + u"/"_s + filename;
}

QString VMConfig::defaultIconFor(VMSpecs::OsType type, const QString& variant)
{
    const QString lv = variant.toLower().remove(u' ');
    QString filename;

    if (lv.contains(u"debian"_s))
        filename = u"papirus-icons/distributor-logo-debian.svg"_s;
    else if (lv.contains(u"kubuntu"_s))
        filename = u"papirus-icons/distributor-logo-kubuntu.svg"_s;
    else if (lv.contains(u"ubuntu"_s))
        filename = u"papirus-icons/distributor-logo-ubuntu.svg"_s;
    else if (lv.contains(u"neon"_s))
        filename = u"papirus-icons/distributor-logo-neon.svg"_s;
    else if (lv.contains(u"archlinux"_s))
        filename = u"papirus-icons/distributor-logo-archlinux.svg"_s;
    else if (lv.contains(u"fedora"_s))
        filename = u"papirus-icons/distributor-logo-fedora.svg"_s;
    else if (lv.contains(u"redhat"_s) || lv.contains(u"rhel"_s))
        filename = u"papirus-icons/distributor-logo-redhat.svg"_s;
    else if (lv.contains(u"opensuse"_s))
        filename = u"papirus-icons/distributor-logo-opensuse.svg"_s;
    else if (lv.contains(u"manjaro"_s))
        filename = u"papirus-icons/distributor-logo-manjaro.svg"_s;
    else if (lv.contains(u"linuxmint"_s) || lv.contains(u"mint"_s))
        filename = u"papirus-icons/distributor-logo-linuxmint.svg"_s;
    else if (lv.contains(u"nixos"_s))
        filename = u"papirus-icons/distributor-logo-nixos.svg"_s;
    else if (lv.contains(u"gentoo"_s))
        filename = u"papirus-icons/distributor-logo-gentoo.svg"_s;
    else if (lv.contains(u"voidlinux"_s) || lv.contains(u"void"_s))
        filename = u"papirus-icons/distributor-logo-voidlinux.svg"_s;
    else if (lv.contains(u"centos"_s))
        filename = u"papirus-icons/distributor-logo-centos.svg"_s;
    else if (lv.contains(u"deepin"_s))
        filename = u"papirus-icons/distributor-logo-deepin.svg"_s;
    else if (lv.contains(u"devuan"_s))
        filename = u"papirus-icons/distributor-logo-devuan.svg"_s;
    else if (lv.contains(u"windows"_s))
        filename = u"papirus-icons/distributor-logo-windows.svg"_s;
    else if (lv.contains(u"msdos"_s) || lv.contains(u"ms-dos"_s))
        filename = u"papirus-icons/distributor-logo-ms-dos.svg"_s;
    else if (lv.contains(u"freedos"_s))
        filename = u"papirus-icons/distributor-logo-freedos.svg"_s;
    else if (lv.contains(u"freebsd"_s))
        filename = u"papirus-icons/distributor-logo-freebsd.svg"_s;
    else if (lv.contains(u"openbsd"_s))
        filename = u"papirus-icons/distributor-logo-openbsd.svg"_s;
    else if (lv.contains(u"netbsd"_s))
        filename = u"papirus-icons/distributor-logo-netbsd.svg"_s;
    else if (lv.contains(u"midnightbsd"_s))
        filename = u"papirus-icons/distributor-logo-midnightbsd.svg"_s;

    if (filename.isEmpty())
        return defaultIconForType(type);

    const QString dir = osIconDir();
    if (dir.isEmpty())
        return defaultIconForType(type);

    const QString path = dir + u"/"_s + filename;
    return QFile::exists(path) ? path : defaultIconForType(type);
}

// --- QEMU Strings ---

QString VMConfig::toQemuString(VMSpecs::CpuModel model)
{
    switch (model) {
    case VMSpecs::CpuModel::Host:
        return u"host"_s;
    case VMSpecs::CpuModel::Qemu64:
        return u"qemu64"_s;
    case VMSpecs::CpuModel::Qemu32:
        return u"qemu32"_s;
    case VMSpecs::CpuModel::I486:
        return u"486"_s;
    case VMSpecs::CpuModel::Pentium:
        return u"pentium"_s;
    case VMSpecs::CpuModel::Pentium2:
        return u"pentium2"_s;
    case VMSpecs::CpuModel::Pentium3:
        return u"pentium3"_s;
    case VMSpecs::CpuModel::Pentium4:
        return u"Prescott"_s;
    case VMSpecs::CpuModel::CoreDuo:
        return u"coreduo"_s;
    case VMSpecs::CpuModel::Core2Duo:
        return u"core2duo"_s;
    case VMSpecs::CpuModel::Nehalem:
        return u"Nehalem"_s;
    case VMSpecs::CpuModel::SandyBridge:
        return u"SandyBridge"_s;
    case VMSpecs::CpuModel::Haswell:
        return u"Haswell"_s;
    case VMSpecs::CpuModel::Skylake:
        return u"Skylake-Client"_s;
    case VMSpecs::CpuModel::Phenom:
        return u"phenom"_s;
    case VMSpecs::CpuModel::Epyc:
        return u"EPYC"_s; // EPYC represents AMD Zen architecture
    default:
        return u"host"_s;
    }
}

QString VMConfig::toQemuString(VMSpecs::NetworkCard card)
{
    switch (card) {
    case VMSpecs::NetworkCard::Virtio:
        return u"virtio-net-pci"_s;
    case VMSpecs::NetworkCard::E1000:
        return u"e1000"_s;
    case VMSpecs::NetworkCard::Rtl8139:
        return u"rtl8139"_s;
    case VMSpecs::NetworkCard::Pcnet:
        return u"pcnet"_s;
    case VMSpecs::NetworkCard::Rtl8029:
        return u"ne2k_pci"_s;
    case VMSpecs::NetworkCard::Ne2000:
        return u"ne2k_isa"_s;
    case VMSpecs::NetworkCard::I82551:
        return u"i82551"_s;
    case VMSpecs::NetworkCard::I82557b:
        return u"i82557b"_s;
    case VMSpecs::NetworkCard::I82559er:
        return u"i82559er"_s;
    case VMSpecs::NetworkCard::None:
        return { };
    }
    return { };
}

QString VMConfig::toQemuString(VMSpecs::DiskInterface interface)
{
    switch (interface) {
    case VMSpecs::DiskInterface::Virtio:
        return u"virtio-blk-pci"_s;
    case VMSpecs::DiskInterface::Nvme:
        return u"nvme"_s;
    case VMSpecs::DiskInterface::SataIde:
        return u"ide-hd"_s;
    }
    return { };
}

QString VMConfig::toQemuString(VMSpecs::DisplayType type)
{
    switch (type) {
    case VMSpecs::DisplayType::Gtk:
        return u"gtk"_s;
    case VMSpecs::DisplayType::Sdl:
        return u"sdl"_s;
    case VMSpecs::DisplayType::Spice:
        return u"none"_s; // SPICE uses -display none; the viewer is separate
    case VMSpecs::DisplayType::Dbus:
        return u"dbus"_s;
    }
    return { };
}

QString VMConfig::toQemuString(VMSpecs::AudioDevice device)
{
    switch (device) {
    case VMSpecs::AudioDevice::Hda:
        return u"intel-hda"_s;
    case VMSpecs::AudioDevice::Ac97:
        return u"ac97"_s;
    case VMSpecs::AudioDevice::Es1370:
        return u"ES1370"_s;
    case VMSpecs::AudioDevice::Sb16:
        return u"sb16"_s;
    case VMSpecs::AudioDevice::Adlib:
        return u"adlib"_s;
    case VMSpecs::AudioDevice::PcSpkr:
        return u"pcspk"_s;
    case VMSpecs::AudioDevice::None:
        return { };
    }
    return { };
}

// --- UI Strings (i18n) ---

QString VMConfig::toUIString(VMSpecs::CpuModel model)
{
    switch (model) {
    case VMSpecs::CpuModel::Host:
        return i18n("Host CPU");
    case VMSpecs::CpuModel::Qemu64:
        return i18n("QEMU Standard CPU 64-Bit");
    case VMSpecs::CpuModel::Qemu32:
        return i18n("QEMU Standard CPU 32-Bit");
    case VMSpecs::CpuModel::I486:
        return i18n("Intel 80486 (1989)");
    case VMSpecs::CpuModel::Pentium:
        return i18n("Intel Pentium (1993)");
    case VMSpecs::CpuModel::Pentium2:
        return i18n("Intel Pentium II (1997)");
    case VMSpecs::CpuModel::Pentium3:
        return i18n("Intel Pentium III (1999)");
    case VMSpecs::CpuModel::Pentium4:
        return i18n("Intel Pentium 4 (Prescott 2004)");
    case VMSpecs::CpuModel::CoreDuo:
        return i18n("Intel Core Duo (32-Bit, 2006)");
    case VMSpecs::CpuModel::Core2Duo:
        return i18n("Intel Core 2 Duo (2006)");
    case VMSpecs::CpuModel::Nehalem:
        return i18n("Intel Core i, 1st Gen (Nehalem 2008)");
    case VMSpecs::CpuModel::SandyBridge:
        return i18n("Intel Core i, 2nd Gen (Sandy Bridge 2011)");
    case VMSpecs::CpuModel::Haswell:
        return i18n("Intel Core i, 4th Gen (Haswell 2013)");
    case VMSpecs::CpuModel::Skylake:
        return i18n("Intel Core i, 6th Gen (Skylake 2015)");
    case VMSpecs::CpuModel::Phenom:
        return i18n("AMD Phenom (2007)");
    case VMSpecs::CpuModel::Epyc:
        return i18n("AMD Ryzen / EPYC (Zen 1, 2017)");
    default:
        return i18n("Unknown");
    }
}

QString VMConfig::toUIString(VMSpecs::NetworkCard card)
{
    switch (card) {
    case VMSpecs::NetworkCard::None:
        return i18n("Not connected");
    case VMSpecs::NetworkCard::Virtio:
        return i18n("VirtIO");
    case VMSpecs::NetworkCard::E1000:
        return i18n("Intel PRO/1000");
    case VMSpecs::NetworkCard::Rtl8139:
        return i18n("Realtek RTL8139");
    case VMSpecs::NetworkCard::Pcnet:
        return i18n("AMD PCnet-Fast III");
    case VMSpecs::NetworkCard::Rtl8029:
        return i18n("Realtek RTL8029");
    case VMSpecs::NetworkCard::I82551:
        return i18n("Intel PRO/100 (i82551)");
    case VMSpecs::NetworkCard::I82557b:
        return i18n("Intel PRO/100 (i82557b)");
    case VMSpecs::NetworkCard::I82559er:
        return i18n("Intel PRO/100 (i82559er)");
    case VMSpecs::NetworkCard::Ne2000:
        return i18n("Novell NE2000");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::AudioDevice device)
{
    switch (device) {
    case VMSpecs::AudioDevice::None:
        return i18n("No Audio");
    case VMSpecs::AudioDevice::Hda:
        return i18n("Intel HD Audio");
    case VMSpecs::AudioDevice::Ac97:
        return i18n("AC'97 (Intel 82801AA)");
    case VMSpecs::AudioDevice::Es1370:
        return i18n("Ensoniq AudioPCI ES1370");
    case VMSpecs::AudioDevice::Sb16:
        return i18n("Creative Sound Blaster 16");
    case VMSpecs::AudioDevice::Adlib:
        return i18n("Yamaha YM3812/Adlib (OPL2)");
    case VMSpecs::AudioDevice::PcSpkr:
        return i18n("PC Speaker");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::DiskInterface interface)
{
    switch (interface) {
    case VMSpecs::DiskInterface::Virtio:
        return u"VirtIO"_s;
    case VMSpecs::DiskInterface::Nvme:
        return u"NVMe"_s;
    case VMSpecs::DiskInterface::SataIde:
        return u"SATA/IDE"_s;
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::GpuType type)
{
    switch (type) {
    case VMSpecs::GpuType::Vga:
        return i18n("Standard VGA");
    case VMSpecs::GpuType::VgaISA:
        return i18n("Standard VGA ISA");
    case VMSpecs::GpuType::CirrusGD5446:
        return i18n("Cirrus Logic GD5446");
    case VMSpecs::GpuType::CirrusGD5430:
        return i18n("Cirrus Logic GD5430");
    case VMSpecs::GpuType::Virtio:
        return i18n("VirtIO VGA");
    case VMSpecs::GpuType::Qxl:
        return i18n("QXL");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::DisplayType type)
{
    switch (type) {
    case VMSpecs::DisplayType::Gtk:
        return i18n("GTK");
    case VMSpecs::DisplayType::Sdl:
        return i18n("SDL");
    case VMSpecs::DisplayType::Spice:
        return i18n("SPICE (Remote Viewer)");
    case VMSpecs::DisplayType::Dbus:
        return i18n("D-Bus (Integrated Viewer)");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::UsbController controller)
{
    switch (controller) {
    case VMSpecs::UsbController::None:
        return i18n("No USB");
    case VMSpecs::UsbController::Xhci:
        return i18n("USB 3.0 XHCI");
    case VMSpecs::UsbController::Ehci:
        return i18n("USB 2.0 EHCI");
    case VMSpecs::UsbController::Default:
        return i18n("USB 1.1 UHCI");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::MachineType type)
{
    switch (type) {
    case VMSpecs::MachineType::Q35:
        return i18n("Standard PC Modern (Q35 + PCIe)");
    case VMSpecs::MachineType::Pc:
        return i18n("Standard PC Legacy (i440FX + PIIX)");
    case VMSpecs::MachineType::PcISA:
        return i18n("ISA-only PC Vintage (IBM PC/AT)");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::OsType type)
{
    switch (type) {
    case VMSpecs::OsType::Linux:
        return i18n("GNU/Linux");
    case VMSpecs::OsType::Windows:
        return i18n("Microsoft Windows");
    case VMSpecs::OsType::DOS:
        return i18n("DOS");
    case VMSpecs::OsType::BSD:
        return i18n("BSD");
    case VMSpecs::OsType::Other:
        return i18n("Other");
    }
    return { };
}

QString VMConfig::toUIString(VMSpecs::NetworkMode mode)
{
    switch (mode) {
    case VMSpecs::NetworkMode::Passt:
        return i18n("User Mode");
    case VMSpecs::NetworkMode::VirtualNetwork:
        return i18n("Virtual Network");
    }
    return { };
}

QString VMConfig::firmwareToUIString(bool useUefi, bool useSecureBoot)
{
    if (!useUefi)
        return i18n("BIOS");
    if (useSecureBoot)
        return i18n("UEFI (OVMF) + Secure Boot");
    return i18n("UEFI (OVMF)");
}

// --- Common Strings (toString) ---

QString VMConfig::toString(VMSpecs::CpuModel model)
{
    switch (model) {
    case VMSpecs::CpuModel::Host:
        return i18n("host");
    case VMSpecs::CpuModel::Qemu64:
        return i18n("qemu64");
    case VMSpecs::CpuModel::Qemu32:
        return i18n("qemu32");
    case VMSpecs::CpuModel::I486:
        return i18n("i486");
    case VMSpecs::CpuModel::Pentium:
        return i18n("pentium");
    case VMSpecs::CpuModel::Pentium2:
        return i18n("pentium2");
    case VMSpecs::CpuModel::Pentium3:
        return i18n("pentium3");
    case VMSpecs::CpuModel::Pentium4:
        return i18n("pentium4");
    case VMSpecs::CpuModel::CoreDuo:
        return i18n("coreduo");
    case VMSpecs::CpuModel::Core2Duo:
        return i18n("core2duo");
    case VMSpecs::CpuModel::Nehalem:
        return i18n("corei1");
    case VMSpecs::CpuModel::SandyBridge:
        return i18n("corei2");
    case VMSpecs::CpuModel::Haswell:
        return i18n("corei4");
    case VMSpecs::CpuModel::Skylake:
        return i18n("corei6");
    case VMSpecs::CpuModel::Phenom:
        return i18n("phenom");
    case VMSpecs::CpuModel::Epyc:
        return i18n("ryzen");
    default:
        return i18n("Unknown");
    }
}

QString VMConfig::toString(VMSpecs::DisplayType type)
{
    switch (type) {
    case VMSpecs::DisplayType::Sdl:
        return u"sdl"_s;
    case VMSpecs::DisplayType::Spice:
        return u"spice"_s;
    case VMSpecs::DisplayType::Dbus:
        return u"dbus"_s;
    default:
        return u"gtk"_s;
    }
}

QString VMConfig::toString(VMSpecs::MachineType type)
{
    if (type == VMSpecs::MachineType::Pc)
        return u"pc"_s;
    if (type == VMSpecs::MachineType::PcISA)
        return u"pc_isa"_s;
    return u"q35"_s;
}

QString VMConfig::toString(VMSpecs::GpuType type)
{
    if (type == VMSpecs::GpuType::VgaISA)
        return u"vga_isa"_s;
    if (type == VMSpecs::GpuType::Virtio)
        return u"virtio"_s;
    if (type == VMSpecs::GpuType::Qxl)
        return u"qxl"_s;
    if (type == VMSpecs::GpuType::CirrusGD5446)
        return u"cirrus"_s;
    if (type == VMSpecs::GpuType::CirrusGD5430)
        return u"cirrus_isa"_s;
    return u"vga"_s;
}

QString VMConfig::toString(VMSpecs::AudioDevice type)
{
    if (type == VMSpecs::AudioDevice::None)
        return u"none"_s;
    if (type == VMSpecs::AudioDevice::Ac97)
        return u"ac97"_s;
    if (type == VMSpecs::AudioDevice::Es1370)
        return u"es1370"_s;
    if (type == VMSpecs::AudioDevice::Sb16)
        return u"sb16"_s;
    if (type == VMSpecs::AudioDevice::Adlib)
        return u"adlib"_s;
    if (type == VMSpecs::AudioDevice::PcSpkr)
        return u"pcspkr"_s;
    return u"hda"_s;
}

QString VMConfig::toString(VMSpecs::NetworkCard type)
{
    if (type == VMSpecs::NetworkCard::E1000)
        return u"e1000"_s;
    if (type == VMSpecs::NetworkCard::Rtl8139)
        return u"rtl8139"_s;
    if (type == VMSpecs::NetworkCard::Pcnet)
        return u"pcnet"_s;
    if (type == VMSpecs::NetworkCard::Rtl8029)
        return u"rtl8029"_s;
    if (type == VMSpecs::NetworkCard::Ne2000)
        return u"ne2000"_s;
    if (type == VMSpecs::NetworkCard::I82551)
        return u"i82551"_s;
    if (type == VMSpecs::NetworkCard::I82557b)
        return u"i82557b"_s;
    if (type == VMSpecs::NetworkCard::I82559er)
        return u"i82559er"_s;
    if (type == VMSpecs::NetworkCard::None)
        return u"none"_s;
    return u"virtio"_s;
}

QString VMConfig::toString(VMSpecs::DiskInterface type)
{
    if (type == VMSpecs::DiskInterface::Nvme)
        return u"nvme"_s;
    if (type == VMSpecs::DiskInterface::SataIde)
        return u"sataide"_s;
    return u"virtio"_s;
}

QString VMConfig::toString(VMSpecs::UsbController type)
{
    if (type == VMSpecs::UsbController::Xhci)
        return u"xhci"_s;
    if (type == VMSpecs::UsbController::Ehci)
        return u"ehci"_s;
    if (type == VMSpecs::UsbController::Default)
        return u"default"_s;
    return u"none"_s;
}

QString VMConfig::toString(PortForwardRule::Protocol proto)
{
    return (proto == PortForwardRule::Protocol::Udp) ? u"udp"_s : u"tcp"_s;
}

QString VMConfig::toString(VMSpecs::OsType type)
{
    if (type == VMSpecs::OsType::Linux)
        return u"linux"_s;
    if (type == VMSpecs::OsType::Windows)
        return u"windows"_s;
    if (type == VMSpecs::OsType::DOS)
        return u"dos"_s;
    if (type == VMSpecs::OsType::BSD)
        return u"bsd"_s;
    if (type == VMSpecs::OsType::Other)
        return u"other"_s;
    return u"linux"_s;
}

QString VMConfig::toString(VMSpecs::NetworkMode mode)
{
    if (mode == VMSpecs::NetworkMode::VirtualNetwork)
        return u"virtual_network"_s;
    return u"passt"_s;
}

QString VMConfig::toString(ImageType type)
{
    switch (type) {
    case ImageType::CloudImage:
        return u"cloud"_s;
    case ImageType::InstallerIso:
        return u"installer"_s;
    case ImageType::Manual:
        return u"manual"_s;
    }
    return u"manual"_s;
}

QString VMConfig::toString(DeployState state)
{
    switch (state) {
    case DeployState::None:
        return u"none"_s;
    case DeployState::Deploying:
        return u"deploying"_s;
    case DeployState::Cloning:
        return u"cloning"_s;
    case DeployState::FirstBoot:
        return u"firstboot"_s;
    case DeployState::Complete:
        return u"complete"_s;
    }
    return u"none"_s;
}

QString VMConfig::toString(IconSource source)
{
    switch (source) {
    case IconSource::SystemTheme:
        return u"system"_s;
    case IconSource::IconSet:
        return u"iconset"_s;
    case IconSource::Custom:
        return u"custom"_s;
    case IconSource::None:
    default:
        return u"none"_s;
    }
}

// --- Parse Strings ---

VMSpecs::CpuModel VMConfig::parseCpuModel(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == i18n("host"))
        return VMSpecs::CpuModel::Host;
    if (str_l_t == i18n("qemu64"))
        return VMSpecs::CpuModel::Qemu64;
    if (str_l_t == i18n("qemu32"))
        return VMSpecs::CpuModel::Qemu32;
    if (str_l_t == i18n("i486"))
        return VMSpecs::CpuModel::I486;
    if (str_l_t == i18n("pentium"))
        return VMSpecs::CpuModel::Pentium;
    if (str_l_t == i18n("pentium2"))
        return VMSpecs::CpuModel::Pentium2;
    if (str_l_t == i18n("pentium3"))
        return VMSpecs::CpuModel::Pentium3;
    if (str_l_t == i18n("pentium4"))
        return VMSpecs::CpuModel::Pentium4;
    if (str_l_t == i18n("coreduo"))
        return VMSpecs::CpuModel::CoreDuo;
    if (str_l_t == i18n("core2duo"))
        return VMSpecs::CpuModel::Core2Duo;
    if (str_l_t == i18n("corei1"))
        return VMSpecs::CpuModel::Nehalem;
    if (str_l_t == i18n("corei2"))
        return VMSpecs::CpuModel::SandyBridge;
    if (str_l_t == i18n("corei4"))
        return VMSpecs::CpuModel::Haswell;
    if (str_l_t == i18n("corei6"))
        return VMSpecs::CpuModel::Skylake;
    if (str_l_t == i18n("phenom"))
        return VMSpecs::CpuModel::Phenom;
    if (str_l_t == i18n("ryzen"))
        return VMSpecs::CpuModel::Epyc;

    return VMSpecs::CpuModel::Host;
}

VMSpecs::DisplayType VMConfig::parseDisplayType(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"sdl"_s)
        return VMSpecs::DisplayType::Sdl;
    if (str_l_t == u"spice"_s)
        return VMSpecs::DisplayType::Spice;
    if (str_l_t == u"dbus"_s)
        return VMSpecs::DisplayType::Dbus;
    if (str_l_t == u"gtk"_s)
        return VMSpecs::DisplayType::Gtk;

    if (str_l_t == KEMUConst::DefaultDisplayType) {
        return VMSpecs::DisplayType::Gtk;
    }

    return parseDisplayType(KEMUConst::DefaultDisplayType);
}

VMSpecs::MachineType VMConfig::parseMachineType(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"pc_isa"_s)
        return VMSpecs::MachineType::PcISA;
    if (str_l_t == u"q35"_s)
        return VMSpecs::MachineType::Q35;
    return VMSpecs::MachineType::Pc;
}

VMSpecs::GpuType VMConfig::parseGpuType(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"vga_isa"_s)
        return VMSpecs::GpuType::VgaISA;
    if (str_l_t == u"virtio"_s)
        return VMSpecs::GpuType::Virtio;
    if (str_l_t == u"qxl"_s)
        return VMSpecs::GpuType::Qxl;
    if (str_l_t == u"cirrus"_s)
        return VMSpecs::GpuType::CirrusGD5446;
    if (str_l_t == u"cirrus_isa"_s)
        return VMSpecs::GpuType::CirrusGD5430;
    return VMSpecs::GpuType::Vga;
}

VMSpecs::AudioDevice VMConfig::parseAudioDevice(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"none"_s)
        return VMSpecs::AudioDevice::None;
    if (str_l_t == u"ac97"_s)
        return VMSpecs::AudioDevice::Ac97;
    if (str_l_t == u"es1370"_s)
        return VMSpecs::AudioDevice::Es1370;
    if (str_l_t == u"sb16"_s)
        return VMSpecs::AudioDevice::Sb16;
    if (str_l_t == u"adlib"_s)
        return VMSpecs::AudioDevice::Adlib;
    if (str_l_t == u"pcspkr"_s)
        return VMSpecs::AudioDevice::PcSpkr;
    return VMSpecs::AudioDevice::Hda;
}

VMSpecs::NetworkCard VMConfig::parseNetworkCard(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"e1000"_s)
        return VMSpecs::NetworkCard::E1000;
    if (str_l_t == u"rtl8139"_s)
        return VMSpecs::NetworkCard::Rtl8139;
    if (str_l_t == u"pcnet"_s)
        return VMSpecs::NetworkCard::Pcnet;
    if (str_l_t == u"rtl8029"_s)
        return VMSpecs::NetworkCard::Rtl8029;
    if (str_l_t == u"ne20000"_s)
        return VMSpecs::NetworkCard::Ne2000;
    if (str_l_t == u"i82551"_s)
        return VMSpecs::NetworkCard::I82551;
    if (str_l_t == u"i82557b"_s)
        return VMSpecs::NetworkCard::I82557b;
    if (str_l_t == u"i82559er"_s)
        return VMSpecs::NetworkCard::I82559er;
    if (str_l_t == u"none"_s)
        return VMSpecs::NetworkCard::None;
    return VMSpecs::NetworkCard::Virtio;
}

VMSpecs::DiskInterface VMConfig::parseDiskInterface(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"nvme"_s)
        return VMSpecs::DiskInterface::Nvme;
    if (str_l_t == u"sataide"_s)
        return VMSpecs::DiskInterface::SataIde;
    return VMSpecs::DiskInterface::Virtio;
}

VMSpecs::UsbController VMConfig::parseUSBController(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"ehci"_s)
        return VMSpecs::UsbController::Ehci;
    if (str_l_t == u"xhci"_s)
        return VMSpecs::UsbController::Xhci;
    if (str_l_t == u"default"_s)
        return VMSpecs::UsbController::Default;
    return VMSpecs::UsbController::None;
}

PortForwardRule::Protocol VMConfig::parseProtocol(const QString& str)
{
    return (str.toLower().trimmed() == u"udp"_s)
        ? PortForwardRule::Protocol::Udp
        : PortForwardRule::Protocol::Tcp;
}

VMSpecs::OsType VMConfig::parseOsType(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"windows"_s)
        return VMSpecs::OsType::Windows;
    if (str_l_t == u"dos"_s)
        return VMSpecs::OsType::DOS;
    if (str_l_t == u"bsd"_s)
        return VMSpecs::OsType::BSD;
    if (str_l_t == u"other"_s)
        return VMSpecs::OsType::Other;
    return VMSpecs::OsType::Linux;
}

VMSpecs::NetworkMode VMConfig::parseNetworkMode(const QString& str)
{
    if (str.toLower().trimmed() == u"virtual_network"_s)
        return VMSpecs::NetworkMode::VirtualNetwork;
    return VMSpecs::NetworkMode::Passt;
}

VMConfig::ImageType VMConfig::parseImageType(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"cloud"_s)
        return ImageType::CloudImage;
    if (str_l_t == u"installer"_s)
        return ImageType::InstallerIso;
    return ImageType::Manual;
}

VMConfig::DeployState VMConfig::parseDeployState(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"deploying"_s)
        return DeployState::Deploying;
    if (str_l_t == u"firstboot"_s)
        return DeployState::FirstBoot;
    if (str_l_t == u"complete"_s)
        return DeployState::Complete;
    return DeployState::None;
}

VMConfig::IconSource VMConfig::parseIconSource(const QString& str)
{
    const QString str_l_t = str.toLower().trimmed();
    if (str_l_t == u"custom"_s)
        return IconSource::Custom;
    if (str_l_t == u"system"_s)
        return IconSource::SystemTheme;
    if (str_l_t == u"iconset"_s)
        return IconSource::IconSet;
    return IconSource::None;
}

QPair<VMSpecs::OsType, QString> VMConfig::guessOSFromText(const QString& string, bool* found)
{
    QString lowerz = string.toLower().remove(u' ');

    // Windows
    if (lowerz.contains(u"windows11"_s) || lowerz.contains(u"win11"_s)) {
        if (found)
            *found = true;
        return { VMSpecs::OsType::Windows, u"windows11"_s };
    }
    if (lowerz.contains(u"windows10"_s) || lowerz.contains(u"win10"_s)) {
        if (found)
            *found = true;
        return { VMSpecs::OsType::Windows, u"windows10"_s };
    }
    if (lowerz.contains(u"windows"_s)) {
        if (found)
            *found = true;
        return { VMSpecs::OsType::Windows, { } };
    }

    // Linux-Distros
    struct VariantHint {
        QString keyword;
        QString variantId;
    };
    static const QList<VariantHint> linuxHints = {
        { u"kubuntu"_s, u"kubuntu"_s },
        { u"ubuntu"_s, u"ubuntu"_s },
        { u"neon"_s, u"kdeneon"_s },
        { u"debian"_s, u"debian"_s },
        { u"fedora"_s, u"fedora"_s },
        { u"arch"_s, u"archlinux"_s },
        { u"manjaro"_s, u"manjaro"_s },
        { u"opensuse"_s, u"opensuse"_s },
        { u"suse"_s, u"opensuse"_s },
        { u"redhat"_s, u"redhat"_s },
        { u"rhel"_s, u"redhat"_s },
        { u"centos"_s, u"centos"_s },
        { u"mint"_s, u"linuxmint"_s },
        { u"nixos"_s, u"nixos"_s },
        { u"void"_s, u"voidlinux"_s },
        { u"gentoo"_s, u"gentoo"_s },
    };

    for (const auto& hint : linuxHints) {
        if (lowerz.contains(hint.keyword)) {
            if (found)
                *found = true;
            return { VMSpecs::OsType::Linux, hint.variantId };
        }
    }

    // BSD
    if (lowerz.contains(u"freebsd"_s)) {
        if (found)
            *found = true;
        return { VMSpecs::OsType::BSD, u"freebsd"_s };
    }
    if (lowerz.contains(u"openbsd"_s)) {
        if (found)
            *found = true;
        return { VMSpecs::OsType::BSD, u"openbsd"_s };
    }

    // DOS
    if (lowerz.contains(u"dos"_s) || lowerz.contains(u"freedos"_s)) {
        if (found)
            *found = true;
        return { VMSpecs::OsType::DOS, lowerz.contains(u"freedos"_s) ? u"freedos"_s : u"msdos"_s };
    }

    if (found)
        *found = false;
    return { VMSpecs::OsType::Other, { } };
}

QJsonObject VMConfig::toJson() const
{
    QJsonObject j;
    j[u"application_name"_s] = QCoreApplication::applicationName();
    j[u"application_version"_s] = QCoreApplication::applicationVersion();
    j[u"name"_s] = m_name;
    j[u"comment"_s] = m_comment;
    j[u"os_type"_s] = toString(m_osType);
    j[u"distro_id"_s] = m_distroId;
    j[u"image_type"_s] = toString(m_imageType);

    j[u"icon_source"_s] = toString(m_iconSource);
    j[u"icon_identifier"_s] = m_iconIdentifier;

    j[u"use_kvm"_s] = m_useKvm;
    j[u"ram"_s] = m_ramMb;
    j[u"use_balloon"_s] = m_useMemoryBallooning;
    j[u"use_prealloc"_s] = m_preallocMemory;
    j[u"cpu_model"_s] = toString(m_cpuModel);
    j[u"cpu_cores"_s] = m_cpuCoreCount;
    j[u"iso"_s] = m_isoPath;
    j[u"boot_iso"_s] = m_bootFromIso;
    j[u"floppy"_s] = m_floppyPath;
    j[u"boot_floppy"_s] = m_bootFromFloppy;
    j[u"extra_args"_s] = m_extraArgs;
    j[u"disk_file"_s] = u"disk.qcow2"_s;
    j[u"disk_size_gb"_s] = m_diskSizeGb;

    j[u"shared_path"_s] = m_sharedPath;
    j[u"shared_name"_s] = m_sharedName;
    j[u"use_uefi"_s] = m_useUefi;
    j[u"use_localtime"_s] = m_useLocalTime;
    j[u"use_tpm"_s] = m_useTpm;
    j[u"use_secureboot"_s] = m_useSecureBoot;
    j[u"use_acpi"_s] = m_useAcpi;

    j[u"gpu_mem"_s] = m_gpuMemMb;
    j[u"display_width"_s] = m_displayResolution.width();
    j[u"display_height"_s] = m_displayResolution.height();
    j[u"display_type"_s] = toString(m_displayType);
    j[u"spice_agent"_s] = m_spiceAgent;
    j[u"spice_usb_redir"_s] = m_spiceUsbRedir;
    j[u"spice_usb_redir_count"_s] = m_spiceUsbRedirCount;
    j[u"machine_type"_s] = toString(m_machineType);
    j[u"gpu_type"_s] = toString(m_gpuType);
    j[u"gpu_3d_acceleration"_s] = m_gpu3dAcceleration;

    j[u"audio_device"_s] = toString(m_audioDevice);
    if (m_audioDevice == VMSpecs::AudioDevice::Sb16 && !m_sb16Config.isDefault()) {
        QJsonObject sb16;
        sb16[u"irq"_s] = m_sb16Config.irq;
        sb16[u"dma"_s] = m_sb16Config.dma;
        sb16[u"dma16"_s] = m_sb16Config.dma16;
        sb16[u"iobase"_s] = m_sb16Config.iobase;
        j[u"sb16_config"_s] = sb16;
    }

    j[u"net_card"_s] = toString(m_networkCard);
    j[u"mac_address"_s] = m_macAddress;
    j[u"network_mode"_s] = toString(m_networkMode);
    j[u"virtual_network"_s] = m_virtualNetworkName;

    if (!m_portForwards.isEmpty()) {
        QJsonArray portFwdArray;
        for (const auto& rule : m_portForwards) {
            QJsonObject ruleObj;
            ruleObj[u"protocol"_s] = toString(rule.protocol);
            ruleObj[u"host_port"_s] = rule.hostPort;
            ruleObj[u"guest_port"_s] = rule.guestPort;
            portFwdArray.append(ruleObj);
        }
        j[u"port_forwards"_s] = portFwdArray;
    }

    j[u"disk_interface"_s] = toString(m_diskInterface);
    j[u"usb_controller"_s] = toString(m_usbController);
    j[u"use_usb_tablet"_s] = m_useUsbTablet;
    j[u"use_usb_keyboard"_s] = m_useUsbKeyboard;

    j[u"deploy_state"_s] = toString(m_deployState);

    return j;
}

VMConfig VMConfig::fromJson(const QJsonObject& j)
{
    VMConfig c;

    c.m_name = j[u"name"_s].toString();
    c.m_comment = j[u"comment"_s].toString();
    c.m_osType = parseOsType(j[u"os_type"_s].toString());
    c.m_distroId = j[u"distro_id"_s].toString();
    c.m_imageType = parseImageType(j[u"image_type"_s].toString());

    c.m_iconSource = parseIconSource(j[u"icon_source"_s].toString());
    c.m_iconIdentifier = j[u"icon_identifier"_s].toString();

    c.m_useKvm = j[u"use_kvm"_s].toBool(GlobalDefaults.useKvm);
    c.m_ramMb = j[u"ram"_s].toInt(GlobalDefaults.ramMb);
    c.m_useMemoryBallooning = j[u"use_balloon"_s].toBool(GlobalDefaults.useMemoryBallooning);
    c.m_preallocMemory = j[u"use_prealloc"_s].toBool(GlobalDefaults.preallocMemory);
    c.m_cpuModel = parseCpuModel(j[u"cpu_model"_s].toString());
    c.m_cpuCoreCount = j[u"cpu_cores"_s].toInt(GlobalDefaults.cpuCoreCount);
    c.m_diskSizeGb = j[u"disk_size_gb"_s].toInt(GlobalDefaults.diskSizeGb);
    c.m_isoPath = j[u"iso"_s].toString();
    c.m_bootFromIso = j[u"boot_iso"_s].toBool(false);
    c.m_floppyPath = j[u"floppy"_s].toString();
    c.m_bootFromFloppy = j[u"boot_floppy"_s].toBool(false);
    c.m_extraArgs = j[u"extra_args"_s].toString();
    c.m_diskPath = j[u"disk_file"_s].toString(KEMUConst::VMDiskFileName);

    c.m_displayType = parseDisplayType(j[u"display_type"_s].toString());
    c.m_spiceAgent = j[u"spice_agent"_s].toBool(GlobalDefaults.spiceAgent);
    c.m_spiceUsbRedir = j[u"spice_usb_redir"_s].toBool(GlobalDefaults.spiceUsbRedir);
    c.m_spiceUsbRedirCount = j[u"spice_usb_redir_count"_s].toInt(GlobalDefaults.spiceUsbRedirCount);
    c.m_gpuType = parseGpuType(j[u"gpu_type"_s].toString());
    c.m_gpu3dAcceleration = j[u"gpu_3d_acceleration"_s].toBool(GlobalDefaults.gpu3dAcceleration);
    c.m_gpuMemMb = j[u"gpu_mem"_s].toInt(GlobalDefaults.GpuMemMb);
    c.m_displayResolution = QSize(
        j[u"display_width"_s].toInt(GlobalDefaults.displayResolution.width()),
        j[u"display_height"_s].toInt(GlobalDefaults.displayResolution.height()));

    c.m_audioDevice = parseAudioDevice(j[u"audio_device"_s].toString());
    if (j.contains(u"sb16_config"_s)) {
        const QJsonObject sb16 = j[u"sb16_config"_s].toObject();
        Sb16Config cfg;
        cfg.irq = sb16[u"irq"_s].toInt(5);
        cfg.dma = sb16[u"dma"_s].toInt(1);
        cfg.dma16 = sb16[u"dma16"_s].toInt(5);
        cfg.iobase = sb16[u"iobase"_s].toInt(0x220);
        c.m_sb16Config = cfg;
    }

    c.m_machineType = parseMachineType(j[u"machine_type"_s].toString());
    c.m_sharedPath = j[u"shared_path"_s].toString();
    c.m_sharedName = j[u"shared_name"_s].toString();
    c.m_useUefi = j[u"use_uefi"_s].toBool(GlobalDefaults.useUefi);
    c.m_useLocalTime = j[u"use_localtime"_s].toBool(GlobalDefaults.useLocalTime);
    c.m_useTpm = j[u"use_tpm"_s].toBool(GlobalDefaults.useTpm);
    c.m_useSecureBoot = j[u"use_secureboot"_s].toBool(GlobalDefaults.useSecureBoot);
    c.m_useAcpi = j[u"use_acpi"_s].toBool(GlobalDefaults.useAcpi);

    c.m_networkCard = parseNetworkCard(j[u"net_card"_s].toString());
    c.m_macAddress = j[u"mac_address"_s].toString();
    c.m_networkMode = parseNetworkMode(j[u"network_mode"_s].toString());
    c.m_virtualNetworkName = j[u"virtual_network"_s].toString();

    if (j.contains(u"port_forwards"_s)) {
        const QJsonArray arr = j[u"port_forwards"_s].toArray();
        for (const auto& val : arr) {
            const QJsonObject ruleObj = val.toObject();
            PortForwardRule rule;
            rule.protocol = parseProtocol(ruleObj[u"protocol"_s].toString());
            rule.hostPort = static_cast<quint16>(ruleObj[u"host_port"_s].toInt());
            rule.guestPort = static_cast<quint16>(ruleObj[u"guest_port"_s].toInt());
            c.m_portForwards.append(rule);
        }
    }

    c.m_diskInterface = parseDiskInterface(j[u"disk_interface"_s].toString());
    c.m_usbController = parseUSBController(j[u"usb_controller"_s].toString());
    c.m_useUsbTablet = j[u"use_usb_tablet"_s].toBool(GlobalDefaults.useUsbTablet);
    c.m_useUsbKeyboard = j[u"use_usb_keyboard"_s].toBool(GlobalDefaults.useUsbKeyboard);

    c.m_deployState = parseDeployState(j[u"deploy_state"_s].toString());

    return c;
}

void VMConfig::normalizePaths(const QDir& baseDir)
{
    auto makeAbsolute = [&baseDir](const QString& path) -> QString {
        if (path.isEmpty())
            return path;
        QFileInfo fi(path);
        if (fi.isAbsolute())
            return fi.absoluteFilePath();
        return baseDir.absoluteFilePath(path);
    };

    m_diskPath = makeAbsolute(m_diskPath);
}
