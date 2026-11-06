/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

class QFormLayout;
class QLayout;
class QWidget;

/**
 * @brief Help buttons beside fields, for explanations too long for a tool tip.
 *
 * Kept apart from Utils, which the autotests build without KWidgetsAddons.
 */
namespace ContextualHelp
{
// The field with a KContextualHelpButton beside it. The button takes the
// field's height, so the row keeps its own.
[[nodiscard]] QLayout *beside(QWidget *field, const QString &help);
// The same as a form row; the label stays the field's buddy, for screen
// readers and access keys.
void addRow(QFormLayout *form, const QString &label, QWidget *field, const QString &help);
}
