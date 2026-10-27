/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDBusMessage>
#include <QHash>
#include <QIcon>
#include <QObject>
#include <QString>
#include <QVariantMap>

#include <functional>

// =============================================================================
// VmLauncher
// =============================================================================
// Application menu entries that start one VM ("kemu --start <vm>"). A native
// KEMU writes them to the user's applications folder itself. In the Flatpak
// the sandbox cannot, so the Dynamic Launcher portal installs them after the
// user confirmed its dialog; the portal also points Exec at the sandbox.
//
// Which VM has an entry is kept in kemurc, so the Flatpak, which cannot see
// the installed files, still knows.
// =============================================================================

class VmLauncher : public QObject
{
    Q_OBJECT

public:
    struct Entry {
        QString vmName;
        QString vmRef; // what --start gets: the UUID, or the name without one
        QString iconName; // theme icon, or empty
        QString iconPath; // icon file, or empty
        QIcon icon; // rendered for the portal
    };

    enum class Result {
        Created,
        Cancelled,
        Failed,
    };
    using Done = std::function<void(Result result, const QString &error)>;

    explicit VmLauncher(QObject *parent = nullptr);

    [[nodiscard]] bool has(const QString &vmName) const;

    void create(const Entry &entry, Done done);
    // After an edit. Native entries follow silently; the portal needs the
    // user again, so the Flatpak only asks when name or reference changed.
    void update(const QString &oldName, const Entry &entry);
    void remove(const QString &vmName);

private Q_SLOTS:
    void onPortalResponse(uint response, const QVariantMap &results, const QDBusMessage &message);

private:
    struct Record {
        QString fileId;
        QString vmRef;
    };
    [[nodiscard]] static Record record(const QString &vmName);
    static void storeRecord(const QString &vmName, const Record &record);
    static void dropRecord(const QString &vmName);

    [[nodiscard]] static bool sandboxed();
    [[nodiscard]] static QString newFileId();
    [[nodiscard]] static QString nativePath(const QString &fileId);
    [[nodiscard]] static QString desktopEntry(const Entry &entry, bool sandboxed);

    void createNative(const Entry &entry, const QString &fileId, const Done &done);
    void createWithPortal(const Entry &entry, const QString &fileId, const Done &done);

    struct PendingInstall {
        Entry entry;
        QString fileId;
        Done done;
    };
    QHash<QString, PendingInstall> m_pending; // by request object path
    void watchRequest(const QString &path);
    void unwatchRequest(const QString &path);
};
