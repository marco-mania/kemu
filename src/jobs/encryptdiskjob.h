/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QDir>
#include <QString>
#include <QStringList>

#include <KJob>

class QemuImgProcess;
class QThread;
class SnapshotManager;

/**
 * @brief Encrypts the system disk of an existing VM and its TPM state, or
 * decrypts them again.
 *
 * The disk is copied into a LUKS encrypted (or plain) image and the TPM
 * state into an encrypted (or plain) one; only when both are ready do they
 * replace the originals. Snapshots do not survive the copy, so the ones of
 * the same name are deleted from the additional disks as well.
 */
class EncryptDiskJob : public KJob
{
    Q_OBJECT

public:
    enum class Mode {
        Encrypt,
        Decrypt
    };

    // @p password is the new one when encrypting, the current one otherwise.
    EncryptDiskJob(Mode mode,
                   const QString &vmName,
                   const QDir &vmDir,
                   const QString &systemDisk,
                   const QStringList &extraDisks,
                   const QStringList &snapshotNames,
                   const QByteArray &password,
                   QObject *parent = nullptr);
    ~EncryptDiskJob() override;

    void start() override;

    [[nodiscard]] Mode mode() const
    {
        return m_mode;
    }

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

    // Problems after the disk was replaced, e.g. a leftover snapshot.
    [[nodiscard]] QStringList warnings() const
    {
        return m_warnings;
    }

protected:
    bool doKill() override;

private:
    void onConvertFinished(bool ok);
    void rekeyTpm();
    void commit();
    [[nodiscard]] bool commitTpm();
    void deleteNextSnapshot();
    void fail(const QString &error);
    void cleanUp();

    [[nodiscard]] QString tmpDiskPath() const;
    [[nodiscard]] QString tpmTargetPath() const;

    Mode m_mode;
    QString m_vmName;
    QDir m_vmDir;
    QString m_systemDisk;
    QStringList m_extraDisks;
    QStringList m_snapshotNames;
    QByteArray m_password;

    QemuImgProcess *m_process = nullptr;
    SnapshotManager *m_snapshots = nullptr;
    bool m_tpmPrepared = false;
    QThread *m_thread = nullptr; // writes the TPM state
    bool m_committed = false; // the encrypted disk replaced the original
    bool m_killed = false;
    QStringList m_warnings;
};
