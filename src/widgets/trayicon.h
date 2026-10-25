/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QObject>
#include <QPointer>

class KStatusNotifierItem;
class QAction;
class QMenu;
class QWidget;
class VirtualEnvironmentModel;

// =============================================================================
// TrayIcon
// =============================================================================
// The system tray entry. A click shows or hides the main window, the menu
// lists the running machines and the stopped ones to start. The icon is
// active while a machine runs and passive otherwise, so an idle KEMU moves
// into the tray's hidden section.
//
// It only reads the model; every action is handed back as a signal, so the
// usual confirmations and error dialogs stay with the main window.
// =============================================================================

class TrayIcon : public QObject
{
    Q_OBJECT

public:
    TrayIcon(VirtualEnvironmentModel *model, QWidget *window, QObject *parent = nullptr);
    ~TrayIcon() override;

    // Whether a tray host is there to show the icon. Without one, hiding the
    // main window would leave KEMU unreachable.
    [[nodiscard]] static bool isAvailable();

    // Status and tooltip from the current machine states.
    void refresh();

Q_SIGNALS:
    void startRequested(const QString &vmName);
    void showDisplayRequested(const QString &vmName);
    void shutdownRequested(const QString &vmName);
    void powerOffRequested(const QString &vmName);
    void quitRequested();

private:
    void rebuildMenu();

    VirtualEnvironmentModel *m_model;
    KStatusNotifierItem *m_item;
    // Our entries go in front of it; the item appends its own standard
    // actions (restore, quit) after it.
    QAction *m_anchor;
    QList<QPointer<QObject>> m_entries;
};
