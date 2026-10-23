/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskpasswordjob.h"

#include <memory>
#include <utility>

#include <unistd.h>

#include <QDir>
#include <QFileInfo>
#include <QThread>

#include <KLocalizedString>

#include "utils/diskencryption.h"
#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"
#include "utils/tpmstate.h"

using namespace Qt::StringLiterals;

namespace
{
const QString kOldId = u"kemu-old-key"_s;
const QString kNewId = u"kemu-new-key"_s;
}

DiskPasswordJob::DiskPasswordJob(const QString &vmName,
                                 const QStringList &imagePaths,
                                 const QByteArray &oldPassword,
                                 const QByteArray &newPassword,
                                 QObject *parent)
    : KJob(parent)
    , m_vmName(vmName)
    , m_imagePaths(imagePaths)
    , m_oldPassword(oldPassword)
    , m_newPassword(newPassword)
{
}

DiskPasswordJob::~DiskPasswordJob()
{
    // QThread must not be destroyed while it runs.
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    for (const int fd : std::as_const(m_fds))
        ::close(fd);
    DiskEncryption::wipe(m_oldPassword);
    DiskEncryption::wipe(m_newPassword);
}

void DiskPasswordJob::start()
{
    Q_EMIT description(this, i18n("Changing the disk password"), QPair<QString, QString>(i18n("VM"), m_vmName));

    if (m_imagePaths.isEmpty() || m_oldPassword.isEmpty() || m_newPassword.isEmpty()) {
        finish(i18n("Nothing to change."));
        return;
    }
    // Removing the old password would then erase the new one as well.
    if (m_oldPassword == m_newPassword) {
        finish(i18n("The new password is the same as the current one."));
        return;
    }
    runNext();
}

void DiskPasswordJob::runNext()
{
    // AddNew walks forward over all images, RollBack backward over those
    // that took the new password.
    if (m_step == Step::RollBack && m_index < 0) {
        finish(m_error);
        return;
    }
    if (m_step != Step::RollBack && m_index >= m_imagePaths.size()) {
        if (m_step == Step::RemoveOld) {
            finish({});
            return;
        }
        // Every disk opens with both passwords now: the moment for the TPM.
        if (!m_tpmDone) {
            rekeyTpm();
            return;
        }
        m_step = Step::RemoveOld;
        m_index = 0;
    }

    const QString path = m_imagePaths.at(m_index);
    emitPercent((m_step == Step::RemoveOld ? m_imagePaths.size() : 0) + m_index, 2 * m_imagePaths.size());

    const int oldFd = DiskEncryption::secretFd(m_oldPassword);
    const int newFd = DiskEncryption::secretFd(m_newPassword);
    m_fds = {oldFd, newFd};
    if (oldFd < 0 || newFd < 0) {
        finish(i18n("The password could not be passed to qemu-img."));
        return;
    }

    QStringList args{u"amend"_s,
                     u"--object"_s,
                     DiskEncryption::secretObject(oldFd, kOldId),
                     u"--object"_s,
                     DiskEncryption::secretObject(newFd, kNewId),
                     u"--image-opts"_s};
    switch (m_step) {
    case Step::AddNew:
        args << DiskEncryption::imageOptions(path, kOldId) << u"-o"_s << u"encrypt.state=active,encrypt.new-secret=%1"_s.arg(kNewId);
        break;
    case Step::RemoveOld:
        // Erases every key slot the old password opens.
        args << DiskEncryption::imageOptions(path, kNewId) << u"-o"_s << u"encrypt.state=inactive,encrypt.old-secret=%1"_s.arg(kOldId);
        break;
    case Step::RollBack:
        args << DiskEncryption::imageOptions(path, kOldId) << u"-o"_s << u"encrypt.state=inactive,encrypt.old-secret=%1"_s.arg(kNewId);
        break;
    }

    m_process = new QemuImgProcess(this);
    connect(m_process, &QemuImgProcess::finished, this, &DiskPasswordJob::onStepFinished);
    const bool started = m_process->start(args, m_fds);

    // The child holds its own copies now.
    for (const int fd : std::as_const(m_fds))
        ::close(fd);
    m_fds.clear();

    if (!started)
        onStepFinished(false);
}

void DiskPasswordJob::onStepFinished(bool ok)
{
    QemuImgProcess *process = std::exchange(m_process, nullptr);
    const QString error = process ? process->errorText() : QString();
    if (process)
        process->deleteLater();

    const QString fileName = QFileInfo(m_imagePaths.at(m_index)).fileName();

    switch (m_step) {
    case Step::AddNew:
        if (!ok) {
            if (!DiskEncryption::isWrongPasswordError(error))
                m_error = i18n("Could not add the new password to %1:\n%2", fileName, error);
            else if (m_index == 0)
                m_error = i18n("The current password is wrong.");
            else
                m_error = i18n("The current password does not open %1.", fileName);
            m_step = Step::RollBack;
            m_index = m_added - 1;
            runNext();
            return;
        }
        ++m_added;
        ++m_index;
        break;
    case Step::RemoveOld:
        if (!ok) {
            // Both passwords open this image now; the new one is in place everywhere.
            finish(i18n("The new password is set, but the old one could not be removed from %1:\n%2", fileName, error));
            return;
        }
        ++m_index;
        break;
    case Step::RollBack:
        --m_index;
        break;
    }
    runNext();
}

void DiskPasswordJob::rekeyTpm()
{
    m_tpmDone = true;
    const QDir stateDir(m_tpmStateDir);
    const TpmState::Encryption state = m_tpmStateDir.isEmpty() ? TpmState::Encryption::NoState : TpmState::encryptionOf(stateDir);
    if (state == TpmState::Encryption::NoState) {
        runNext();
        return;
    }

    // A state from before the encryption is still plain.
    const QByteArray oldKey = state == TpmState::Encryption::Encrypted ? m_oldPassword : QByteArray();
    const QString swtpm = ExternalTools::instance().path(ExternalTools::Swtpm);
    const QByteArray newKey = m_newPassword;
    const QString target = stateDir.filePath(u".tpm-rekey"_s);

    // rekey() blocks for about a second, so it runs on a thread of its own.
    auto result = std::make_shared<QString>();
    m_thread = QThread::create([swtpm, stateDir, oldKey, newKey, target, result] {
        *result = TpmState::rekey(swtpm, stateDir, oldKey, newKey, QDir(target));
    });
    connect(m_thread, &QThread::finished, this, [this, result] {
        m_thread->deleteLater();
        m_thread = nullptr;
        onTpmRekeyed(*result);
    });
    m_thread->start();
}

void DiskPasswordJob::onTpmRekeyed(const QString &error)
{
    if (!error.isEmpty()) {
        m_error = i18n("The TPM state could not be moved to the new password:\n%1", error);
        m_step = Step::RollBack;
        m_index = m_added - 1;
        runNext();
        return;
    }
    m_tpmRekeyed = true;
    runNext();
}

void DiskPasswordJob::finish(const QString &error)
{
    // The disks open with the new password from here on, so does the TPM.
    if (m_tpmRekeyed && m_step == Step::RemoveOld) {
        const QDir stateDir(m_tpmStateDir);
        if (!TpmState::commit(stateDir, QDir(stateDir.filePath(u".tpm-rekey"_s))) && error.isEmpty()) {
            setError(UserDefinedError);
            setErrorText(i18n("The new password is set, but the TPM state could not be replaced."));
            emitResult();
            return;
        }
    } else if (m_tpmRekeyed) {
        QDir(QDir(m_tpmStateDir).filePath(u".tpm-rekey"_s)).removeRecursively();
    }

    if (!error.isEmpty()) {
        setError(UserDefinedError);
        setErrorText(error);
    } else {
        emitPercent(1, 1);
    }
    emitResult();
}
