/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCborMap>
#include <QHash>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>

#include <optional>

// One guest output as DISPLAY_LAYOUT reports it (section 7.1).
struct OutputInfo {
    QString name; // connector, e.g. "Virtual-1"
    bool enabled = true;
    QRect geometry; // global coordinates: logical on Wayland, pixels on X11
    QSize modeSize; // pixels
    int refreshMilliHz = 0;
    double scale = 1.0;
    QString transform = QStringLiteral("normal");
    bool primary = false;
    std::optional<uint> head;

    bool operator==(const OutputInfo &) const = default;
};

// Where the guest's monitors are, from the compositor or the X server.
class DisplayLayoutSource : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual QList<OutputInfo> outputs() const = 0;
    // For DISPLAY_LAYOUT's "source": "xdg-output", "wayland" or "x11-randr".
    virtual QString sourceName() const = 0;

Q_SIGNALS:
    void changed();
};

// Maps connector names to the heads of the guest's virtio-gpu device
// (section 7.3, implementation in section 11.4).
class HeadMapper
{
public:
    explicit HeadMapper(const QString &sysfsDrm = QStringLiteral("/sys/class/drm"));
    QHash<QString, uint> heads() const;

private:
    QString m_root;
};

// The bounding rectangle of the enabled outputs.
QRect pointerSpace(const QList<OutputInfo> &outputs);

QCborMap layoutMessage(const QList<OutputInfo> &outputs, quint64 generation, const QString &source);
