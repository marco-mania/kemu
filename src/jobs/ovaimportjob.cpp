/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovaimportjob.h"

#include "utils/externaltools.h"
#include "utils/vmconfig.h"

OvaImportJob::OvaImportJob(const QString &ovaPath, const QString &vmBaseDir, int diskSizeGb, QObject *parent)
    : KJob(parent)
    , m_ovaPath(ovaPath)
    , m_vmBaseDir(vmBaseDir)
    , m_diskSizeGb(diskSizeGb)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void OvaImportJob::start()
{
    if (!m_stagingDir.isValid()) {
        setError(1);
        setErrorText(i18n("Failed to create temporary directory."));
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       i18n("Importing OVA"),
                       QPair<QString, QString>(i18n("File"), QFileInfo(m_ovaPath).fileName()),
                       QPair<QString, QString>(i18n("Step"), i18n("Extracting archive...")));

    emitPercent(0, 1000);

    // Setup threaded execution for extraction
    m_cancelImport = false;
    m_watcher = new QFutureWatcher<bool>(this);
    connect(m_watcher, &QFutureWatcher<bool>::finished, this, &OvaImportJob::onExtractionFinished);

    // Start extraction in a background thread to prevent UI freezing
    const QFuture<bool> future = QtConcurrent::run(&OvaImportJob::performOvaExtraction, this);
    m_watcher->setFuture(future);
}

bool OvaImportJob::performOvaExtraction()
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, m_ovaPath.toUtf8().constData(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    qint64 totalCompressedBytes = QFileInfo(m_ovaPath).size();
    if (totalCompressedBytes <= 0)
        totalCompressedBytes = 1;

    struct archive_entry *entry;
    bool success = true;
    char buffer[1024 * 1024]; // 1 MB chunk buffer

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        if (m_cancelImport) {
            success = false;
            break;
        }

        const QString currentFileName = QString::fromUtf8(archive_entry_pathname(entry));

        // Security check: prevent directory traversal
        if (currentFileName.contains(u"..") || currentFileName.startsWith(u"/")) {
            continue;
        }

        const QString fullPath = m_stagingDir.filePath(currentFileName);

        // Some OVAs nest files in subdirectories - create them first.
        QDir().mkpath(QFileInfo(fullPath).path());

        QFile outFile(fullPath);

        if (currentFileName.endsWith(u".ovf"_s, Qt::CaseInsensitive))
            m_ovfPath = fullPath;

        if (!outFile.open(QIODevice::WriteOnly)) {
            success = false;
            break;
        }

        while (true) {
            if (m_cancelImport) {
                success = false;
                break;
            }

            const la_ssize_t readBytes = archive_read_data(a, buffer, sizeof(buffer));
            if (readBytes == 0)
                break; // EOF
            if (readBytes < 0) {
                success = false;
                break;
            } // Error

            outFile.write(buffer, readBytes);

            // Report progress (thread-safe dispatch)
            const qint64 processedBytes = archive_filter_bytes(a, -1);
            const int extractPercent = static_cast<int>((processedBytes * 100) / totalCompressedBytes);

            // Map 0-100% extraction to 0-150 (out of 1000) for the total job progress
            const int scaledPercent = qMin(static_cast<int>(extractPercent * 1.5), 150);

            QMetaObject::invokeMethod(
                this,
                [this, scaledPercent]() {
                    emitPercent(scaledPercent, 1000);
                },
                Qt::QueuedConnection);
        }

        outFile.close();
        if (!success)
            break;
    }

    archive_read_close(a);
    archive_read_free(a);

    return success;
}

void OvaImportJob::onExtractionFinished()
{
    const bool success = m_watcher->result();
    m_watcher->deleteLater();
    m_watcher = nullptr;

    if (m_cancelImport) {
        if (!m_vmName.isEmpty()) {
            QDir(m_vmBaseDir + u"/"_s + m_vmName).removeRecursively();
        }
        setError(KJob::KilledJobError);
        emitResult();
        return;
    }

    if (!success) {
        setError(2);
        setErrorText(i18n("Failed to extract OVA archive."));
        emitResult();
        return;
    }

    // Extraction is done, now we can parse the OVF in the main thread
    if (!parseOvf()) {
        setError(3);
        setErrorText(i18n("Failed to parse OVF descriptor or unsupported format."));
        emitResult();
        return;
    }

    // Check if a VM with this name already exists
    if (QDir(m_vmBaseDir + u"/"_s + m_vmName).exists()) {
        setError(4);
        setErrorText(
            i18n("A VM named '%1' already exists. "
                 "Please rename or delete it first.",
                 m_vmName));
        emitResult();
        return;
    }

    QDir().mkpath(m_vmBaseDir + u"/"_s + m_vmName);

    // Proceed to the conversion phase
    convertDisk();
}

bool OvaImportJob::parseOvf()
{
    if (m_ovfPath.isEmpty())
        return false; // no .ovf in the archive -> caller reports parse failure

    QFile ovfFile(m_ovfPath);
    if (!ovfFile.open(QIODevice::ReadOnly))
        return false;

    QXmlStreamReader xml(&ovfFile);

    int ramMb = GlobalDefaults.ramMb;
    int cpuCoreCount = GlobalDefaults.cpuCoreCount;
    int diskCapacityGb = 0;
    QString vmName;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isStartElement()) {
            // Get VM name from VirtualSystem
            if (xml.name() == u"VirtualSystem") {
                vmName = xml.attributes().value(u"ovf:id"_s).toString();
            } else if (xml.name() == u"Name" && !xml.attributes().hasAttribute(u"ovf:id"_s)) { // Name element (child of VirtualSystem)
                const QString text = xml.readElementText();
                if (!text.isEmpty())
                    vmName = text;
            } else if (xml.name() == u"ResourceType") { // ResourceType + VirtualQuantity
                const QString resType = xml.readElementText();

                // Forward-reading within the same item
                // We need VirtualQuantity as a sibling element
                // -> Simpler approach: note the ResourceType and continue reading
                // (Simplified implementation)
                if (resType == u"3"_s) {
                    // CPU – VirtualQuantity follows
                    while (!xml.atEnd()) {
                        xml.readNext();
                        if (xml.isStartElement() && xml.name() == u"VirtualQuantity") {
                            cpuCoreCount = xml.readElementText().toInt();
                            break;
                        }
                        if (xml.isEndElement() && xml.name() == u"Item")
                            break;
                    }
                } else if (resType == u"4"_s) {
                    // Memory: collect quantity and its allocation units.
                    qint64 qty = 0;
                    QString units;
                    while (!xml.atEnd()) {
                        xml.readNext();
                        if (xml.isStartElement() && xml.name() == u"VirtualQuantity")
                            qty = xml.readElementText().toLongLong();
                        else if (xml.isStartElement() && xml.name() == u"AllocationUnits")
                            units = xml.readElementText();
                        else if (xml.isEndElement() && xml.name() == u"Item")
                            break;
                    }
                    // Default to MiB (the common OVF case) when units are absent.
                    ramMb = static_cast<int>(ovfQuantityToBytes(qty, units, 1024LL * 1024) / (1024LL * 1024));
                } else if (resType == u"10"_s) {
                    // Network Adapter
                    while (!xml.atEnd()) {
                        xml.readNext();
                        if (xml.isStartElement() && xml.name() == u"MACAddress") {
                            QString mac = xml.readElementText();

                            // OVF often stores the MAC address without colons (e.g.,
                            // "525400123456"). We reformat it for QEMU and the UI to make it
                            // look nice:
                            if (mac.length() == 12 && !mac.contains(u':')) {
                                mac = QString(u"%1:%2:%3:%4:%5:%6"_s)
                                          .arg(mac.mid(0, 2), mac.mid(2, 2), mac.mid(4, 2), mac.mid(6, 2), mac.mid(8, 2), mac.mid(10, 2));
                            }

                            m_importedConfig.setMacAddress(mac);
                            break;
                        }
                        if (xml.isEndElement() && xml.name() == u"Item")
                            break;
                    }
                }
            } else if (xml.name() == u"Disk") {
                const QXmlStreamAttributes attrs = xml.attributes();
                const qint64 cap = attrs.value(u"ovf:capacity"_s).toLongLong();
                const QString capUnits = attrs.value(u"ovf:capacityAllocationUnits"_s).toString();
                if (cap > 0) {
                    const qint64 bytes = ovfQuantityToBytes(cap, capUnits, 1); // default unit: byte
                    diskCapacityGb = static_cast<int>((bytes + (1LL << 30) - 1) / (1LL << 30)); // round up
                }
            } else if (xml.name() == u"File") {
                // References/File: the disk image href (any format/name).
                const QString href = xml.attributes().value(u"ovf:href"_s).toString();
                if (m_diskPath.isEmpty() && !href.isEmpty())
                    m_diskPath = m_stagingDir.filePath(href);
            }
        }
    }

    if (vmName.isEmpty()) {
        // Fallback: File name without extension
        vmName = QFileInfo(m_ovaPath).completeBaseName();
        if (vmName.endsWith(u".ova"_s, Qt::CaseInsensitive))
            vmName.chop(4);
    }

    // Remove invalid characters from the VM name
    vmName.replace(QRegularExpression(u"[^a-zA-Z0-9_\\-. ]"_s), u"_"_s);
    m_vmName = vmName;

    m_importedConfig.setName(vmName);
    m_importedConfig.setRamMb(ramMb > 0 ? ramMb : GlobalDefaults.ramMb);
    m_importedConfig.setCpuCoreCount(cpuCoreCount > 0 ? cpuCoreCount : GlobalDefaults.cpuCoreCount);
    m_importedConfig.setDiskSizeGb(diskCapacityGb > 0 ? diskCapacityGb : (m_diskSizeGb > 0 ? m_diskSizeGb : GlobalDefaults.diskSizeGb));
    // Conservative profile for foreign appliances: maximise the chance the
    // imported guest boots without extra drivers. Users can refine per-VM later.
    m_importedConfig.setMachineType(VMSpecs::MachineType::Pc); // i440fx
    m_importedConfig.setUseUefi(false); // SeaBIOS
    m_importedConfig.setDiskInterface(VMSpecs::DiskInterface::SataIde); // IDE on pc
    m_importedConfig.setNetworkCard(VMSpecs::NetworkCard::E1000);
    m_importedConfig.setGpuType(VMSpecs::GpuType::Vga);
    m_importedConfig.setPointerDevice(VMSpecs::PointerDevice::Ps2);
    m_importedConfig.setKeyboardDevice(VMSpecs::KeyboardDevice::Ps2);
    m_importedConfig.setComment(i18n("Imported from OVA"));

    return true;
}

void OvaImportJob::convertDisk()
{
    const auto &tools = ExternalTools::instance();

    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       i18n("Importing OVA"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Step"), i18n("Converting disk format...")));

    const QString targetQcow2 = m_vmBaseDir + u"/"_s + m_vmName + u"/disk.qcow2"_s;

    m_process = new KProcess(this);
    m_process->setOutputChannelMode(KProcess::SeparateChannels);
    m_process->setProgram(tools.path(ExternalTools::QemuImg));
    m_process->setArguments({u"convert"_s, u"-p"_s, u"-O"_s, u"qcow2"_s, m_diskPath, targetQcow2});

    if (m_diskPath.isEmpty() || !QFile::exists(m_diskPath)) {
        setError(7);
        setErrorText(i18n("No disk image found in the OVA archive."));
        emitResult();
        return;
    }

    connect(m_process, &KProcess::readyReadStandardOutput, this, &OvaImportJob::onConvertReadyRead);
    connect(m_process, static_cast<void (KProcess::*)(int, QProcess::ExitStatus)>(&KProcess::finished), this, &OvaImportJob::onConvertFinished);

    m_process->start();
}

void OvaImportJob::onConvertReadyRead()
{
    m_process->setReadChannel(QProcess::StandardOutput);
    while (m_process->canReadLine()) {
        const QString line = QString::fromUtf8(m_process->readLine()).trimmed();

        const QRegularExpression rx(u"\\((\\d+\\.\\d+)/100%\\)"_s);
        const QRegularExpressionMatch match = rx.match(line);
        if (match.hasMatch()) {
            bool ok;
            const double percent = match.captured(1).toDouble(&ok);
            if (ok) {
                // 15% (150) were used for extraction, the remaining 85% (850) are for
                // conversion
                emitPercent(150 + qRound(percent * 8.5), 1000);
            }
        }
    }
}

void OvaImportJob::onConvertFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        QDir(m_vmBaseDir + u"/"_s + m_vmName).removeRecursively();
        setError(6);
        setErrorText(i18n("qemu-img convert from VMDK failed (exit code %1).", exitCode));
        emitResult();
        return;
    }

    m_importedConfig.setDiskPath(m_vmBaseDir + u"/"_s + m_vmName + u"/disk.qcow2"_s);

    emitPercent(1000, 1000);
    emitResult();
}

bool OvaImportJob::doKill()
{
    m_cancelImport = true;

    // Process phase: disconnect before killing so onConvertFinished() cannot
    // emit a second result.
    if (m_process && m_process->state() != QProcess::NotRunning) {
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
    }

    // Thread phase: defer to onExtractionFinished(), which cleans up.
    if (m_watcher && m_watcher->isRunning())
        return false;

    // Convert phase (or before anything started): clean up and let KJob emit
    // the single KilledJobError result.
    if (!m_vmName.isEmpty())
        QDir(m_vmBaseDir + u"/"_s + m_vmName).removeRecursively();

    return true;
}

// Convert an OVF quantity + AllocationUnits string to bytes.
// Handles the programmatic form ("byte * 2^20") and textual units
// ("MegaBytes", ...). Falls back to defaultUnitBytes when empty/unknown.
qint64 OvaImportJob::ovfQuantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes)
{
    if (quantity <= 0)
        return 0;

    static const QRegularExpression powRx(QStringLiteral(R"(byte\s*\*\s*2\^(\d+))"), QRegularExpression::CaseInsensitiveOption);
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
