// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbusviewerwindow.h"

#include "kemu_dbusdisplay_debug.h"

// =============================================================================
// Constructor
// =============================================================================

DbusViewerWindow::DbusViewerWindow(QemuDisplayListener *listener,
                                   QemuControlConnection *ctrl,
                                   const QString &consolePath,
                                   QemuAudioListener *audioOut,
                                   QemuAudioInListener *audioIn,
                                   bool audioOutConnected,
                                   bool audioInConnected,
                                   bool hasUsbController,
                                   const ViewerDevicePrefs &prefs,
                                   QWidget *parent)
    : QMainWindow(parent)
    , m_ctrl(ctrl)
    , m_consolePath(consolePath)
    , m_listener(listener)
    , m_audioOut(audioOut)
    , m_audioIn(audioIn)
    , m_audioOutConnected(audioOutConnected)
    , m_audioInConnected(audioInConnected)
    , m_hasUsbController(hasUsbController)
    , m_prefs(prefs)
    , m_clipDirection(ClipboardDirection::Off)
{
    // Display widget as the central widget
    m_display = new QemuDisplayWidget(listener, ctrl, consolePath, this);
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidget(m_display);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    setCentralWidget(m_scrollArea);

    // Transient messages belong over the guest image, not in the status bar.
    // The viewport is the right parent: it is always exactly the visible area,
    // while m_display can be much larger than the window.
    m_osd = new OsdOverlay(m_scrollArea->viewport());

    // On the zero-copy display path the guest frame sits in a sub-surface
    // above the whole widget stack, so the pill has to be mirrored into a
    // sub-surface of its own. A no-op while that path is inactive.
    connect(m_osd, &OsdOverlay::appearanceChanged, this, [this] {
        if (!m_display || !m_display->isZeroCopyActive()) {
            return;
        }
        const bool visible = m_osd->isVisible();
        m_display->setOverlayImage(visible ? m_osd->renderToImage() : QImage(), m_osd->mapTo(this, QPoint(0, 0)), visible);
    });

    createActions();
    createMenus();
    createStatusBar();

    // Resolution updates – both paths (Software + DMA-BUF)
    connect(listener, &QemuDisplayListener::scanoutChanged, this, &DbusViewerWindow::onScanoutChanged);
    connect(listener, &QemuDisplayListener::dmaBufScanout, this, &DbusViewerWindow::onScanoutChanged);

    // QEMU termination: Stop notifiers immediately, then close with delay
    connect(listener, &QemuDisplayListener::connectionLost, this, &DbusViewerWindow::onQemuDisconnected);

    connect(m_display, &QemuDisplayWidget::fullscreenToggleRequested, m_actFullscreen, &QAction::trigger);

    connect(m_display, &QemuDisplayWidget::relativeGrabChanged, this, &DbusViewerWindow::onPointerCaptureChanged);
    connect(m_display, &QemuDisplayWidget::pointerModeChanged, this, &DbusViewerWindow::updateMouseStatus);

    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow: audioOutConnected=" << m_audioOutConnected << "audioInConnected=" << m_audioInConnected;

    m_actGrabMouse->setEnabled(m_display->isPointerCaptureSupported());

    resize(m_lastWindowSize);

    QMetaObject::invokeMethod(
        this,
        [this] {
            setClipboardMode(m_prefs.clipboard);
        },
        Qt::QueuedConnection);
}

DbusViewerWindow::~DbusViewerWindow()
{
    teardownClipboard();
}

// =============================================================================
// Create Actions
// =============================================================================

void DbusViewerWindow::createActions()
{
    // --- Machine ---
    m_actShutdown = new QAction(QIcon::fromTheme(u"system-shutdown"_s), i18n("&Shutdown"), this);
    m_actShutdown->setStatusTip(i18n("Send ACPI power-down signal to the guest"));
    connect(m_actShutdown, &QAction::triggered, this, &DbusViewerWindow::shutdownRequested);

    m_actReset = new QAction(QIcon::fromTheme(u"system-reboot"_s), i18n("&Reset"), this);
    m_actReset->setStatusTip(i18n("Reset the virtual machine (hard reset)"));
    connect(m_actReset, &QAction::triggered, this, &DbusViewerWindow::resetRequested);

    m_actPowerOff = new QAction(QIcon::fromTheme(u"process-stop"_s), i18n("Power-&off"), this);
    m_actPowerOff->setStatusTip(i18n("Force power-off the virtual machine (may cause data loss)"));
    connect(m_actPowerOff, &QAction::triggered, this, &DbusViewerWindow::powerOffRequested);

    m_actSendCAD = new QAction(QIcon::fromTheme(u"input-keyboard"_s), i18n("Send Ctrl+Alt+&Del"), this);
    m_actSendCAD->setStatusTip(i18n("Send the Ctrl+Alt+Del key sequence to the guest"));
    connect(m_actSendCAD, &QAction::triggered, m_display, &QemuDisplayWidget::sendCtrlAltDel);

    m_actSnapshots = new QAction(QIcon::fromTheme(u"camera-photo"_s), i18n("Manage &Snapshots..."), this);
    m_actSnapshots->setStatusTip(i18n("Open the snapshot manager for this VM"));
    connect(m_actSnapshots, &QAction::triggered, this, &DbusViewerWindow::snapshotManagerRequested);

    m_actQuit = new QAction(QIcon::fromTheme(u"window-close"_s), i18n("&Quit Viewer"), this);
    m_actQuit->setShortcut(QKeySequence(u"Ctrl+W"_s));
    m_actQuit->setStatusTip(i18n("Close the viewer window (the VM keeps running)"));
    connect(m_actQuit, &QAction::triggered, this, &QWidget::close);

    // --- View ---
    m_actFullscreen = new QAction(i18n("&Fullscreen"), this);
    m_actFullscreen->setShortcut(QKeySequence(Qt::Key_F11));
    m_actFullscreen->setCheckable(true);
    m_actFullscreen->setStatusTip(i18n("Toggle fullscreen mode"));
    connect(m_actFullscreen, &QAction::toggled, this, &DbusViewerWindow::onToggleFullscreen);

    m_actFitWindow = new QAction(QIcon::fromTheme(u"zoom-fit-best"_s), i18n("&Fit Window to Display"), this);
    m_actFitWindow->setShortcut(QKeySequence(u"Ctrl+Shift+R"_s));
    m_actFitWindow->setStatusTip(i18n("Resize the window so the guest display fits exactly"));
    connect(m_actFitWindow, &QAction::triggered, this, &DbusViewerWindow::onFitWindowToDisplay);

    m_actScaleToWindow = new QAction(i18n("&Scale Display to Window"), this);
    m_actScaleToWindow->setCheckable(true);
    m_actScaleToWindow->setStatusTip(i18n("Scale the guest display so it always fills the window"));
    connect(m_actScaleToWindow, &QAction::toggled, this, &DbusViewerWindow::onToggleScaleToWindow);

    m_actResizeWindow = new QAction(i18n("Auto-&Resize Window"), this);
    m_actResizeWindow->setCheckable(true);
    m_actResizeWindow->setChecked(true); // before connect(): no spurious toggle
    m_actResizeWindow->setStatusTip(i18n("Keep the window matched to the guest display size"));
    connect(m_actResizeWindow, &QAction::toggled, this, &DbusViewerWindow::onToggleResizeWindow);

    m_actZoomIn = new QAction(i18n("Zoom &In"), this);
    m_actZoomIn->setShortcut(QKeySequence::ZoomIn);
    m_actZoomIn->setStatusTip(i18n("Increase zoom level"));
    connect(m_actZoomIn, &QAction::triggered, this, &DbusViewerWindow::onZoomIn);

    m_actZoomOut = new QAction(i18n("Zoom &Out"), this);
    m_actZoomOut->setShortcut(QKeySequence::ZoomOut);
    m_actZoomOut->setStatusTip(i18n("Decrease zoom level"));
    connect(m_actZoomOut, &QAction::triggered, this, &DbusViewerWindow::onZoomOut);

    m_actZoomReset = new QAction(QIcon::fromTheme(u"zoom-original"_s), i18n("&Zoom to 100%"), this);
    m_actZoomReset->setStatusTip(i18n("Show the guest display at its native pixel size"));

    m_actGrabMouse = new QAction(QIcon::fromTheme(u"input-mouse"_s), i18n("&Capture Mouse"), this);
    m_actGrabMouse->setCheckable(true);
    m_actGrabMouse->setStatusTip(i18n("Confine the mouse pointer to the VM (release with Ctrl+Alt+G)"));
    if (!m_display->isPointerCaptureSupported()) {
        m_actGrabMouse->setEnabled(false);
        m_actGrabMouse->setToolTip(i18nc("@info:tooltip", "Pointer capture requires a Wayland session"));
    }
    connect(m_actGrabMouse, &QAction::toggled, m_display, &QemuDisplayWidget::setPointerCaptured);

    // --- Devices: Audio ---
    m_actAudioOut = new QAction(i18n("Audio &Output"), this);
    m_actAudioOut->setCheckable(true);
    m_actAudioOut->setChecked(m_audioOutConnected);
    m_actAudioOut->setEnabled(m_audioOutConnected);
    if (m_audioOutConnected) {
        m_actAudioOut->setStatusTip(i18n("Toggle audio output (playback)"));
    } else {
        m_actAudioOut->setStatusTip(
            i18n("Audio runs via system audio – start QEMU "
                 "with -audiodev dbus for D-Bus control"));
    }
    connect(m_actAudioOut, &QAction::toggled, this, &DbusViewerWindow::onToggleAudioOut);

    m_actAudioIn = new QAction(i18n("Audio &Input (Microphone)"), this);
    m_actAudioIn->setCheckable(true);
    m_actAudioIn->setChecked(m_audioInConnected);
    m_actAudioIn->setEnabled(m_audioInConnected);
    if (m_audioInConnected) {
        m_actAudioIn->setStatusTip(i18n("Toggle audio input (microphone)"));
    } else {
        m_actAudioIn->setStatusTip(
            i18n("Microphone runs via system audio – start QEMU with -audiodev "
                 "dbus for D-Bus control"));
    }
    connect(m_actAudioIn, &QAction::toggled, this, &DbusViewerWindow::onToggleAudioIn);

    // --- Devices: Removable media ---
    m_actChangeIso = new QAction(QIcon::fromTheme(u"media-optical"_s), i18n("&Change Image..."), this);
    m_actChangeIso->setStatusTip(i18n("Insert or replace the CD/DVD image"));
    connect(m_actChangeIso, &QAction::triggered, this, &DbusViewerWindow::changeIsoRequested);

    m_actEjectIso = new QAction(QIcon::fromTheme(u"media-eject"_s), i18n("&Eject"), this);
    m_actEjectIso->setStatusTip(i18n("Eject the current CD/DVD image"));
    m_actEjectIso->setEnabled(false);
    connect(m_actEjectIso, &QAction::triggered, this, &DbusViewerWindow::ejectIsoRequested);

    m_actChangeFloppy = new QAction(QIcon::fromTheme(u"media-floppy"_s), i18n("&Change Image..."), this);
    m_actChangeFloppy->setStatusTip(i18n("Insert or replace the floppy image"));
    connect(m_actChangeFloppy, &QAction::triggered, this, &DbusViewerWindow::changeFloppyRequested);

    m_actEjectFloppy = new QAction(QIcon::fromTheme(u"media-eject"_s), i18n("&Eject"), this);
    m_actEjectFloppy->setStatusTip(i18n("Eject the current floppy image"));
    m_actEjectFloppy->setEnabled(false);
    connect(m_actEjectFloppy, &QAction::triggered, this, &DbusViewerWindow::ejectFloppyRequested);

    // --- Devices: Clipboard ---
    auto *clipGroup = new QActionGroup(this);
    clipGroup->setExclusive(true);

    m_actClipOff = new QAction(i18n("Off"), this);
    m_actClipOff->setCheckable(true);
    m_actClipOff->setChecked(true);
    clipGroup->addAction(m_actClipOff);
    connect(m_actClipOff, &QAction::triggered, this, &DbusViewerWindow::onClipboardOff);

    m_actClipToHost = new QAction(i18n("Guest -> Host"), this);
    m_actClipToHost->setCheckable(true);
    clipGroup->addAction(m_actClipToHost);
    connect(m_actClipToHost, &QAction::triggered, this, &DbusViewerWindow::onClipboardToHost);

    m_actClipToGuest = new QAction(i18n("Host -> Guest"), this);
    m_actClipToGuest->setCheckable(true);
    clipGroup->addAction(m_actClipToGuest);
    connect(m_actClipToGuest, &QAction::triggered, this, &DbusViewerWindow::onClipboardToGuest);

    m_actClipBoth = new QAction(i18n("<-> Bidirectional"), this);
    m_actClipBoth->setCheckable(true);
    clipGroup->addAction(m_actClipBoth);
    connect(m_actClipBoth, &QAction::triggered, this, &DbusViewerWindow::onClipboardBoth);

    // -- Devices: Network ---
    m_actNetworkLink = new QAction(i18nc("@action:inmenu", "&Network Connected"), this);
    m_actNetworkLink->setCheckable(true);
    m_actNetworkLink->setIcon(QIcon::fromTheme(u"network-wired"_s));
    m_actNetworkLink->setToolTip(i18nc("@info:tooltip", "Plug or unplug the virtual network cable"));
    m_actNetworkLink->setChecked(m_prefs.networkLinkUp); // before connect(): no spurious toggle
    connect(m_actNetworkLink, &QAction::toggled, this, &DbusViewerWindow::onToggleNetworkLink);

    // --- Help ---
    m_actAbout = new QAction(i18n("&About..."), this);
    connect(m_actAbout, &QAction::triggered, this, &DbusViewerWindow::onAbout);
}

// =============================================================================
// Create Menus
// =============================================================================

void DbusViewerWindow::createMenus()
{
    // Machine
    QMenu *machineMenu = menuBar()->addMenu(i18n("&Machine"));
    machineMenu->addAction(m_actShutdown);
    machineMenu->addAction(m_actReset);
    machineMenu->addAction(m_actPowerOff);
    machineMenu->addSeparator();
    machineMenu->addAction(m_actSendCAD);
    machineMenu->addSeparator();
    machineMenu->addAction(m_actSnapshots);
    machineMenu->addSeparator();
    machineMenu->addAction(m_actQuit);

    // View
    QMenu *viewMenu = menuBar()->addMenu(i18n("&View"));
    viewMenu->addAction(m_actFullscreen);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actScaleToWindow);
    viewMenu->addAction(m_actResizeWindow);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actFitWindow);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actZoomIn);
    viewMenu->addAction(m_actZoomOut);
    viewMenu->addAction(m_actZoomReset);

    // Devices
    QMenu *devicesMenu = menuBar()->addMenu(i18n("&Devices"));

    devicesMenu->addAction(m_actAudioOut);
    devicesMenu->addAction(m_actAudioIn);
    devicesMenu->addSeparator();

    QMenu *mouseMenu = devicesMenu->addMenu(i18n("&Mouse"));
    mouseMenu->setToolTipsVisible(true);
    mouseMenu->addAction(m_actGrabMouse);

    devicesMenu->addSeparator();

    m_cdMenu = devicesMenu->addMenu(QIcon::fromTheme(u"media-optical"_s), i18n("&CD/DVD"));
    m_cdMenu->addAction(m_actChangeIso);
    m_cdMenu->addAction(m_actEjectIso);

    m_floppyMenu = devicesMenu->addMenu(QIcon::fromTheme(u"media-floppy"_s), i18n("&Floppy"));
    m_floppyMenu->addAction(m_actChangeFloppy);
    m_floppyMenu->addAction(m_actEjectFloppy);

    devicesMenu->addSeparator();

    // USB submenu (populated by MainWindow on aboutToShow)
    m_usbMenu = devicesMenu->addMenu(QIcon::fromTheme(u"drive-removable-media-usb"_s), i18n("&USB Devices"));
    m_usbMenu->setEnabled(m_hasUsbController);
    if (!m_hasUsbController) {
        auto *a = m_usbMenu->addAction(i18n("No USB controller configured"));
        a->setEnabled(false);
    }
    devicesMenu->addSeparator();

    m_clipMenu = devicesMenu->addMenu(i18n("&Clipboard"));
    m_clipMenu->addAction(m_actClipOff);
    m_clipMenu->addAction(m_actClipToHost);
    m_clipMenu->addAction(m_actClipToGuest);
    m_clipMenu->addAction(m_actClipBoth);

    devicesMenu->addSeparator();
    devicesMenu->addAction(m_actNetworkLink);

    // Help
    QMenu *helpMenu = menuBar()->addMenu(i18n("&Help"));
    helpMenu->addAction(m_actAbout);
}

// =============================================================================
// Status Bar
// =============================================================================

void DbusViewerWindow::createStatusBar()
{
    m_lblResolution = new QLabel(u"–"_s, this);
    m_lblResolution->setAlignment(Qt::AlignCenter);
    m_lblResolution->setAccessibleName(i18nc("@info:whatsthis", "Guest resolution"));
    m_lblResolution->setToolTip(i18nc("@info:tooltip", "Resolution reported by the guest"));

    // Secondary information, and setForegroundRole() follows colour scheme
    // changes where a hand-patched palette would go stale.
    m_lblResolution->setForegroundRole(QPalette::PlaceholderText);

    // Fixed width from the widest plausible value: a mode switch must not
    // reflow the whole bar.
    m_lblResolution->setFixedWidth(m_lblResolution->fontMetrics().horizontalAdvance(u"8888 × 8888"_s) + 8);

    m_indCdrom = new StatusIndicator(this);
    m_indCdrom->setIconNames(u"media-optical"_s, u"media-optical"_s);
    m_indCdrom->setAccessibleName(i18nc("@info:whatsthis", "CD/DVD drive"));
    m_indCdrom->setPopupMenu(m_cdMenu);

    m_indFloppy = new StatusIndicator(this);
    m_indFloppy->setIconNames(u"media-floppy"_s, u"media-floppy"_s);
    m_indFloppy->setAccessibleName(i18nc("@info:whatsthis", "Floppy drive"));
    m_indFloppy->setPopupMenu(m_floppyMenu);

    m_indUsb = new StatusIndicator(this);
    m_indUsb->setIconNames(u"drive-removable-media-usb"_s, u"drive-removable-media-usb"_s);
    m_indUsb->setAccessibleName(i18nc("@info:whatsthis", "USB devices"));
    m_indUsb->setPopupMenu(m_usbMenu);

    m_indNetwork = new StatusIndicator(this);
    m_indNetwork->setIconNames(u"network-wired-activated"_s, u"network-disconnect"_s);
    m_indNetwork->setAccessibleName(i18nc("@info:whatsthis", "Network"));
    m_indNetwork->setVisible(false); // until setNetworkState() says otherwise
    // trigger() is a no-op while the action is disabled, which is exactly the
    // guard we want during a pending QMP link change.
    connect(m_indNetwork, &StatusIndicator::activated, m_actNetworkLink, &QAction::trigger);

    m_indClipboard = new StatusIndicator(this);
    m_indClipboard->setIconNames(u"edit-paste"_s, u"edit-paste"_s);
    m_indClipboard->setAccessibleName(i18nc("@info:whatsthis", "Clipboard sharing"));
    m_indClipboard->setPopupMenu(m_clipMenu);

    m_indAudioIn = new StatusIndicator(this);
    m_indAudioIn->setIconNames(u"audio-input-microphone"_s, u"audio-input-microphone-muted"_s);
    m_indAudioIn->setAccessibleName(i18nc("@info:whatsthis", "Microphone"));
    connect(m_indAudioIn, &StatusIndicator::activated, m_actAudioIn, &QAction::trigger);

    m_indAudioOut = new StatusIndicator(this);
    m_indAudioOut->setIconNames(u"audio-volume-high"_s, u"audio-volume-muted"_s);
    m_indAudioOut->setAccessibleName(i18nc("@info:whatsthis", "Audio output"));
    connect(m_indAudioOut, &StatusIndicator::activated, m_actAudioOut, &QAction::trigger);

    m_indMouse = new StatusIndicator(this);
    m_indMouse->setIconNames(u"input-mouse"_s, u"input-mouse"_s);
    m_indMouse->setAccessibleName(i18nc("@info:whatsthis", "Pointer"));
    connect(m_indMouse, &StatusIndicator::activated, m_actGrabMouse, &QAction::trigger);

    statusBar()->addPermanentWidget(m_lblResolution);
    statusBar()->addPermanentWidget(m_indCdrom);
    statusBar()->addPermanentWidget(m_indFloppy);
    statusBar()->addPermanentWidget(m_indUsb);
    statusBar()->addPermanentWidget(m_indNetwork);
    statusBar()->addPermanentWidget(m_indClipboard);
    statusBar()->addPermanentWidget(m_indAudioIn);
    statusBar()->addPermanentWidget(m_indAudioOut);
    statusBar()->addPermanentWidget(m_indMouse);

    updateStatusBar();
    updateMouseStatus();
    statusBar()->showMessage(i18n("Ready - connected to %1", m_consolePath), 5000);
}

void DbusViewerWindow::updateStatusBar()
{
    // Audio output
    if (!m_audioOutConnected) {
        m_indAudioOut->setStatus(StatusIndicator::Unavailable,
                                 i18nc("@info:tooltip",
                                       "Audio output: system audio.\n"
                                       "D-Bus audio not available - start QEMU with "
                                       "-audiodev dbus,id=snd0 -machine audiodev=snd0"));
    } else if (m_actAudioOut->isChecked()) {
        m_indAudioOut->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Audio output active - click to mute"));
    } else {
        m_indAudioOut->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "Audio output muted - click to unmute"));
    }

    // Audio input
    if (!m_audioInConnected) {
        m_indAudioIn->setStatus(StatusIndicator::Unavailable,
                                i18nc("@info:tooltip",
                                      "Microphone: system audio.\n"
                                      "D-Bus audio not available - start QEMU with "
                                      "-audiodev dbus,id=snd0 -machine audiodev=snd0"));
    } else if (m_actAudioIn->isChecked()) {
        m_indAudioIn->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Microphone forwarding active - click to mute"));
    } else {
        m_indAudioIn->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "Microphone muted - click to unmute"));
    }

    // Removable media
    m_indCdrom->setVisible(m_hasCdDrive);
    if (m_hasCdDrive) {
        if (m_hasIso) {
            m_indCdrom->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "CD/DVD: %1", m_isoName));
        } else {
            m_indCdrom->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "CD/DVD drive empty - click to insert an image"));
        }
    }

    m_indFloppy->setVisible(m_hasFloppyDrive);
    if (m_hasFloppyDrive) {
        if (m_hasFloppy) {
            m_indFloppy->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Floppy: %1", m_floppyName));
        } else {
            m_indFloppy->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "Floppy drive empty - click to insert an image"));
        }
    }

    // Clipboard. Emblem convention: down = towards the host, up = towards the
    // guest; bidirectional needs no arrow.
    switch (m_clipDirection) {
    case ClipboardDirection::Off:
        m_indClipboard->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "Clipboard sharing off - click to change"));
        break;
    case ClipboardDirection::ToHost:
        m_indClipboard->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Clipboard: guest to host"), u"go-down"_s);
        break;
    case ClipboardDirection::ToGuest:
        m_indClipboard->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Clipboard: host to guest"), u"go-up"_s);
        break;
    case ClipboardDirection::Both:
        m_indClipboard->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Clipboard: bidirectional"));
        break;
    }

    // Network
    m_indNetwork->setVisible(m_hasNic);
    if (m_hasNic) {
        if (m_prefs.networkLinkUp) {
            m_indNetwork->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "%1 - cable plugged in", m_nicLabel));
        } else {
            m_indNetwork->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "%1 - cable unplugged", m_nicLabel));
        }
    }

    // USB. The menu fills itself on aboutToShow; the indicator only needs
    // the count MainWindow hands us.
    m_indUsb->setVisible(m_hasUsbController);
    if (m_hasUsbController) {
        if (m_usbAttached > 0) {
            m_indUsb->setStatus(StatusIndicator::Active, i18ncp("@info:tooltip", "%1 USB device attached", "%1 USB devices attached", m_usbAttached));
        } else {
            m_indUsb->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "No USB devices attached - click to attach"));
        }
    }
}

void DbusViewerWindow::popupIndicatorMenu(StatusIndicator *indicator, QMenu *menu)
{
    if (!indicator || !menu) {
        return;
    }

    // Open upwards: the indicator sits at the bottom edge of the window.
    const QSize hint = menu->sizeHint();
    menu->popup(indicator->mapToGlobal(QPoint(indicator->width() / 2 - hint.width() / 2, -hint.height())));
}

void DbusViewerWindow::showOsd(const QString &text, int holdMs)
{
    if (m_osd) {
        m_osd->showMessage(text, holdMs);
    }
}

// =============================================================================
// Slots: View
// =============================================================================

void DbusViewerWindow::onToggleFullscreen(bool checked)
{
    if (checked) {
        showFullScreen();
    } else {
        showNormal();
    }
    Q_EMIT inhibitStateChanged();
}

void DbusViewerWindow::onZoomIn()
{
    m_scale += 0.25f;
    if (m_scale > 5.0f) {
        m_scale = 5.0f; // Cap zoom level to prevent excessive sizes
    }
    updateWindowAndDisplaySize();
}

void DbusViewerWindow::onZoomOut()
{
    m_scale -= 0.25f;
    if (m_scale < 0.5f) {
        m_scale = 0.5f; // Minimum zoom level
    }
    updateWindowAndDisplaySize();
}

void DbusViewerWindow::onZoomReset()
{
    m_scale = 1.0f;
    updateWindowAndDisplaySize();
}

void DbusViewerWindow::onFitWindowToDisplay()
{
    if (m_scanoutSize.width() <= 0 || m_scanoutSize.height() <= 0) {
        statusBar()->showMessage(i18n("Guest display size not known yet"), 3000);
        return;
    }

    const bool wasFullscreen = isFullScreen();
    if (wasFullscreen) {
        // Triggers toggled() -> onToggleFullscreen(false) -> showNormal().
        m_actFullscreen->setChecked(false);
    }

    // Auto-scaling has no user scale to preserve, so 100% is the only
    // meaningful target. The other modes keep whatever zoom is set.
    if (m_contentFitsWindow) {
        m_scale = 1.0f;
    }

    auto doResize = [this] {
        fitWindowToContent(true);
    };

    if (wasFullscreen) {
        QTimer::singleShot(0, this, doResize);
    } else {
        doResize();
    }
}

void DbusViewerWindow::onToggleScaleToWindow(bool checked)
{
    m_contentFitsWindow = checked;
    // Auto-scaling owns the window size, so the resize option steps aside
    // while it is on and returns to its own setting afterwards.
    m_windowFitsContent = !checked && m_actResizeWindow->isChecked();
    m_actResizeWindow->setEnabled(!checked);

    updateViewActions();
    m_lastWindowSize = {0, 0}; // force a resize on the next update
    updateWindowAndDisplaySize();
}

void DbusViewerWindow::onToggleResizeWindow(bool checked)
{
    m_windowFitsContent = checked && !m_contentFitsWindow;
    m_lastWindowSize = {0, 0};
    updateWindowAndDisplaySize();
}

// =============================================================================
// Slots: Audio
// =============================================================================
// GDBus registration with QEMU is one-time and persists.
// The toggle controls our mute layer: when muted, Write() data is
// discarded (output) or Read() returns silence (input).

void DbusViewerWindow::onToggleAudioOut(bool checked)
{
    if (m_audioOut) {
        m_audioOut->setMuted(!checked);
        qCDebug(KEMU_DBUSDISPLAY) << "Audio output:" << (checked ? "ON" : "MUTED");
    }
    m_prefs.audioOut = checked;
    updateStatusBar();
    Q_EMIT devicePrefsChanged(m_prefs);
}

void DbusViewerWindow::onToggleAudioIn(bool checked)
{
    if (m_audioIn) {
        m_audioIn->setMuted(!checked);
        qCDebug(KEMU_DBUSDISPLAY) << "Audio input:" << (checked ? "ON" : "MUTED");
    }
    m_prefs.audioIn = checked;
    updateStatusBar();
    Q_EMIT devicePrefsChanged(m_prefs);
}

// =============================================================================
// Slots: Network
// =============================================================================

void DbusViewerWindow::onToggleNetworkLink(bool checked)
{
    // Re-enabled by setNetworkLinkState() once QMP has answered.
    m_actNetworkLink->setEnabled(false);
    Q_EMIT networkLinkChangeRequested(checked);
}

// =============================================================================
// Slots: Clipboard
// =============================================================================

void DbusViewerWindow::onClipboardOff()
{
    teardownClipboard();
    m_clipDirection = ClipboardDirection::Off;
    m_prefs.clipboard = ClipboardDirection::Off;
    updateStatusBar();
    statusBar()->showMessage(i18n("Clipboard disabled"), 3000);
    Q_EMIT devicePrefsChanged(m_prefs);
}

void DbusViewerWindow::onClipboardToHost()
{
    setupClipboard(ClipboardDirection::ToHost);
}

void DbusViewerWindow::onClipboardToGuest()
{
    setupClipboard(ClipboardDirection::ToGuest);
}

void DbusViewerWindow::onClipboardBoth()
{
    setupClipboard(ClipboardDirection::Both);
}

void DbusViewerWindow::setupClipboard(ClipboardDirection dir)
{
    teardownClipboard();
    m_clipDirection = dir; // effective state (may revert on failure)
    m_prefs.clipboard = dir; // intended state (persisted)
    Q_EMIT devicePrefsChanged(m_prefs);

    m_clipboard = new QemuClipboardManager(m_ctrl, dir, this);
    if (!m_clipboard->setup()) {
        statusBar()->showMessage(i18n("Clipboard registration failed – QEMU needs a clipboard agent in the guest"), 5000);
        delete m_clipboard;
        m_clipboard = nullptr;
        m_clipDirection = ClipboardDirection::Off;
        m_actClipOff->setChecked(true);
    } else {
        QString dirStr;
        switch (dir) {
        case ClipboardDirection::ToHost:
            dirStr = i18n("Guest -> Host");
            break;
        case ClipboardDirection::ToGuest:
            dirStr = i18n("Host -> Guest");
            break;
        case ClipboardDirection::Both:
            dirStr = i18n("<-> Bidirectional");
            break;
        default:
            break;
        }
        statusBar()->showMessage(i18n("Clipboard enabled: %1", dirStr), 3000);
    }

    updateStatusBar();
}

void DbusViewerWindow::teardownClipboard()
{
    if (m_clipboard) {
        delete m_clipboard;
        m_clipboard = nullptr;
    }
}

void DbusViewerWindow::setSuspended(bool suspended)
{
    if (m_suspended == suspended)
        return;
    m_suspended = suspended;

    if (m_display)
        m_display->setDimmed(suspended);

    if (suspended) {
        // Sticky message (timeout 0). Cleared explicitly when we resume.
        statusBar()->showMessage(i18n("VM suspended - saving state..."));
    } else {
        statusBar()->clearMessage();
    }

    if (m_actNetworkLink)
        m_actNetworkLink->setEnabled(m_hasNic && !suspended);
}

// =============================================================================
// Slots: Scanout
// =============================================================================

void DbusViewerWindow::onScanoutChanged(int width, int height)
{
    if (width > 0 && height > 0) {
        // Only on a real change: QEMU repeats the scanout on every mode probe.
        if (m_scanoutSize != QSize(width, height)) {
            m_scanoutSize = {width, height};
            m_lblResolution->setText(u"%1 × %2"_s.arg(width).arg(height));
            m_lblResolution->setToolTip(i18nc("@info:tooltip", "Resolution reported by the guest"));
            updateWindowAndDisplaySize();
        }
    } else {
        m_scanoutSize = {};
        m_lblResolution->setText(u"–"_s);
        m_lblResolution->setToolTip(i18nc("@info:tooltip", "No display output from the guest"));
    }
}

// =============================================================================
// About Dialog
// =============================================================================

void DbusViewerWindow::onAbout()
{
    KAboutApplicationDialog dialog(KAboutData::applicationData(), this);
    dialog.exec();
}

void DbusViewerWindow::updateMouseStatus()
{
    if (!m_indMouse) {
        return;
    }

    if (!m_display->isGuestAbsolute() && !m_display->isPointerCaptureSupported()) {
        m_indMouse->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "Relative pointer - capture requires a Wayland session"), u"object-locked"_s);
        return;
    }

    if (m_display->isPointerCaptured()) {
        m_indMouse->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Pointer captured - press Ctrl+Alt+G to release"), u"object-locked"_s);
        return;
    }

    if (m_display->isGuestAbsolute()) {
        m_indMouse->setStatus(StatusIndicator::Active, i18nc("@info:tooltip", "Absolute pointer - host and guest cursor are synced"));
    } else {
        m_indMouse->setStatus(StatusIndicator::Inactive, i18nc("@info:tooltip", "Pointer released - click into the display to capture"));
    }
}

void DbusViewerWindow::onPointerCaptureChanged(bool active)
{
    const bool wasChecked = m_actGrabMouse && m_actGrabMouse->isChecked();

    if (m_actGrabMouse) {
        QSignalBlocker block(m_actGrabMouse);
        m_actGrabMouse->setChecked(active);
    }

    if (active) {
        showOsd(i18nc("@info:status", "Pointer captured - press Ctrl+Alt+G to release"), 3000);
    } else if (wasChecked) {
        // Only report a release if we were actually holding the pointer.
        showOsd(i18nc("@info:status", "Pointer released"), 1500);
    } else {
        showOsd(i18nc("@info:status", "Pointer capture is not available"), 2500);
    }

    updateMouseStatus();
    Q_EMIT inhibitStateChanged();
}

// =============================================================================
// Close Event
// =============================================================================

void DbusViewerWindow::closeEvent(QCloseEvent *event)
{
    teardownClipboard();
    Q_EMIT viewerClosed();
    event->accept();
}

// =============================================================================
// Change Event
// =============================================================================

void DbusViewerWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange) {
        const auto *stateEvent = static_cast<QWindowStateChangeEvent *>(event);
        const bool wasFullscreen = stateEvent->oldState() & Qt::WindowFullScreen;

        if (windowState() & Qt::WindowFullScreen) {
            menuBar()->hide();
            statusBar()->hide();
            {
                QSignalBlocker block(m_actFullscreen);
                m_actFullscreen->setChecked(true);
            }

            // Covers the window manager's own fullscreen path too, not just
            // our action. Skipped when only an unrelated state bit changed,
            // for example on restore from minimised.
            if (!wasFullscreen) {
                showFullscreenHint();
            }

            auto doResize = [this]() {
                updateWindowAndDisplaySize();
            };
            QTimer::singleShot(100, this, doResize); // context object: cancels on destruction
        } else {
            menuBar()->show();
            statusBar()->show();
            {
                QSignalBlocker block(m_actFullscreen);
                m_actFullscreen->setChecked(false);
            }
            updateWindowAndDisplaySize();
        }
        Q_EMIT inhibitStateChanged();
    }
    QWidget::changeEvent(event); // Always call the base class implementation
}

void DbusViewerWindow::resizeEvent(QResizeEvent *event)
{
    if (m_contentFitsWindow) {
        fitContentToWindow();
    }
    QWidget::resizeEvent(event);
}

// =============================================================================
// Public Method: Set clipboard mode (e.g., from CLI)
// =============================================================================

void DbusViewerWindow::setClipboardMode(ClipboardDirection dir)
{
    switch (dir) {
    case ClipboardDirection::Off:
        m_actClipOff->setChecked(true);
        onClipboardOff();
        break;
    case ClipboardDirection::ToHost:
        m_actClipToHost->setChecked(true);
        onClipboardToHost();
        break;
    case ClipboardDirection::ToGuest:
        m_actClipToGuest->setChecked(true);
        onClipboardToGuest();
        break;
    case ClipboardDirection::Both:
        m_actClipBoth->setChecked(true);
        onClipboardBoth();
        break;
    }
}

// =============================================================================
// QEMU Termination
// =============================================================================
void DbusViewerWindow::onQemuDisconnected()
{
    // The listeners have already stopped their own dispatching.
    // Delayed close – we might still be inside a GLib callback.
    // Calling close() directly would destroy Qt widgets from within a GLib stack
    // frame.
    QTimer::singleShot(0, this, &QWidget::close);
}

// =============================================================================
// Subsequent Audio Activation (asynchronous)
// =============================================================================

void DbusViewerWindow::audioOutReady()
{
    m_audioOutConnected = true;
    m_actAudioOut->setEnabled(true);

    if (m_audioOut)
        m_audioOut->setMuted(!m_prefs.audioOut);
    m_actAudioOut->setChecked(m_prefs.audioOut);

    m_actAudioOut->setStatusTip(i18n("Toggle audio output (playback)"));

    updateStatusBar();
    statusBar()->showMessage(i18n("Audio output connected"), 3000);
    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow: Audio output activated subsequently";
}

void DbusViewerWindow::audioInReady()
{
    m_audioInConnected = true;
    m_actAudioIn->setEnabled(true);

    if (m_audioIn)
        m_audioIn->setMuted(!m_prefs.audioIn);
    m_actAudioIn->setChecked(m_prefs.audioIn);

    m_actAudioIn->setStatusTip(i18n("Toggle audio input (microphone)"));

    updateStatusBar();
    statusBar()->showMessage(i18n("Audio input connected"), 3000);
    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow: Audio input activated subsequently";
}

void DbusViewerWindow::setMediaState(bool hasIso, const QString &isoName, bool hasFloppy, const QString &floppyName)
{
    m_hasIso = hasIso;
    m_isoName = isoName;
    m_hasFloppy = hasFloppy;
    m_floppyName = floppyName;

    if (m_actEjectIso)
        m_actEjectIso->setEnabled(hasIso);
    if (m_actEjectFloppy)
        m_actEjectFloppy->setEnabled(hasFloppy);

    updateStatusBar();
}

void DbusViewerWindow::setDriveAvailability(bool hasCdDrive, bool hasFloppyDrive)
{
    m_hasCdDrive = hasCdDrive;
    m_hasFloppyDrive = hasFloppyDrive;
    updateStatusBar();
}

void DbusViewerWindow::setNetworkState(const QString &nicLabel, bool linkUp)
{
    m_nicLabel = nicLabel;
    m_hasNic = !nicLabel.isEmpty();
    m_prefs.networkLinkUp = linkUp;

    if (m_actNetworkLink) {
        QSignalBlocker block(m_actNetworkLink);
        m_actNetworkLink->setVisible(m_hasNic);
        m_actNetworkLink->setChecked(m_hasNic && linkUp);
        m_actNetworkLink->setEnabled(m_hasNic && !m_suspended);
    }

    updateStatusBar();
}

void DbusViewerWindow::setNetworkLinkState(bool up, bool persist)
{
    m_prefs.networkLinkUp = up;

    if (m_actNetworkLink) {
        QSignalBlocker block(m_actNetworkLink); // a revert must not re-fire the request
        m_actNetworkLink->setChecked(up);
        m_actNetworkLink->setEnabled(m_hasNic && !m_suspended);
    }

    updateStatusBar();
    if (persist)
        Q_EMIT devicePrefsChanged(m_prefs);
}

void DbusViewerWindow::setUsbState(int attachedCount)
{
    m_usbAttached = attachedCount;
    updateStatusBar();
}

void DbusViewerWindow::updateWindowAndDisplaySize()
{
    if (isFullScreen() || isMaximized()) {
        // We always scale the content to fit the window, because that's probably
        // why the user entered these modes.
        fitContentToWindow();
    } else if (m_contentFitsWindow) {
        fitContentToWindow();
    } else if (m_windowFitsContent) {
        fitWindowToContent(false);
    } else {
        m_display->setScale(m_scale);
    }
}

void DbusViewerWindow::fitContentToWindow()
{
    if (m_scanoutSize.isEmpty())
        return; // no scanout yet (or display disabled) - nothing to fit
    QSize availableSize = m_scrollArea->size() * devicePixelRatio();
    qreal scale = qMin(qreal(availableSize.width()) / qreal(m_scanoutSize.width()), qreal(availableSize.height()) / m_scanoutSize.height());
    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow::fitContentToWindow" << availableSize << m_scanoutSize << scale;
    m_display->setScale(scale);
}

void DbusViewerWindow::fitWindowToContent(bool force)
{
    m_display->setScale(m_scale);

    int menuH = menuBar()->isVisible() ? menuBar()->height() : 0;
    int statusH = statusBar()->isVisible() ? statusBar()->height() : 0;
    int targetW = qMax(m_display->width(), 720); // Leave room for the status bar
    int targetH = m_display->height() + menuH + statusH;
    QSize windowSize = {targetW, targetH};

    // Prevent fighting with tiling window managers: Only resize if the target size actually changes.
    if (force || windowSize != m_lastWindowSize) {
        resize(windowSize);
        m_lastWindowSize = windowSize;
    }
}

void DbusViewerWindow::updateViewActions()
{
    const bool zoomable = !m_contentFitsWindow;
    m_actZoomIn->setEnabled(zoomable);
    m_actZoomOut->setEnabled(zoomable);
    m_actZoomReset->setEnabled(zoomable);

    // A one-shot fit is pointless while the window follows the display anyway.
    m_actFitWindow->setEnabled(!m_windowFitsContent);
}

void DbusViewerWindow::showFullscreenHint()
{
    // Read the key off the action: the hint exists only to name it, so it
    // must not be able to go stale.
    const QString key = m_actFullscreen->shortcut().toString(QKeySequence::NativeText);
    if (key.isEmpty()) {
        return;
    }

    showOsd(i18nc("@info:status", "Press %1 to leave fullscreen", key), 3500);
}
