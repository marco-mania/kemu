/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "utils.h"

#include "kemu_debug.h"

extern "C" {
char *__crypt_sha512(const char *key, const char *setting, char *output);
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

bool waitForSocket(const QString &sockPath, int timeoutMs)
{
    if (QFile::exists(sockPath)) {
        return true;
    }

    const QFileInfo fileInfo(sockPath);
    const QString dirPath = fileInfo.absolutePath();

    QEventLoop loop;
    QTimer timer;
    QFileSystemWatcher watcher;

    watcher.addPath(dirPath);

    bool found = false;

    QObject::connect(&watcher, &QFileSystemWatcher::directoryChanged, [&]() {
        if (QFile::exists(sockPath)) {
            found = true;
            timer.stop();
            loop.quit();
        }
    });

    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        watcher.disconnect();
        loop.quit();
    });

    timer.start(timeoutMs);
    loop.exec();

    // Cleanup: remove path and clear watcher
    watcher.removePath(dirPath);

    return found;
}

OvmfPaths findOvmfPaths(bool secureBoot, const QString &customDir)
{
    if (!customDir.isEmpty()) {
        const QDir dir(customDir);

        QStringList codeNames;
        QStringList varsNames;

        if (secureBoot) {
            codeNames = {u"OVMF_CODE.secboot.4m.fd"_s, u"OVMF_CODE_4M.secboot.fd"_s, u"OVMF_CODE.secboot.fd"_s, u"OVMF_CODE.4m.fd"_s, u"OVMF_CODE.fd"_s};
            varsNames = {u"OVMF_VARS.4m.ms.fd"_s, u"OVMF_VARS_4M.ms.fd"_s, u"OVMF_VARS.ms.fd"_s, u"OVMF_VARS.4m.fd"_s, u"OVMF_VARS.fd"_s};
        } else {
            codeNames = {u"OVMF_CODE.4m.fd"_s, u"OVMF_CODE.fd"_s, u"OVMF.fd"_s};
            varsNames = {u"OVMF_VARS.4m.fd"_s, u"OVMF_VARS.fd"_s};
        }

        QString foundCode, foundVars;
        for (const auto &n : codeNames)
            if (dir.exists(n)) {
                foundCode = dir.filePath(n);
                break;
            }
        for (const auto &n : varsNames)
            if (dir.exists(n)) {
                foundVars = dir.filePath(n);
                break;
            }
        if (!foundCode.isEmpty())
            return {foundCode, foundVars};
    }

    if (secureBoot) {
        // Secure Boot
        const QList<QPair<QString, QString>> sbPairs = {
            // Arch / edk2-ovmf
            {u"/usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd"_s, u"/usr/share/edk2/x64/OVMF_VARS.4m.fd"_s},
            {u"/usr/share/edk2/x64/OVMF_CODE.4m.fd"_s, u"/usr/share/edk2/x64/OVMF_VARS.4m.fd"_s},
            // Fedora
            {u"/usr/share/edk2/ovmf/OVMF_CODE.secboot.fd"_s, u"/usr/share/edk2/ovmf/OVMF_VARS.secboot.fd"_s},
            // Debian / Ubuntu
            {u"/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"_s, u"/usr/share/OVMF/OVMF_VARS_4M.ms.fd"_s},
            {u"/usr/share/OVMF/OVMF_CODE.secboot.fd"_s, u"/usr/share/OVMF/OVMF_VARS.ms.fd"_s},
            {u"/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"_s, u"/usr/share/OVMF/OVMF_VARS_4M.fd"_s},
            {u"/usr/share/OVMF/OVMF_CODE.secboot.fd"_s, u"/usr/share/OVMF/OVMF_VARS.fd"_s},
            // openSUSE
            {u"/usr/share/qemu/ovmf-x86_64-smm-ms-code.bin"_s, u"/usr/share/qemu/ovmf-x86_64-smm-ms-vars.bin"_s},
            {u"/usr/share/qemu/ovmf-x86_64-smm-code.bin"_s, u"/usr/share/qemu/ovmf-x86_64-smm-vars.bin"_s},
        };

        for (const auto &pair : sbPairs)
            if (QFile::exists(pair.first))
                return {pair.first, pair.second};
    }

    const QList<QPair<QString, QString>> ovmfPairs = {{u"/usr/share/edk2/x64/OVMF_CODE.4m.fd"_s, u"/usr/share/edk2/x64/OVMF_VARS.4m.fd"_s},
                                                      {u"/usr/share/edk2/x64/OVMF_CODE.fd"_s, u"/usr/share/edk2/x64/OVMF_VARS.fd"_s},
                                                      {u"/usr/share/OVMF/OVMF_CODE.fd"_s, u"/usr/share/OVMF/OVMF_VARS.fd"_s},
                                                      {u"/usr/share/OVMF/OVMF_CODE.4m.fd"_s, u"/usr/share/OVMF/OVMF_VARS.4m.fd"_s},
                                                      {u"/usr/share/ovmf/OVMF.fd"_s, u"/usr/share/ovmf/OVMF_VARS.fd"_s},
                                                      {u"/usr/share/ovmf/OVMF.4m.fd"_s, u"/usr/share/ovmf/OVMF_VARS.4m.fd"_s},
                                                      {u"/usr/share/qemu/OVMF.fd"_s, u"/usr/share/qemu/OVMF_VARS.fd"_s}};

    for (const auto &pair : ovmfPairs)
        if (QFile::exists(pair.first))
            return {pair.first, pair.second};

    return {};
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

} // namespace Utils
