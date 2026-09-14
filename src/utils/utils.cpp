/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "utils.h"

#include "kemu_debug.h"

extern "C" {
char *__crypt_sha512(const char *key, const char *setting, char *output);
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
}

namespace Utils
{

QString generateCloudInitPasswordHash(const QString &password)
{
    if (password.isEmpty()) {
        return password;
    }

    // Generate a random salt (16 characters)
    // Allowed characters for crypt() salts are [a-zA-Z0-9./]
    const QString chars = u"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./"_s;

    // $6$ defines the SHA-512 algorithm
    QString salt = u"$6$"_s;
    for (int i = 0; i < 16; ++i) {
        salt.append(chars.at(QRandomGenerator::global()->bounded(chars.length())));
    }
    salt.append(u"$"_s); // Salt terminator

    // Convert Qt strings to proper C strings (UTF-8)
    const QByteArray passBytes = password.toUtf8();
    const QByteArray saltBytes = salt.toUtf8();

    char outputBuffer[128] = {0};

    const char *hashResult = __crypt_sha512(passBytes.constData(), saltBytes.constData(), outputBuffer);

    if (hashResult && qstrcmp(hashResult, "*") != 0) {
        return QString::fromUtf8(hashResult);
    }

    qCWarning(KEMU) << "Failed to generate cloud-init hash via __crypt_sha512!";

    return u"!"_s;
}

namespace
{

QStringList ovmfSearchDirs(const QString &customDir)
{
    static const QStringList systemDirs = {u"/usr/share/edk2/x64"_s,
                                           u"/usr/share/edk2/ovmf"_s,
                                           u"/usr/share/OVMF"_s,
                                           u"/usr/share/ovmf"_s,
                                           u"/usr/share/qemu"_s};

    QStringList dirs;
    if (!customDir.isEmpty())
        dirs << customDir;

    // Firmware shipped next to our own binaries always wins.
    const QDir prefix(QCoreApplication::applicationDirPath() + u"/.."_s);
    dirs << prefix.absoluteFilePath(u"share/qemu"_s) << prefix.absoluteFilePath(u"share/kemu/ovmf"_s);

    dirs << systemDirs;

    // Only present when a host OS is mounted into the namespace.
    if (QFileInfo::exists(u"/run/host/usr"_s)) {
        for (const QString &d : systemDirs)
            dirs << u"/run/host"_s + d;
    }

    return dirs;
}

QString firstExisting(const QDir &dir, const QStringList &names)
{
    for (const QString &n : names)
        if (dir.exists(n))
            return dir.filePath(n);
    return {};
}

OvmfPaths searchDirs(const QString &customDir, const QStringList &codeNames, const QStringList &varsNames)
{
    for (const QString &d : ovmfSearchDirs(customDir)) {
        const QDir dir(d);
        const QString code = firstExisting(dir, codeNames);
        if (code.isEmpty())
            continue;

        // A 4MB code image needs 4MB vars - mixing the two bricks the boot,
        // so prefer candidates of the same size family.
        const bool is4m = code.contains("4m"_L1, Qt::CaseInsensitive);
        QStringList vars = varsNames;
        std::stable_partition(vars.begin(), vars.end(), [is4m](const QString &n) {
            return n.contains("4m"_L1, Qt::CaseInsensitive) == is4m;
        });

        return {code, firstExisting(dir, vars)};
    }
    return {};
}

} // namespace

OvmfPaths findOvmfPaths(bool secureBoot, const QString &customDir)
{
    static const QStringList sbCode = {u"OVMF_CODE.secboot.4m.fd"_s,
                                       u"OVMF_CODE_4M.secboot.fd"_s,
                                       u"OVMF_CODE.secboot.fd"_s,
                                       u"edk2-x86_64-secure-code.fd"_s,
                                       u"ovmf-x86_64-smm-ms-code.bin"_s,
                                       u"ovmf-x86_64-smm-code.bin"_s};

    static const QStringList sbVars = {u"OVMF_VARS.4m.ms.fd"_s,
                                       u"OVMF_VARS_4M.ms.fd"_s,
                                       u"OVMF_VARS.ms.fd"_s,
                                       u"OVMF_VARS.secboot.fd"_s,
                                       u"ovmf-x86_64-smm-ms-vars.bin"_s,
                                       u"ovmf-x86_64-smm-vars.bin"_s,
                                       u"OVMF_VARS_4M.fd"_s,
                                       u"OVMF_VARS.4m.fd"_s,
                                       u"OVMF_VARS.fd"_s,
                                       u"edk2-i386-vars.fd"_s};

    static const QStringList code =
        {u"OVMF_CODE.4m.fd"_s, u"OVMF_CODE_4M.fd"_s, u"OVMF_CODE.fd"_s, u"OVMF.4m.fd"_s, u"OVMF.fd"_s, u"edk2-x86_64-code.fd"_s, u"ovmf-x86_64-code.bin"_s};

    static const QStringList vars = {u"OVMF_VARS.4m.fd"_s, u"OVMF_VARS_4M.fd"_s, u"OVMF_VARS.fd"_s, u"edk2-i386-vars.fd"_s, u"ovmf-x86_64-vars.bin"_s};

    if (secureBoot) {
        const OvmfPaths sb = searchDirs(customDir, sbCode, sbVars);
        if (!sb.codePath.isEmpty())
            return sb;
        // Fall through to plain OVMF, as before.
    }

    return searchDirs(customDir, code, vars);
}

QString detectHostKeyboardLayout()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kxkbrc"_s);
    const KConfigGroup layoutGroup(config, u"Layout"_s);

    const QStringList layouts = layoutGroup.readEntry(u"LayoutList"_s, QStringList());

    if (!layouts.isEmpty())
        return layouts.first().left(2);

    // Fallback
    return QLocale::system().name().left(2);
}

QString generateRandomMac(const QStringList &excludes)
{
    QRandomGenerator *rng = QRandomGenerator::global();
    QString mac;
    do {
        mac = u"52:54:00:%1:%2:%3"_s.arg(rng->bounded(256), 2, 16, QChar(u'0'))
                  .arg(rng->bounded(256), 2, 16, QChar(u'0'))
                  .arg(rng->bounded(256), 2, 16, QChar(u'0'));
    } while (excludes.contains(mac, Qt::CaseInsensitive));
    return mac;
}

bool isValidMacAddress(const QString &mac)
{
    static const QRegularExpression re(u"^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$"_s);
    return re.match(mac).hasMatch();
}

QString quoteShellArg(const QString &arg)
{
    const QStringView view = arg;
    bool needsQuoting = false;
    for (const auto ch : view) {
        if (ch.isSpace() || ch == u'\t' || ch == u'\n' || ch == u'$' || ch == u'`' || ch == u'\\' || ch == u'"' || ch == u'!') {
            needsQuoting = true;
            break;
        }
    }
    if (!needsQuoting)
        return arg;
    QString escaped = arg;
    escaped.replace(u'\'', u"'\\''"_s);
    return u"'"_s + escaped + u"'"_s;
}

QString vmNameToHostname(const QString &vmName)
{
    QString host;
    host.reserve(vmName.size());

    for (const QChar c : vmName.toLower()) {
        if (c.isLetterOrNumber() && c.unicode() < 128)
            host.append(c);
        else if (!host.isEmpty() && !host.endsWith(u'-'))
            host.append(u'-');
    }

    while (host.endsWith(u'-'))
        host.chop(1);

    return host.isEmpty() ? u"kemu-vm"_s : host.left(63);
}

void setDiesWithParent(KProcess *process, int inheritFd)
{
    // Runs in the child between fork and exec, so it must stay
    // async-signal-safe.
    process->setChildProcessModifier([inheritFd] {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (inheritFd >= 0)
            fcntl(inheritFd, F_SETFD, 0);
    });
}

void appendTail(QByteArray &tail, const QByteArray &data, int maxBytes)
{
    tail.append(data);
    if (tail.size() > maxBytes)
        tail.remove(0, tail.size() - maxBytes);
}

} // namespace Utils
