/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskencryption.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

#include <QProcess>

using namespace Qt::StringLiterals;

namespace DiskEncryption
{

namespace
{
QString escapeOption(const QString &value)
{
    QString escaped = value;
    escaped.replace(u',', u",,"_s);
    return escaped;
}
}

int secretFd(const QByteArray &secret)
{
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0)
        return -1;

    // A password fits into the pipe buffer, so the write never blocks.
    qsizetype written = 0;
    while (written < secret.size()) {
        const ssize_t n = ::write(fds[1], secret.constData() + written, secret.size() - written);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return -1;
        }
        written += n;
    }
    ::close(fds[1]);
    return fds[0];
}

void inheritFds(QProcess *process, const QList<int> &fds)
{
    // Copied into a plain array: the modifier runs between fork and exec,
    // where only async-signal-safe calls are allowed.
    constexpr qsizetype kMaxFds = 4;
    std::array<int, kMaxFds> list{};
    list.fill(-1);
    for (qsizetype i = 0; i < std::min(fds.size(), kMaxFds); ++i)
        list[i] = fds.at(i);

    process->setChildProcessModifier([list] {
        for (const int fd : list) {
            if (fd >= 0)
                ::fcntl(fd, F_SETFD, 0);
        }
    });
}

QString verifyPassword(const QString &qemuImg, const QString &path, const QByteArray &password)
{
    const int fd = secretFd(password);
    if (fd < 0)
        return u"pipe() failed"_s;

    // Listing the snapshots opens the image, and with it the LUKS header.
    QProcess process;
    process.setProgram(qemuImg);
    process.setArguments({u"snapshot"_s, u"-l"_s, u"--object"_s, secretObject(fd), u"--image-opts"_s, imageOptions(path)});
    inheritFds(&process, {fd});
    process.start();
    ::close(fd);

    if (!process.waitForFinished(60000)) {
        process.kill();
        process.waitForFinished();
        return u"qemu-img did not respond"_s;
    }
    if (process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
        return {};

    const QString error = QString::fromUtf8(process.readAllStandardError()).trimmed();
    return error.isEmpty() ? u"qemu-img failed"_s : error;
}

QString secretObject(int fd, const QString &id)
{
    return u"secret,id=%1,file=/dev/fd/%2"_s.arg(id).arg(fd);
}

QString imageOptions(const QString &path, const QString &secretId)
{
    return u"driver=qcow2,file.filename=%1,encrypt.key-secret=%2"_s.arg(escapeOption(path), secretId);
}

QString createOptions(const QString &secretId)
{
    return u"encrypt.format=luks,encrypt.key-secret=%1"_s.arg(secretId);
}

bool isWrongPasswordError(const QString &errorText)
{
    return errorText.contains(u"Invalid password"_s, Qt::CaseInsensitive);
}

void wipe(QByteArray &secret)
{
    if (secret.isEmpty())
        return;
    // Through a volatile pointer, so the stores are not optimized away.
    volatile char *data = secret.data();
    for (qsizetype i = 0; i < secret.size(); ++i)
        data[i] = 0;
    secret.clear();
}

}
