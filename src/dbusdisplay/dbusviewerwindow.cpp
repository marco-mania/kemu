// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbusviewerwindow.h"

#include "kemu_dbusdisplay_debug.h"

// =============================================================================
// Constructor
// =============================================================================

DbusViewerWindow::DbusViewerWindow(QemuDisplayListener *listener,
                                   QDBusConnection &bus,
                                   const QString &consolePath,
                                   QemuAudioListener *audioOut,
                                   QemuAudioInListener *audioIn,
                                   bool audioOutConnected,
                                   bool audioInConnected,
                                   bool hasUsbController,
                                   QWidget *parent)
    : QMainWindow(parent)
    , m_bus(bus)
    , m_consolePath(consolePath)
    , m_listener(listener)
    , m_audioOut(audioOut)
    , m_audioIn(audioIn)
    , m_audioOutConnected(audioOutConnected)
    , m_audioInConnected(audioInConnected)
    , m_hasUsbController(hasUsbController)
    , m_clipDirection(ClipboardDirection::Off)
{
    // Display widget as the central widget
    m_display = new QemuDisplayWidget(listener, bus, consolePath, this);
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidget(m_display);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    setCentralWidget(m_scrollArea);

    createActions();
    createMenus();
    createStatusBar();

    // Resolution updates – both paths (Software + DMA-BUF)
    connect(listener, &QemuDisplayListener::scanoutChanged, this, &DbusViewerWindow::onScanoutChanged);
    connect(listener, &QemuDisplayListener::dmaBufScanout, this, &DbusViewerWindow::onScanoutChanged);

    // QEMU termination: Stop notifiers immediately, then close with delay
    connect(listener, &QemuDisplayListener::connectionLost, this, &DbusViewerWindow::onQemuDisconnected);

    connect(m_display, &QemuDisplayWidget::fullscreenToggleRequested, m_actFullscreen, &QAction::trigger);

    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow: audioOutConnected=" << m_audioOutConnected << "audioInConnected=" << m_audioInConnected;

    resize(m_lastWindowSize);
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

    m_actNativeSize = new QAction(QIcon::fromTheme(u"zoom-original"_s), i18n("&Reset to Native Size"), this);
    m_actNativeSize->setShortcut(QKeySequence(u"Ctrl+Shift+R"_s));
    m_actNativeSize->setStatusTip(i18n("Resize the window to the guest's native resolution"));
    connect(m_actNativeSize, &QAction::triggered, this, &DbusViewerWindow::onResetToNativeSize);

    m_windowActionGroup = new QActionGroup(this);
    m_windowActionGroup->setExclusive(true);
    connect(m_windowActionGroup, &QActionGroup::triggered, this, &DbusViewerWindow::onToggleWindowMode);

    m_actWindowFitsContent = new QAction(i18n("Window Fits &Content"), m_windowActionGroup);
    m_actWindowFitsContent->setCheckable(true);
    m_actWindowFitsContent->setChecked(true);
    m_actWindowFitsContent->setStatusTip(i18n("Fit window size to content"));

    m_actContentFitsWindow = new QAction(i18n("&Content Fits Window"), m_windowActionGroup);
    m_actContentFitsWindow->setCheckable(true);
    m_actContentFitsWindow->setStatusTip(i18n("Fit content to window size"));

    m_actManualZoom = new QAction(i18n("&Manual Zoom"), m_windowActionGroup);
    m_actManualZoom->setCheckable(true);
    m_actManualZoom->setStatusTip(i18n("Control zoom level and window size manually"));

    m_actZoomIn = new QAction(i18n("Zoom &In"), this);
    m_actZoomIn->setShortcut(QKeySequence::ZoomIn);
    m_actZoomIn->setStatusTip(i18n("Increase zoom level"));
    connect(m_actZoomIn, &QAction::triggered, this, &DbusViewerWindow::onZoomIn);

    m_actZoomOut = new QAction(i18n("Zoom &Out"), this);
    m_actZoomOut->setShortcut(QKeySequence::ZoomOut);
    m_actZoomOut->setStatusTip(i18n("Decrease zoom level"));
    connect(m_actZoomOut, &QAction::triggered, this, &DbusViewerWindow::onZoomOut);

    m_actZoomReset = new QAction(i18n("&Reset Zoom"), this);
    m_actZoomReset->setStatusTip(i18n("Reset zoom level to 100%"));
    connect(m_actZoomReset, &QAction::triggered, this, &DbusViewerWindow::onZoomReset);

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
    machineMenu->addAction(m_actSnapshots);
    machineMenu->addSeparator();
    machineMenu->addAction(m_actQuit);

    // View
    QMenu *viewMenu = menuBar()->addMenu(i18n("&View"));
    viewMenu->addAction(m_actFullscreen);
    viewMenu->addAction(m_actNativeSize);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actWindowFitsContent);
    viewMenu->addAction(m_actContentFitsWindow);
    viewMenu->addAction(m_actManualZoom);
    viewMenu->addSeparator();
    viewMenu->addAction(m_actZoomIn);
    viewMenu->addAction(m_actZoomOut);
    viewMenu->addAction(m_actZoomReset);

    // Devices
    QMenu *devicesMenu = menuBar()->addMenu(i18n("&Devices"));

    devicesMenu->addAction(m_actAudioOut);
    devicesMenu->addAction(m_actAudioIn);
    devicesMenu->addSeparator();

    // USB submenu (populated by MainWindow on aboutToShow)
    m_usbMenu = devicesMenu->addMenu(QIcon::fromTheme(u"drive-removable-media-usb"_s), i18n("&USB Devices"));
    m_usbMenu->setEnabled(m_hasUsbController);
    if (!m_hasUsbController) {
        auto *a = m_usbMenu->addAction(i18n("No USB controller configured"));
        a->setEnabled(false);
    }
    devicesMenu->addSeparator();

    QMenu *clipMenu = devicesMenu->addMenu(i18n("&Clipboard"));
    clipMenu->addAction(m_actClipOff);
    clipMenu->addAction(m_actClipToHost);
    clipMenu->addAction(m_actClipToGuest);
    clipMenu->addAction(m_actClipBoth);

    // Help
    QMenu *helpMenu = menuBar()->addMenu(i18n("&Help"));
    helpMenu->addAction(m_actAbout);
}

// =============================================================================
// Status Bar
// =============================================================================

void DbusViewerWindow::createStatusBar()
{
    m_lblResolution = new QLabel(i18n("No Display"));
    m_lblResolution->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_lblResolution->setMinimumWidth(140);
    m_lblResolution->setAlignment(Qt::AlignCenter);

    m_lblAudioOut = new QLabel;
    m_lblAudioOut->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_lblAudioOut->setMinimumWidth(100);
    m_lblAudioOut->setAlignment(Qt::AlignCenter);

    m_lblAudioIn = new QLabel;
    m_lblAudioIn->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_lblAudioIn->setMinimumWidth(100);
    m_lblAudioIn->setAlignment(Qt::AlignCenter);

    m_lblClipboard = new QLabel;
    m_lblClipboard->setFrameStyle(QFrame::Panel | QFrame::Sunken);
    m_lblClipboard->setMinimumWidth(120);
    m_lblClipboard->setAlignment(Qt::AlignCenter);

    statusBar()->addPermanentWidget(m_lblResolution);
    statusBar()->addPermanentWidget(m_lblAudioOut);
    statusBar()->addPermanentWidget(m_lblAudioIn);
    statusBar()->addPermanentWidget(m_lblClipboard);

    updateStatusBar();
    statusBar()->showMessage(i18n("Ready - connected to %1", m_consolePath), 5000);
}

void DbusViewerWindow::updateStatusBar()
{
    // Audio output
    if (!m_audioOutConnected) {
        m_lblAudioOut->setText(i18n("Sound: System"));
        m_lblAudioOut->setToolTip(
            i18n("D-Bus audio not available – QEMU uses system audio directly.\n"
                 "For D-Bus control: -audiodev dbus,id=snd0 -machine audiodev=snd0"));
    } else if (m_actAudioOut->isChecked()) {
        m_lblAudioOut->setText(i18n("Sound: ON"));
        m_lblAudioOut->setToolTip(i18n("Audio output via D-Bus active"));
    } else {
        m_lblAudioOut->setText(i18n("Sound: OFF"));
        m_lblAudioOut->setToolTip(i18n("Audio output muted"));
    }

    // Audio input
    if (!m_audioInConnected) {
        m_lblAudioIn->setText(i18n("Mic: System"));
        m_lblAudioIn->setToolTip(
            i18n("D-Bus microphone not available – QEMU uses system audio directly.\n"
                 "For D-Bus control: -audiodev dbus,id=snd0 -machine audiodev=snd0"));
    } else if (m_actAudioIn->isChecked()) {
        m_lblAudioIn->setText(i18n("Mic: ON"));
        m_lblAudioIn->setToolTip(i18n("Microphone forwarding via D-Bus active"));
    } else {
        m_lblAudioIn->setText(i18n("Mic: OFF"));
        m_lblAudioIn->setToolTip(i18n("Microphone forwarding disabled"));
    }

    // Clipboard
    switch (m_clipDirection) {
    case ClipboardDirection::Off:
        m_lblClipboard->setText(i18n("Clipboard: OFF"));
        m_lblClipboard->setToolTip(i18n("Clipboard disabled"));
        break;
    case ClipboardDirection::ToHost:
        m_lblClipboard->setText(i18n("Clipboard: G→H"));
        m_lblClipboard->setToolTip(i18n("Clipboard: Guest → Host"));
        break;
    case ClipboardDirection::ToGuest:
        m_lblClipboard->setText(i18n("Clipboard: H→G"));
        m_lblClipboard->setToolTip(i18n("Clipboard: Host → Guest"));
        break;
    case ClipboardDirection::Both:
        m_lblClipboard->setText(i18n("Clipboard: BI"));
        m_lblClipboard->setToolTip(i18n("Clipboard: Bidirectional"));
        break;
    }
}

// =============================================================================
// Slots: View
// =============================================================================

void DbusViewerWindow::onToggleFullscreen(bool checked)
{
    if (checked) {
        // First-time hint; KMessageBox persists the user's choice in kemurc.
        KMessageBox::information(this, i18n("Press <b>F11</b> to leave fullscreen mode again."), i18n("Fullscreen Mode"), u"ShowFullscreenHint"_s);
        showFullScreen();
    } else {
        showNormal();
    }
}

void DbusViewerWindow::onToggleWindowMode(QAction *action)
{
    if (action == m_actManualZoom) {
        m_windowFitsContent = false;
        m_contentFitsWindow = false;
    } else if (action == m_actWindowFitsContent) {
        m_windowFitsContent = true;
        m_contentFitsWindow = false;
    } else if (action == m_actContentFitsWindow) {
        m_windowFitsContent = false;
        m_contentFitsWindow = true;
    }
    m_actZoomIn->setEnabled(!m_contentFitsWindow);
    m_actZoomOut->setEnabled(!m_contentFitsWindow);
    m_actZoomReset->setEnabled(!m_contentFitsWindow);
    m_lastWindowSize = {0, 0}; // Force resizing the window when changing modes
    updateWindowAndDisplaySize();
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

void DbusViewerWindow::onResetToNativeSize()
{
    if (m_scanoutSize.width() <= 0 || m_scanoutSize.height() <= 0) {
        statusBar()->showMessage(i18n("Native display size not yet known"), 3000);
        return;
    }

    const bool wasFullscreen = isFullScreen();
    if (wasFullscreen) {
        // Triggers toggled() -> onToggleFullscreen(false) -> showNormal().
        m_actFullscreen->setChecked(false);
    }

    if (m_contentFitsWindow) {
        m_scale = 1.0f; // Same as "Reset Zoom" in "Content Fits Window" mode
    }
    auto doResize = [this] {
        fitWindowToContent(true);
    };

    // Defer the resize until the window state transition has settled,
    // otherwise the WM may overwrite our geometry while leaving fullscreen.
    if (wasFullscreen) {
        QTimer::singleShot(0, this, doResize);
    } else {
        doResize();
    }
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
    updateStatusBar();
}

void DbusViewerWindow::onToggleAudioIn(bool checked)
{
    if (m_audioIn) {
        m_audioIn->setMuted(!checked);
        qCDebug(KEMU_DBUSDISPLAY) << "Audio input:" << (checked ? "ON" : "MUTED");
    }
    updateStatusBar();
}

// =============================================================================
// Slots: Clipboard
// =============================================================================

void DbusViewerWindow::onClipboardOff()
{
    teardownClipboard();
    m_clipDirection = ClipboardDirection::Off;
    updateStatusBar();
    statusBar()->showMessage(i18n("Clipboard disabled"), 3000);
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
    m_clipDirection = dir;

    m_clipboard = new QemuClipboardManager(m_bus, dir, this);
    if (!m_clipboard->setup()) {
        statusBar()->showMessage(i18n("Clipboard registration failed – "
                                      "QEMU needs a clipboard agent in the guest"),
                                 5000);
        delete m_clipboard;
        m_clipboard = nullptr;
        m_clipDirection = ClipboardDirection::Off;
        m_actClipOff->setChecked(true);
    } else {
        QString dirStr;
        switch (dir) {
        case ClipboardDirection::ToHost:
            dirStr = i18n("Guest → Host");
            break;
        case ClipboardDirection::ToGuest:
            dirStr = i18n("Host → Guest");
            break;
        case ClipboardDirection::Both:
            dirStr = i18n("↔ Bidirectional");
            break;
        default:
            break;
        }
        statusBar()->showMessage(i18n("Clipboard enabled: %1").arg(dirStr), 3000);
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
}

// =============================================================================
// Slots: Scanout
// =============================================================================

void DbusViewerWindow::onScanoutChanged(int width, int height)
{
    if (width > 0 && height > 0) {
        m_lblResolution->setText(u"%1 × %2"_s.arg(width).arg(height));
        if (m_scanoutSize != QSize(width, height)) {
            m_scanoutSize = {width, height};
            updateWindowAndDisplaySize();
        }
    } else {
        m_scanoutSize = {};
        m_lblResolution->setText(i18n("No Display"));
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
        // Check if the current state is FullScreen
        if (this->windowState() & Qt::WindowFullScreen) {
            menuBar()->hide();
            statusBar()->hide();
            m_actFullscreen->setChecked(true);
            auto doResize = [this]() {
                updateWindowAndDisplaySize();
            };
            QTimer::singleShot(100, doResize); // 0 doesn't work
        } else {
            menuBar()->show();
            statusBar()->show();
            m_actFullscreen->setChecked(false);
            updateWindowAndDisplaySize();
        }
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
    // IMMEDIATELY deactivate all socket notifiers – prevents further
    // g_main_context_iteration() calls on the dead connection.
    m_display->stopAllGLibWatching();

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
    m_actAudioOut->setChecked(true);
    m_actAudioOut->setStatusTip(i18n("Toggle audio output (playback)"));

    if (m_audioOut) {
        m_display->watchGLibFd(m_audioOut->socketFd(), m_audioOut->mainContext());
    }

    updateStatusBar();
    statusBar()->showMessage(i18n("Audio output connected"), 3000);
    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow: Audio output activated subsequently";
}

void DbusViewerWindow::audioInReady()
{
    m_audioInConnected = true;
    m_actAudioIn->setEnabled(true);
    m_actAudioIn->setChecked(true);
    m_actAudioIn->setStatusTip(i18n("Toggle audio input (microphone)"));

    if (m_audioIn) {
        m_display->watchGLibFd(m_audioIn->socketFd(),
                               m_audioIn->mainContext());    // NEU
    }

    updateStatusBar();
    statusBar()->showMessage(i18n("Audio input connected"), 3000);
    qCDebug(KEMU_DBUSDISPLAY) << "DbusViewerWindow: Audio input activated subsequently";
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
