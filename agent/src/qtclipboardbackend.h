/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "clipboardbackend.h"

class QClipboard;

// QClipboard on X11: the X server serves selections to any client and
// reports their changes through XFixes, focus or not. Also GNOME's
// clipboard, through Xwayland.
class QtClipboardBackend : public ClipboardBackend
{
    Q_OBJECT

public:
    explicit QtClipboardBackend(QObject *parent = nullptr);

    QString name() const override;
    bool supportsPrimary() const override;
    const QMimeData *mimeData(Protocol::Selection selection) const override;
    void setMimeData(Protocol::Selection selection, QMimeData *data) override;
    void clear(Protocol::Selection selection) override;

private:
    QClipboard *m_clipboard;
};
