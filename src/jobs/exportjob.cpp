/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "exportjob.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <KLocalizedString>

#include "const.h"
#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

namespace
{

// config.json is written separately and must remain the first archive entry.
QStringList payloadFileNames()
{
    return {KEMUConst::VMDiskFileName, KEMUConst::OvmfVarsFileName, u"tpm2-00.permall"_s};
}

constexpr qint64 kSpaceHeadroom = 50LL << 20;

}

ExportJob::ExportJob(const QString &vmName, const QString &vmPath, const QString &outputFilePath, QObject *parent)
    : ArchiveStreamJob(parent)
    , m_vmName(vmName)
    , m_vmPath(vmPath)
    , m_outputFilePath(outputFilePath)
{
}

void ExportJob::start()
{
    m_tmpOutputPath = m_outputFilePath + u".tmp"_s;
    QFile::remove(m_tmpOutputPath);

    Q_EMIT description(this,
                       i18n("Exporting virtual machine"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("File"), QFileInfo(m_outputFilePath).fileName()));

    m_configData = buildPortableConfig();
    if (m_configData.isEmpty()) {
        setError(SourceError);
        setErrorText(i18n("Could not read the configuration of VM '%1'.", m_vmName));
        emitResult();
        return;
    }

    const QDir vmDir(m_vmPath);

    if (!vmDir.exists(KEMUConst::VMDiskFileName)) {
        setError(SourceError);
        setErrorText(i18n("The disk image of VM '%1' is missing.", m_vmName));
        emitResult();
        return;
    }

    m_filesToPack.clear();
    m_totalBytesToPack = m_configData.size();

    for (const QString &fileName : payloadFileNames()) {
        if (vmDir.exists(fileName)) {
            m_filesToPack << fileName;
            m_totalBytesToPack += QFileInfo(vmDir, fileName).size();
        }
    }

    for (const DiskConfig &disk : VMConfig::fromJson(QJsonDocument::fromJson(m_configData).object()).extraDisks()) {
        if (!vmDir.exists(disk.fileName)) {
            setError(SourceError);
            setErrorText(i18n("The disk image %1 of VM '%2' is missing.", disk.fileName, m_vmName));
            emitResult();
            return;
        }
        m_filesToPack << disk.fileName;
        m_totalBytesToPack += QFileInfo(vmDir, disk.fileName).size();
    }

    if (!m_customIconFileName.isEmpty()) {
        m_filesToPack << m_customIconFileName;
        m_totalBytesToPack += QFileInfo(vmDir, m_customIconFileName).size();
    }

    QString spaceMessage;
    if (!hasFreeSpace(QFileInfo(m_tmpOutputPath).absolutePath(), m_totalBytesToPack + kSpaceHeadroom, &spaceMessage)) {
        setError(DiskSpaceError);
        setErrorText(spaceMessage);
        emitResult();
        return;
    }

    beginPhase(0, 1000, m_totalBytesToPack);
    startWorker();
}

QByteArray ExportJob::buildPortableConfig()
{
    QFile file(m_vmPath + u"/"_s + KEMUConst::VMConfigFileName);
    if (!file.open(QIODevice::ReadOnly))
        return {};

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return {};

    QJsonObject obj = doc.object();

    // Host-specific values must not travel with the archive.
    static const QStringList hostSpecific = {u"iso"_s, u"floppy"_s, u"shared_path"_s, u"virtual_network"_s, u"extra_args"_s};
    for (const QString &key : hostSpecific) {
        if (obj.contains(key))
            obj[key] = QString();
    }

    obj[u"boot_iso"_s] = false;
    obj[u"boot_floppy"_s] = false;
    obj[u"network_mode"_s] = VMConfig::toString(VMSpecs::NetworkMode::UserMode);
    obj[u"deploy_state"_s] = VMConfig::toString(VMConfig::DeployState::Complete);
    obj.remove(u"port_forwards"_s);

    if (obj.value(u"icon_source"_s).toString() == u"custom"_s) {
        QString iconId = obj.value(u"icon_identifier"_s).toString();
        if (iconId.isEmpty())
            iconId = VMConfig::customIconFileName();
        if (QFile::exists(m_vmPath + u"/"_s + iconId))
            m_customIconFileName = iconId;
    }

    return QJsonDocument(obj).toJson(QJsonDocument::Indented);
}

bool ExportJob::runWorker()
{
    archive *a = archive_write_new();
    if (!a) {
        recordError(i18n("Could not initialize libarchive."));
        return false;
    }

    if (archive_write_add_filter_zstd(a) != ARCHIVE_OK || archive_write_set_format_pax_restricted(a) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while configuring the archive"));
        archive_write_free(a);
        return false;
    }

    const QString options = u"zstd:threads=0,zstd:compression-level=%1"_s.arg(KEMUConst::DefaultZSTDCompressionLevelForExport);
    if (archive_write_set_options(a, options.toUtf8().constData()) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while configuring zstd"));
        archive_write_free(a);
        return false;
    }

    if (archive_write_open_filename(a, QFile::encodeName(m_tmpOutputPath).constData()) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while creating %1", m_tmpOutputPath));
        archive_write_free(a);
        return false;
    }

    postInfoMessage(i18n("Packing archive..."));

    bool ok = writeBufferEntry(a, m_configData, KEMUConst::VMConfigFileName);
    reportPhaseBytes(m_configData.size());

    for (const QString &fileName : std::as_const(m_filesToPack)) {
        if (!ok || isCancelled()) {
            ok = false;
            break;
        }
        ok = writeFileEntry(a, m_vmPath + u"/"_s + fileName, fileName);
    }

    return closeWriteArchive(a, ok);
}

void ExportJob::workerFinished(bool ok)
{
    if (!ok) {
        QFile::remove(m_tmpOutputPath);
        setError(ArchiveError);
        setErrorText(workerErrorText().isEmpty() ? i18n("Failed to create archive.") : workerErrorText());
        emitResult();
        return;
    }

    QFile::remove(m_outputFilePath);
    if (!QFile::rename(m_tmpOutputPath, m_outputFilePath)) {
        QFile::remove(m_tmpOutputPath);
        setError(ArchiveError);
        setErrorText(i18n("Failed to move archive to target location."));
        emitResult();
        return;
    }

    emitPercent(1000, 1000);
    emitResult();
}

void ExportJob::discardPartialOutput()
{
    QFile::remove(m_tmpOutputPath);
}
