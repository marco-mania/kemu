/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

#include "protocol.h"

class QMimeData;

// The guest's clipboard and primary selection, as seen by a process that
// has no window and never has focus.
class ClipboardBackend : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // For HELLO: "ext-data-control", "wlr-data-control" or "x11".
    virtual QString name() const = 0;
    virtual bool supportsPrimary() const = 0;

    // Valid until control returns to the event loop.
    virtual const QMimeData *mimeData(Protocol::Selection selection) const = 0;
    // Takes ownership.
    virtual void setMimeData(Protocol::Selection selection, QMimeData *data) = 0;
    virtual void clear(Protocol::Selection selection) = 0;

Q_SIGNALS:
    void changed(Protocol::Selection selection);
};
