/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "utils.h"

#include "const.h"

#include <KLocalizedString>

#include "kemu_debug.h"

extern "C" {
char *__crypt_sha512(const char *key, const char *setting, char *output);
#include <algorithm>
#include <array>
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

QString configuredOvmfDir()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    return config->group(u"General"_s).readEntry("OvmfPath", QString());
}

OvmfVarsResult ensureOvmfVars(const QDir &vmDir, bool secureBoot)
{
    OvmfVarsResult result;
    const OvmfPaths ovmf = findOvmfPaths(secureBoot, configuredOvmfDir());
    if (ovmf.codePath.isEmpty())
        return result;

    const QString target = vmDir.filePath(KEMUConst::OvmfVarsFileName);
    if (QFileInfo::exists(target)) {
        result.status = OvmfVarsResult::Status::Present;
        return result;
    }
    if (ovmf.varsPath.isEmpty())
        return result;

    result.templatePath = ovmf.varsPath;
    const QString name = QFileInfo(ovmf.varsPath).fileName();
    result.keysMissing = secureBoot && !name.contains(".ms."_L1) && !name.contains("-ms-"_L1) && !name.contains("secboot"_L1);

    // Templates are read-only; QEMU writes the NVRAM into this copy.
    if (!QFile::copy(ovmf.varsPath, target) || !QFile::setPermissions(target, QFile::ReadOwner | QFile::WriteOwner)) {
        QFile::remove(target);
        result.status = OvmfVarsResult::Status::CopyFailed;
        return result;
    }

    result.status = OvmfVarsResult::Status::Created;
    return result;
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

QByteArray macAddressToBytes(const QString &mac)
{
    if (!isValidMacAddress(mac))
        return {};

    QByteArray result;
    result.reserve(6);
    const QStringList parts = mac.split(u':');
    for (const QString &part : parts)
        result.append(char(part.toUInt(nullptr, 16)));
    return result;
}

bool isValidDnsHostname(const QString &hostname)
{
    static const QRegularExpression re(u"^[a-z0-9]([a-z0-9-]{0,61}[a-z0-9])?$"_s);
    return re.match(hostname).hasMatch();
}

bool isValidNetworkName(const QString &name, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };

    if (name.isEmpty())
        return fail(i18n("A name is required."));
    if (name.size() > 32)
        return fail(i18n("The name must not be longer than 32 characters."));

    static const QRegularExpression allowed(u"^[a-zA-Z0-9_\\-. ]+$"_s);
    if (!allowed.match(name).hasMatch())
        return fail(i18n("Only letters, digits, spaces and the characters _ - . are allowed."));

    // The name becomes a directory: "." and ".." would escape the runtime
    // directory, a leading dot would hide it.
    if (name.startsWith(u'.') || name.endsWith(u'.'))
        return fail(i18n("The name must not start or end with a dot."));

    return true;
}

bool isValidDnsDomain(const QString &domain)
{
    if (domain.isEmpty() || domain.size() > 189)
        return false;
    const QStringList labels = domain.split(u'.');
    return std::all_of(labels.cbegin(), labels.cend(), [](const QString &label) {
        return isValidDnsHostname(label);
    });
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

QString escapeQemuOptionValue(const QString &value)
{
    QString escaped = value;
    escaped.replace(u',', u",,"_s);
    return escaped;
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

void setDiesWithParent(KProcess *process, const QList<int> &inheritFds)
{
    // Copied into a plain array: the modifier runs in the child between fork
    // and exec, where touching a shared container is not safe.
    constexpr qsizetype kMaxInheritedFds = 4;
    std::array<int, kMaxInheritedFds> fds{};
    fds.fill(-1);
    const qsizetype count = std::min(inheritFds.size(), kMaxInheritedFds);
    for (qsizetype i = 0; i < count; ++i)
        fds[i] = inheritFds.at(i);
    if (inheritFds.size() > kMaxInheritedFds)
        qCWarning(KEMU) << "setDiesWithParent: ignoring" << (inheritFds.size() - kMaxInheritedFds) << "descriptors beyond the limit";

    // Runs in the child between fork and exec, so it must stay
    // async-signal-safe.
    process->setChildProcessModifier([fds, count] {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        for (qsizetype i = 0; i < count; ++i) {
            if (fds[i] >= 0)
                fcntl(fds[i], F_SETFD, 0);
        }
    });
}

QProcessEnvironment environmentWithoutDbusAutolaunch()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    // Both libraries try $XDG_RUNTIME_DIR/bus (the systemd user bus) first.
    const QString runtimeDir = env.value(u"XDG_RUNTIME_DIR"_s);
    if (!env.contains(u"DBUS_SESSION_BUS_ADDRESS"_s) && (runtimeDir.isEmpty() || !QFileInfo::exists(runtimeDir + u"/bus"_s)))
        env.insert(u"DBUS_SESSION_BUS_ADDRESS"_s, u"disabled:"_s);
    return env;
}

void appendTail(QByteArray &tail, const QByteArray &data, int maxBytes)
{
    tail.append(data);
    if (tail.size() > maxBytes)
        tail.remove(0, tail.size() - maxBytes);
}

} // namespace Utils
