/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmlauncher.h"

#include <KConfigGroup>
#include <KLocalizedString>
#include <KSharedConfig>

#include <algorithm>

#include <QBuffer>
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPixmap>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>
#include <QWidget>

#include "utils/sandboxcapabilities.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr auto PortalService = "org.freedesktop.portal.Desktop"_L1;
constexpr auto PortalPath = "/org/freedesktop/portal/desktop"_L1;
constexpr auto LauncherInterface = "org.freedesktop.portal.DynamicLauncher"_L1;
constexpr auto RequestInterface = "org.freedesktop.portal.Request"_L1;
constexpr auto RegistryGroup = "Launchers"_L1;

// A serialized GIcon as the portal takes it: ('bytes', <ay>).
struct PortalIcon {
    QString type;
    QDBusVariant data;
};

QDBusArgument &operator<<(QDBusArgument &argument, const PortalIcon &icon)
{
    argument.beginStructure();
    argument << icon.type << icon.data;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalIcon &icon)
{
    argument.beginStructure();
    argument >> icon.type >> icon.data;
    argument.endStructure();
    return argument;
}

// Exec argument quoting of the Desktop Entry specification, followed by the
// escaping every string value gets.
QString execArgument(const QString &argument)
{
    static const QString plain = u"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_./:{}"_s;
    QString quoted = argument;
    if (argument.isEmpty() || std::any_of(argument.cbegin(), argument.cend(), [](QChar c) {
            return !plain.contains(c);
        })) {
        quoted.clear();
        for (const QChar c : argument) {
            if (c == u'"' || c == u'`' || c == u'$' || c == u'\\')
                quoted += u'\\';
            quoted += c;
        }
        quoted = u'"' + quoted + u'"';
    }
    quoted.replace(u'%', u"%%"_s);
    quoted.replace(u'\\', u"\\\\"_s);
    return quoted;
}

// Name and comments are single-line strings.
QString entryString(const QString &value)
{
    QString escaped = value;
    escaped.replace(u'\\', u"\\\\"_s);
    escaped.replace(u'\n', u"\\n"_s);
    escaped.replace(u'\r', u"\\r"_s);
    escaped.replace(u'\t', u"\\t"_s);
    return escaped;
}

QString parentWindowId(QWidget *window)
{
    // Wayland needs an exported xdg-foreign handle; without one the portal
    // shows its dialog unparented, which is fine.
    if (window && QGuiApplication::platformName() == u"xcb"_s)
        return u"x11:%1"_s.arg(window->window()->winId(), 0, 16);
    return {};
}

}

Q_DECLARE_METATYPE(PortalIcon)

VmLauncher::VmLauncher(QObject *parent)
    : QObject(parent)
{
    qDBusRegisterMetaType<PortalIcon>();
}

bool VmLauncher::has(const QString &vmName) const
{
    const Record r = record(vmName);
    if (r.fileId.isEmpty())
        return false;
    // Natively the file is the truth: it may have been deleted by hand.
    return sandboxed() || QFile::exists(nativePath(r.fileId));
}

void VmLauncher::create(const Entry &entry, QWidget *window, Done done)
{
    // The same file again for an existing entry, so it is replaced in place.
    QString fileId = record(entry.vmName).fileId;
    if (fileId.isEmpty())
        fileId = newFileId();

    if (sandboxed())
        createWithPortal(entry, fileId, window, done);
    else
        createNative(entry, fileId, done);
}

void VmLauncher::update(const QString &oldName, const Entry &entry, QWidget *window)
{
    const Record old = record(oldName);
    if (old.fileId.isEmpty())
        return;

    if (oldName != entry.vmName) {
        dropRecord(oldName);
        storeRecord(entry.vmName, old);
    }

    if (!sandboxed()) {
        if (QFile::exists(nativePath(old.fileId)))
            createNative(entry, old.fileId, {});
        else
            dropRecord(entry.vmName);
        return;
    }

    if (oldName == entry.vmName && old.vmRef == entry.vmRef)
        return;
    createWithPortal(entry, old.fileId, window, {});
}

void VmLauncher::remove(const QString &vmName)
{
    const Record r = record(vmName);
    if (r.fileId.isEmpty())
        return;
    dropRecord(vmName);

    if (!sandboxed()) {
        QFile::remove(nativePath(r.fileId));
        return;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(PortalService, PortalPath, LauncherInterface, u"Uninstall"_s);
    call << r.fileId << QVariantMap();
    QDBusConnection::sessionBus().asyncCall(call);
}

VmLauncher::Record VmLauncher::record(const QString &vmName)
{
    const QStringList value = KSharedConfig::openConfig(u"kemurc"_s)->group(RegistryGroup).readEntry(vmName, QStringList());
    return {value.value(0), value.value(1)};
}

void VmLauncher::storeRecord(const QString &vmName, const Record &record)
{
    KConfigGroup group = KSharedConfig::openConfig(u"kemurc"_s)->group(RegistryGroup);
    group.writeEntry(vmName, QStringList{record.fileId, record.vmRef});
    group.sync();
}

void VmLauncher::dropRecord(const QString &vmName)
{
    KConfigGroup group = KSharedConfig::openConfig(u"kemurc"_s)->group(RegistryGroup);
    group.deleteEntry(vmName);
    group.sync();
}

bool VmLauncher::sandboxed()
{
    return SandboxCapabilities::instance().isFlatpak();
}

QString VmLauncher::newFileId()
{
    // The portal only accepts ids below the application's own.
    return u"org.kde.kemu.vm_%1.desktop"_s.arg(QUuid::createUuid().toString(QUuid::Id128));
}

QString VmLauncher::nativePath(const QString &fileId)
{
    return QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + u'/' + fileId;
}

QString VmLauncher::desktopEntry(const Entry &entry, bool sandboxed)
{
    // The portal rewrites Exec to run the sandboxed application; natively it
    // has to be this very binary, which need not be in PATH.
    const QString program = sandboxed ? u"kemu"_s : execArgument(QCoreApplication::applicationFilePath());
    const QString exec = program + u" --start "_s + execArgument(entry.vmRef);

    QString icon = entry.iconPath.isEmpty() ? entry.iconName : entry.iconPath;
    if (icon.isEmpty())
        icon = u"org.kde.kemu"_s;
    // The portal sets the icon the user confirmed; a path from inside the
    // sandbox would mean nothing outside anyway.
    const QString iconLine = sandboxed ? QString() : u"Icon=%1\n"_s.arg(entryString(icon));

    QString text =
        u"[Desktop Entry]\n"
        "Type=Application\n"
        "Name=%1\n"
        "GenericName=%2\n"
        "Comment=%3\n"
        "Exec=%4\n"
        "%5"
        "Categories=System;Emulator;\n"
        // A headless VM never shows a window to end the feedback.
        "StartupNotify=false\n"
        "X-KEMU-VirtualMachine=%6\n"_s.arg(entryString(entry.vmName),
                                           entryString(i18n("Virtual Machine")),
                                           entryString(i18n("Start this virtual machine with KEMU")),
                                           exec,
                                           iconLine,
                                           entryString(entry.vmRef));

    // The portal's handling of actions is not something to rely on.
    if (!sandboxed) {
        text +=
            u"Actions=headless;\n"
            "\n"
            "[Desktop Action headless]\n"
            "Name=%1\n"
            "Exec=%2 --headless\n"_s.arg(entryString(i18n("Start Headless")), exec);
    }
    return text;
}

void VmLauncher::createNative(const Entry &entry, const QString &fileId, const Done &done)
{
    const QString path = nativePath(fileId);
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(desktopEntry(entry, false).toUtf8()) < 0 || !file.commit()) {
        if (done)
            done(Result::Failed, i18n("Could not write %1: %2", path, file.errorString()));
        return;
    }
    // Some menus only list entries they may execute.
    QFile::setPermissions(path, QFile::permissions(path) | QFileDevice::ExeUser);

    storeRecord(entry.vmName, {fileId, entry.vmRef});
    if (done)
        done(Result::Created, {});
}

void VmLauncher::createWithPortal(const Entry &entry, const QString &fileId, QWidget *window, const Done &done)
{
    QDBusConnection bus = QDBusConnection::sessionBus();

    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    QPixmap pixmap = entry.icon.pixmap(QSize(256, 256), 1.0);
    if (pixmap.isNull())
        pixmap = QIcon::fromTheme(u"org.kde.kemu"_s).pixmap(QSize(256, 256), 1.0);
    pixmap.save(&buffer, "PNG");
    const PortalIcon icon{u"bytes"_s, QDBusVariant(png)};

    // Subscribe before the call: the response may come before the reply.
    const QString token = u"kemu%1"_s.arg(QRandomGenerator::global()->generate());
    const QString sender = bus.baseService().mid(1).replace(u'.', u'_');
    const QString requestPath = u"/org/freedesktop/portal/desktop/request/%1/%2"_s.arg(sender, token);
    m_pending.insert(requestPath, {entry, fileId, done});
    watchRequest(requestPath);

    QDBusMessage call = QDBusMessage::createMethodCall(PortalService, PortalPath, LauncherInterface, u"PrepareInstall"_s);
    call << parentWindowId(window) << entry.vmName << QVariant::fromValue(QDBusVariant(QVariant::fromValue(icon)))
         << QVariantMap{{u"handle_token"_s, token}, {u"modal"_s, true}, {u"launcher_type"_s, 1u}};

    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher, requestPath]() {
        watcher->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *watcher;

        if (reply.isError()) {
            unwatchRequest(requestPath);
            const PendingInstall pending = m_pending.take(requestPath);
            if (pending.done)
                pending.done(Result::Failed,
                             reply.error().type() == QDBusError::UnknownInterface || reply.error().type() == QDBusError::UnknownMethod
                                 ? i18n("The desktop portal does not support launchers.")
                                 : reply.error().message());
            return;
        }

        // Older portals pick the path themselves.
        const QString actual = reply.value().path();
        if (actual != requestPath && m_pending.contains(requestPath)) {
            unwatchRequest(requestPath);
            m_pending.insert(actual, m_pending.take(requestPath));
            watchRequest(actual);
        }
    });
}

void VmLauncher::watchRequest(const QString &path)
{
    QDBusConnection::sessionBus().connect(PortalService, path, RequestInterface, u"Response"_s, this, SLOT(onPortalResponse(uint, QVariantMap, QDBusMessage)));
}

void VmLauncher::unwatchRequest(const QString &path)
{
    QDBusConnection::sessionBus()
        .disconnect(PortalService, path, RequestInterface, u"Response"_s, this, SLOT(onPortalResponse(uint, QVariantMap, QDBusMessage)));
}

void VmLauncher::onPortalResponse(uint response, const QVariantMap &results, const QDBusMessage &message)
{
    const QString path = message.path();
    if (!m_pending.contains(path))
        return;
    unwatchRequest(path);
    const PendingInstall pending = m_pending.take(path);

    if (response != 0) {
        if (pending.done)
            pending.done(response == 1 ? Result::Cancelled : Result::Failed, response == 1 ? QString() : i18n("The desktop portal refused the launcher."));
        return;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(PortalService, PortalPath, LauncherInterface, u"Install"_s);
    call << results.value(u"token"_s).toString() << pending.fileId << desktopEntry(pending.entry, true) << QVariantMap();

    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [watcher, pending]() {
        watcher->deleteLater();
        const QDBusPendingReply<> reply = *watcher;
        if (reply.isError()) {
            if (pending.done)
                pending.done(Result::Failed, reply.error().message());
            return;
        }
        // The user may have renamed it in the dialog; the record keeps ours.
        storeRecord(pending.entry.vmName, {pending.fileId, pending.entry.vmRef});
        if (pending.done)
            pending.done(Result::Created, {});
    });
}
