/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "contextualhelp.h"

#include <KContextualHelpButton>

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>

QLayout *ContextualHelp::beside(QWidget *field, const QString &help)
{
    auto *row = new QHBoxLayout();
    row->addWidget(field);
    row->addWidget(new KContextualHelpButton(help, field, nullptr));
    row->addStretch();
    return row;
}

void ContextualHelp::addRow(QFormLayout *form, const QString &label, QWidget *field, const QString &help)
{
    QLayout *row = beside(field, help);
    form->addRow(label, row);
    if (auto *buddy = qobject_cast<QLabel *>(form->labelForField(row)))
        buddy->setBuddy(field);
}
