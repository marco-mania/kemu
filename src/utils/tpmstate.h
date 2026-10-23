/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QDir>
#include <QString>

// The swtpm state of a VM with an encrypted system disk is encrypted with
// the same password. swtpm cannot open a plain state with a key, nor
// re-key one, so the state is moved over: a swtpm on the old state saves
// it as a blob, a second one with the new key loads it and writes it out.
namespace TpmState
{

inline constexpr auto FileName = "tpm2-00.permall";

enum class Encryption {
    NoState,
    Plain,
    Encrypted
};

// Reads the header flags of the state file in @p stateDir.
[[nodiscard]] Encryption encryptionOf(const QDir &stateDir);

// swtpm "--key" value reading the passphrase from descriptor @p fd.
[[nodiscard]] QString keyOption(int fd);

// swtpm_ioctl, looked up next to @p swtpm first.
[[nodiscard]] QString ioctlPath(const QString &swtpm);

// Writes the state of @p stateDir, opened with @p oldKey (empty: plain),
// encrypted with @p newKey into the new directory @p targetDir. The
// original stays untouched. Blocks for about a second; returns the error.
[[nodiscard]] QString rekey(const QString &swtpm, const QDir &stateDir, const QByteArray &oldKey, const QByteArray &newKey, const QDir &targetDir);

// Replaces the state file in @p stateDir with the one in @p targetDir and
// removes @p targetDir.
[[nodiscard]] bool commit(const QDir &stateDir, const QDir &targetDir);

}
