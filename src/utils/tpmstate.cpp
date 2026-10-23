/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "tpmstate.h"

#include <memory>

#include <unistd.h>

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>

#include "diskencryption.h"

using namespace Qt::StringLiterals;

namespace TpmState
{

namespace
{
// Header: version, minimum version, header size (16 bit), flags (16 bit).
constexpr int kHeaderSize = 6;
constexpr quint16 kFlagEncrypted = 0x0001;

constexpr int kSocketTimeoutMs = 5000;
constexpr int kCommandTimeoutMs = 15000;

struct Swtpm {
    std::unique_ptr<QProcess> process;
    QString ctrlPath;
};

QString stderrOf(QProcess *process)
{
    return QString::fromUtf8(process->readAllStandardError()).trimmed();
}

// Starts swtpm on @p dir and waits for its control socket.
QString startSwtpm(const QString &swtpm, const QDir &dir, const QString &ctrlPath, const QByteArray &key, Swtpm &out)
{
    QStringList args{u"socket"_s, u"--tpmstate"_s, u"dir=%1"_s.arg(dir.absolutePath()), u"--ctrl"_s, u"type=unixio,path=%1"_s.arg(ctrlPath), u"--tpm2"_s};

    int keyFd = -1;
    if (!key.isEmpty()) {
        keyFd = DiskEncryption::secretFd(key);
        if (keyFd < 0)
            return u"pipe() failed"_s;
        args << u"--key"_s << keyOption(keyFd);
    }

    out.ctrlPath = ctrlPath;
    out.process = std::make_unique<QProcess>();
    out.process->setProgram(swtpm);
    out.process->setArguments(args);
    if (keyFd >= 0)
        DiskEncryption::inheritFds(out.process.get(), {keyFd});
    out.process->start();
    if (keyFd >= 0)
        ::close(keyFd);

    if (!out.process->waitForStarted(kSocketTimeoutMs))
        return out.process->errorString();

    QElapsedTimer timer;
    timer.start();
    while (!QFileInfo::exists(ctrlPath)) {
        if (out.process->state() == QProcess::NotRunning || timer.elapsed() > kSocketTimeoutMs) {
            out.process->waitForFinished(1000);
            const QString error = stderrOf(out.process.get());
            return error.isEmpty() ? u"swtpm did not start"_s : error;
        }
        QThread::msleep(20);
    }
    return {};
}

QString runIoctl(const QString &ioctl, const Swtpm &tpm, const QStringList &args)
{
    QProcess process;
    process.start(ioctl, QStringList{u"--unix"_s, tpm.ctrlPath} + args);
    if (!process.waitForFinished(kCommandTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        return u"swtpm_ioctl did not respond"_s;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return {};

    // A wrong key makes swtpm report the reason and exit.
    tpm.process->waitForFinished(1000);
    const QString swtpmError = stderrOf(tpm.process.get());
    const QString ioctlError = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return swtpmError.isEmpty() ? ioctlError : swtpmError;
}

void stopSwtpm(const QString &ioctl, Swtpm &tpm)
{
    if (!tpm.process || tpm.process->state() == QProcess::NotRunning)
        return;
    (void)runIoctl(ioctl, tpm, {u"-s"_s});
    if (!tpm.process->waitForFinished(kSocketTimeoutMs)) {
        tpm.process->kill();
        tpm.process->waitForFinished();
    }
}

void wipeFile(const QString &path)
{
    QFile file(path);
    if (file.open(QIODevice::ReadWrite)) {
        file.write(QByteArray(file.size(), '\0'));
        file.flush();
        ::fsync(file.handle());
        file.close();
    }
    QFile::remove(path);
}
}

Encryption encryptionOf(const QDir &stateDir)
{
    QFile file(stateDir.filePath(QLatin1StringView(FileName)));
    if (!file.exists())
        return Encryption::NoState;
    if (!file.open(QIODevice::ReadOnly))
        return Encryption::Plain;

    const QByteArray header = file.read(kHeaderSize);
    if (header.size() < kHeaderSize)
        return Encryption::Plain;
    return (qFromBigEndian<quint16>(header.constData() + 4) & kFlagEncrypted) ? Encryption::Encrypted : Encryption::Plain;
}

QString keyOption(int fd)
{
    return u"pwdfd=%1,mode=aes-256-cbc,kdf=pbkdf2"_s.arg(fd);
}

QString ioctlPath(const QString &swtpm)
{
    const QString sibling = QFileInfo(swtpm).dir().filePath(u"swtpm_ioctl"_s);
    if (QFileInfo(sibling).isExecutable())
        return sibling;
    return QStandardPaths::findExecutable(u"swtpm_ioctl"_s);
}

QString rekey(const QString &swtpm, const QDir &stateDir, const QByteArray &oldKey, const QByteArray &newKey, const QDir &targetDir)
{
    const QString ioctl = ioctlPath(swtpm);
    if (swtpm.isEmpty() || ioctl.isEmpty())
        return u"swtpm or swtpm_ioctl not found"_s;

    // Short socket paths, and the plain state blob only on a RAM disk.
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    QTemporaryDir work(base + u"/kemu-tpm-XXXXXX"_s);
    if (!work.isValid())
        return u"no temporary directory"_s;
    const QString blob = work.filePath(u"state.blob"_s);

    QString error;
    {
        Swtpm tpm;
        error = startSwtpm(swtpm, stateDir, work.filePath(u"old.ctl"_s), oldKey, tpm);
        if (error.isEmpty())
            error = runIoctl(ioctl, tpm, {u"-i"_s});
        if (error.isEmpty())
            error = runIoctl(ioctl, tpm, {u"--save"_s, u"permanent"_s, blob});
        stopSwtpm(ioctl, tpm);
    }

    if (error.isEmpty()) {
        // Leftovers of an interrupted run would be loaded as the new state.
        QDir(targetDir).removeRecursively();
        if (!targetDir.mkpath(u"."_s))
            error = u"cannot create %1"_s.arg(targetDir.absolutePath());
    }
    if (error.isEmpty()) {
        Swtpm tpm;
        error = startSwtpm(swtpm, targetDir, work.filePath(u"new.ctl"_s), newKey, tpm);
        if (error.isEmpty())
            error = runIoctl(ioctl, tpm, {u"--load"_s, u"permanent"_s, blob});
        if (error.isEmpty())
            error = runIoctl(ioctl, tpm, {u"-i"_s});
        stopSwtpm(ioctl, tpm);
    }

    if (QFile::exists(blob))
        wipeFile(blob);

    if (error.isEmpty() && encryptionOf(targetDir) != (newKey.isEmpty() ? Encryption::Plain : Encryption::Encrypted))
        error = u"swtpm did not write the new state"_s;
    if (!error.isEmpty())
        QDir(targetDir).removeRecursively();
    return error;
}

bool commit(const QDir &stateDir, const QDir &targetDir)
{
    const QString current = stateDir.filePath(QLatin1StringView(FileName));
    const QString replaced = current + u".old"_s;
    const QString incoming = targetDir.filePath(QLatin1StringView(FileName));

    QFile::remove(replaced);
    if (!QFile::rename(current, replaced))
        return false;
    if (!QFile::rename(incoming, current)) {
        QFile::rename(replaced, current);
        return false;
    }
    wipeFile(replaced);
    QDir(targetDir).removeRecursively();
    return true;
}

}
