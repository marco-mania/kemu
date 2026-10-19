/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <KProcess>

#include <QApplication>
#include <QByteArray>
#include <QDir>
// #include <QMetaEnum>
#include <QProcessEnvironment>
#include <QString>
#include <QStringView>

using namespace Qt::StringLiterals;

namespace Utils
{

enum class ChecksumType {
    NONE = 0,
    SHA256,
    SHA512,
    MD5
};

// QEMU reports epoch seconds, parsed as UTC; the user wants local time in
// their own format.
inline QString formatTimestamp(const QDateTime &dt)
{
    return QLocale::system().toString(dt.toLocalTime(), QLocale::ShortFormat);
}

inline QString toString(ChecksumType type)
{
    switch (type) {
    case ChecksumType::SHA256:
        return u"SHA256"_s;
    case ChecksumType::SHA512:
        return u"SHA512"_s;
    case ChecksumType::MD5:
        return u"MD5"_s;
    case ChecksumType::NONE:
    default:
        return u"NONE"_s;
    }
}

inline ChecksumType parseChecksumType(const QString &str)
{
    if (str.compare("SHA256"_L1, Qt::CaseInsensitive) == 0)
        return ChecksumType::SHA256;
    if (str.compare("SHA512"_L1, Qt::CaseInsensitive) == 0)
        return ChecksumType::SHA512;
    if (str.compare("MD5"_L1, Qt::CaseInsensitive) == 0)
        return ChecksumType::MD5;
    if (str.compare("NONE"_L1, Qt::CaseInsensitive) == 0)
        return ChecksumType::NONE;

    return ChecksumType::NONE;
}

/**
 * @brief Generates a SHA-512 ($6$) Linux password hash for cloud-init.
 * * @param password The plaintext password.
 * @return The generated hash including the salt, or the plaintext password as a
 * fallback on failure.
 */
QString generateCloudInitPasswordHash(const QString &password);

/**
 * @brief Waits synchronously (without blocking the event loop) for a file to be
 * created.
 * @param sockPath  The absolute or relative path to the expected socket file.
 * @param timeoutMs Maximum wait time in milliseconds (default: 5000ms).
 * @return true if the file exists or was created within the timeout. false on
 * timeout.
 */
// bool waitForSocket(const QString &sockPath, int timeoutMs = 5000);

struct OvmfPaths {
    QString codePath;
    QString varsPath;
};

/**
 * @brief Locates the appropriate OVMF firmware (UEFI) files for the virtual
 * machine.
 * @param secureBoot If true, searches specifically for Secure Boot compatible
 * firmware (e.g., files containing Microsoft certificates/keys).
 * @param customDir  An optional custom directory path to search first. Can be
 * empty.
 * @return OvmfPaths A struct containing the absolute paths to the code file
 * (.fd) and the vars template. If the firmware cannot be found, the paths will
 * be empty.
 */
OvmfPaths findOvmfPaths(bool secureBoot = false, const QString &customDir = {});

/// Firmware directory from the settings; empty means auto-detect.
QString configuredOvmfDir();

struct OvmfVarsResult {
    enum class Status {
        Created,
        Present,
        FirmwareMissing,
        CopyFailed
    };
    Status status = Status::FirmwareMissing;
    QString templatePath;
    /// Secure Boot was requested, but the template has no pre-enrolled keys.
    bool keysMissing = false;
};

/**
 * @brief Makes sure a VM can boot with UEFI: the firmware is installed and the
 * VM directory holds its own copy of the matching VARS template.
 *
 * An existing VARS file is never replaced, it carries the VM's NVRAM.
 */
OvmfVarsResult ensureOvmfVars(const QDir &vmDir, bool secureBoot);

/**
 * @brief Detects the host's configured keyboard layout via KDE System Settings.
 *
 * Uses the KConfig API to read the user's saved layout configuration directly
 * from the KDE settings file (kxkbrc).
 * Note: This returns the *primary configured* layout, not necessarily the
 * currently active one if the user actively switches layouts during a session.
 *
 * @return A QString containing the short language code of the primary layout
 * (e.g., "de").
 */
QString detectHostKeyboardLayout();

QString generateRandomMac(const QStringList &excludes = {});
bool isValidMacAddress(QStringView mac);

// Parses "52:54:00:12:34:56" into its 6 raw bytes. Returns an empty
// array for invalid input.
QByteArray macAddressToBytes(const QString &mac);

// Single-label RFC 1123 host name: [a-z0-9-], 1-63 chars, no leading or
// trailing '-'. Used for the per-network DNS names of VMs.
bool isValidDnsHostname(QStringView hostname);

// Name of a virtual network. Same character set as a VM name (see
// VMConfig::isValidVmName), but shorter: the name is a KConfig group key and
// shows up in log output, where 32 characters are plenty.
bool isValidNetworkName(QStringView name, QString *error = nullptr);

// Dot-separated list of valid host name labels, e.g. "lab.example.com".
// Capped so that "<63-char host>.<domain>" stays a legal 253-byte name.
bool isValidDnsDomain(QStringView domain);

QString quoteShellArg(const QString &arg);

// QEMU splits option strings such as "file=...,format=raw" at commas, so a
// comma inside a value has to be doubled.
QString escapeQemuOptionValue(const QString &value);

// Derives an RFC-1123 style host name from a VM name. VM names allow spaces,
// dots and underscores, host names do not.
QString vmNameToHostname(const QString &vmName);

// Installs PR_SET_PDEATHSIG=SIGTERM so the child dies with KEMU.
// Replaces any previously installed child process modifier.
// Every descriptor in inheritFds gets its CLOEXEC flag cleared, so the child
// keeps exactly those. Never combine with
// UnixProcessParameters::CloseFileDescriptors: that would close them despite
// the cleared flag.
void setDiesWithParent(KProcess *process, const QList<int> &inheritFds = {});

// The system environment, unless it offers no session bus: then libdbus and
// GDBus would fork "dbus-launch --autolaunch" on X11 (SDL), which a
// seccomp sandbox with spawn=deny answers with SIGSYS. The bus address is
// set to one that fails instead.
QProcessEnvironment environmentWithoutDbusAutolaunch();

// Appends to a rolling tail, discarding the oldest bytes beyond maxBytes.
void appendTail(QByteArray &tail, const QByteArray &data, int maxBytes);

} // namespace Utils
