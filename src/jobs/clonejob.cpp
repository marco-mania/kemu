/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clonejob.h"
#include "const.h"
#include "utils/linkedclones.h"
#include "utils/qemuimgprocess.h"

#include <utility>

CloneJob::CloneJob(const QString &vmName, const QString &srcPath, const QString &dstPath, const QStringList &diskFileNames, QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_srcPath(srcPath)
    , m_dstPath(dstPath)
    , m_diskFileNames(diskFileNames)
{
    m_cloneName = QDir(dstPath).dirName();
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

void CloneJob::start()
{
    const auto &tools = ExternalTools::instance();

    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        setError(1);
        setErrorText(i18n("qemu-img not found. Please check Settings."));
        emitResult();
        return;
    }

    Q_EMIT description(this,
                       m_linked ? i18n("Creating linked clone") : i18n("Cloning virtual machine"),
                       QPair<QString, QString>(i18n("Source"), m_vmName),
                       QPair<QString, QString>(i18n("Clone"), m_cloneName));

    QDir().mkpath(m_dstPath);

    if (!copyMetadataFiles()) {
        QDir(m_dstPath).removeRecursively();
        setError(2);
        setErrorText(i18n("Failed to copy VM metadata files."));
        emitResult();
        return;
    }

    for (const QString &fileName : std::as_const(m_diskFileNames)) {
        if (!QFile::exists(m_srcPath + u"/"_s + fileName)) {
            QDir(m_dstPath).removeRecursively();
            setError(3);
            setErrorText(i18n("Source disk image not found: %1", fileName));
            emitResult();
            return;
        }
    }

    startNextDisk();
}

void CloneJob::startNextDisk()
{
    const QString fileName = m_diskFileNames.at(m_index);

    m_process = new QemuImgProcess(this);

    connect(m_process, &QemuImgProcess::progress, this, [this](double percent) {
        const int perDisk = 1000 / m_diskFileNames.size();
        emitPercent(m_index * perDisk + qRound(percent * perDisk / 100.0), 1000);
    });
    connect(m_process, &QemuImgProcess::finished, this, &CloneJob::onConvertFinished);

    const QString target = m_dstPath + u"/"_s + fileName;
    QStringList args;
    if (m_linked) {
        const QString backing = LinkedClones::backingReference(QDir(m_srcPath).dirName(), fileName);
        args = {u"create"_s, u"-q"_s, u"-f"_s, u"qcow2"_s, u"-b"_s, backing, u"-F"_s, u"qcow2"_s, target};
    } else {
        args = {u"convert"_s, u"-p"_s, u"-O"_s, u"qcow2"_s, m_srcPath + u"/"_s + fileName, target};
    }

    if (!m_process->start(args)) {
        QDir(m_dstPath).removeRecursively();
        setError(4);
        setErrorText(m_process->errorText());
        emitResult();
    }
}

bool CloneJob::copyMetadataFiles()
{
    const QDir srcDir(m_srcPath);
    const QStringList entries = srcDir.entryList(QDir::Files);

    for (const QString &file : entries) {
        if (m_diskFileNames.contains(file))
            continue;
        if (file.endsWith(u".sock"_s) || file.endsWith(u".pid"_s) || file == KEMUConst::DeploySerialLogFileName)
            continue;
        // Fresh overlays carry no snapshots, so their descriptions stay behind.
        if (m_linked && file.endsWith(u".snapshots.json"_s))
            continue;

        if (!QFile::copy(m_srcPath + u"/"_s + file, m_dstPath + u"/"_s + file))
            return false;
    }
    return true;
}

void CloneJob::onConvertFinished(bool ok)
{
    QemuImgProcess *process = std::exchange(m_process, nullptr);
    process->deleteLater();

    if (m_killed)
        return;

    if (!ok) {
        QDir(m_dstPath).removeRecursively();
        setError(5);
        setErrorText(process->errorText());
        emitResult();
        return;
    }

    if (++m_index < m_diskFileNames.size()) {
        startNextDisk();
        return;
    }

    emitPercent(1000, 1000);
    emitResult();
}

bool CloneJob::doKill()
{
    m_killed = true;

    if (m_process)
        m_process->abort();

    QDir(m_dstPath).removeRecursively();
    return true;
}
