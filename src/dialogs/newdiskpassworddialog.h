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
class KNewPasswordWidget;

/**
 * @brief Asks for a new password for the system disk and whether KWallet
 * keeps it.
 */
class NewDiskPasswordDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewDiskPasswordDialog(QWidget *parent = nullptr);

    void setPrompt(const QString &prompt);

    [[nodiscard]] QString password() const;

    [[nodiscard]] bool storeInWallet() const;
    void setStoreInWallet(bool store);

private:
    void updateOkButton();

    QLabel *m_promptLabel = nullptr;
    KNewPasswordWidget *m_passwordWidget = nullptr;
    QCheckBox *m_walletCheck = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};
