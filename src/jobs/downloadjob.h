/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>

#include <KJob>
#include <KLocalizedString>

#include "utils/utils.h"

using namespace Qt::StringLiterals;

/**
 * @brief KJob that downloads an ISO file and verifies its checksum.
 *
 * Features:
 * * Skips download if file already exists and is verified.
 * * Reports progress via KJob::setPercent().
 * * Writes a .verified marker file upon successful checksum match.
 * * Supports SHA-256, SHA-512, or MD5 checksums.
 *
 */
class DownloadJob : public KJob
{
    Q_OBJECT

public:
    /**
     * @param url           Direct download URL.
     * @param localPath     Full local file path to save the ISO to.
     * @param checksumType  SHA256, SHA512 or MD5.
     * @param checksum      Expected checksum hex string (empty = skip
     * verification).
     * @param parent        Parent QObject.
     */
    explicit DownloadJob(const QString &url,
                         const QString &localPath,
                         const Utils::ChecksumType checksumType = Utils::ChecksumType::NONE,
                         const QString &checksum = {},
                         QObject *parent = nullptr);

    ~DownloadJob() override;

    void start() override;

    [[nodiscard]] QString url() const
    {
        return m_url;
    }
    [[nodiscard]] QString localPath() const
    {
        return m_localPath;
    }
    [[nodiscard]] bool wasCached() const
    {
        return m_wasCached;
    }

Q_SIGNALS:
    // emitted with a human-readable status message for UI display
    void statusMessage(const QString &message);

protected:
    bool doKill() override;

private Q_SLOTS:
    void onDownloadProgress(qint64 received, qint64 total);
    void onDownloadFinished();
    void onReadyRead();

private:
    void startDownload();
    void verifyChecksum();

    QCryptographicHash::Algorithm hashAlgorithm() const;

    QString m_url;
    QString m_localPath;
    Utils::ChecksumType m_checksumType;
    QString m_checksum;

    QNetworkAccessManager *m_nam = nullptr;
    QPointer<QNetworkReply> m_reply;
    QFile *m_outputFile = nullptr;
    QCryptographicHash *m_runningHash = nullptr;

    bool m_wasCached = false;
};
