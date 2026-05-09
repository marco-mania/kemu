/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCloseEvent>
#include <QDBusConnection>
#include <QDebug>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>

#include <KLocalizedString>

#include "dbusdisplay/audioinlistener.h"
#include "dbusdisplay/audiolistener.h"
#include "dbusdisplay/clipboard.h"
#include "dbusdisplay/displaywidget.h"
#include "dbusdisplay/listener.h"

using namespace Qt::StringLiterals;

// =============================================================================
// DbusViewerWindow
// =============================================================================
// Wraps the QemuDisplayWidget with:
//   - Menu bar (Machine, View, Devices)
//   - Status bar (Resolution, Sound status, Clipboard status)
//   - Toggles for audio output, audio input, and clipboard
// =============================================================================

class DbusViewerWindow : public QMainWindow {

    Q_OBJECT

public:
    DbusViewerWindow(QemuDisplayListener* listener,
        QDBusConnection& bus,
        const QString& consolePath,
        QemuAudioListener* audioOut,
        QemuAudioInListener* audioIn,
        bool audioOutConnected,
        bool audioInConnected,
        QWidget* parent = nullptr);
    ~DbusViewerWindow() override;

    QemuDisplayWidget* displayWidget() const { return m_display; }

    // Set clipboard mode (for initial CLI configuration).
    // Must be called after show(), also sets the corresponding menu action.
    void setClipboardMode(ClipboardDirection dir);

    // Subsequent audio activation (after asynchronous D-Bus registration)
    void audioOutReady();
    void audioInReady();

Q_SIGNALS:
    void viewerClosed();

protected:
    void closeEvent(QCloseEvent* event) override;
    void changeEvent(QEvent* event) override;

private Q_SLOTS:
    void onToggleFullscreen(bool checked);
    void onResetToNativeSize();
    void onToggleAudioOut(bool checked);
    void onToggleAudioIn(bool checked);
    void onClipboardOff();
    void onClipboardToHost();
    void onClipboardToGuest();
    void onClipboardBoth();
    void onScanoutChanged(int width, int height);
    void onQemuDisconnected();
    void onAbout();

private:
    void createActions();
    void createMenus();
    void createStatusBar();
    void updateStatusBar();
    void setupClipboard(ClipboardDirection dir);
    void teardownClipboard();

    QDBusConnection m_bus;
    QString m_consolePath;

    // Core components
    QemuDisplayWidget* m_display = nullptr;
    QemuDisplayListener* m_listener = nullptr;
    QemuAudioListener* m_audioOut = nullptr;
    QemuAudioInListener* m_audioIn = nullptr;
    QemuClipboardManager* m_clipboard = nullptr;

    bool m_audioOutConnected = false;
    bool m_audioInConnected = false;

    // Menu actions
    QAction* m_actQuit = nullptr;
    QAction* m_actFullscreen = nullptr;
    QAction* m_actNativeSize = nullptr;
    QAction* m_actAudioOut = nullptr;
    QAction* m_actAudioIn = nullptr;
    QAction* m_actClipOff = nullptr;
    QAction* m_actClipToHost = nullptr;
    QAction* m_actClipToGuest = nullptr;
    QAction* m_actClipBoth = nullptr;
    QAction* m_actAbout = nullptr;

    // Status bar labels
    QLabel* m_lblResolution = nullptr;
    QLabel* m_lblAudioOut = nullptr;
    QLabel* m_lblAudioIn = nullptr;
    QLabel* m_lblClipboard = nullptr;

    ClipboardDirection m_clipDirection;

    int m_lastTargetW = 1280;
    int m_lastTargetH = 720;
};
