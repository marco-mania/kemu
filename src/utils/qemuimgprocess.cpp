/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "qemuimgprocess.h"

#include <KLocalizedString>

#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include "diskencryption.h"
#include "externaltools.h"

using namespace Qt::StringLiterals;

namespace
{
constexpr int kStderrTailBytes = 4096;
}

QemuImgProcess::QemuImgProcess(QObject *parent)
    : QObject(parent)
{
}

double QemuImgProcess::parseProgress(QStringView chunk)
{
    static const QRegularExpression rx(u"\\(\\s*(\\d+(?:\\.\\d+)?)/100%\\)"_s);

    double last = -1.0;
    auto it = rx.globalMatchView(chunk);
    while (it.hasNext()) {
        bool ok = false;
        const double value = it.next().capturedView(1).toDouble(&ok);
        if (ok)
            last = value;
    }
    return last;
}

bool QemuImgProcess::start(const QStringList &arguments, const QList<int> &inheritFds)
{
    const auto &tools = ExternalTools::instance();
    if (!tools.isAvailable(ExternalTools::QemuImg)) {
        m_errorText = i18n("qemu-img not found. Please check Settings.");
        return false;
    }

    m_process = new KProcess(this);
    m_process->setOutputChannelMode(KProcess::SeparateChannels);
    m_process->setProgram(tools.path(ExternalTools::QemuImg));
    m_process->setArguments(arguments);
    if (!inheritFds.isEmpty())
        DiskEncryption::inheritFds(m_process, inheritFds);

    connect(m_process, &KProcess::readyReadStandardOutput, this, &QemuImgProcess::handleStdout);

    connect(m_process, &KProcess::readyReadStandardError, this, [this]() {
        m_stderrTail.append(m_process->readAllStandardError());
        if (m_stderrTail.size() > kStderrTailBytes)
            m_stderrTail.remove(0, m_stderrTail.size() - kStderrTailBytes);
    });

    connect(m_process, &KProcess::errorOccurred, this, [this](KProcess::ProcessError) {
        if (m_reported)
            return;
        m_reported = true;
        m_errorText = i18n("Could not run qemu-img: %1", m_process->errorString());
        Q_EMIT finished(false);
    });

    connect(m_process, &KProcess::finished, this, &QemuImgProcess::handleFinished);

    m_process->start();
    return true;
}

void QemuImgProcess::handleStdout()
{
    m_stdoutBuffer.append(m_process->readAllStandardOutput());

    const qsizetype lastBreak = qMax(m_stdoutBuffer.lastIndexOf('\r'), m_stdoutBuffer.lastIndexOf('\n'));
    if (lastBreak < 0)
        return;

    const QString complete = QString::fromUtf8(m_stdoutBuffer.left(lastBreak));
    m_stdoutBuffer.remove(0, lastBreak + 1);

    const double percent = parseProgress(complete);
    if (percent >= 0.0)
        Q_EMIT progress(percent);
}

void QemuImgProcess::handleFinished(int exitCode, KProcess::ExitStatus status)
{
    if (m_reported)
        return;
    m_reported = true;

    const bool ok = status == KProcess::NormalExit && exitCode == 0;
    if (!ok) {
        const QString tail = QString::fromUtf8(m_stderrTail).trimmed();
        m_errorText = tail.isEmpty() ? i18n("qemu-img failed (exit code %1).", exitCode) : i18n("qemu-img failed (exit code %1):\n%2", exitCode, tail);
    }

    Q_EMIT finished(ok);
}

void QemuImgProcess::abort()
{
    // Suppress signals for a deliberate kill.
    m_reported = true;

    if (m_process && m_process->state() != KProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

QemuImgProcess::ImageInfo QemuImgProcess::inspect(const QString &imagePath, int timeoutMs)
{
    ImageInfo info;

    const auto &tools = ExternalTools::instance();
    if (!QFile::exists(imagePath) || !tools.isAvailable(ExternalTools::QemuImg))
        return info;

    KProcess process;
    process.setOutputChannelMode(KProcess::SeparateChannels);
    process.setProgram(tools.path(ExternalTools::QemuImg));
    process.setArguments({u"info"_s, u"--output=json"_s, u"--force-share"_s, imagePath});
    process.start();

    if (!process.waitForFinished(timeoutMs) || process.exitStatus() != KProcess::NormalExit || process.exitCode() != 0)
        return info;

    const QJsonObject obj = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
    info.virtualSizeBytes = static_cast<qint64>(obj.value(u"virtual-size"_s).toDouble());
    info.actualSizeBytes = static_cast<qint64>(obj.value(u"actual-size"_s).toDouble());
    info.backingFile = obj.value(u"backing-filename"_s).toString();
    info.backingFormat = obj.value(u"backing-filename-format"_s).toString();
    info.isValid = info.virtualSizeBytes > 0;

    return info;
}
