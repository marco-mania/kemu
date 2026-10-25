/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "encryptdiskjob.h"

#include <memory>
#include <utility>

#include <unistd.h>

#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QThread>

#include <KFormat>
#include <KLocalizedString>

#include "utils/diskencryption.h"
#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"
#include "utils/snapshotmanager.h"
#include "utils/tpmstate.h"

using namespace Qt::StringLiterals;

namespace
{
// Share of the progress bar taken by copying the disk.
constexpr int kConvertBand = 900;
constexpr qint64 kSpaceHeadroom = 64LL << 20;
}

EncryptDiskJob::EncryptDiskJob(Mode mode,
                               const QString &vmName,
                               const QDir &vmDir,
                               const QString &systemDisk,
                               const QStringList &extraDisks,
                               const QStringList &snapshotNames,
                               const QByteArray &password,
                               QObject *parent)
    : KJob(parent)
    , m_mode(mode)
    , m_vmName(vmName)
    , m_vmDir(vmDir)
    , m_systemDisk(systemDisk)
    , m_extraDisks(extraDisks)
    , m_snapshotNames(snapshotNames)
    , m_password(password)
{
    setCapabilities(Killable);
    setProperty("immediateProgressReporting", true);
}

EncryptDiskJob::~EncryptDiskJob()
{
    // QThread must not be destroyed while it runs.
    if (m_thread) {
        m_thread->wait();
        delete m_thread;
    }
    DiskEncryption::wipe(m_password);
}

QString EncryptDiskJob::tmpDiskPath() const
{
    return m_systemDisk + (m_mode == Mode::Encrypt ? u".encrypt.tmp"_s : u".decrypt.tmp"_s);
}

QString EncryptDiskJob::tpmTargetPath() const
{
    return m_vmDir.filePath(u".tpm-rekey"_s);
}

void EncryptDiskJob::start()
{
    const QString title = m_mode == Mode::Encrypt ? i18n("Encrypting the system disk") : i18n("Decrypting the system disk");
    Q_EMIT description(this, title, QPair<QString, QString>(i18n("VM"), m_vmName));

    if (m_password.isEmpty() || !QFile::exists(m_systemDisk)) {
        fail(i18n("The system disk of '%1' is missing.", m_vmName));
        return;
    }

    // The copy has about the size the image takes on disk now.
    const qint64 required = QFileInfo(m_systemDisk).size() + kSpaceHeadroom;
    const QStorageInfo storage(m_vmDir.absolutePath());
    if (storage.isValid() && storage.bytesAvailable() < required) {
        const KFormat format;
        fail(
            i18n("Not enough free disk space.\nRequired: %1\nAvailable: %2", format.formatByteSize(required), format.formatByteSize(storage.bytesAvailable())));
        return;
    }

    QFile::remove(tmpDiskPath());
    const int keyFd = DiskEncryption::secretFd(m_password);
    if (keyFd < 0) {
        fail(i18n("The password could not be passed to qemu-img."));
        return;
    }

    m_process = new QemuImgProcess(this);
    connect(m_process, &QemuImgProcess::progress, this, [this](double percent) {
        emitPercent(qRound(percent * kConvertBand / 100.0), 1000);
    });
    connect(m_process, &QemuImgProcess::finished, this, &EncryptDiskJob::onConvertFinished);

    QStringList args{u"convert"_s, u"-p"_s, u"--object"_s, DiskEncryption::secretObject(keyFd)};
    if (m_mode == Mode::Encrypt)
        args << u"-f"_s << u"qcow2"_s << u"-O"_s << u"qcow2"_s << u"-o"_s << DiskEncryption::createOptions() << m_systemDisk;
    else
        args << u"--image-opts"_s << DiskEncryption::imageOptions(m_systemDisk) << u"-O"_s << u"qcow2"_s;
    args << tmpDiskPath();
    const bool started = m_process->start(args, {keyFd});
    ::close(keyFd);
    if (!started)
        onConvertFinished(false);
}

void EncryptDiskJob::onConvertFinished(bool ok)
{
    QemuImgProcess *process = std::exchange(m_process, nullptr);
    const QString error = process ? process->errorText() : QString();
    if (process)
        process->deleteLater();

    if (m_killed)
        return;
    if (!ok) {
        if (m_mode == Mode::Decrypt && DiskEncryption::isWrongPasswordError(error))
            fail(i18n("The password is wrong."));
        else if (m_mode == Mode::Encrypt)
            fail(i18n("The system disk could not be encrypted:\n%1", error));
        else
            fail(i18n("The system disk could not be decrypted:\n%1", error));
        return;
    }
    rekeyTpm();
}

void EncryptDiskJob::rekeyTpm()
{
    const bool encrypt = m_mode == Mode::Encrypt;
    const TpmState::Encryption current = TpmState::encryptionOf(m_vmDir);
    if (current != (encrypt ? TpmState::Encryption::Plain : TpmState::Encryption::Encrypted)) {
        commit();
        return;
    }

    Q_EMIT infoMessage(this, encrypt ? i18n("Encrypting the TPM state…") : i18n("Decrypting the TPM state…"));
    const QString swtpm = ExternalTools::instance().path(ExternalTools::Swtpm);
    const QString ioctl = ExternalTools::instance().path(ExternalTools::SwtpmIoctl);
    const QDir stateDir = m_vmDir;
    const QByteArray oldKey = encrypt ? QByteArray() : m_password;
    const QByteArray newKey = encrypt ? m_password : QByteArray();
    const QString target = tpmTargetPath();

    // rekey() blocks for about a second, so it runs on a thread of its own.
    auto result = std::make_shared<QString>();
    m_thread = QThread::create([swtpm, ioctl, stateDir, oldKey, newKey, target, result] {
        *result = TpmState::rekey(swtpm, ioctl, stateDir, oldKey, newKey, QDir(target));
    });
    connect(m_thread, &QThread::finished, this, [this, encrypt, result] {
        m_thread->deleteLater();
        m_thread = nullptr;
        if (!result->isEmpty()) {
            fail(encrypt ? i18n("The TPM state could not be encrypted:\n%1", *result) : i18n("The TPM state could not be decrypted:\n%1", *result));
            return;
        }
        m_tpmPrepared = true;
        commit();
    });
    m_thread->start();
}

bool EncryptDiskJob::commitTpm()
{
    return !m_tpmPrepared || TpmState::commit(m_vmDir, QDir(tpmTargetPath()));
}

void EncryptDiskJob::commit()
{
    // A plain TPM state next to an encrypted disk is migrated on the next
    // start, the other way round it could not be opened. So the TPM state
    // goes first when decrypting and last when encrypting.
    if (m_mode == Mode::Decrypt && !commitTpm()) {
        fail(i18n("The TPM state could not be replaced."));
        return;
    }

    // Both new copies exist; swap them in.
    const QString backup = m_systemDisk + u".orig"_s;
    QFile::remove(backup);
    if (!QFile::rename(m_systemDisk, backup)) {
        fail(i18n("The system disk could not be replaced."));
        return;
    }
    if (!QFile::rename(tmpDiskPath(), m_systemDisk)) {
        QFile::rename(backup, m_systemDisk);
        fail(i18n("The system disk could not be replaced."));
        return;
    }
    m_committed = true;
    if (m_mode == Mode::Encrypt && !commitTpm())
        m_warnings << i18n("The TPM state could not be replaced and stays unencrypted.");

    QFile::remove(backup);
    QFile::remove(SnapshotManager::descriptionFilePath(m_systemDisk));
    emitPercent(kConvertBand + 50, 1000);

    deleteNextSnapshot();
}

void EncryptDiskJob::deleteNextSnapshot()
{
    // The system disk lost its snapshots; the additional disks follow suit.
    if (m_snapshotNames.isEmpty() || m_extraDisks.isEmpty()) {
        emitPercent(1000, 1000);
        emitResult();
        return;
    }

    if (!m_snapshots)
        m_snapshots = new SnapshotManager(this);

    const QString name = m_snapshotNames.takeFirst();
    m_snapshots->deleteOfflineSnapshot(m_extraDisks, name, [this, name](bool ok, const QString &error) {
        if (!ok)
            m_warnings << i18n("The snapshot '%1' could not be deleted from the additional disks: %2", name, error);
        deleteNextSnapshot();
    });
}

void EncryptDiskJob::fail(const QString &error)
{
    cleanUp();
    setError(UserDefinedError);
    setErrorText(error);
    emitResult();
}

void EncryptDiskJob::cleanUp()
{
    QFile::remove(tmpDiskPath());
    QDir(tpmTargetPath()).removeRecursively();
}

bool EncryptDiskJob::doKill()
{
    // The TPM step takes a second and cannot be interrupted; the swap after
    // it is quick, so the job just finishes.
    if (m_thread || m_committed)
        return false;
    m_killed = true;
    if (m_process)
        m_process->abort();
    cleanUp();
    return true;
}
