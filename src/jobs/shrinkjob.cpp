/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "shrinkjob.h"

#include "utils/diskencryption.h"
#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"
#include "utils/snapshotmanager.h"

#include <utility>

#include <unistd.h>

ShrinkJob::ShrinkJob(const QString &vmName, const QStringList &diskPaths, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_diskPaths(diskPaths)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

ShrinkJob::~ShrinkJob()
{
    for (QByteArray &key : m_diskKeys)
        DiskEncryption::wipe(key);
}

void ShrinkJob::setDiskKey(const QString &diskPath, const QByteArray &key)
{
    m_diskKeys.insert(diskPath, key);
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

    // An encrypted image is read with its key and written encrypted again,
    // with the same password.
    if (m_diskKeys.contains(diskPath)) {
        const int keyFd = DiskEncryption::secretFd(m_diskKeys.value(diskPath));
        if (keyFd < 0) {
            setError(1);
            setErrorText(i18n("The password could not be passed to qemu-img."));
            emitResult();
            return;
        }
        const QStringList args{u"convert"_s,
                               u"-p"_s,
                               u"--object"_s,
                               DiskEncryption::secretObject(keyFd),
                               u"--image-opts"_s,
                               DiskEncryption::imageOptions(diskPath),
                               u"-O"_s,
                               u"qcow2"_s,
                               u"-o"_s,
                               DiskEncryption::createOptions(),
                               m_tmpPath};
        const bool started = m_process->start(args, {keyFd});
        ::close(keyFd);
        if (!started) {
            setError(1);
            setErrorText(m_process->errorText());
            emitResult();
        }
        return;
    }

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
        // Keeps qemu-img's wording, so a wrong password can be recognized.
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

    deleteNextSnapshot();
}

void ShrinkJob::deleteNextSnapshot()
{
    if (m_snapshotNames.isEmpty() || m_otherDisks.isEmpty()) {
        emitPercent(1000, 1000);
        emitResult();
        return;
    }

    if (!m_snapshots) {
        m_snapshots = new SnapshotManager(this);
        for (auto it = m_diskKeys.cbegin(); it != m_diskKeys.cend(); ++it)
            m_snapshots->setDiskKey(it.key(), it.value());
    }

    const QString name = m_snapshotNames.takeFirst();
    m_snapshots->deleteOfflineSnapshot(m_otherDisks, name, [this, name](bool ok, const QString &error) {
        if (!ok) {
            m_warnings << i18n("The snapshot '%1' could not be deleted from the other disks: %2", name, error);
        } else {
            // The system disk may be among them, with the descriptions.
            for (const QString &disk : std::as_const(m_otherDisks))
                m_snapshots->removeDescription(disk, name);
        }
        deleteNextSnapshot();
    });
}

bool ShrinkJob::doKill()
{
    if (m_process)
        m_process->abort();
    if (!m_tmpPath.isEmpty())
        QFile::remove(m_tmpPath);
    return true;
}
