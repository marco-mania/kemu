/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QStringList>
#include <QVariantMap>

// Whether our logind session is the active one on its seat (section 3.4).
// Without logind the session counts as always active.
class SessionWatcher : public QObject
{
    Q_OBJECT

public:
    explicit SessionWatcher(QObject *parent = nullptr);

    bool isActive() const
    {
        return m_active;
    }

Q_SIGNALS:
    void activeChanged(bool active);

private Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    void readActive();
    void setActive(bool active);

    QString m_path;
    bool m_active = true;
};
