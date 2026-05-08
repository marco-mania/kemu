// SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "dbusviewerwindow.h"

// =============================================================================
// Constructor
// =============================================================================

DbusViewerWindow::DbusViewerWindow(QemuDisplayListener* listener,
    QDBusConnection& bus,
    const QString& consolePath,
    QemuAudioListener* audioOut,
    QemuAudioInListener* audioIn,
    bool audioOutConnected,
    bool audioInConnected,
    QWidget* parent)
    : QMainWindow(parent)
    , m_bus(bus)
    , m_consolePath(consolePath)
    , m_listener(listener)
    , m_audioOut(audioOut)
    , m_audioIn(audioIn)
    , m_audioOutConnected(audioOutConnected)
    , m_audioInConnected(audioInConnected)
    , m_clipDirection(ClipboardDirection::Off)
{
    // Display widget as the central widget
    m_display = new QemuDisplayWidget(listener, bus, consolePath, this);
    setCentralWidget(m_display);

    createActions();
    createMenus();
    createStatusBar();

    // Resolution updates – both paths (Software + DMA-BUF)
    connect(listener, &QemuDisplayListener::scanoutChanged, this, &DbusViewerWindow::onScanoutChanged);
    connect(listener, &QemuDisplayListener::dmaBufScanout, this, &DbusViewerWindow::onScanoutChanged);

    // QEMU termination: Stop notifiers immediately, then close with delay
    connect(listener, &QemuDisplayListener::connectionLost, this, &DbusViewerWindow::onQemuDisconnected);

    qDebug() << "DbusViewerWindow: audioOutConnected=" << m_audioOutConnected
             << "audioInConnected=" << m_audioInConnected;

    resize(m_lastTargetW, m_lastTargetH);
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
    m_actQuit = new QAction(i18n("&Quit"), this);
    m_actQuit->setShortcut(QKeySequence::Quit);
    m_actQuit->setStatusTip(i18n("Quit application"));
    connect(m_actQuit, &QAction::triggered, this, &QWidget::close);

    // --- View ---
    m_actFullscreen = new QAction(i18n("&Fullscreen"), this);
    m_actFullscreen->setShortcut(QKeySequence(Qt::Key_F11));
    m_actFullscreen->setCheckable(true);
    m_actFullscreen->setStatusTip(i18n("Toggle fullscreen mode"));
    connect(m_actFullscreen, &QAction::toggled, this, &DbusViewerWindow::onToggleFullscreen);

    // --- Devices: Audio ---
    m_actAudioOut = new QAction(i18n("Audio &Output"), this);
    m_actAudioOut->setCheckable(true);
    m_actAudioOut->setChecked(m_audioOutConnected);
    m_actAudioOut->setEnabled(m_audioOutConnected);
    if (m_audioOutConnected) {
        m_actAudioOut->setStatusTip(i18n("Toggle audio output (playback)"));
    } else {
        m_actAudioOut->setStatusTip(i18n("Audio runs via system audio – start QEMU with -audiodev dbus for D-Bus control"));
    }
    connect(m_actAudioOut, &QAction::toggled, this, &DbusViewerWindow::onToggleAudioOut);

    m_actAudioIn = new QAction(i18n("Audio &Input (Microphone)"), this);
    m_actAudioIn->setCheckable(true);
    m_actAudioIn->setChecked(m_audioInConnected);
    m_actAudioIn->setEnabled(m_audioInConnected);
    if (m_audioInConnected) {
        m_actAudioIn->setStatusTip(i18n("Toggle audio input (microphone)"));
    } else {
        m_actAudioIn->setStatusTip(i18n("Microphone runs via system audio – start QEMU with -audiodev dbus for D-Bus control"));
    }
    connect(m_actAudioIn, &QAction::toggled, this, &DbusViewerWindow::onToggleAudioIn);

    // --- Devices: Clipboard ---
    auto* clipGroup = new QActionGroup(this);
    clipGroup->setExclusive(true);

    m_actClipOff = new QAction(i18n("Off"), this);
    m_actClipOff->setCheckable(true);
    m_actClipOff->setChecked(true);
    clipGroup->addAction(m_actClipOff);
    connect(m_actClipOff, &QAction::triggered, this, &DbusViewerWindow::onClipboardOff);

    m_actClipToHost = new QAction(i18n("Guest → Host"), this);
    m_actClipToHost->setCheckable(true);
    clipGroup->addAction(m_actClipToHost);
    connect(m_actClipToHost, &QAction::triggered, this, &DbusViewerWindow::onClipboardToHost);

    m_actClipToGuest = new QAction(i18n("Host → Guest"), this);
    m_actClipToGuest->setCheckable(true);
    clipGroup->addAction(m_actClipToGuest);
    connect(m_actClipToGuest, &QAction::triggered, this, &DbusViewerWindow::onClipboardToGuest);

    m_actClipBoth = new QAction(i18n("↔ Bidirectional"), this);
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
    QMenu* machineMenu = menuBar()->addMenu(i18n("&Machine"));
    machineMenu->addAction(m_actQuit);

    // View
    QMenu* viewMenu = menuBar()->addMenu(i18n("&View"));
    viewMenu->addAction(m_actFullscreen);

    // Devices
    QMenu* devicesMenu = menuBar()->addMenu(i18n("&Devices"));

    devicesMenu->addAction(m_actAudioOut);
    devicesMenu->addAction(m_actAudioIn);
    devicesMenu->addSeparator();

    QMenu* clipMenu = devicesMenu->addMenu(i18n("&Clipboard"));
    clipMenu->addAction(m_actClipOff);
    clipMenu->addAction(m_actClipToHost);
    clipMenu->addAction(m_actClipToGuest);
    clipMenu->addAction(m_actClipBoth);

    // Help
    QMenu* helpMenu = menuBar()->addMenu(i18n("&Help"));
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
    statusBar()->showMessage(i18n("Ready – connected to %1").arg(m_consolePath), 5000);
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
        menuBar()->hide();
        statusBar()->hide();
        showFullScreen();
    } else {
        showNormal();
        menuBar()->show();
        statusBar()->show();
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
        qDebug() << "Audio output:" << (checked ? "ON" : "MUTED");
    }
    updateStatusBar();
}

void DbusViewerWindow::onToggleAudioIn(bool checked)
{
    if (m_audioIn) {
        m_audioIn->setMuted(!checked);
        qDebug() << "Audio input:" << (checked ? "ON" : "MUTED");
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
        statusBar()->showMessage(
            i18n("Clipboard registration failed – "
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
        statusBar()->showMessage(
            i18n("Clipboard enabled: %1").arg(dirStr), 3000);
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

// =============================================================================
// Slots: Scanout
// =============================================================================

void DbusViewerWindow::onScanoutChanged(int width, int height)
{
    if (width > 0 && height > 0) {
        m_lblResolution->setText(u"%1 × %2"_s.arg(width).arg(height));

        // Adjust window size to framebuffer (+ menu/status bar)
        if (!isFullScreen()) {
            qreal dpr = devicePixelRatio();
            int menuH = menuBar()->isVisible() ? menuBar()->height() : 0;
            int statusH = statusBar()->isVisible() ? statusBar()->height() : 0;
            int targetW = qRound(width / dpr);
            int targetH = qRound(height / dpr) + menuH + statusH;

            // Prevent fighting with tiling window managers: Only resize if the target size actually changes.
            if (targetW != m_lastTargetW || targetH != m_lastTargetH) {
                resize(targetW, targetH);
                m_lastTargetW = targetW;
                m_lastTargetH = targetH;
            }
        }
    } else {
        m_lblResolution->setText(i18n("No Display"));
    }
}

// =============================================================================
// About Dialog
// =============================================================================

void DbusViewerWindow::onAbout()
{
    QMessageBox::about(this, i18n("About QEMU VM Viewer"),
        i18n("<h3>QEMU VM Viewer</h3>"
             "<p>A Qt6-based viewer for QEMU VMs "
             "via the D-Bus display protocol.</p>"
             "<p><b>Features:</b></p>"
             "<ul>"
             "<li>Framebuffer display (Software + DMA-BUF)</li>"
             "<li>Keyboard and mouse forwarding</li>"
             "<li>Audio output and input</li>"
             "<li>Clipboard synchronization</li>"
             "</ul>"
             "<p>Requires QEMU with <code>-display dbus</code>.</p>"));
}

// =============================================================================
// Close Event
// =============================================================================

void DbusViewerWindow::closeEvent(QCloseEvent* event)
{
    teardownClipboard();
    Q_EMIT viewerClosed();
    event->accept();
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
    // Calling close() directly would destroy Qt widgets from within a GLib stack frame.
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
        m_display->watchGLibFd(m_audioOut->socketFd());
    }

    updateStatusBar();
    statusBar()->showMessage(i18n("Audio output connected"), 3000);
    qDebug() << "DbusViewerWindow: Audio output activated subsequently";
}

void DbusViewerWindow::audioInReady()
{
    m_audioInConnected = true;
    m_actAudioIn->setEnabled(true);
    m_actAudioIn->setChecked(true);
    m_actAudioIn->setStatusTip(i18n("Toggle audio input (microphone)"));

    if (m_audioIn) {
        m_display->watchGLibFd(m_audioIn->socketFd());
    }

    updateStatusBar();
    statusBar()->showMessage(i18n("Audio input connected"), 3000);
    qDebug() << "DbusViewerWindow: Audio input activated subsequently";
}
