/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shrinkjob.h"

#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"
#include "utils/snapshotmanager.h"

#include <utility>

ShrinkJob::ShrinkJob(const QString &vmName, const QStringList &diskPaths, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_diskPaths(diskPaths)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void ShrinkJob::start()
{
    if (!ExternalTools::instance().isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    if (m_diskPaths.isEmpty()) {
        emitResult();
        return;
    }

    startNext();
}

void ShrinkJob::startNext()
{
    const QString diskPath = m_diskPaths.at(m_index);
    m_tmpPath = diskPath + u".shrink.tmp"_s;
    QFile::remove(m_tmpPath);

    Q_EMIT description(this,
                       m_flatten ? i18n("Making disk image independent") : i18n("Compacting disk image"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Disk"), QFileInfo(diskPath).fileName()));

    m_process = new QemuImgProcess(this);

    connect(m_process, &QemuImgProcess::progress, this, [this](double percent) {
        const int perDisk = 1000 / m_diskPaths.size();
        emitPercent(m_index * perDisk + qRound(percent * perDisk / 100.0), 1000);
    });
    connect(m_process, &QemuImgProcess::finished, this, &ShrinkJob::onConvertFinished);

    QStringList args = {u"convert"_s, u"-p"_s, u"-f"_s, u"qcow2"_s, u"-O"_s, u"qcow2"_s};
    if (!m_flatten) {
        // An overlay stays an overlay; -B is resolved relative to the output.
        const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(diskPath);
        if (!info.isValid) {
            setError(1);
            setErrorText(i18n("Could not read the disk image %1.", QFileInfo(diskPath).fileName()));
            emitResult();
            return;
        }
        if (!info.backingFile.isEmpty())
            args << u"-B"_s << info.backingFile << u"-F"_s << (info.backingFormat.isEmpty() ? u"qcow2"_s : info.backingFormat);
    }
    args << diskPath << m_tmpPath;

    if (!m_process->start(args)) {
        setError(1);
        setErrorText(m_process->errorText());
        emitResult();
    }
}

void ShrinkJob::onConvertFinished(bool ok)
{
    const QString diskPath = m_diskPaths.at(m_index);
    QemuImgProcess *process = std::exchange(m_process, nullptr);
    process->deleteLater();

    if (!ok) {
        QFile::remove(m_tmpPath);
        setError(2);
        setErrorText(process->errorText());
        emitResult();
        return;
    }

    const QString backupPath = diskPath + u".old"_s;
    QFile::remove(backupPath);

    if (!QFile::rename(diskPath, backupPath)) {
        QFile::remove(m_tmpPath);
        setError(3);
        setErrorText(i18n("Failed to rename original disk to backup"));
        emitResult();
        return;
    }

    if (!QFile::rename(m_tmpPath, diskPath)) {
        QFile::rename(backupPath, diskPath);
        setError(4);
        setErrorText(i18n("Failed to rename shrunk disk to original location"));
        emitResult();
        return;
    }

    QFile::remove(backupPath);
    m_tmpPath.clear();
    // The rewritten image has no snapshots left to describe.
    QFile::remove(SnapshotManager::descriptionFilePath(diskPath));

    if (++m_index < m_diskPaths.size()) {
        startNext();
        return;
    }

    emitPercent(1000, 1000);
    emitResult();
}

bool ShrinkJob::doKill()
{
    if (m_process)
        m_process->abort();
    if (!m_tmpPath.isEmpty())
        QFile::remove(m_tmpPath);
    return true;
}
