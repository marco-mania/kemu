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
#include <QXmlStreamWriter>

#include <KLocalizedString>

#include "utils/qemuimgprocess.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int kConvertBand = 800;
constexpr int kManifestBand = 900;
constexpr qsizetype kHashChunk = 1 << 20;

// DMTF CIM_OperatingSystem OSType. 36 = Linux, 1 = Other. Extend as needed.
int cimOsType(VMSpecs::OsType type)
{
    return type == VMSpecs::OsType::Linux ? 36 : 1;
}

void writeRasd(QXmlStreamWriter &xml, const QString &element, const QString &text)
{
    xml.writeStartElement(u"rasd:"_s + element);
    xml.writeCharacters(text);
    xml.writeEndElement();
}

}

OvaExportJob::OvaExportJob(const QString &vmName,
                           const VMConfig &config,
                           const QString &diskPath,
                           qint64 diskCapacityBytes,
                           const QString &outputFilePath,
                           QObject *parent)
    : ArchiveStreamJob(parent)
    , m_vmName(vmName)
    , m_config(config)
    , m_diskPath(diskPath)
    , m_diskCapacityBytes(diskCapacityBytes)
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

    const qint64 sourceSize = QFileInfo(m_diskPath).size();
    if (sourceSize <= 0) {
        setError(SourceError);
        setErrorText(i18n("The disk image of VM '%1' is missing or empty.", m_vmName));
        emitResult();
        return;
    }

    // Staging copy plus the finished OVA both live on this filesystem.
    QString spaceMessage;
    if (!hasFreeSpace(outputDir, 2 * sourceSize, &spaceMessage)) {
        setError(DiskSpaceError);
        setErrorText(spaceMessage);
        emitResult();
        return;
    }

    m_vmdkFileName = m_vmName + u"-disk1.vmdk"_s;
    m_ovfFileName = m_vmName + u".ovf"_s;
    m_mfFileName = m_vmName + u".mf"_s;

    Q_EMIT description(this,
                       i18n("Exporting as OVA"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Step"), i18n("Converting disk image...")));

    m_converter = new QemuImgProcess(this);

    connect(m_converter, &QemuImgProcess::progress, this, [this](double percent) {
        emitPercent(qRound(percent * (kConvertBand / 100.0)), 1000);
    });
    connect(m_converter, &QemuImgProcess::finished, this, &OvaExportJob::onConvertFinished);

    const QString vmdkPath = m_stagingDir->filePath(m_vmdkFileName);
    const QStringList args = {u"convert"_s, u"-p"_s, u"-O"_s, u"vmdk"_s, u"-o"_s, u"subformat=streamOptimized"_s, m_diskPath, vmdkPath};

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
    postInfoMessage(i18n("Creating manifest (SHA-256)..."));
    if (!createManifest())
        return false;

    if (isCancelled())
        return false;

    postInfoMessage(i18n("Packaging OVA..."));
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

    const qint64 vmdkSize = QFileInfo(m_stagingDir->filePath(m_vmdkFileName)).size();

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();

    xml.writeStartElement(u"Envelope"_s);
    xml.writeAttribute(u"xmlns"_s, u"http://schemas.dmtf.org/ovf/envelope/1"_s);
    xml.writeAttribute(u"xmlns:ovf"_s, u"http://schemas.dmtf.org/ovf/envelope/1"_s);
    xml.writeAttribute(u"xmlns:rasd"_s, u"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData"_s);
    xml.writeAttribute(u"xmlns:vssd"_s, u"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData"_s);
    xml.writeAttribute(u"xmlns:xsi"_s, u"http://www.w3.org/2001/XMLSchema-instance"_s);

    // References
    xml.writeStartElement(u"References"_s);
    xml.writeStartElement(u"File"_s);
    xml.writeAttribute(u"ovf:href"_s, m_vmdkFileName);
    xml.writeAttribute(u"ovf:id"_s, u"file1"_s);
    xml.writeAttribute(u"ovf:size"_s, QString::number(vmdkSize));
    xml.writeEndElement();
    xml.writeEndElement();

    // DiskSection
    xml.writeStartElement(u"DiskSection"_s);
    xml.writeTextElement(u"Info"_s, u"Virtual disk information"_s);
    xml.writeStartElement(u"Disk"_s);
    // ovf:capacity defaults to bytes; use the exact virtual size.
    xml.writeAttribute(u"ovf:capacity"_s, QString::number(m_diskCapacityBytes));
    xml.writeAttribute(u"ovf:diskId"_s, u"vmdisk1"_s);
    xml.writeAttribute(u"ovf:fileRef"_s, u"file1"_s);
    xml.writeAttribute(u"ovf:format"_s, u"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized"_s);
    xml.writeEndElement();
    xml.writeEndElement();

    // NetworkSection
    xml.writeStartElement(u"NetworkSection"_s);
    xml.writeTextElement(u"Info"_s, u"Network information"_s);
    xml.writeStartElement(u"Network"_s);
    xml.writeAttribute(u"ovf:name"_s, u"NAT"_s);
    xml.writeTextElement(u"Description"_s, u"NAT network"_s);
    xml.writeEndElement();
    xml.writeEndElement();

    // VirtualSystem
    xml.writeStartElement(u"VirtualSystem"_s);
    xml.writeAttribute(u"ovf:id"_s, m_vmName);
    xml.writeTextElement(u"Info"_s, m_config.comment().isEmpty() ? u"Exported from KEMU"_s : m_config.comment());
    xml.writeTextElement(u"Name"_s, m_vmName);

    xml.writeStartElement(u"OperatingSystemSection"_s);
    xml.writeAttribute(u"ovf:id"_s, QString::number(cimOsType(m_config.osType())));
    xml.writeTextElement(u"Info"_s, u"Guest operating system"_s);
    xml.writeTextElement(u"Description"_s, VMConfig::toString(m_config.osType()));
    xml.writeEndElement();

    xml.writeStartElement(u"VirtualHardwareSection"_s);
    xml.writeTextElement(u"Info"_s, u"Hardware configuration"_s);

    xml.writeStartElement(u"System"_s);
    xml.writeTextElement(u"vssd:ElementName"_s, u"Virtual Hardware Family"_s);
    xml.writeTextElement(u"vssd:InstanceID"_s, u"0"_s);
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

    // SATA controller
    xml.writeStartElement(u"Item"_s);
    writeRasd(xml, u"Description"_s, u"SATA Controller"_s);
    writeRasd(xml, u"ElementName"_s, u"SATA Controller"_s);
    writeRasd(xml, u"InstanceID"_s, u"3"_s);
    writeRasd(xml, u"ResourceSubType"_s, u"AHCI"_s);
    writeRasd(xml, u"ResourceType"_s, u"20"_s);
    xml.writeEndElement();

    // Hard disk
    xml.writeStartElement(u"Item"_s);
    writeRasd(xml, u"AddressOnParent"_s, u"0"_s);
    writeRasd(xml, u"Description"_s, u"Hard disk image"_s);
    writeRasd(xml, u"ElementName"_s, u"disk1"_s);
    writeRasd(xml, u"HostResource"_s, u"ovf:/disk/vmdisk1"_s);
    writeRasd(xml, u"InstanceID"_s, u"4"_s);
    writeRasd(xml, u"Parent"_s, u"3"_s);
    writeRasd(xml, u"ResourceType"_s, u"17"_s);
    xml.writeEndElement();

    // Network adapter
    if (m_config.networkCard() != VMSpecs::NetworkCard::None) {
        xml.writeStartElement(u"Item"_s);
        if (!m_config.macAddress().isEmpty()) {
            QString mac = m_config.macAddress();
            mac.remove(u':');
            // OVF carries the MAC in rasd:Address; rasd:MACAddress is not part
            // of the CIM schema and is ignored by other tools.
            writeRasd(xml, u"Address"_s, mac);
        }
        writeRasd(xml, u"AutomaticAllocation"_s, u"true"_s);
        writeRasd(xml, u"Connection"_s, u"NAT"_s);
        writeRasd(xml, u"ElementName"_s, u"Network Adapter 1"_s);
        writeRasd(xml, u"InstanceID"_s, u"5"_s);
        writeRasd(xml, u"ResourceType"_s, u"10"_s);
        xml.writeEndElement();
    }

    xml.writeEndElement(); // VirtualHardwareSection
    xml.writeEndElement(); // VirtualSystem
    xml.writeEndElement(); // Envelope
    xml.writeEndDocument();

    file.close();
    return file.error() == QFileDevice::NoError;
}

bool OvaExportJob::createManifest()
{
    const QStringList files = {m_ovfFileName, m_vmdkFileName};

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
    const QStringList filesToPack = {m_ovfFileName, m_mfFileName, m_vmdkFileName};

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
