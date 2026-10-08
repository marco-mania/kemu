/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>

/**
 * @brief Asks how to abort a running deployment.
 *
 * A clean shutdown lets the guest close its file systems; a power-off is
 * immediate but may leave the disk inconsistent.
 */
class DeploymentAbortDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Choice {
        Cancel,
        ShutdownAndKeep,
        PowerOffAndKeep,
        PowerOffAndDelete,
    };

    explicit DeploymentAbortDialog(const QString &vmName, bool canShutdown, QWidget *parent = nullptr);

    [[nodiscard]] Choice choice() const;

    // Survives the parent going away while the dialog is open.
    static Choice ask(const QString &vmName, bool canShutdown, QWidget *parent);

private:
    Choice m_choice = Choice::Cancel;
};
