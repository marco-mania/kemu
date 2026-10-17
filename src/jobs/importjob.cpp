/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "importjob.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

#include <KLocalizedString>

#include "const.h"
#include "kemu_debug.h"
#include "utils/archivepaths.h"
#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

namespace
{
constexpr qint64 kMaxConfigBytes = 1 << 20;
constexpr int kMaxHeadersToScan = 4;
constexpr qint64 kSpaceHeadroom = 50LL << 20;
}

ImportJob::ImportJob(const QString &archivePath, const QString &vmBaseDir, QObject *parent)
    : ArchiveStreamJob(parent)
    , m_archivePath(archivePath)
    , m_vmBaseDir(vmBaseDir)
{
}

void ImportJob::start()
{
    QByteArray configData;
    if (!readConfigFromArchive(&configData) || !parseConfig(configData)) {
        setError(ContentError);
        setErrorText(i18n("Could not determine VM name from archive. The archive may be invalid."));
        emitResult();
        return;
    }

    m_targetDir = ArchivePaths::resolveWithin(QDir(m_vmBaseDir), m_vmName);
    if (m_targetDir.isEmpty()) {
        setError(ContentError);
        setErrorText(i18n("The archive contains an invalid VM name."));
        emitResult();
        return;
    }

    if (QDir(m_targetDir).exists()) {
        setError(TargetExistsError);
        setErrorText(i18n("A VM named '%1' already exists. Please rename or delete it first.", m_vmName));
        emitResult();
        return;
    }

    // The uncompressed payload is unknown up front; the archive size is a floor.
    QString spaceMessage;
    const qint64 archiveSize = QFileInfo(m_archivePath).size();
    if (!hasFreeSpace(m_vmBaseDir, archiveSize + kSpaceHeadroom, &spaceMessage)) {
        setError(DiskSpaceError);
        setErrorText(spaceMessage);
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       i18n("Importing virtual machine"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("File"), QFileInfo(m_archivePath).fileName()));

    beginPhase(0, 1000, archiveSize);
    startWorker();
}

bool ImportJob::readConfigFromArchive(QByteArray *out)
{
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, QFile::encodeName(m_archivePath).constData(), 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }

    bool found = false;
    archive_entry *entry = nullptr;

    // config.json is written first, so a short scan is enough. Bailing out
    // early avoids decompressing a multi-GB stream on the main thread.
    for (int scanned = 0; scanned < kMaxHeadersToScan; ++scanned) {
        if (nextEntry(a, &entry) != EntryResult::Ready)
            break;

        if (QString::fromUtf8(archive_entry_pathname(entry)) != KEMUConst::VMConfigFileName)
            continue;

        const qint64 size = archive_entry_size(entry);
        if (size <= 0 || size > kMaxConfigBytes)
            break;

        out->resize(size);
        found = archive_read_data(a, out->data(), static_cast<size_t>(size)) == size;
        break;
    }

    archive_read_close(a);
    archive_read_free(a);

    return found;
}

bool ImportJob::parseConfig(const QByteArray &data)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        qCWarning(KEMU) << "Could not parse the config file:" << parseError.errorString();
        return false;
    }

    const QJsonObject obj = doc.object();

    if (obj.value(u"application_name"_s).toString() != QCoreApplication::applicationName())
        return false;
    if (!obj.contains(u"application_version"_s))
        return false;

    m_vmName = ArchivePaths::sanitizeVmName(obj.value(u"name"_s).toString());

    for (const DiskConfig &disk : VMConfig::fromJson(obj).extraDisks())
        m_extraDiskFileNames << disk.fileName;

    return !m_vmName.isEmpty();
}

bool ImportJob::isExpectedEntry(const QString &name) const
{
    static const QSet<QString> allowed = {KEMUConst::VMConfigFileName,
                                          KEMUConst::VMDiskFileName,
                                          KEMUConst::OvmfVarsFileName,
                                          u"tpm2-00.permall"_s,
                                          VMConfig::customIconFileName()};
    return allowed.contains(name) || m_extraDiskFileNames.contains(name);
}

bool ImportJob::runWorker()
{
    archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, QFile::encodeName(m_archivePath).constData(), 10240) != ARCHIVE_OK) {
        recordArchiveError(a, i18n("while opening %1", m_archivePath));
        archive_read_free(a);
        return false;
    }

    const QDir targetDir(m_targetDir);
    QDir().mkpath(m_targetDir);

    postInfoMessage(i18n("Extracting archive..."));

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
        if (!isExpectedEntry(name))
            continue;

        const QString destPath = ArchivePaths::resolveWithin(targetDir, name);
        if (destPath.isEmpty())
            continue;

        if (!extractEntryTo(a, destPath)) {
            ok = false;
            break;
        }
    }

    archive_read_close(a);
    archive_read_free(a);

    if (ok && !QFile::exists(targetDir.filePath(KEMUConst::VMDiskFileName))) {
        recordError(i18n("The archive does not contain a disk image."));
        ok = false;
    }

    return ok;
}

void ImportJob::workerFinished(bool ok)
{
    if (!ok) {
        QDir(m_targetDir).removeRecursively();
        setError(ArchiveError);
        setErrorText(workerErrorText().isEmpty() ? i18n("Archive extraction failed.") : workerErrorText());
        emitResult();
        return;
    }

    emitPercent(1000, 1000);
    emitResult();
}

void ImportJob::discardPartialOutput()
{
    if (!m_targetDir.isEmpty())
        QDir(m_targetDir).removeRecursively();
}
