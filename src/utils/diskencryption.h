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

class QProcess;

// LUKS encrypted qcow2 disks. The password reaches QEMU and qemu-img as a
// secret object read from an inherited pipe, never on the command line.
namespace DiskEncryption
{

// Id of the secret object in QEMU and qemu-img command lines.
inline constexpr auto SecretId = "kemu-disk-key";

// Descriptor the exported start script passes the password on.
inline constexpr int ScriptSecretFd = 3;

// Read end of a pipe that holds @p secret, close-on-exec; -1 on failure.
// The caller hands it to the child and closes it afterwards.
[[nodiscard]] int secretFd(const QByteArray &secret);

// Lets the child of @p process inherit @p fds (close-on-exec otherwise).
void inheritFds(QProcess *process, const QList<int> &fds);

// Opens the image at @p path with @p password, which costs about as long as
// the key derivation (~2 s). Empty if it unlocks, otherwise the error.
[[nodiscard]] QString verifyPassword(const QString &qemuImg, const QString &path, const QByteArray &password);

// "-object" value reading the secret from descriptor @p fd.
[[nodiscard]] QString secretObject(int fd, const QString &id = QString::fromLatin1(SecretId));

// --image-opts value that opens the encrypted image at @p path.
[[nodiscard]] QString imageOptions(const QString &path, const QString &secretId = QString::fromLatin1(SecretId));

// "-o" value creating an encrypted image.
[[nodiscard]] QString createOptions(const QString &secretId = QString::fromLatin1(SecretId));

// qemu-img and QEMU report a wrong password this way.
[[nodiscard]] bool isWrongPasswordError(const QString &errorText);

// Overwrites the bytes before they are released.
void wipe(QByteArray &secret);

}
