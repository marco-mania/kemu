/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskimportjob.h"

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>

#include <KFormat>
#include <KLocalizedString>

#include "utils/diskimagecheck.h"
#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"

#include <utility>

DiskImportJob::DiskImportJob(const QString &vmName, const QDir &vmDir, const QList<Item> &items, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_vmDir(vmDir)
    , m_items(items)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

DiskImportJob::~DiskImportJob()
{
    // Closing KEMU mid-conversion must not leave a half-written image behind.
    if (m_process) {
        m_process->abort();
        QFile::remove(targetPath());
    }
}

void DiskImportJob::start()
{
    if (!ExternalTools::instance().isAvailable(ExternalTools::QemuImg)) {
        setError(UserDefinedError);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    // The image may come from anywhere. Files it refers to (backing files,
    // VMDK extents) must sit next to it, or a crafted image could copy
    // arbitrary host files into the VM.
    m_formats.clear();
    for (const Item &item : std::as_const(m_items)) {
        const DiskImageCheck::Inspection inspection =
            DiskImageCheck::inspect(ExternalTools::instance().path(ExternalTools::QemuImg), item.sourcePath, QFileInfo(item.sourcePath).absolutePath(), true);
        if (!inspection.problem.isEmpty()) {
            setError(UserDefinedError);
            setErrorText(i18n("The disk image cannot be imported: %1", inspection.problem));
            emitResult();
            return;
        }
        m_formats << inspection.format;
    }

    qint64 required = 0;
    for (const Item &item : std::as_const(m_items)) {
        const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(item.sourcePath);
        required += info.isValid ? info.actualSizeBytes : QFileInfo(item.sourcePath).size();
    }

    const QStorageInfo storage(m_vmDir.absolutePath());
    if (storage.isValid() && storage.isReady() && storage.bytesAvailable() < required) {
        const KFormat format;
        setError(UserDefinedError);
        setErrorText(
            i18n("Not enough free disk space.\nRequired: %1\nAvailable: %2", format.formatByteSize(required), format.formatByteSize(storage.bytesAvailable())));
        emitResult();
        return;
    }

    if (m_items.isEmpty()) {
        emitResult();
        return;
    }

    startNext();
}

QString DiskImportJob::targetPath() const
{
    return m_vmDir.absoluteFilePath(m_items.at(m_index).disk.fileName);
}

void DiskImportJob::startNext()
{
    const Item &item = m_items.at(m_index);

    Q_EMIT description(this,
                       i18n("Importing disk image"),
                       QPair<QString, QString>(i18n("VM"), m_vmName),
                       QPair<QString, QString>(i18n("Image"), QFileInfo(item.sourcePath).fileName()));

    // Never overwrite: the name was free when the dialog handed it out.
    if (QFile::exists(targetPath())) {
        setError(UserDefinedError);
        setErrorText(i18n("%1 already exists in the VM folder.", item.disk.fileName));
        emitResult();
        return;
    }

    m_process = new QemuImgProcess(this);

    connect(m_process, &QemuImgProcess::progress, this, [this](double percent) {
        const int perDisk = 1000 / m_items.size();
        emitPercent(m_index * perDisk + qRound(percent * perDisk / 100.0), 1000);
    });
    connect(m_process, &QemuImgProcess::finished, this, &DiskImportJob::onConvertFinished);

    if (!m_process->start({u"convert"_s, u"-p"_s, u"-f"_s, m_formats.at(m_index), u"-O"_s, u"qcow2"_s, item.sourcePath, targetPath()})) {
        setError(UserDefinedError);
        setErrorText(m_process->errorText());
        emitResult();
    }
}

void DiskImportJob::onConvertFinished(bool ok)
{
    QemuImgProcess *process = std::exchange(m_process, nullptr);
    process->deleteLater();

    if (!ok) {
        QFile::remove(targetPath());
        setError(UserDefinedError);
        setErrorText(i18n("Could not import %1:\n%2", QFileInfo(m_items.at(m_index).sourcePath).fileName(), process->errorText()));
        emitResult();
        return;
    }

    m_imported << m_items.at(m_index).disk;

    if (++m_index < m_items.size()) {
        startNext();
        return;
    }

    emitPercent(1000, 1000);
    emitResult();
}

bool DiskImportJob::doKill()
{
    if (m_process) {
        m_process->abort();
        QFile::remove(targetPath());
    }
    return true;
}
