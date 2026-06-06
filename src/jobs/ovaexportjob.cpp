/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovaexportjob.h"

#include "utils/externaltools.h"

OvaExportJob::OvaExportJob(const QString &vmName, const VMConfig &config, const QString &diskPath, const QString &outputFilePath, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_config(config)
    , m_diskPath(diskPath)
    , m_outputFilePath(outputFilePath)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void OvaExportJob::start()
{
    if (!m_stagingDir.isValid()) {
        setError(1);
        setErrorText(i18n("Failed to create temporary directory."));
        emitResult();
        return;
    }

    const auto &tools = ExternalTools::instance();

    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
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

    // step 1: qcow2 → vmdk
    const QString vmdkPath = m_stagingDir.filePath(m_vmdkFileName);

    m_process = new KProcess(this);
    m_process->setOutputChannelMode(KProcess::SeparateChannels);
    m_process->setProgram(tools.path(ExternalTools::QemuImg));
    m_process->setArguments({u"convert"_s, u"-p"_s, u"-O"_s, u"vmdk"_s, u"-o"_s, u"subformat=streamOptimized"_s, m_diskPath, vmdkPath});

    connect(m_process, &KProcess::readyReadStandardOutput, this, &OvaExportJob::onConvertReadyRead);
    connect(m_process, QOverload<int, QProcess::ExitStatus>::of(&KProcess::finished), this, &OvaExportJob::onConvertFinished);

    m_process->start();

    if (!m_process->waitForStarted(5000)) {
        setError(2);
        setErrorText(i18n("Failed to start qemu-img: %1", m_process->errorString()));
        emitResult();
    }
}

void OvaExportJob::onConvertReadyRead()
{
    m_outputBuffer.append(m_process->readAllStandardOutput());

    int lastBreakIndex = -1;
    for (int i = m_outputBuffer.size() - 1; i >= 0; --i) {
        const char c = m_outputBuffer.at(i);
        if (c == '\r' || c == '\n') {
            lastBreakIndex = i;
            break;
        }
    }
    if (lastBreakIndex == -1)
        return;

    const QByteArray completeData = m_outputBuffer.left(lastBreakIndex);
    m_outputBuffer.remove(0, lastBreakIndex + 1);

    const QString output = QString::fromLocal8Bit(completeData);
    const QStringView view(output);

    const QStringView suffix(u"/100%)");
    const QChar prefix(u'(');
    int endIndex = view.lastIndexOf(suffix);

    while (endIndex != -1) {
        const int startIndex = view.lastIndexOf(prefix, endIndex);
        if (startIndex != -1) {
            const QStringView numberView = view.mid(startIndex + 1, endIndex - startIndex - 1);
            bool ok;
            const double percent = numberView.toDouble(&ok);
            if (ok) {
                // disk conversion is ~80% of whole job
                emitPercent(qRound(percent * 8), 1000);
                break;
            }
        }
        endIndex = view.lastIndexOf(suffix, startIndex > 0 ? startIndex - 1 : -1);
    }
}

void OvaExportJob::onConvertFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (exitStatus != QProcess::NormalExit || exitCode != 0) {
        setError(3);
        setErrorText(i18n("qemu-img convert to VMDK failed (exit code %1).", exitCode));
        emitResult();
        return;
    }

    createOvfDescriptor();

    m_cancelExport = false;
    m_watcher = new QFutureWatcher<bool>(this);
    connect(m_watcher, &QFutureWatcher<bool>::finished, this, &OvaExportJob::onPackageFinished);

    const QFuture<bool> future = QtConcurrent::run(&OvaExportJob::performBackgroundTasks, this);
    m_watcher->setFuture(future);
}

bool OvaExportJob::performBackgroundTasks()
{
    QMetaObject::invokeMethod(
        this,
        [this]() {
            Q_EMIT infoMessage(this, i18n("Creating Manifest (SHA256)..."));
        },
        Qt::QueuedConnection);

    if (!createManifestInThread()) {
        return false;
    }

    if (m_cancelExport) {
        return false;
    }

    QMetaObject::invokeMethod(
        this,
        [this]() {
            Q_EMIT infoMessage(this, i18n("Packaging OVA..."));
        },
        Qt::QueuedConnection);

    return performOvaPackagingInThread();
}

void OvaExportJob::createOvfDescriptor()
{
    const QString ovfPath = m_stagingDir.filePath(m_ovfFileName);
    QFile file(ovfPath);
    if (!file.open(QIODevice::WriteOnly))
        return;

    const QFileInfo vmdkInfo(m_stagingDir.filePath(m_vmdkFileName));
    const qint64 vmdkSize = vmdkInfo.size();

    QXmlStreamWriter xml(&file);
    xml.setAutoFormatting(true);
    xml.writeStartDocument();

    // OVF Envelope
    xml.writeStartElement(u"Envelope"_s);
    xml.writeAttribute(u"xmlns"_s, u"http://schemas.dmtf.org/ovf/envelope/1"_s);
    xml.writeAttribute(u"xmlns:rasd"_s, u"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_ResourceAllocationSettingData"_s);
    xml.writeAttribute(u"xmlns:vssd"_s, u"http://schemas.dmtf.org/wbem/wscim/1/cim-schema/2/CIM_VirtualSystemSettingData"_s);
    xml.writeAttribute(u"xmlns:ovf"_s, u"http://schemas.dmtf.org/ovf/envelope/1"_s);
    xml.writeAttribute(u"xmlns:vbox"_s, u"http://www.virtualbox.org/ovf/machine"_s);

    // References
    xml.writeStartElement(u"References"_s);
    xml.writeStartElement(u"File"_s);
    xml.writeAttribute(u"ovf:href"_s, m_vmdkFileName);
    xml.writeAttribute(u"ovf:id"_s, u"file1"_s);
    xml.writeAttribute(u"ovf:size"_s, QString::number(vmdkSize));
    xml.writeEndElement(); // File
    xml.writeEndElement(); // References

    // DiskSection
    xml.writeStartElement(u"DiskSection"_s);
    xml.writeStartElement(u"Info"_s);
    xml.writeCharacters(u"Virtual disk information"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"Disk"_s);
    xml.writeAttribute(u"ovf:capacity"_s, QString::number(static_cast<qint64>(m_config.diskSizeGb()) * 1073741824LL));
    xml.writeAttribute(u"ovf:diskId"_s, u"vmdisk1"_s);
    xml.writeAttribute(u"ovf:fileRef"_s, u"file1"_s);
    xml.writeAttribute(u"ovf:format"_s, u"http://www.vmware.com/interfaces/specifications/vmdk.html#streamOptimized"_s);
    xml.writeEndElement(); // Disk
    xml.writeEndElement(); // DiskSection

    // NetworkSection
    xml.writeStartElement(u"NetworkSection"_s);
    xml.writeStartElement(u"Info"_s);
    xml.writeCharacters(u"Network information"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"Network"_s);
    xml.writeAttribute(u"ovf:name"_s, u"NAT"_s);
    xml.writeEndElement();
    xml.writeEndElement(); // NetworkSection

    // VirtualSystem
    xml.writeStartElement(u"VirtualSystem"_s);
    xml.writeAttribute(u"ovf:id"_s, m_vmName);

    xml.writeStartElement(u"Info"_s);
    xml.writeCharacters(m_config.comment().isEmpty() ? i18n("Exported from KEMU") : m_config.comment());
    xml.writeEndElement();

    xml.writeStartElement(u"Name"_s);
    xml.writeCharacters(m_vmName);
    xml.writeEndElement();

    // VirtualHardwareSection
    xml.writeStartElement(u"VirtualHardwareSection"_s);
    xml.writeStartElement(u"Info"_s);
    xml.writeCharacters(u"Hardware configuration"_s);
    xml.writeEndElement();

    // System
    xml.writeStartElement(u"System"_s);
    xml.writeStartElement(u"vssd:ElementName"_s);
    xml.writeCharacters(u"Virtual Hardware Family"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"vssd:InstanceID"_s);
    xml.writeCharacters(u"0"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"vssd:VirtualSystemType"_s);
    xml.writeCharacters(u"vmx-13"_s);
    xml.writeEndElement();
    xml.writeEndElement(); // System

    // CPU (ResourceType 3)
    xml.writeStartElement(u"Item"_s);
    xml.writeStartElement(u"rasd:Description"_s);
    xml.writeCharacters(u"Number of virtual CPUs"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ElementName"_s);
    xml.writeCharacters(i18n("%1 virtual CPU(s)", m_config.cpuCoreCount()));
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:InstanceID"_s);
    xml.writeCharacters(u"1"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ResourceType"_s);
    xml.writeCharacters(u"3"_s); // Processor
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:VirtualQuantity"_s);
    xml.writeCharacters(QString::number(m_config.cpuCoreCount()));
    xml.writeEndElement();
    xml.writeEndElement(); // Item

    // Memory (ResourceType 4)
    xml.writeStartElement(u"Item"_s);
    xml.writeStartElement(u"rasd:Description"_s);
    xml.writeCharacters(u"Memory Size"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ElementName"_s);
    xml.writeCharacters(i18n("%1 MB of memory", m_config.ramMb()));
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:InstanceID"_s);
    xml.writeCharacters(u"2"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ResourceType"_s);
    xml.writeCharacters(u"4"_s); // Memory
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:VirtualQuantity"_s);
    xml.writeCharacters(QString::number(m_config.ramMb()));
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:AllocationUnits"_s);
    xml.writeCharacters(u"MegaBytes"_s);
    xml.writeEndElement();
    xml.writeEndElement(); // Item

    // SATA Controller (ResourceType 20)
    xml.writeStartElement(u"Item"_s);
    xml.writeStartElement(u"rasd:Description"_s);
    xml.writeCharacters(u"SATA Controller"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ElementName"_s);
    xml.writeCharacters(u"SATA Controller"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:InstanceID"_s);
    xml.writeCharacters(u"3"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ResourceType"_s);
    xml.writeCharacters(u"20"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ResourceSubType"_s);
    xml.writeCharacters(u"AHCI"_s);
    xml.writeEndElement();
    xml.writeEndElement(); // Item

    // Hard Disk (ResourceType 17)
    xml.writeStartElement(u"Item"_s);
    xml.writeStartElement(u"rasd:Description"_s);
    xml.writeCharacters(u"Hard disk image"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ElementName"_s);
    xml.writeCharacters(u"disk1"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:HostResource"_s);
    xml.writeCharacters(u"ovf:/disk/vmdisk1"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:InstanceID"_s);
    xml.writeCharacters(u"4"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:Parent"_s);
    xml.writeCharacters(u"3"_s);
    xml.writeEndElement();
    xml.writeStartElement(u"rasd:ResourceType"_s);
    xml.writeCharacters(u"17"_s); // Disk Drive
    xml.writeEndElement();
    xml.writeEndElement(); // Item

    // Network (ResourceType 10)
    if (m_config.networkCard() != VMSpecs::NetworkCard::None) {
        xml.writeStartElement(u"Item"_s);
        xml.writeStartElement(u"rasd:Connection"_s);
        xml.writeCharacters(u"NAT"_s);
        xml.writeEndElement();
        xml.writeStartElement(u"rasd:ElementName"_s);
        xml.writeCharacters(u"Network Adapter 1"_s);
        xml.writeEndElement();
        xml.writeStartElement(u"rasd:InstanceID"_s);
        xml.writeCharacters(u"5"_s);
        xml.writeEndElement();
        xml.writeStartElement(u"rasd:ResourceType"_s);
        xml.writeCharacters(u"10"_s);
        xml.writeEndElement();
        if (!m_config.macAddress().isEmpty()) {
            QString cleanMac = m_config.macAddress();
            cleanMac.remove(u':');
            xml.writeStartElement(u"rasd:MACAddress"_s);
            xml.writeCharacters(cleanMac);
            xml.writeEndElement();
        }
        xml.writeEndElement(); // Item
    }

    xml.writeEndElement(); // VirtualHardwareSection
    xml.writeEndElement(); // VirtualSystem
    xml.writeEndElement(); // Envelope
    xml.writeEndDocument();
}

bool OvaExportJob::createManifestInThread()
{
    const QString mfPath = m_stagingDir.filePath(m_mfFileName);
    QStringList files = {m_ovfFileName, m_vmdkFileName};
    QStringList lines;

    qint64 totalBytes = 0;
    for (const QString &fileName : std::as_const(files)) {
        totalBytes += QFileInfo(m_stagingDir.filePath(fileName)).size();
    }

    qint64 processedBytes = 0;
    char buffer[1024 * 1024]; // 1 MB Chunk-Buffer

    for (const QString &fileName : std::as_const(files)) {
        if (m_cancelExport)
            return false;

        QFile f(m_stagingDir.filePath(fileName));
        if (!f.open(QIODevice::ReadOnly))
            return false;

        QCryptographicHash hash(QCryptographicHash::Sha256);

        while (!f.atEnd()) {
            if (m_cancelExport)
                return false;

            const qint64 readBytes = f.read(buffer, sizeof(buffer));
            if (readBytes > 0) {
                hash.addData(QByteArrayView(buffer, readBytes));
                processedBytes += readBytes;

                if (totalBytes > 0) {
                    const int hashPercent = static_cast<int>((processedBytes * 50) / totalBytes);
                    const int totalPercent = 850 + hashPercent;

                    QMetaObject::invokeMethod(
                        this,
                        [this, totalPercent]() {
                            emitPercent(totalPercent, 1000);
                        },
                        Qt::QueuedConnection);
                }
            }
        }

        lines << u"SHA256(%1)= %2"_s.arg(fileName, QString::fromLatin1(hash.result().toHex()));
    }

    if (m_cancelExport)
        return false;

    QFile mfFile(mfPath);
    if (!mfFile.open(QIODevice::WriteOnly))
        return false;

    mfFile.write(lines.join(u'\n').toUtf8());
    mfFile.write("\n");

    return true;
}

void OvaExportJob::packageOva()
{
    // Setup threaded execution for packaging
    m_cancelExport = false;
    m_watcher = new QFutureWatcher<bool>(this);
    connect(m_watcher, &QFutureWatcher<bool>::finished, this, &OvaExportJob::onPackageFinished);

    // Start packaging in a background thread to prevent UI freezing
    const QFuture<bool> future = QtConcurrent::run(&OvaExportJob::performOvaPackagingInThread, this);
    m_watcher->setFuture(future);
}

bool OvaExportJob::performOvaPackagingInThread()
{
    struct archive *a = archive_write_new();
    archive_write_set_format_ustar(a);

    if (archive_write_open_filename(a, m_outputFilePath.toUtf8().constData()) != ARCHIVE_OK) {
        archive_write_free(a);
        return false;
    }

    // Important for the OVA standard: The order must be correct!
    // First OVF, then manifest (MF), then the VMDK(s).
    QStringList filesToPack = {m_ovfFileName, m_mfFileName, m_vmdkFileName};

    qint64 totalBytes = 0;
    for (const QString &fileName : std::as_const(filesToPack)) {
        totalBytes += QFileInfo(m_stagingDir.filePath(fileName)).size();
    }

    qint64 processedBytes = 0;
    char buffer[1024 * 1024];
    bool success = true;

    for (const QString &fileName : std::as_const(filesToPack)) {
        if (m_cancelExport) {
            success = false;
            break;
        }

        const QString fullPath = m_stagingDir.filePath(fileName);
        QFile file(fullPath);
        if (!file.open(QIODevice::ReadOnly)) {
            success = false;
            break;
        }

        struct archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, fileName.toUtf8().constData());
        archive_entry_set_size(entry, file.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);

        if (archive_write_header(a, entry) != ARCHIVE_OK) {
            archive_entry_free(entry);
            success = false;
            break;
        }

        while (!file.atEnd()) {
            if (m_cancelExport) {
                success = false;
                break;
            }

            const qint64 readBytes = file.read(buffer, sizeof(buffer));
            if (readBytes > 0) {
                const la_ssize_t written = archive_write_data(a, buffer, readBytes);
                if (written < 0) {
                    // An error occurred during writing (e.g., disk is full)
                    // We record the error and break out of the loops.
                    success = false;
                    break;
                }

                processedBytes += readBytes;

                if (totalBytes > 0) {
                    // Fortschritt für das Packen liegt zwischen 900 und 1000
                    const int packagePercent = static_cast<int>((processedBytes * 100) / totalBytes);
                    const int totalPercent = 900 + packagePercent;

                    QMetaObject::invokeMethod(
                        this,
                        [this, totalPercent]() {
                            emitPercent(totalPercent, 1000);
                        },
                        Qt::QueuedConnection);
                }
            }
        }

        archive_entry_free(entry);
        if (!success) {
            break;
        }
    }

    archive_write_close(a);
    archive_write_free(a);

    return success;
}

void OvaExportJob::onPackageFinished()
{
    // Retrieve the boolean result from performBackgroundTasks()
    const bool success = m_watcher->result();
    m_watcher->deleteLater();
    m_watcher = nullptr;

    // Case 1: The job was intentionally canceled by the user (via doKill)
    if (m_cancelExport) {
        QFile::remove(m_outputFilePath); // Clean up partial file
        setError(KJob::KilledJobError);
        setErrorText(i18n("Export was canceled."));
        emitResult();
        return;
    }

    // Case 2: A real error occurred (e.g., disk full, file not readable)
    if (!success) {
        QFile::remove(m_outputFilePath); // Clean up here as well!
        setError(5);
        setErrorText(i18n("Failed to create OVA archive. Please check disk space."));
        emitResult();
        return;
    }

    // Case 3: Everything completed successfully!
    emitPercent(1000, 1000);
    Q_EMIT infoMessage(this, i18n("OVA Export completed successfully."));
    emitResult();
}

void OvaExportJob::cleanup()
{
    // QTemporaryDir will clean up
}

bool OvaExportJob::doKill()
{
    m_cancelExport = true;

    // Process phase: stop reacting to qemu-img *before* killing it, so
    // onConvertFinished() cannot emit a second result.
    if (m_process && m_process->state() != QProcess::NotRunning) {
        disconnect(m_process, nullptr, this, nullptr);
        m_process->kill();
    }

    // Thread phase: let the worker unwind; onPackageFinished() (which checks
    // m_cancelExport) removes the partial file and emits the result.
    if (m_watcher && m_watcher->isRunning())
        return false;

    // Otherwise we are still in the convert phase: clean up and let KJob emit
    // the single KilledJobError result.
    QFile::remove(m_outputFilePath);
    return true;
}
