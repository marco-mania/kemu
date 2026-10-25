/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovaexportjob.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QXmlStreamWriter>

#include <algorithm>

#include <KLocalizedString>

#include "utils/qemuimgprocess.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int kConvertBand = 800;
constexpr int kManifestBand = 900;
constexpr qsizetype kHashChunk = 1 << 20;

// Two widespread vendor extensions. The URIs, the prefixes they are bound to
// and the values written into them are format identifiers that other tools
// match literally.
const QString kConfigExtNs = u"http://www.vmware.com/schema/ovf"_s;
const QString kMachineExtNs = u"http://www.virtualbox.org/ovf/machine"_s;

struct GuestOs {
    int cimId = 1; // CIM_OperatingSystem OSType, 1: Other
    QString machineOsType; // OS type of the machine section extension
    QString configOsType; // guest id of the configuration extension
    QString description;
};

// Other tools pick their defaults from the vendor ids, the CIM id is only a
// fallback. Only CIM ids that are unambiguous are used.
GuestOs guestOsFor(const VMConfig &config)
{
    const bool is64 = config.architecture() != VMSpecs::Architecture::I386;
    const QString suffix = is64 ? u"_64"_s : QString();
    const QString distro = config.distroId();

    switch (config.osFamily()) {
    case VMSpecs::OsFamily::Windows: {
        struct Windows {
            QString distro;
            int cimId;
            QString machineId;
            QString configId32;
            QString configId64;
            QString description;
        };
        static const QList<Windows> versions = {
            {u"windows11"_s, 1, u"Windows11"_s, u"windows11_64Guest"_s, u"windows11_64Guest"_s, u"Windows 11"_s},
            {u"windows10"_s, 1, u"Windows10"_s, u"windows9Guest"_s, u"windows9_64Guest"_s, u"Windows 10"_s},
            {u"windows7"_s, 105, u"Windows7"_s, u"windows7Guest"_s, u"windows7_64Guest"_s, u"Windows 7"_s},
            {u"windowsxp"_s, 67, u"WindowsXP"_s, u"winXPProGuest"_s, u"winXPPro64Guest"_s, u"Windows XP"_s},
            {u"windows2000"_s, 58, u"Windows2000"_s, u"win2000ProGuest"_s, u"win2000ProGuest"_s, u"Windows 2000"_s},
            {u"windowsnt4"_s, 18, u"WindowsNT4"_s, u"winNTGuest"_s, u"winNTGuest"_s, u"Windows NT 4"_s},
        };
        for (const Windows &w : versions) {
            if (w.distro == distro) {
                // The machine section has no 64-bit ids for the oldest versions.
                const bool machine64 = is64 && w.cimId != 58 && w.cimId != 18;
                return {w.cimId, w.machineId + (machine64 ? u"_64"_s : QString()), is64 ? w.configId64 : w.configId32, w.description};
            }
        }
        return {1, u"Windows10"_s + suffix, is64 ? u"windows9_64Guest"_s : u"windows9Guest"_s, u"Windows"_s};
    }

    case VMSpecs::OsFamily::Linux: {
        static const QHash<QString, std::pair<QString, QString>> distros = {
            {u"debian"_s, {u"Debian"_s, u"Debian"_s}},
            {u"ubuntu"_s, {u"Ubuntu"_s, u"Ubuntu"_s}},
            {u"kubuntu"_s, {u"Ubuntu"_s, u"Kubuntu"_s}},
            {u"kdeneon"_s, {u"Ubuntu"_s, u"KDE neon"_s}},
            {u"linuxmint"_s, {u"Ubuntu"_s, u"Linux Mint"_s}},
            {u"fedora"_s, {u"Fedora"_s, u"Fedora"_s}},
            {u"archlinux"_s, {u"ArchLinux"_s, u"Arch Linux"_s}},
            {u"manjaro"_s, {u"ArchLinux"_s, u"Manjaro"_s}},
            {u"opensuse"_s, {u"OpenSUSE"_s, u"openSUSE"_s}},
            {u"redhat"_s, {u"RedHat"_s, u"Red Hat Enterprise Linux"_s}},
            {u"centos"_s, {u"RedHat"_s, u"CentOS"_s}},
            {u"gentoo"_s, {u"Gentoo"_s, u"Gentoo"_s}},
        };
        const auto [family, name] = distros.value(distro, {u"Linux"_s, u"Linux"_s});
        const bool ubuntuFamily = family == u"Ubuntu"_s;
        const QString configId = ubuntuFamily ? (is64 ? u"ubuntu64Guest"_s : u"ubuntuGuest"_s) : (is64 ? u"otherLinux64Guest"_s : u"otherLinuxGuest"_s);
        // The machine section calls 32-bit generic Linux "Linux26".
        const QString machineId = family == u"Linux"_s && !is64 ? u"Linux26"_s : family + suffix;
        return {36, machineId, configId, name};
    }

    case VMSpecs::OsFamily::BSD:
        if (distro == u"openbsd"_s)
            return {1, u"OpenBSD"_s + suffix, is64 ? u"other64Guest"_s : u"otherGuest"_s, u"OpenBSD"_s};
        return {42, u"FreeBSD"_s + suffix, is64 ? u"freebsd64Guest"_s : u"freebsdGuest"_s, u"FreeBSD"_s};

    case VMSpecs::OsFamily::DOS:
        return {14, u"DOS"_s, u"dosGuest"_s, u"DOS"_s};

    default:
        return {1, u"Other"_s + suffix, is64 ? u"other64Guest"_s : u"otherGuest"_s, u"Other"_s};
    }
}

// rasd:ResourceSubType as common hypervisors write it. Models they lack are
// exported as E1000, which every guest has a driver for. virtio has no
// standard name; "VirtioNet" is the spelling that is recognised.
QString networkSubTypeFor(VMSpecs::NetworkCard card)
{
    switch (card) {
    case VMSpecs::NetworkCard::Virtio:
        return u"VirtioNet"_s;
    case VMSpecs::NetworkCard::Pcnet:
        return u"PCNet32"_s;
    default:
        return u"E1000"_s;
    }
}

struct Controller {
    enum class Kind {
        Ide,
        Sata,
        Nvme
    };
    Kind kind;
    int instanceId;
    int address; // n-th controller of its kind
    int ports;
    int usedPorts = 0;
};

// KEMU's SATA/IDE is AHCI on Q35 and PIIX IDE otherwise. virtio-blk has no
// OVF equivalent; AHCI is what other tools and the guest support best.
Controller::Kind controllerKindFor(VMSpecs::DiskInterface diskInterface, VMSpecs::MachineType machine)
{
    switch (diskInterface) {
    case VMSpecs::DiskInterface::Nvme:
        return Controller::Kind::Nvme;
    case VMSpecs::DiskInterface::SataIde:
        return machine == VMSpecs::MachineType::Q35 ? Controller::Kind::Sata : Controller::Kind::Ide;
    default:
        return Controller::Kind::Sata;
    }
}

void writeRasd(QXmlStreamWriter &xml, const QString &element, const QString &text)
{
    xml.writeStartElement(u"rasd:"_s + element);
    xml.writeCharacters(text);
    xml.writeEndElement();
}

}

OvaExportJob::OvaExportJob(const QString &vmName, const VMConfig &config, const QList<DiskImage> &disks, const QString &outputFilePath, QObject *parent)
    : ArchiveStreamJob(parent)
    , m_vmName(vmName)
    , m_config(config)
    , m_disks(disks)
    , m_outputFilePath(outputFilePath)
{
}

void OvaExportJob::start()
{
    const QString outputDir = QFileInfo(m_outputFilePath).absolutePath();

    // QDir::tempPath() is tmpfs on most distributions; a multi-GB VMDK must
    // not land there. Stage next to the output file instead.
    m_stagingDir = std::make_unique<QTemporaryDir>(outputDir + u"/.kemu-export-XXXXXX"_s);
    if (!m_stagingDir->isValid()) {
        setError(StagingError);
        setErrorText(i18n("Failed to create a staging directory in %1.", outputDir));
        emitResult();
        return;
    }

    if (m_disks.isEmpty()) {
        setError(SourceError);
        setErrorText(i18n("The disk image of VM '%1' is missing or empty.", m_vmName));
        emitResult();
        return;
    }

    qint64 sourceSize = 0;
    for (const DiskImage &disk : std::as_const(m_disks)) {
        const qint64 size = QFileInfo(disk.path).size();
        if (size <= 0) {
            setError(SourceError);
            setErrorText(i18n("The disk image %1 of VM '%2' is missing or empty.", QFileInfo(disk.path).fileName(), m_vmName));
            emitResult();
            return;
        }
        sourceSize += size;
    }

    // Staging copy plus the finished OVA both live on this filesystem.
    QString spaceMessage;
    if (!hasFreeSpace(outputDir, 2 * sourceSize, &spaceMessage)) {
        setError(DiskSpaceError);
        setErrorText(spaceMessage);
        emitResult();
        return;
    }

    for (qsizetype i = 0; i < m_disks.size(); ++i)
        m_vmdkFileNames << m_vmName + u"-disk%1.vmdk"_s.arg(i + 1);
    m_ovfFileName = m_vmName + u".ovf"_s;
    m_mfFileName = m_vmName + u".mf"_s;

    convertNextDisk();
}

void OvaExportJob::convertNextDisk()
{
    Q_EMIT description(this,
                       i18n("Exporting as OVA"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Step"),
                                               m_disks.size() > 1 ? i18n("Converting disk image %1 of %2…", m_convertIndex + 1, m_disks.size())
                                                                  : i18n("Converting disk image…")));

    if (m_converter)
        m_converter->deleteLater();
    m_converter = new QemuImgProcess(this);

    connect(m_converter, &QemuImgProcess::progress, this, [this](double percent) {
        const double perDisk = kConvertBand / double(m_disks.size());
        emitPercent(qRound(m_convertIndex * perDisk + percent * perDisk / 100.0), 1000);
    });
    connect(m_converter, &QemuImgProcess::finished, this, &OvaExportJob::onConvertFinished);

    const QString vmdkPath = m_stagingDir->filePath(m_vmdkFileNames.at(m_convertIndex));
    const QStringList args =
        {u"convert"_s, u"-p"_s, u"-f"_s, u"qcow2"_s, u"-O"_s, u"vmdk"_s, u"-o"_s, u"subformat=streamOptimized"_s, m_disks.at(m_convertIndex).path, vmdkPath};

    if (!m_converter->start(args)) {
        setError(ToolError);
        setErrorText(m_converter->errorText());
        emitResult();
    }
}

void OvaExportJob::onConvertFinished(bool ok)
{
    if (!ok) {
        setError(ToolError);
        setErrorText(m_converter->errorText());
        emitResult();
        return;
    }

    if (++m_convertIndex < m_disks.size()) {
        convertNextDisk();
        return;
    }

    if (!createOvfDescriptor()) {
        setError(StagingError);
        setErrorText(i18n("Failed to write the OVF descriptor."));
        emitResult();
        return;
    }

    startWorker();
}

bool OvaExportJob::runWorker()
{
    postInfoMessage(i18n("Creating manifest (SHA-256)…"));
    if (!createManifest())
        return false;

    if (isCancelled())
        return false;

    postInfoMessage(i18n("Packaging OVA…"));
    return packageOva();
}

void OvaExportJob::workerFinished(bool ok)
{
    if (!ok) {
        QFile::remove(m_outputFilePath);
        setError(ArchiveError);
        setErrorText(workerErrorText().isEmpty() ? i18n("Failed to create the OVA archive.") : workerErrorText());
        emitResult();
        return;
    }

    emitPercent(1000, 1000);
    Q_EMIT infoMessage(this, i18n("OVA export completed successfully."));
    emitResult();
}

void OvaExportJob::discardPartialOutput()
{
    QFile::remove(m_outputFilePath);
}

void OvaExportJob::abortExternalTools()
{
    if (m_converter)
        m_converter->abort();
}

bool OvaExportJob::createOvfDescriptor()
{
    QFile file(m_stagingDir->filePath(m_ovfFileName));
    if (!file.open(QIODevice::WriteOnly))
        return false;

    const bool hasNetwork = m_config.networkCard() != VMSpecs::NetworkCard::None;
    const GuestOs os = guestOsFor(m_config);

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();

    xml.writeStartElement(u"Envelope"_s);
    xml.writeAttribute(u"ovf:version"_s, u"1.0"_s);
    xml.writeAttribute(u"xmlns"_s, u"http://schemas.dmtf.org/ovf/envelope/1"_s);
    xml.writeAttribute(u"xmlns:ovf"_s, u"http://schemas.dmtf.org/ovf/envelope/1"_s);
    xml.writeAttribute(u"xmlns:rasd"_s, u"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData"_s);
    xml.writeAttribute(u"xmlns:vssd"_s, u"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData"_s);
    xml.writeAttribute(u"xmlns:vmw"_s, kConfigExtNs);
    xml.writeAttribute(u"xmlns:vbox"_s, kMachineExtNs);

    // References
    xml.writeStartElement(u"References"_s);
    for (qsizetype i = 0; i < m_vmdkFileNames.size(); ++i) {
        xml.writeStartElement(u"File"_s);
        xml.writeAttribute(u"ovf:href"_s, m_vmdkFileNames.at(i));
        xml.writeAttribute(u"ovf:id"_s, u"file%1"_s.arg(i + 1));
        xml.writeAttribute(u"ovf:size"_s, QString::number(QFileInfo(m_stagingDir->filePath(m_vmdkFileNames.at(i))).size()));
        xml.writeEndElement();
    }
    xml.writeEndElement();

    // DiskSection
    xml.writeStartElement(u"DiskSection"_s);
    xml.writeTextElement(u"Info"_s, u"Virtual disk information"_s);
    for (qsizetype i = 0; i < m_disks.size(); ++i) {
        xml.writeStartElement(u"Disk"_s);
        // ovf:capacity defaults to bytes; use the exact virtual size.
        xml.writeAttribute(u"ovf:capacity"_s, QString::number(m_disks.at(i).capacityBytes));
        xml.writeAttribute(u"ovf:diskId"_s, u"vmdisk%1"_s.arg(i + 1));
        xml.writeAttribute(u"ovf:fileRef"_s, u"file%1"_s.arg(i + 1));
        xml.writeAttribute(u"ovf:format"_s, u"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized"_s);
        xml.writeEndElement();
    }
    xml.writeEndElement();

    // NetworkSection
    if (hasNetwork) {
        xml.writeStartElement(u"NetworkSection"_s);
        xml.writeTextElement(u"Info"_s, u"Network information"_s);
        xml.writeStartElement(u"Network"_s);
        xml.writeAttribute(u"ovf:name"_s, u"NAT"_s);
        xml.writeTextElement(u"Description"_s, u"NAT network"_s);
        xml.writeEndElement();
        xml.writeEndElement();
    }

    // VirtualSystem
    xml.writeStartElement(u"VirtualSystem"_s);
    xml.writeAttribute(u"ovf:id"_s, m_vmName);
    xml.writeTextElement(u"Info"_s, u"A virtual machine exported from KEMU"_s);
    xml.writeTextElement(u"Name"_s, m_vmName);

    if (!m_config.comment().isEmpty()) {
        xml.writeStartElement(u"AnnotationSection"_s);
        xml.writeTextElement(u"Info"_s, u"Description of the virtual machine"_s);
        xml.writeTextElement(u"Annotation"_s, m_config.comment());
        xml.writeEndElement();
    }

    xml.writeStartElement(u"OperatingSystemSection"_s);
    xml.writeAttribute(u"ovf:id"_s, QString::number(os.cimId));
    xml.writeAttribute(u"vmw:osType"_s, os.configOsType);
    xml.writeTextElement(u"Info"_s, u"Guest operating system"_s);
    xml.writeTextElement(u"Description"_s, os.description);
    xml.writeStartElement(u"vbox:OSType"_s);
    xml.writeAttribute(u"ovf:required"_s, u"false"_s);
    xml.writeCharacters(os.machineOsType);
    xml.writeEndElement();
    xml.writeEndElement();

    xml.writeStartElement(u"VirtualHardwareSection"_s);
    xml.writeTextElement(u"Info"_s, u"Hardware configuration"_s);

    xml.writeStartElement(u"System"_s);
    xml.writeTextElement(u"vssd:ElementName"_s, u"Virtual Hardware Family"_s);
    xml.writeTextElement(u"vssd:InstanceID"_s, u"0"_s);
    xml.writeTextElement(u"vssd:VirtualSystemIdentifier"_s, m_vmName);
    xml.writeTextElement(u"vssd:VirtualSystemType"_s, u"vmx-13"_s);
    xml.writeEndElement();

    // The CIM RASD schema is an xs:sequence in alphabetical order, so the
    // children of every Item below must stay sorted.

    // CPU
    xml.writeStartElement(u"Item"_s);
    writeRasd(xml, u"Description"_s, u"Number of virtual CPUs"_s);
    writeRasd(xml, u"ElementName"_s, u"%1 virtual CPU(s)"_s.arg(m_config.cpuCoreCount()));
    writeRasd(xml, u"InstanceID"_s, u"1"_s);
    writeRasd(xml, u"ResourceType"_s, u"3"_s);
    writeRasd(xml, u"VirtualQuantity"_s, QString::number(m_config.cpuCoreCount()));
    xml.writeEndElement();

    // Memory
    xml.writeStartElement(u"Item"_s);
    writeRasd(xml, u"AllocationUnits"_s, u"byte * 2^20"_s);
    writeRasd(xml, u"Description"_s, u"Memory Size"_s);
    writeRasd(xml, u"ElementName"_s, u"%1 MB of memory"_s.arg(m_config.ramMb()));
    writeRasd(xml, u"InstanceID"_s, u"2"_s);
    writeRasd(xml, u"ResourceType"_s, u"4"_s);
    writeRasd(xml, u"VirtualQuantity"_s, QString::number(m_config.ramMb()));
    xml.writeEndElement();

    // Controllers are created in the order the disks need them, so the
    // system disk sits on the first one: OVF has no boot order of its own.
    int instanceId = 3;
    QList<Controller> controllers;
    QList<std::pair<int, int>> diskSlots; // controller InstanceID, port
    const QList<DiskConfig> extraDisks = m_config.extraDisks();
    for (qsizetype i = 0; i < m_disks.size(); ++i) {
        const VMSpecs::DiskInterface diskInterface = i == 0 || i > extraDisks.size() ? m_config.diskInterface() : extraDisks.at(i - 1).diskInterface;
        const Controller::Kind kind = controllerKindFor(diskInterface, m_config.machineType());

        auto it = std::find_if(controllers.begin(), controllers.end(), [kind](const Controller &c) {
            return c.kind == kind && c.usedPorts < c.ports;
        });
        if (it == controllers.end()) {
            const int sameKind = static_cast<int>(std::count_if(controllers.cbegin(), controllers.cend(), [kind](const Controller &c) {
                return c.kind == kind;
            }));
            // An IDE channel holds master and slave; AHCI has 30 ports.
            controllers.append({kind, instanceId++, sameKind, kind == Controller::Kind::Ide ? 2 : 30});
            it = controllers.end() - 1;
        }
        diskSlots.append({it->instanceId, it->usedPorts++});
    }

    for (const Controller &c : std::as_const(controllers)) {
        xml.writeStartElement(u"Item"_s);
        writeRasd(xml, u"Address"_s, QString::number(c.address));
        switch (c.kind) {
        case Controller::Kind::Ide:
            writeRasd(xml, u"Description"_s, u"IDE Controller"_s);
            writeRasd(xml, u"ElementName"_s, u"ideController%1"_s.arg(c.address));
            writeRasd(xml, u"InstanceID"_s, QString::number(c.instanceId));
            writeRasd(xml, u"ResourceSubType"_s, u"PIIX4"_s);
            writeRasd(xml, u"ResourceType"_s, u"5"_s);
            break;
        case Controller::Kind::Sata:
            writeRasd(xml, u"Description"_s, u"SATA Controller"_s);
            writeRasd(xml, u"ElementName"_s, u"sataController%1"_s.arg(c.address));
            writeRasd(xml, u"InstanceID"_s, QString::number(c.instanceId));
            writeRasd(xml, u"ResourceSubType"_s, u"AHCI"_s);
            writeRasd(xml, u"ResourceType"_s, u"20"_s);
            break;
        case Controller::Kind::Nvme:
            writeRasd(xml, u"Description"_s, u"NVMe Controller"_s);
            writeRasd(xml, u"ElementName"_s, u"nvmeController%1"_s.arg(c.address));
            writeRasd(xml, u"InstanceID"_s, QString::number(c.instanceId));
            writeRasd(xml, u"ResourceSubType"_s, u"vmware.nvme.controller"_s);
            writeRasd(xml, u"ResourceType"_s, u"20"_s);
            break;
        }
        xml.writeEndElement();
    }

    // Hard disks
    for (qsizetype i = 0; i < m_disks.size(); ++i) {
        xml.writeStartElement(u"Item"_s);
        writeRasd(xml, u"AddressOnParent"_s, QString::number(diskSlots.at(i).second));
        writeRasd(xml, u"Description"_s, u"Hard disk image"_s);
        writeRasd(xml, u"ElementName"_s, u"disk%1"_s.arg(i + 1));
        writeRasd(xml, u"HostResource"_s, u"ovf:/disk/vmdisk%1"_s.arg(i + 1));
        writeRasd(xml, u"InstanceID"_s, QString::number(instanceId++));
        writeRasd(xml, u"Parent"_s, QString::number(diskSlots.at(i).first));
        writeRasd(xml, u"ResourceType"_s, u"17"_s);
        xml.writeEndElement();
    }

    // Network adapter
    if (hasNetwork) {
        xml.writeStartElement(u"Item"_s);
        // OVF carries the MAC in rasd:Address; rasd:MACAddress is not part of
        // the CIM schema and is ignored by other tools.
        if (!m_config.macAddress().isEmpty())
            writeRasd(xml, u"Address"_s, m_config.macAddress().toLower());
        writeRasd(xml, u"AutomaticAllocation"_s, u"true"_s);
        writeRasd(xml, u"Connection"_s, u"NAT"_s);
        writeRasd(xml, u"ElementName"_s, u"Network Adapter 1"_s);
        writeRasd(xml, u"InstanceID"_s, QString::number(instanceId++));
        writeRasd(xml, u"ResourceSubType"_s, networkSubTypeFor(m_config.networkCard()));
        writeRasd(xml, u"ResourceType"_s, u"10"_s);
        xml.writeEndElement();
    }

    // Firmware: OVF 1.x has no field for it, the configuration extension is
    // the one other tools know.
    const auto writeConfigEntry = [&xml](const QString &key, const QString &value) {
        xml.writeStartElement(u"vmw:Config"_s);
        xml.writeAttribute(u"ovf:required"_s, u"false"_s);
        xml.writeAttribute(u"vmw:key"_s, key);
        xml.writeAttribute(u"vmw:value"_s, value);
        xml.writeEndElement();
    };
    writeConfigEntry(u"firmware"_s, m_config.useUefi() ? u"efi"_s : u"bios"_s);
    if (m_config.useUefi())
        writeConfigEntry(u"bootOptions.efiSecureBootEnabled"_s, m_config.useSecureBoot() ? u"true"_s : u"false"_s);

    xml.writeEndElement(); // VirtualHardwareSection
    xml.writeEndElement(); // VirtualSystem
    xml.writeEndElement(); // Envelope
    xml.writeEndDocument();

    file.close();
    return file.error() == QFileDevice::NoError;
}

bool OvaExportJob::createManifest()
{
    const QStringList files = QStringList{m_ovfFileName} + m_vmdkFileNames;

    qint64 totalBytes = 0;
    for (const QString &fileName : files)
        totalBytes += QFileInfo(m_stagingDir->filePath(fileName)).size();

    beginPhase(kConvertBand, kManifestBand, totalBytes);

    QByteArray chunk(kHashChunk, Qt::Uninitialized);
    QStringList lines;
    qint64 processed = 0;

    for (const QString &fileName : files) {
        QFile f(m_stagingDir->filePath(fileName));
        if (!f.open(QIODevice::ReadOnly)) {
            recordError(i18n("Cannot read %1: %2", fileName, f.errorString()));
            return false;
        }

        QCryptographicHash hash(QCryptographicHash::Sha256);

        while (!f.atEnd()) {
            if (isCancelled())
                return false;

            const qint64 read = f.read(chunk.data(), chunk.size());
            if (read < 0) {
                recordError(i18n("Cannot read %1: %2", fileName, f.errorString()));
                return false;
            }
            if (read == 0)
                break;

            hash.addData(QByteArrayView(chunk.constData(), read));
            processed += read;
            reportPhaseBytes(processed);
        }

        lines << u"SHA256(%1)= %2"_s.arg(fileName, QString::fromLatin1(hash.result().toHex()));
    }

    QFile mfFile(m_stagingDir->filePath(m_mfFileName));
    if (!mfFile.open(QIODevice::WriteOnly)) {
        recordError(i18n("Cannot write %1: %2", m_mfFileName, mfFile.errorString()));
        return false;
    }

    mfFile.write(lines.join(u'\n').toUtf8());
    mfFile.write("\n");
    mfFile.close();

    return mfFile.error() == QFileDevice::NoError;
}

bool OvaExportJob::packageOva()
{
    archive *a = archive_write_new();
    if (!a) {
        recordError(i18n("Could not initialize libarchive."));
        return false;
    }

    // ustar caps entry size at 8 GiB and names at 100 characters;
    // pax_restricted stays ustar-compatible and only extends when needed.
    if (archive_write_set_format_pax_restricted(a) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while configuring the archive"));
        archive_write_free(a);
        return false;
    }

    if (archive_write_open_filename(a, QFile::encodeName(m_outputFilePath).constData()) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while creating %1", m_outputFilePath));
        archive_write_free(a);
        return false;
    }

    // OVA requires the descriptor first, then the manifest, then the disks.
    const QStringList filesToPack = QStringList{m_ovfFileName, m_mfFileName} + m_vmdkFileNames;

    qint64 totalBytes = 0;
    for (const QString &fileName : filesToPack)
        totalBytes += QFileInfo(m_stagingDir->filePath(fileName)).size();

    beginPhase(kManifestBand, 1000, totalBytes);

    bool ok = true;
    for (const QString &fileName : filesToPack) {
        if (isCancelled()) {
            ok = false;
            break;
        }
        if (!writeFileEntry(a, m_stagingDir->filePath(fileName), fileName)) {
            ok = false;
            break;
        }
    }

    return closeWriteArchive(a, ok);
}
