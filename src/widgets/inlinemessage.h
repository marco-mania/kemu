/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <KMessageWidget>

/**
 * @brief Inline messages with the icon of their type.
 *
 * KMessageWidget shows only the colour of its type; an icon beside the text
 * says it again, for those who do not tell the colours apart.
 */
namespace InlineMessage
{
// Sets the type of @p message and the icon that goes with it.
void setType(KMessageWidget *message, KMessageWidget::MessageType type);
}
