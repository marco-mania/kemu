/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "expanddiskjob.h"

#include <utility>

#include <unistd.h>

#include <QFileInfo>

#include <KLocalizedString>

#include "utils/diskencryption.h"
#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"

using namespace Qt::StringLiterals;

ExpandDiskJob::ExpandDiskJob(const QString &vmName, const QString &diskPath, qint64 newSizeBytes, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_diskPath(diskPath)
    , m_newSizeBytes(newSizeBytes)
{
}

ExpandDiskJob::~ExpandDiskJob()
{
    DiskEncryption::wipe(m_key);
}

void ExpandDiskJob::start()
{
    Q_EMIT description(this,
                       i18n("Expanding disk image"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Disk"), QFileInfo(m_diskPath).fileName()));

    if (!ExternalTools::instance().isAvailable(ExternalTools::QemuImg)) {
        setError(UserDefinedError);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    // Growing only: qemu-img refuses to shrink without --shrink anyway.
    const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(m_diskPath);
    if (!info.isValid || m_newSizeBytes <= info.virtualSizeBytes) {
        setError(UserDefinedError);
        setErrorText(info.isValid ? i18n("The disk image is already this large.")
                                  : i18n("Could not read the disk image %1.", QFileInfo(m_diskPath).fileName()));
        emitResult();
        return;
    }

    m_process = new QemuImgProcess(this);
    connect(m_process, &QemuImgProcess::finished, this, &ExpandDiskJob::onFinished);

    QStringList args{u"resize"_s};
    QList<int> keyFds;
    if (!m_key.isEmpty()) {
        const int keyFd = DiskEncryption::secretFd(m_key);
        if (keyFd < 0) {
            setError(UserDefinedError);
            setErrorText(i18n("The password could not be passed to qemu-img."));
            emitResult();
            return;
        }
        keyFds << keyFd;
        args << u"--object"_s << DiskEncryption::secretObject(keyFd) << u"--image-opts"_s << DiskEncryption::imageOptions(m_diskPath);
    } else {
        args << u"-f"_s << u"qcow2"_s << m_diskPath;
    }
    args << QString::number(m_newSizeBytes);

    const bool started = m_process->start(args, keyFds);
    for (const int fd : std::as_const(keyFds))
        ::close(fd);
    if (!started)
        onFinished(false);
}

void ExpandDiskJob::onFinished(bool ok)
{
    QemuImgProcess *process = std::exchange(m_process, nullptr);
    if (!ok) {
        setError(UserDefinedError);
        // Keeps qemu-img's wording, so a wrong password can be recognized.
        setErrorText(process ? process->errorText() : i18n("qemu-img could not be started."));
    }
    if (process)
        process->deleteLater();
    emitResult();
}
