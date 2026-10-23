/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <KJob>

class QemuImgProcess;
class QThread;

/**
 * @brief Changes the password of LUKS encrypted qcow2 images.
 *
 * Only the key slots change, the data is not rewritten. The new password
 * is added to every image first and the old one removed afterwards, so each
 * image opens with at least one of them at any time. If adding fails, the
 * new password is taken out of the images that already had it.
 */
class DiskPasswordJob : public KJob
{
    Q_OBJECT

public:
    DiskPasswordJob(const QString &vmName,
                    const QStringList &imagePaths,
                    const QByteArray &oldPassword,
                    const QByteArray &newPassword,
                    QObject *parent = nullptr);
    ~DiskPasswordJob() override;

    // The TPM state in @p stateDir gets the new password as well. Moved
    // over between adding and removing the disk password.
    void setTpmStateDir(const QString &stateDir)
    {
        m_tpmStateDir = stateDir;
    }

    void start() override;

    [[nodiscard]] QString vmName() const
    {
        return m_vmName;
    }

private:
    enum class Step {
        AddNew,
        RemoveOld,
        RollBack
    };

    void runNext();
    void onStepFinished(bool ok);
    void rekeyTpm();
    void onTpmRekeyed(const QString &error);
    void finish(const QString &error);

    QString m_vmName;
    QStringList m_imagePaths;
    QByteArray m_oldPassword;
    QByteArray m_newPassword;

    Step m_step = Step::AddNew;
    qsizetype m_index = 0;
    qsizetype m_added = 0; // images that accepted the new password
    QString m_error;

    QemuImgProcess *m_process = nullptr;
    QList<int> m_fds;

    QString m_tpmStateDir;
    QThread *m_thread = nullptr; // runs the TPM re-keying
    bool m_tpmRekeyed = false; // the new state waits in tpmTargetDir()
    bool m_tpmDone = false;
};
