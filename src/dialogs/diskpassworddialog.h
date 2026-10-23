/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <KPasswordDialog>

/**
 * @brief Asks for the password of encrypted disks and checks it.
 *
 * The dialog stays open on a wrong password. Checking opens the LUKS header
 * of @p imagePath, which takes about two seconds.
 */
class DiskPasswordDialog : public KPasswordDialog
{
    Q_OBJECT

public:
    DiskPasswordDialog(const QString &imagePath, bool offerKeep, QWidget *parent = nullptr);

protected:
    bool checkPassword() override;

private:
    QString m_imagePath;
};
