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
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QScrollArea>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QToolBar>
#include <QWindowStateChangeEvent>

#include <KAboutApplicationDialog>
#include <KAboutData>
#include <KLocalizedString>

#include "dbusdisplay/audioinlistener.h"
#include "dbusdisplay/audiolistener.h"
#include "dbusdisplay/clipboard.h"
#include "dbusdisplay/controlconnection.h"
#include "dbusdisplay/displaywidget.h"
#include "dbusdisplay/listener.h"
#include "dbusdisplay/osdoverlay.h"
#include "dbusdisplay/statusindicator.h"

using namespace Qt::StringLiterals;

struct ViewerDevicePrefs {
    bool audioOut = true; // default: on (current behaviour)
    bool audioIn = false; // default: off
    ClipboardDirection clipboard = ClipboardDirection::Off; // default: off
    bool networkLinkUp = true; // virtual cable plugged in
};

// =============================================================================
// DbusViewerWindow
// =============================================================================
// Wraps the QemuDisplayWidget with:
//   - Menu bar (Machine, View, Devices)
//   - Status bar (Resolution, Sound status, Clipboard status)
//   - Toggles for audio output, audio input, and clipboard
// =============================================================================

class DbusViewerWindow : public QMainWindow
{
    Q_OBJECT

public:
    DbusViewerWindow(QemuDisplayListener *listener,
                     QemuControlConnection *ctrl,
                     const QString &consolePath,
                     QemuAudioListener *audioOut,
                     QemuAudioInListener *audioIn,
                     bool audioOutConnected,
                     bool audioInConnected,
                     bool hasUsbController,
                     const ViewerDevicePrefs &prefs,
                     QWidget *parent = nullptr);
    ~DbusViewerWindow() override;

    QemuDisplayWidget *displayWidget() const
    {
        return m_display;
    }

    // Set clipboard mode (for initial CLI configuration).
    // Must be called after show(), also sets the corresponding menu action.
    void setClipboardMode(ClipboardDirection dir);

    // Subsequent audio activation (after asynchronous D-Bus registration)
    void audioOutReady();
    void audioInReady();

    // Reflect the VM's removable-media state in menu + status bar.
    // Called by MainWindow on viewer creation and after every media change.
    void setMediaState(bool hasIso, const QString &isoName, bool hasFloppy, const QString &floppyName);

    // Which removable drives the VM actually has. Indicators for absent
    // drives are hidden instead of showing an "empty" state.
    void setDriveAvailability(bool hasCdDrive, bool hasFloppyDrive);

    // Describe the VM's NIC. Empty label = no network card (action hidden).
    // Call once after construction, like setMediaState().
    void setNetworkState(const QString &nicLabel, bool linkUp);

    // Confirm - or revert - a link change once QMP has answered.
    // persist=false for states QEMU reports on its own (see section 7).
    void setNetworkLinkState(bool up, bool persist = true);

    QMenu *usbMenu() const
    {
        return m_usbMenu;
    }

    void setSuspended(bool suspended);

Q_SIGNALS:
    void viewerClosed();
    void shutdownRequested();
    void resetRequested();
    void powerOffRequested();
    void snapshotManagerRequested();

    void changeIsoRequested();
    void ejectIsoRequested();
    void changeFloppyRequested();
    void ejectFloppyRequested();

    void networkLinkChangeRequested(bool up);

    void inhibitStateChanged(); // fullscreen or pointer capture changed

    void devicePrefsChanged(const ViewerDevicePrefs &prefs);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private Q_SLOTS:
    void onToggleFullscreen(bool checked);
    void onToggleScaleToWindow(bool checked);
    void onToggleResizeWindow(bool checked);
    void onFitWindowToDisplay();
    void onZoomIn();
    void onZoomOut();
    void onZoomReset();
    void onToggleAudioOut(bool checked);
    void onToggleAudioIn(bool checked);
    void onToggleNetworkLink(bool checked);
    void onClipboardOff();
    void onClipboardToHost();
    void onClipboardToGuest();
    void onClipboardBoth();
    void onScanoutChanged(int width, int height);
    void onQemuDisconnected();
    void onAbout();
    void updateMouseStatus();
    void onPointerCaptureChanged(bool active);

private:
    void createActions();
    void createMenus();
    void createStatusBar();
    void updateStatusBar();
    void popupIndicatorMenu(StatusIndicator *indicator, QMenu *menu);
    void showOsd(const QString &text, int holdMs = 2500);
    void setupClipboard(ClipboardDirection dir);
    void teardownClipboard();
    Q_INVOKABLE void updateWindowAndDisplaySize();
    void fitContentToWindow();
    void fitWindowToContent(bool force);
    void updateViewActions();
    void showFullscreenHint();

    QemuControlConnection *m_ctrl;
    QString m_consolePath;

    // Core components
    QScrollArea *m_scrollArea = nullptr;
    QemuDisplayWidget *m_display = nullptr;
    QemuDisplayListener *m_listener = nullptr;
    QemuAudioListener *m_audioOut = nullptr;
    QemuAudioInListener *m_audioIn = nullptr;
    QemuClipboardManager *m_clipboard = nullptr;

    bool m_audioOutConnected = false;
    bool m_audioInConnected = false;

    bool m_hasUsbController = false;

    // Menu actions
    QAction *m_actShutdown = nullptr;
    QAction *m_actReset = nullptr;
    QAction *m_actPowerOff = nullptr;
    QAction *m_actSendCAD = nullptr;
    QAction *m_actSnapshots = nullptr;
    QAction *m_actQuit = nullptr;
    QAction *m_actFullscreen = nullptr;
    QAction *m_actFitWindow = nullptr;
    QAction *m_actScaleToWindow = nullptr;
    QAction *m_actResizeWindow = nullptr;
    QAction *m_actZoomIn = nullptr;
    QAction *m_actZoomOut = nullptr;
    QAction *m_actZoomReset = nullptr;
    QAction *m_actAudioOut = nullptr;
    QAction *m_actAudioIn = nullptr;
    QAction *m_actChangeIso = nullptr;
    QAction *m_actEjectIso = nullptr;
    QAction *m_actChangeFloppy = nullptr;
    QAction *m_actEjectFloppy = nullptr;
    QAction *m_actClipOff = nullptr;
    QAction *m_actClipToHost = nullptr;
    QAction *m_actClipToGuest = nullptr;
    QAction *m_actClipBoth = nullptr;
    QAction *m_actGrabMouse = nullptr;
    QAction *m_actNetworkLink = nullptr;
    QAction *m_actAbout = nullptr;

    QMenu *m_usbMenu = nullptr;

    QLabel *m_lblResolution = nullptr;

    // Status bar indicators
    StatusIndicator *m_indCdrom = nullptr;
    StatusIndicator *m_indFloppy = nullptr;
    StatusIndicator *m_indNetwork = nullptr;
    StatusIndicator *m_indClipboard = nullptr;
    StatusIndicator *m_indAudioIn = nullptr;
    StatusIndicator *m_indAudioOut = nullptr;
    StatusIndicator *m_indMouse = nullptr;

    // Device menus, reused as popups by the indicators above
    QMenu *m_cdMenu = nullptr;
    QMenu *m_floppyMenu = nullptr;
    QMenu *m_clipMenu = nullptr;

    OsdOverlay *m_osd = nullptr;

    ViewerDevicePrefs m_prefs;
    ClipboardDirection m_clipDirection;

    bool m_hasIso = false;
    bool m_hasFloppy = false;
    bool m_hasCdDrive = true;
    bool m_hasFloppyDrive = true;
    QString m_isoName;
    QString m_floppyName;

    QString m_nicLabel;
    bool m_hasNic = false;

    QSize m_scanoutSize;
    QSize m_lastWindowSize = {1280, 720};

    qreal m_scale = 1.0f;
    bool m_windowFitsContent = true;
    bool m_contentFitsWindow = false;

    bool m_suspended = false;
};
