/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QTimer>

class QLabel;
class QProgressBar;

/**
 * @brief Counts down to a host shutdown after a guest has powered itself off.
 *
 * Accepted on timeout or "Shut Down Now", rejected on Cancel. Cancel is the
 * default button, so a stray Enter never shuts the computer down early.
 */
class HostShutdownDialog : public QDialog
{
    Q_OBJECT

public:
    explicit HostShutdownDialog(const QString &vmName, int seconds, QWidget *parent = nullptr);

private:
    void updateCountdown();

    QLabel *m_countdownLabel = nullptr;
    QProgressBar *m_progress = nullptr;
    QTimer m_timer;
    int m_remaining = 0;
};
