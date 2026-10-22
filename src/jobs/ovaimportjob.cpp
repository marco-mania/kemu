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

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtMath>

#include <KLocalizedString>

#include <algorithm>
#include <limits>
#include <optional>

#include "utils/archivepaths.h"
#include "utils/diskimagecheck.h"
#include "utils/externaltools.h"
#include "utils/ovfparser.h"
#include "utils/qemuimgprocess.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int kExtractBand = 150;
constexpr qsizetype kMaxDescriptorSize = 16 << 20;

// KEMU has no SCSI controller; SATA/IDE is what every guest has a driver for.
VMSpecs::DiskInterface diskInterfaceFor(OvfStorageBus bus)
{
    return bus == OvfStorageBus::Nvme ? VMSpecs::DiskInterface::Nvme : VMSpecs::DiskInterface::SataIde;
}

// Models without a QEMU counterpart (E1000e, vendor specific ones) fall back to E1000.
VMSpecs::NetworkCard networkCardFor(const QString &model)
{
    const QString lower = model.toLower();
    if (lower.contains(u"virtio"_s))
        return VMSpecs::NetworkCard::Virtio;
    if (lower.contains(u"pcnet"_s) || lower.contains(u"am79c97"_s))
        return VMSpecs::NetworkCard::Pcnet;
    if (lower.contains(u"8139"_s))
        return VMSpecs::NetworkCard::Rtl8139;
    return VMSpecs::NetworkCard::E1000;
}

QByteArray fileDigest(const QString &path, QCryptographicHash::Algorithm algorithm)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QCryptographicHash hash(algorithm);
    if (!hash.addData(&file))
        return {};
    return hash.result().toHex();
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

std::optional<OvfDescriptor> OvaImportJob::readDescriptor(const QString &ovaPath)
{
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    std::optional<OvfDescriptor> result;
    if (archive_read_open_filename(a, QFile::encodeName(ovaPath).constData(), 10240) == ARCHIVE_OK) {
        archive_entry *entry = nullptr;
        // The descriptor comes first; libarchive skips the data of anything
        // before it. Same choice as runWorker(): the first .ovf entry.
        while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
            const QString name = QString::fromUtf8(archive_entry_pathname(entry));
            if (archive_entry_filetype(entry) != AE_IFREG || !name.endsWith(u".ovf"_s, Qt::CaseInsensitive))
                continue;

            QByteArray data;
            char buffer[65536];
            la_ssize_t n;
            while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0 && data.size() <= kMaxDescriptorSize)
                data.append(buffer, n);
            if (n == 0) {
                QBuffer device(&data);
                device.open(QIODevice::ReadOnly);
                result = OvfParser::parse(&device);
            }
            break;
        }
    }

    archive_read_free(a);
    return result;
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

        // The manifest usually precedes the disks, so they are hashed while
        // they are extracted.
        const auto expected = m_manifest.constFind(QDir::cleanPath(name));
        std::optional<QCryptographicHash> hash;
        if (expected != m_manifest.constEnd())
            hash.emplace(expected->algorithm);

        if (!extractEntryTo(a, destPath, hash ? &*hash : nullptr)) {
            ok = false;
            break;
        }

        if (hash) {
            if (hash->result().toHex() != expected->digest) {
                recordError(i18n("The checksum of %1 does not match the manifest. The OVA archive is damaged or was modified.", name));
                ok = false;
                break;
            }
            m_verified.insert(expected.key());
        }

        if (name.endsWith(u".ovf"_s, Qt::CaseInsensitive) && m_ovfPath.isEmpty())
            m_ovfPath = destPath;

        if (name.endsWith(u".mf"_s, Qt::CaseInsensitive) && !m_hasManifest && !readManifest(destPath)) {
            ok = false;
            break;
        }
    }

    archive_read_close(a);
    archive_read_free(a);

    return ok && verifyRemainingManifestEntries();
}

bool OvaImportJob::readManifest(const QString &path)
{
    QFile file(path);
    const auto entries = file.open(QIODevice::ReadOnly) ? OvfParser::parseManifest(&file) : std::nullopt;
    if (!entries) {
        recordError(i18n("The manifest of the OVA archive could not be read."));
        return false;
    }

    m_hasManifest = true;
    for (const OvfManifestEntry &entry : *entries)
        m_manifest.insert(QDir::cleanPath(entry.fileName), entry);
    return true;
}

// Entries extracted before the manifest, in archives that do not follow the
// order OVF prescribes.
bool OvaImportJob::verifyRemainingManifestEntries()
{
    const QDir stagingDir(m_stagingDir->path());
    for (auto it = m_manifest.cbegin(); it != m_manifest.cend(); ++it) {
        if (m_verified.contains(it.key()))
            continue;
        if (isCancelled())
            return false;

        const QString path = ArchivePaths::resolveWithin(stagingDir, it.key());
        if (path.isEmpty() || !QFile::exists(path)) {
            recordError(i18n("%1 is listed in the manifest but missing from the OVA archive.", it.key()));
            return false;
        }
        if (fileDigest(path, it->algorithm) != it->digest) {
            recordError(i18n("The checksum of %1 does not match the manifest. The OVA archive is damaged or was modified.", it.key()));
            return false;
        }
    }
    return true;
}

void OvaImportJob::workerFinished(bool ok)
{
    if (!ok) {
        setError(ArchiveError);
        setErrorText(workerErrorText().isEmpty() ? i18n("Failed to extract the OVA archive.") : workerErrorText());
        emitResult();
        return;
    }

    QString parseError;
    if (!parseOvf(&parseError)) {
        setError(ContentError);
        setErrorText(parseError.isEmpty() ? i18n("Failed to parse the OVF descriptor or unsupported format.") : parseError);
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

    if (m_sourceDisks.isEmpty()) {
        setError(ContentError);
        setErrorText(i18n("No disk image found in the OVA archive."));
        emitResult();
        return;
    }

    for (SourceDisk &disk : m_sourceDisks) {
        if (disk.path.isEmpty())
            continue;
        if (!QFile::exists(disk.path)) {
            setError(ContentError);
            setErrorText(i18n("The disk image %1 is missing from the OVA archive.", QFileInfo(disk.path).fileName()));
            emitResult();
            return;
        }

        // Only files from the archive itself may end up in the VM.
        const DiskImageCheck::Inspection inspection =
            DiskImageCheck::inspect(ExternalTools::instance().path(ExternalTools::QemuImg), disk.path, m_stagingDir->path(), false);
        if (!inspection.problem.isEmpty()) {
            setError(ContentError);
            setErrorText(i18n("The OVA archive cannot be imported: %1", inspection.problem));
            emitResult();
            return;
        }
        disk.format = inspection.format;
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

bool OvaImportJob::parseOvf(QString *error)
{
    if (m_ovfPath.isEmpty())
        return false;

    const std::optional<OvfDescriptor> ovf = OvfParser::parseFile(m_ovfPath);
    if (!ovf)
        return false;

    const QDir stagingDir(m_stagingDir->path());
    for (const OvfDisk &disk : ovf->disks) {
        if (disk.isBlank()) {
            if (disk.capacityBytes <= 0) {
                *error = i18n("The appliance defines an empty disk without a size.");
                return false;
            }
            m_sourceDisks.append({{}, disk.capacityBytes});
            continue;
        }
        if (!disk.compression.isEmpty() || disk.chunkSize > 0) {
            *error = i18n("The disk image %1 is stored compressed or split into chunks, which is not supported yet.", disk.href);
            return false;
        }
        // The descriptor is untrusted: a disk must not point outside the archive.
        const QString path = ArchivePaths::resolveWithin(stagingDir, disk.href);
        if (path.isEmpty()) {
            *error = i18n("The OVF descriptor refers to a disk image outside the archive (%1).", disk.href);
            return false;
        }
        m_sourceDisks.append({path, 0});
    }

    m_vmName = ArchivePaths::sanitizeVmName(ovf->name.isEmpty() ? QFileInfo(m_ovaPath).completeBaseName() : ovf->name);
    if (m_vmName.isEmpty())
        return false;

    qint64 diskCapacityBytes = 0;
    for (const OvfDisk &disk : ovf->disks) {
        if (disk.capacityBytes > 0) {
            diskCapacityBytes = disk.capacityBytes;
            break;
        }
    }
    const int diskCapacityGb = diskCapacityBytes > 0 ? static_cast<int>((diskCapacityBytes + (1LL << 30) - 1) / (1LL << 30)) : 0;

    m_importedConfig.setName(m_vmName);
    const qint64 ramMb = ovf->memoryBytes / (1024LL * 1024);
    m_importedConfig.setRamMb(ramMb > 0 ? static_cast<int>(std::min<qint64>(ramMb, std::numeric_limits<int>::max())) : GlobalDefaults.ramMb);
    const int maxCores = VMConfig::maxCoresForModel(m_importedConfig.cpuModel());
    m_importedConfig.setCpuCoreCount(ovf->cpuCount > 0 ? std::min(ovf->cpuCount, maxCores) : std::min(GlobalDefaults.cpuCoreCount, maxCores));
    m_importedConfig.setDiskSizeGb(diskCapacityGb > 0 ? diskCapacityGb : (m_diskSizeGb > 0 ? m_diskSizeGb : GlobalDefaults.diskSizeGb));
    if (const QString mac = ovf->firstMacAddress(); !mac.isEmpty())
        m_importedConfig.setMacAddress(mac);

    const QString osText = ovf->osType + u' ' + ovf->osDescription;
    bool osFound = false;
    auto [osFamily, distroId] = VMConfig::guessOSFromText(osText, &osFound);
    if (!osFound && osText.contains(u"linux"_s, Qt::CaseInsensitive))
        osFamily = VMSpecs::OsFamily::Linux;
    m_importedConfig.setOsFamily(osFamily);
    m_importedConfig.setDistroId(distroId);
    // Windows keeps the hardware clock in local time.
    m_importedConfig.setUseLocalTime(osFamily == VMSpecs::OsFamily::Windows);

    // Keep what the guest was installed with where KEMU has it: a changed
    // firmware or boot controller leaves most guests unbootable. Everything
    // else falls back to devices every guest has drivers for.
    const bool efi = ovf->firmware == OvfFirmware::Efi;
    const bool ahci = std::any_of(ovf->disks.cbegin(), ovf->disks.cend(), [](const OvfDisk &disk) {
        return disk.bus == OvfStorageBus::Sata;
    });
    m_importedConfig.setMachineType(efi || ahci ? VMSpecs::MachineType::Q35 : VMSpecs::MachineType::Pc);
    m_importedConfig.setUseUefi(efi);
    m_importedConfig.setUseSecureBoot(efi && ovf->secureBoot);
    m_importedConfig.setDiskInterface(diskInterfaceFor(ovf->disks.isEmpty() ? OvfStorageBus::Unknown : ovf->disks.first().bus));
    m_importedConfig.setNetworkCard(networkCardFor(ovf->networkAdapters.isEmpty() ? QString() : ovf->networkAdapters.first().model));
    m_importedConfig.setGpuType(VMSpecs::GpuType::Vga);
    m_importedConfig.setPointerDevice(VMSpecs::PointerDevice::Ps2);
    m_importedConfig.setKeyboardDevice(VMSpecs::KeyboardDevice::Ps2);
    m_importedConfig.setComment(ovf->annotation.isEmpty() ? i18n("Imported from OVA") : ovf->annotation);

    if (ovf->virtualSystemCount > 1)
        Q_EMIT infoMessage(this, i18n("The appliance contains %1 virtual machines; only the first one, '%2', is imported.", ovf->virtualSystemCount, m_vmName));

    QList<DiskConfig> extraDisks;
    QStringList taken;
    for (qsizetype i = 1; i < m_sourceDisks.size(); ++i) {
        DiskConfig disk;
        disk.fileName = VMConfig::nextExtraDiskFileName(taken);
        disk.diskInterface = diskInterfaceFor(ovf->disks.at(i).bus);
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

    const SourceDisk &disk = m_sourceDisks.at(m_convertIndex);
    const QString target = targetPathFor(m_convertIndex);
    QStringList args;
    if (disk.path.isEmpty()) {
        args = {u"create"_s, u"-q"_s, u"-f"_s, u"qcow2"_s, target, QString::number(disk.blankBytes)};
    } else {
        args = {u"convert"_s, u"-p"_s, u"-f"_s, disk.format, u"-O"_s, u"qcow2"_s, disk.path, target};
    }
    if (!m_converter->start(args)) {
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
