/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDropEvent>
#include <QStandardItem>
#include <QTreeView>

class VMTreeView : public QTreeView {

    Q_OBJECT

public:
    using QTreeView::QTreeView;

Q_SIGNALS:
    void itemDropped();

protected:
    void dropEvent(QDropEvent* event) override
    {
        QTreeView::dropEvent(event);
        if (event->isAccepted())
            Q_EMIT itemDropped();
    }
};
