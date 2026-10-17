/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovaimportjob.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QtMath>

#include <KLocalizedString>

#include "utils/archivepaths.h"
#include "utils/qemuimgprocess.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int kExtractBand = 150;

const QString kOvfNs = u"http://schemas.dmtf.org/ovf/envelope/1"_s;

// Namespace-aware lookup with a fallback for descriptors that declare no
// namespace at all.
QString ovfAttr(const QXmlStreamReader &xml, const QString &name)
{
    const QXmlStreamAttributes attrs = xml.attributes();
    if (attrs.hasAttribute(kOvfNs, name))
        return attrs.value(kOvfNs, name).toString();
    return attrs.value(u"ovf:"_s + name).toString();
}

// Collects the children of one <Item> so that element order does not matter.
// The CIM schema sorts them alphabetically, which puts AllocationUnits before
// ResourceType.
QHash<QString, QString> readItem(QXmlStreamReader &xml)
{
    QHash<QString, QString> fields;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == u"Item")
            break;
        if (xml.isStartElement())
            fields.insert(xml.name().toString(), xml.readElementText());
    }

    return fields;
}

QString normalizeMac(QString mac)
{
    mac.remove(u':');
    mac.remove(u'-');
    if (mac.length() != 12)
        return {};

    QStringList parts;
    for (int i = 0; i < 12; i += 2)
        parts << mac.mid(i, 2);
    return parts.join(u':').toLower();
}

}

OvaImportJob::OvaImportJob(const QString &ovaPath, const QString &vmBaseDir, int diskSizeGb, QObject *parent)
    : ArchiveStreamJob(parent)
    , m_ovaPath(ovaPath)
    , m_vmBaseDir(vmBaseDir)
    , m_diskSizeGb(diskSizeGb)
{
}

void OvaImportJob::start()
{
    // Staging next to the VM directory, not in tmpfs.
    m_stagingDir = std::make_unique<QTemporaryDir>(m_vmBaseDir + u"/.kemu-import-XXXXXX"_s);
    if (!m_stagingDir->isValid()) {
        setError(StagingError);
        setErrorText(i18n("Failed to create a staging directory in %1.", m_vmBaseDir));
        emitResult();
        return;
    }

    const qint64 archiveSize = QFileInfo(m_ovaPath).size();

    // Extracted VMDK plus converted qcow2 both land here.
    QString spaceMessage;
    if (!hasFreeSpace(m_vmBaseDir, 3 * archiveSize, &spaceMessage)) {
        setError(DiskSpaceError);
        setErrorText(spaceMessage);
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       i18n("Importing OVA"),
                       QPair<QString, QString>(i18n("File"), QFileInfo(m_ovaPath).fileName()),
                       QPair<QString, QString>(i18n("Step"), i18n("Extracting archive...")));

    beginPhase(0, kExtractBand, archiveSize);
    startWorker();
}

bool OvaImportJob::runWorker()
{
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, QFile::encodeName(m_ovaPath).constData(), 10240) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while opening %1", m_ovaPath));
        archive_read_free(a);
        return false;
    }

    const QDir stagingDir(m_stagingDir->path());
    bool ok = true;
    archive_entry *entry = nullptr;

    for (;;) {
        if (isCancelled()) {
            ok = false;
            break;
        }

        const EntryResult r = nextEntry(a, &entry);
        if (r == EntryResult::EndOfArchive)
            break;
        if (r == EntryResult::Failed) {
            ok = false;
            break;
        }

        if (archive_entry_filetype(entry) != AE_IFREG)
            continue;

        const QString name = QString::fromUtf8(archive_entry_pathname(entry));
        const QString destPath = ArchivePaths::resolveWithin(stagingDir, name);
        if (destPath.isEmpty())
            continue;

        // Some OVAs nest files in subdirectories.
        QDir().mkpath(QFileInfo(destPath).path());

        if (!extractEntryTo(a, destPath)) {
            ok = false;
            break;
        }

        if (name.endsWith(u".ovf"_s, Qt::CaseInsensitive))
            m_ovfPath = destPath;
    }

    archive_read_close(a);
    archive_read_free(a);

    return ok;
}

void OvaImportJob::workerFinished(bool ok)
{
    if (!ok) {
        setError(ArchiveError);
        setErrorText(workerErrorText().isEmpty() ? i18n("Failed to extract the OVA archive.") : workerErrorText());
        emitResult();
        return;
    }

    if (!parseOvf()) {
        setError(ContentError);
        setErrorText(i18n("Failed to parse the OVF descriptor or unsupported format."));
        emitResult();
        return;
    }

    m_targetDir = ArchivePaths::resolveWithin(QDir(m_vmBaseDir), m_vmName);
    if (m_targetDir.isEmpty()) {
        setError(ContentError);
        setErrorText(i18n("The OVF descriptor contains an invalid VM name."));
        emitResult();
        return;
    }

    if (QDir(m_targetDir).exists()) {
        setError(TargetExistsError);
        setErrorText(i18n("A VM named '%1' already exists. Please rename or delete it first.", m_vmName));
        emitResult();
        return;
    }

    if (m_sourceDisks.isEmpty() || !QFile::exists(m_sourceDisks.first())) {
        setError(ContentError);
        setErrorText(i18n("No disk image found in the OVA archive."));
        emitResult();
        return;
    }

    for (const QString &path : std::as_const(m_sourceDisks)) {
        if (!QFile::exists(path)) {
            setError(ContentError);
            setErrorText(i18n("The disk image %1 is missing from the OVA archive.", QFileInfo(path).fileName()));
            emitResult();
            return;
        }
    }

    QDir().mkpath(m_targetDir);
    convertDisk();
}

void OvaImportJob::discardPartialOutput()
{
    if (!m_targetDir.isEmpty())
        QDir(m_targetDir).removeRecursively();
}

void OvaImportJob::abortExternalTools()
{
    if (m_converter)
        m_converter->abort();
}

bool OvaImportJob::parseOvf()
{
    if (m_ovfPath.isEmpty())
        return false;

    QFile ovfFile(m_ovfPath);
    if (!ovfFile.open(QIODevice::ReadOnly))
        return false;

    QXmlStreamReader xml(&ovfFile);

    int ramMb = GlobalDefaults.ramMb;
    int cpuCoreCount = GlobalDefaults.cpuCoreCount;
    qint64 diskCapacityBytes = 0;
    QString vmName;
    QString macAddress;

    QHash<QString, QString> fileHrefs; // ovf:id -> ovf:href
    QStringList diskFileRefs;

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;

        const QStringView element = xml.name();

        if (element == u"VirtualSystem") {
            if (vmName.isEmpty())
                vmName = ovfAttr(xml, u"id"_s);

        } else if (element == u"Name") {
            const QString text = xml.readElementText();
            if (!text.isEmpty())
                vmName = text;

        } else if (element == u"File") {
            const QString id = ovfAttr(xml, u"id"_s);
            const QString href = ovfAttr(xml, u"href"_s);
            if (!href.isEmpty())
                fileHrefs.insert(id, href);

        } else if (element == u"Disk") {
            diskFileRefs << ovfAttr(xml, u"fileRef"_s);

            if (diskCapacityBytes == 0) {
                const qint64 capacity = ovfAttr(xml, u"capacity"_s).toLongLong();
                const QString units = ovfAttr(xml, u"capacityAllocationUnits"_s);
                if (capacity > 0)
                    diskCapacityBytes = ovfQuantityToBytes(capacity, units, 1); // default unit: byte
            }

        } else if (element == u"Item") {
            const QHash<QString, QString> item = readItem(xml);

            switch (item.value(u"ResourceType"_s).toInt()) {
            case 3:
                cpuCoreCount = item.value(u"VirtualQuantity"_s).toInt();
                break;

            case 4: {
                const qint64 quantity = item.value(u"VirtualQuantity"_s).toLongLong();
                const QString units = item.value(u"AllocationUnits"_s);
                // Default to MiB, the common OVF case, when units are absent.
                ramMb = static_cast<int>(ovfQuantityToBytes(quantity, units, 1024LL * 1024) / (1024LL * 1024));
                break;
            }

            case 10: {
                // OVF carries the MAC in rasd:Address; rasd:MACAddress is a
                // non-standard extension some tools emit.
                QString raw = item.value(u"Address"_s);
                if (raw.isEmpty())
                    raw = item.value(u"MACAddress"_s);
                if (macAddress.isEmpty())
                    macAddress = normalizeMac(raw);
                break;
            }

            default:
                break;
            }
        }
    }

    if (xml.hasError())
        return false;

    // Disks in the order of the DiskSection, the first one is the system disk.
    // Taking File references directly would pick up ISOs and NVRAM blobs.
    for (const QString &ref : std::as_const(diskFileRefs)) {
        const QString href = fileHrefs.value(ref);
        if (!href.isEmpty())
            m_sourceDisks << m_stagingDir->filePath(href);
    }
    if (m_sourceDisks.isEmpty() && !fileHrefs.isEmpty())
        m_sourceDisks << m_stagingDir->filePath(fileHrefs.constBegin().value());

    if (vmName.isEmpty())
        vmName = QFileInfo(m_ovaPath).completeBaseName();

    m_vmName = ArchivePaths::sanitizeVmName(vmName);
    if (m_vmName.isEmpty())
        return false;

    const int diskCapacityGb = diskCapacityBytes > 0 ? static_cast<int>((diskCapacityBytes + (1LL << 30) - 1) / (1LL << 30)) : 0;

    m_importedConfig.setName(m_vmName);
    m_importedConfig.setRamMb(ramMb > 0 ? ramMb : GlobalDefaults.ramMb);
    m_importedConfig.setCpuCoreCount(cpuCoreCount > 0 ? cpuCoreCount : GlobalDefaults.cpuCoreCount);
    m_importedConfig.setDiskSizeGb(diskCapacityGb > 0 ? diskCapacityGb : (m_diskSizeGb > 0 ? m_diskSizeGb : GlobalDefaults.diskSizeGb));
    if (!macAddress.isEmpty())
        m_importedConfig.setMacAddress(macAddress);

    // Conservative profile for foreign appliances: maximise the chance the
    // imported guest boots without extra drivers.
    m_importedConfig.setMachineType(VMSpecs::MachineType::Pc);
    m_importedConfig.setUseUefi(false);
    m_importedConfig.setDiskInterface(VMSpecs::DiskInterface::SataIde);
    m_importedConfig.setNetworkCard(VMSpecs::NetworkCard::E1000);
    m_importedConfig.setGpuType(VMSpecs::GpuType::Vga);
    m_importedConfig.setPointerDevice(VMSpecs::PointerDevice::Ps2);
    m_importedConfig.setKeyboardDevice(VMSpecs::KeyboardDevice::Ps2);
    m_importedConfig.setComment(i18n("Imported from OVA"));

    QList<DiskConfig> extraDisks;
    QStringList taken;
    for (qsizetype i = 1; i < m_sourceDisks.size(); ++i) {
        DiskConfig disk;
        disk.fileName = VMConfig::nextExtraDiskFileName(taken);
        disk.diskInterface = VMSpecs::DiskInterface::SataIde;
        disk.serial = VMConfig::defaultDiskSerial(disk.fileName);
        taken << disk.fileName;
        extraDisks << disk;
    }

    // Prefer the plain PC chipset, but not at the price of a data disk: AHCI
    // on Q35 has more ports than PIIX IDE.
    const int availableDisks = static_cast<int>(extraDisks.size());
    m_importedConfig.setExtraDisks(extraDisks);
    if (!m_importedConfig.storageLayoutError().isEmpty())
        m_importedConfig.setMachineType(VMSpecs::MachineType::Q35);
    while (!extraDisks.isEmpty() && !m_importedConfig.storageLayoutError().isEmpty()) {
        extraDisks.removeLast();
        m_importedConfig.setExtraDisks(extraDisks);
    }

    if (extraDisks.size() < availableDisks) {
        m_sourceDisks.resize(extraDisks.size() + 1);
        Q_EMIT infoMessage(this, i18n("The appliance contains more disks than can be attached; only the first %1 are imported.", m_sourceDisks.size()));
    }

    return true;
}

QString OvaImportJob::targetPathFor(qsizetype index) const
{
    const QString fileName = index == 0 ? QString(KEMUConst::VMDiskFileName) : m_importedConfig.extraDisks().at(index - 1).fileName;
    return m_targetDir + u"/"_s + fileName;
}

void OvaImportJob::convertDisk()
{
    Q_EMIT description(this,
                       i18n("Importing OVA"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Step"),
                                               m_sourceDisks.size() > 1 ? i18n("Converting disk %1 of %2...", m_convertIndex + 1, m_sourceDisks.size())
                                                                        : i18n("Converting disk format...")));

    if (m_converter)
        m_converter->deleteLater();
    m_converter = new QemuImgProcess(this);

    connect(m_converter, &QemuImgProcess::progress, this, [this](double percent) {
        const double perDisk = (1000 - kExtractBand) / double(m_sourceDisks.size());
        emitPercent(kExtractBand + qRound(m_convertIndex * perDisk + percent * perDisk / 100.0), 1000);
    });
    connect(m_converter, &QemuImgProcess::finished, this, &OvaImportJob::onConvertFinished);

    if (!m_converter->start({u"convert"_s, u"-p"_s, u"-O"_s, u"qcow2"_s, m_sourceDisks.at(m_convertIndex), targetPathFor(m_convertIndex)})) {
        QDir(m_targetDir).removeRecursively();
        setError(ToolError);
        setErrorText(m_converter->errorText());
        emitResult();
    }
}

void OvaImportJob::onConvertFinished(bool ok)
{
    if (!ok) {
        QDir(m_targetDir).removeRecursively();
        setError(ToolError);
        setErrorText(m_converter->errorText());
        emitResult();
        return;
    }

    if (++m_convertIndex < m_sourceDisks.size()) {
        convertDisk();
        return;
    }

    const QString targetQcow2 = targetPathFor(0);
    m_importedConfig.setDiskPath(targetQcow2);

    // The OVF capacity is a hint; the converted image is the truth.
    const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(targetQcow2);
    if (info.isValid)
        m_importedConfig.setDiskSizeGb(qCeil(info.virtualSizeBytes / double(1LL << 30)));

    emitPercent(1000, 1000);
    emitResult();
}

qint64 OvaImportJob::ovfQuantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes)
{
    if (quantity <= 0)
        return 0;

    static const QRegularExpression powRx(uR"(byte\s*\*\s*2\^(\d+))"_s, QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = powRx.match(units);
    if (m.hasMatch())
        return quantity * (1LL << m.captured(1).toInt());

    const QString u = units.trimmed().toLower();
    if (u.isEmpty())
        return quantity * defaultUnitBytes;
    if (u == u"byte"_s || u == u"bytes"_s)
        return quantity;
    if (u.startsWith(u"kilo"_s))
        return quantity * 1024LL;
    if (u.startsWith(u"mega"_s))
        return quantity * 1024LL * 1024;
    if (u.startsWith(u"giga"_s))
        return quantity * 1024LL * 1024 * 1024;
    return quantity * defaultUnitBytes;
}
