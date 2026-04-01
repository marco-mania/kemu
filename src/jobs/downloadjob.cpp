/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "downloadjob.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

DownloadJob::DownloadJob(const QString& url,
    const QString& localPath,
    const Utils::ChecksumType checksumType,
    const QString& checksum,
    QObject* parent)
    : KJob(parent)
    , m_url(url)
    , m_localPath(localPath)
    , m_checksumType(checksumType)
    , m_checksum(checksum.toLower())
{
    setCapabilities(KJob::Killable);
}

DownloadJob::~DownloadJob()
{
    delete m_runningHash;
    delete m_outputFile;
}

void DownloadJob::start()
{

    if (QFile::exists(m_localPath)) {
        Q_EMIT statusMessage(i18n("ISO already downloaded."));
        emitResult();
        return;
    }

    // TODO: resume support
    QDir().mkpath(QFileInfo(m_localPath).absolutePath());

    Q_EMIT statusMessage(i18n("Starting download…"));
    QTimer::singleShot(0, this, &DownloadJob::startDownload);
}

bool DownloadJob::doKill()
{
    if (m_reply) {
        m_reply->abort();
    }
    if (m_outputFile) {
        m_outputFile->close();
        // remove incomplete file
        QFile::remove(m_localPath + u".part"_s);
    }
    return true;
}

void DownloadJob::startDownload()
{
    m_nam = new QNetworkAccessManager(this);

    // write to a .part file, rename on success
    const QString partPath = m_localPath + u".part"_s;
    m_outputFile = new QFile(partPath);
    if (!m_outputFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(1);
        setErrorText(i18n("Cannot open file for writing: %1", m_outputFile->errorString()));
        emitResult();
        return;
    }

    // hash for streaming verification
    if (m_checksumType != Utils::ChecksumType::NONE)
        m_runningHash = new QCryptographicHash(hashAlgorithm());

    QNetworkRequest request { QUrl(m_url) };

    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);

    // Set a recognizable User-Agent
    request.setHeader(QNetworkRequest::UserAgentHeader, u"%1/%2 (KDE Virtual Machine Manager)"_s.arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion()));

    m_reply = m_nam->get(request);

    connect(m_reply, &QNetworkReply::downloadProgress, this, &DownloadJob::onDownloadProgress);
    connect(m_reply, &QNetworkReply::readyRead, this, &DownloadJob::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &DownloadJob::onDownloadFinished);
}

void DownloadJob::onReadyRead()
{
    if (!m_reply || !m_outputFile)
        return;

    const QByteArray data = m_reply->readAll();
    m_outputFile->write(data);
    if (m_runningHash && m_checksumType != Utils::ChecksumType::NONE)
        m_runningHash->addData(data);
}

void DownloadJob::onDownloadProgress(qint64 received, qint64 total)
{
    if (total > 0) {
        setTotalAmount(KJob::Bytes, total);
        setProcessedAmount(KJob::Bytes, received);

        unsigned long pct = static_cast<unsigned long>(received * 100 / total);
        setPercent(pct);

        double receivedMb = received / (1024.0 * 1024.0);
        double totalMb = total / (1024.0 * 1024.0);
        Q_EMIT statusMessage(i18n("Downloading: %1 / %2 MB (%3%)",
            QString::number(receivedMb, 'f', 1),
            QString::number(totalMb, 'f', 1),
            pct));
    }
}

void DownloadJob::onDownloadFinished()
{
    if (!m_reply)
        return;

    if (m_reply->bytesAvailable() > 0)
        onReadyRead();

    m_outputFile->close();

    if (m_reply->error() != QNetworkReply::NoError) {
        setError(2);
        setErrorText(i18n("Download failed: %1", m_reply->errorString()));
        QFile::remove(m_localPath + u".part"_s);
        emitResult();
        return;
    }

    int httpStatus = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpStatus >= 400) {
        setError(3);
        setErrorText(i18n("HTTP error %1", httpStatus));
        QFile::remove(m_localPath + u".part"_s);
        emitResult();
        return;
    }

    Q_EMIT statusMessage(i18n("Download complete. Verifying checksum…"));

    QFile::remove(m_localPath);
    if (!QFile::rename(m_localPath + u".part"_s, m_localPath)) {
        setError(4);
        setErrorText(i18n("Failed to rename downloaded file."));
        emitResult();
        return;
    }

    verifyChecksum();
}

void DownloadJob::verifyChecksum()
{
    if (!m_runningHash || m_checksum.isEmpty() || m_checksumType != Utils::ChecksumType::NONE) {
        // No checksum provided – skip verification
        Q_EMIT statusMessage(i18n("Download complete (no checksum verification)."));
        emitResult();
        return;
    }

    const QString computed = QString::fromLatin1(m_runningHash->result().toHex());

    if (computed != m_checksum) {
        setError(5);
        setErrorText(i18n("Checksum mismatch!\n\nExpected: %1\nGot: %2", m_checksum, computed));
        QFile::remove(m_localPath);
        emitResult();
        return;
    }

    Q_EMIT statusMessage(i18n("ISO downloaded and verified successfully."));
    emitResult();
}

QCryptographicHash::Algorithm DownloadJob::hashAlgorithm() const
{
    if (m_checksumType == Utils::ChecksumType::SHA512)
        return QCryptographicHash::Sha512;
    if (m_checksumType == Utils::ChecksumType::MD5)
        return QCryptographicHash::Md5;

    return QCryptographicHash::Sha256;
}
