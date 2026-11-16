/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "qtclipboardbackend.h"

#include <QClipboard>
#include <QGuiApplication>

using namespace Qt::StringLiterals;

namespace
{
QClipboard::Mode toMode(Protocol::Selection selection)
{
    return selection == Protocol::Selection::Primary ? QClipboard::Selection : QClipboard::Clipboard;
}
} // namespace

QtClipboardBackend::QtClipboardBackend(QObject *parent)
    : ClipboardBackend(parent)
    , m_clipboard(QGuiApplication::clipboard())
{
    connect(m_clipboard, &QClipboard::changed, this, [this](QClipboard::Mode mode) {
        if (mode == QClipboard::Clipboard)
            Q_EMIT changed(Protocol::Selection::Clipboard);
        else if (mode == QClipboard::Selection)
            Q_EMIT changed(Protocol::Selection::Primary);
    });
}

QString QtClipboardBackend::name() const
{
    return u"x11"_s;
}

bool QtClipboardBackend::supportsPrimary() const
{
    return m_clipboard->supportsSelection();
}

const QMimeData *QtClipboardBackend::mimeData(Protocol::Selection selection) const
{
    return m_clipboard->mimeData(toMode(selection));
}

void QtClipboardBackend::setMimeData(Protocol::Selection selection, QMimeData *data)
{
    m_clipboard->setMimeData(data, toMode(selection));
}

void QtClipboardBackend::clear(Protocol::Selection selection)
{
    m_clipboard->clear(toMode(selection));
}
