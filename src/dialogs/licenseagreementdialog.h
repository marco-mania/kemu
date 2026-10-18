/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QStringList>

/**
 * @brief Shows the license terms an appliance ships with.
 *
 * OVF requires them to be accepted before the appliance is deployed.
 */
class LicenseAgreementDialog : public QDialog
{
    Q_OBJECT

public:
    explicit LicenseAgreementDialog(const QString &applianceName, const QStringList &licenses, QWidget *parent = nullptr);

    // Survives the parent going away while the dialog is open.
    static bool ask(const QString &applianceName, const QStringList &licenses, QWidget *parent);
};
