/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;

/**
 * @brief Asks for the name of a clone and whether it is a linked one.
 */
class CloneVmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CloneVmDialog(const QString &sourceName, const QString &suggestedName, QWidget *parent = nullptr);

    // Disables the linked option and shows @p reason instead of its hint.
    void setLinkedCloneBlocker(const QString &reason);

    [[nodiscard]] QString cloneName() const;
    [[nodiscard]] bool isLinkedClone() const;

private:
    QLineEdit *m_nameEdit = nullptr;
    QCheckBox *m_linkedCheck = nullptr;
    QLabel *m_linkedHint = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
