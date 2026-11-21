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
#include <QHash>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPair>
#include <QPointer>
#include <QScrollArea>
#include <QSet>
#include <QStatusBar>
#include <QStringList>
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
#include "dbusdisplay/graphicsstatus.h"
#include "dbusdisplay/listener.h"
#include "dbusdisplay/multiheadmousemapping.h"
#include "dbusdisplay/osdoverlay.h"
#include "dbusdisplay/statusindicator.h"
#include "dbusdisplay/waylandkeyboardshortcutsinhibit.h"

using namespace Qt::StringLiterals;

// What the viewer shows about the QEMU guest agent. Filled by MainWindow,
// so the viewer needs no knowledge of the agent client.
struct GuestAgentView {
    enum class State {
        Absent, // no channel: the indicator is hidden
        Waiting, // no agent running in the guest
        Connecting,
        Connected,
        Unresponsive,
    };
    State state = State::Absent;
    QString version;
    QString hostName;
    QString osName;
    QStringList addresses;
};

struct ViewerDevicePrefs {
    bool audioOut = true; // default: on (current behaviour)
    bool audioIn = false; // default: off
    ClipboardDirection clipboard = ClipboardDirection::Off; // default: off
    bool clipboardFilterPasswords = true; // password manager entries stay on the host
    bool clipboardPrimary = false; // the selection (middle click) as well
    bool clipboardFiles = false; // copied files and folders as well
    int clipboardFilesLimitMiB = 256; // per copy
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

    void saveScreenshot();
    void setStatusBarShown(bool shown);
    void setAdditionalDisplays(const QList<QPair<uint, uint>> &displays, const QSet<uint> &openDisplays);
    void setSecondaryDisplayPlacement(MultiheadMouseMapping::SecondaryPlacement placement);
    void setSecondaryDisplayPosition(const QPoint &position);
    void showLayoutDetectionFailed();
    // kemu-agent reports the monitor arrangement: nothing to choose.
    void setDisplayLayoutFromAgent(bool fromAgent);

    QemuDisplayWidget *displayWidget() const
    {
        return m_display;
    }

    // Set clipboard mode (for initial CLI configuration).
    // Must be called after show(), also sets the corresponding menu action.
    void setClipboardMode(ClipboardDirection dir);

    // The kemu-agent channel of the VM; nullptr without one. Clipboard
    // sharing runs through it. Lives as long as QEMU, so set once.
    void setKemuAgent(KemuAgentClient *agent);
    // No kemu-agent for the guest system (Windows, BSD, DOS): the clipboard
    // menu stays off and says why, unless an agent connects after all.
    void setKemuAgentUnsupported(const QString &reason);
    // Why the kemu-agent CD image cannot be inserted (empty: it can). Devices
    // menu and the kemu-agent indicator offer it.
    void setAgentMediumUnavailable(const QString &reason);

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

    // State of the guest agent and what it reports about the guest.
    void setGuestAgentState(const GuestAgentView &view);

    // The 3D acceleration QEMU runs the VM with. The status bar shows it,
    // and what the guest makes of it once kemu-agent reports. Call once
    // after construction, like setNetworkState().
    void setGraphicsRequest(const GraphicsRequest &request);

    // How many USB devices are currently redirected into the VM.
    // Call after every attach/detach, like setMediaState().
    void setUsbState(int attachedCount);

    QMenu *usbMenu() const
    {
        return m_usbMenu;
    }

    void setSuspended(bool suspended);

    // Reflects the per-run "shut down host with guest" switch.
    void setShutdownHostWithGuest(bool armed, bool available, const QString &unavailableReason = {});

Q_SIGNALS:
    void viewerClosed();
    void subDisplayToggled(uint consoleId, bool open);
    void secondaryPlacementChanged(MultiheadMouseMapping::SecondaryPlacement placement);
    void secondaryPositionChanged(const QPoint &position);
    void shutdownRequested();
    void resetRequested();
    void powerOffRequested();
    void shutdownHostWithGuestToggled(bool armed);
    void snapshotManagerRequested();

    void changeIsoRequested();
    void ejectIsoRequested();
    void changeFloppyRequested();
    void ejectFloppyRequested();
    void insertAgentMediumRequested();

    void networkLinkChangeRequested(bool up);

    void inhibitStateChanged(); // fullscreen or pointer capture changed

    void devicePrefsChanged(const ViewerDevicePrefs &prefs);
    void statusBarShownChanged(bool shown);

protected:
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private Q_SLOTS:
    void onToggleFullscreen(bool checked);
    void onToggleStatusBar(bool shown);
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
    void onToggleClipboardFilterPasswords(bool checked);
    void onToggleClipboardPrimary(bool checked);
    void onToggleClipboardFiles(bool checked);
    void onClipboardFilesLimit(int limitMiB);
    void onScanoutChanged(int width, int height);
    void onQemuDisconnected();
    void onAbout();
    void updateMouseStatus();
    void onPointerCaptureChanged(bool active);
    void onKeyboardShortcutsInhibitSupportedChanged(bool supported);
    void onKeyboardShortcutsInhibitStateChanged();

private:
    void createActions();
    void createMenus();
    void createStatusBar();
    void updateStatusBar();
    void updateSecondaryPlacementMenu();
    void popupIndicatorMenu(StatusIndicator *indicator, QMenu *menu);
    void showOsd(const QString &text, int holdMs = 2500);
    void setupClipboard(ClipboardDirection dir);
    void updateKemuAgentIndicator();
    void updateGraphicsIndicator();
    void updateClipboardMenu(const ClipboardMenuState &state);
    QString clipboardUnavailableReason(const ClipboardMenuState &state) const;
    QString kemuAgentAwayText() const;
    void teardownClipboard();
    Q_INVOKABLE void updateWindowAndDisplaySize();
    void fitContentToWindow();
    void fitWindowToContent(bool force);
    void updateViewActions();
    void showFullscreenHint();
    bool setKeyboardShortcutsInhibited(bool inhibit);

    QemuControlConnection *m_ctrl;
    QString m_consolePath;

    // Core components
    QScrollArea *m_scrollArea = nullptr;
    QemuDisplayWidget *m_display = nullptr;
    QemuDisplayListener *m_listener = nullptr;
    QemuAudioListener *m_audioOut = nullptr;
    QemuAudioInListener *m_audioIn = nullptr;
    AgentClipboard *m_clipboard = nullptr;
    QPointer<KemuAgentClient> m_kemuAgent;
    QString m_kemuAgentUnsupported; // why the guest system has none; empty: it may come

    bool m_audioOutConnected = false;
    bool m_audioInConnected = false;

    bool m_hasUsbController = false;

    // Menu actions
    QAction *m_actShutdown = nullptr;
    QAction *m_actReset = nullptr;
    QAction *m_actPowerOff = nullptr;
    QAction *m_actShutdownHost = nullptr;
    QAction *m_actSendCAD = nullptr;
    QAction *m_actSendSysRq = nullptr;
    QAction *m_actSendAltSysRq = nullptr;
    QAction *m_actSnapshots = nullptr;
    QAction *m_actScreenshot = nullptr;
    QAction *m_actQuit = nullptr;
    QAction *m_actFullscreen = nullptr;
    QAction *m_actShowStatusBar = nullptr;
    bool m_statusBarShown = true;
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
    QAction *m_actInsertAgentMedium = nullptr;
    QMenu *m_kemuAgentMenu = nullptr; // behind the kemu-agent indicator
    QAction *m_actEjectFloppy = nullptr;
    QAction *m_actClipHint = nullptr; // why the settings are off with an agent running
    QAction *m_clipHintSeparator = nullptr;
    QAction *m_actClipOff = nullptr;
    QAction *m_actClipToHost = nullptr;
    QAction *m_actClipToGuest = nullptr;
    QAction *m_actClipBoth = nullptr;
    QAction *m_actClipFilterPasswords = nullptr;
    QAction *m_actClipPrimary = nullptr;
    QAction *m_actClipFiles = nullptr;
    QMenu *m_clipFilesLimitMenu = nullptr;
    QAction *m_actGrabMouse = nullptr;
    QAction *m_actKeyboardShortcutsInhibit = nullptr;
    QAction *m_actNetworkLink = nullptr;
    QAction *m_actAbout = nullptr;

    QMenu *m_usbMenu = nullptr;
    QMenu *m_displaysMenu = nullptr;
    QMenu *m_secondaryPlacementMenu = nullptr;
    QHash<int, QAction *> m_secondaryPlacementActions;
    MultiheadMouseMapping::SecondaryPlacement m_secondaryPlacement = MultiheadMouseMapping::SecondaryPlacement::Automatic;
    QPoint m_secondaryPosition = QPoint(1920, 0);
    bool m_layoutFromAgent = false;
    QHash<uint, QAction *> m_subDisplayActions;

    QLabel *m_lblResolution = nullptr;

    // Status bar indicators
    StatusIndicator *m_indCdrom = nullptr;
    StatusIndicator *m_indFloppy = nullptr;
    StatusIndicator *m_indNetwork = nullptr;
    StatusIndicator *m_indClipboard = nullptr;
    StatusIndicator *m_indAudioIn = nullptr;
    StatusIndicator *m_indAudioOut = nullptr;
    StatusIndicator *m_indMouse = nullptr;
    StatusIndicator *m_indKeyboardShortcutsInhibit = nullptr;
    StatusIndicator *m_indUsb = nullptr;
    StatusIndicator *m_indGuestAgent = nullptr;
    StatusIndicator *m_indGraphics = nullptr;
    StatusIndicator *m_indKemuAgent = nullptr;

    int m_usbAttached = 0;

    // Device menus, reused as popups by the indicators above
    QMenu *m_cdMenu = nullptr;
    QMenu *m_floppyMenu = nullptr;
    QMenu *m_clipMenu = nullptr;

    OsdOverlay *m_osd = nullptr;
    WaylandKeyboardShortcutsInhibit *m_keyboardShortcutsInhibit = nullptr;

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

    GuestAgentView m_guestAgent;
    GraphicsRequest m_graphicsRequest;

    QSize m_scanoutSize;
    QSize m_lastWindowSize = {1280, 720};

    qreal m_scale = 1.0f;
    bool m_windowFitsContent = true;
    bool m_contentFitsWindow = false;

    bool m_suspended = false;
};
