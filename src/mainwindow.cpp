/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"

#include "const.h"
#include "dbusdisplay/dbusdisplayconst.h"
#include "dialogs/createvmdialog.h"
#include "dialogs/settingsdialog.h"
#include "dialogs/snapshotdialog.h"
#include "jobs/clonejob.h"
#include "jobs/downloadjob.h"
#include "jobs/exportjob.h"
#include "jobs/importjob.h"
#include "jobs/ovaexportjob.h"
#include "jobs/ovaimportjob.h"
#include "jobs/shrinkjob.h"
#include "utils/namespacehelper.h"
#include "utils/qemucommandbuilder.h"
#include "utils/usbenumerator.h"
#include "utils/utils.h"
#include "utils/vmconfig.h"

MainWindow::MainWindow(QWidget* parent)
    : KXmlGuiWindow(parent)
{
    setupUi();
    setupActions();

    jobTracker = new KUiServerV2JobTracker(this);

    setupGUI(Default, u"kemuui.rc"_s);

    QDir dir = getVmBaseDir();
    if (!dir.exists())
        dir.mkpath(u"."_s);

    loadVmList();

    // Preselect first VM
    if (vmModel->rowCount() > 0) {
        QModelIndex firstIndex = vmModel->index(0, 0);

        if (vmModel->hasChildren(firstIndex) && vmModel->data(firstIndex, VmModel::ItemTypeRole).toInt() == VmModel::Folder) {

            if (vmTreeView->isExpanded(firstIndex)) {
                QModelIndex firstChild = vmModel->index(0, 0, firstIndex);
                if (firstChild.isValid())
                    firstIndex = firstChild;
            }
        }

        vmTreeView->setCurrentIndex(firstIndex);
    }

    updateActionStates();

    // coming from setupUi()
    connect(vmTreeView, &VMTreeView::expanded, this, [this]() { saveFolderStructure(); });
    connect(vmTreeView, &VMTreeView::collapsed, this, [this]() { saveFolderStructure(); });

    ExternalTools::instance().refresh();

    m_imageDatabase.loadFromFile(QStandardPaths::locate(QStandardPaths::AppDataLocation, u"data/templates.json"_s));

    m_networkManager = new VirtualNetworkManager(this);
    m_networkManager->load();

    QTimer::singleShot(0, this, [this]() {
        QStringList missing = ExternalTools::instance().missingToolNames();
        if (missing.isEmpty())
            return;

        auto result = KMessageBox::warningContinueCancel(
            this,
            i18n("The following tools were not found:\n\n%1\n\n"
                 "Some features may not work correctly.\n"
                 "You can configure the paths in Settings.",
                missing.join(u"\n"_s)),
            i18n("Missing Tools"),
            KGuiItem(i18n("Open Settings…"), u"configure"_s));

        if (result == KMessageBox::Continue)
            openSettingsPage(SettingsDialog::Executables);
    });
}

MainWindow::~MainWindow()
{
    for (auto* job : std::as_const(creatingVMs)) {
        if (job) {
            job->disconnect();
            job->kill();
        }
    }
    creatingVMs.clear();

    for (auto* job : std::as_const(activeImports)) {
        if (job) {
            job->disconnect();
            job->kill();
        }
    }
    activeImports.clear();

    for (const QString& name : vmModel->vmNames()) {
        VmEntry* e = vmModel->entryByName(name);
        if (!e)
            continue;

        if (e->qmpClient) {
            e->qmpClient->disconnect();
            if (e->qmpClient->isConnected())
                e->qmpClient->sendCommandSync(u"quit"_s, QJsonObject(), 1000);
            e->qmpClient->disconnectFromVm();
        }

        if (e->qemuProcess) {
            e->qemuProcess->disconnect();
            if (e->qemuProcess->state() == KProcess::Running) {
                e->qemuProcess->terminate();
                if (!e->qemuProcess->waitForFinished(2000))
                    e->qemuProcess->kill();
            }
        }

        if (e->swtpmProcess) {
            e->swtpmProcess->disconnect();
            e->swtpmProcess->terminate();
            if (!e->swtpmProcess->waitForFinished(2000))
                e->swtpmProcess->kill();
            delete e->swtpmProcess;
        }

        if (e->virtiofsdProcess) {
            e->virtiofsdProcess->disconnect();
            e->virtiofsdProcess->terminate();
            if (!e->virtiofsdProcess->waitForFinished(2000))
                e->virtiofsdProcess->kill();
            delete e->virtiofsdProcess;
        }

        if (e->passtProcess) {
            e->passtProcess->terminate();
            if (!e->passtProcess->waitForFinished(3000))
                e->passtProcess->kill();
            delete e->passtProcess;
            QFile::remove(getVmBaseDir().filePath(name) + u"/passt.sock"_s);
        }

        if (e->spiceViewerProcess) {
            e->spiceViewerProcess->terminate();
            if (!e->spiceViewerProcess->waitForFinished(2000))
                e->spiceViewerProcess->kill();
            delete e->spiceViewerProcess;
        }

        if (e->dbusDaemonProcess) {
            e->dbusDaemonProcess->terminate();
            if (!e->dbusDaemonProcess->waitForFinished(2000))
                e->dbusDaemonProcess->kill();
            delete e->dbusDaemonProcess;
            QFile::remove(getVmBaseDir().filePath(name) + u"/dbus-bus.sock"_s);
            QFile::remove(getVmBaseDir().filePath(name) + u"/dbus-bus.conf"_s);
        }

        stopDbusViewer(name);
        cleanupDbusSocket(name);

        releaseNetwork(e->config);
    }
}

bool MainWindow::queryClose()
{
    bool hasShrinking = false, hasRunning = false;
    for (const QString& name : vmModel->vmNames()) {
        const VmEntry* e = vmModel->entryByName(name);
        if (!e)
            continue;
        if (e->state == VmEntry::State::Shrinking)
            hasShrinking = true;
        if (e->state == VmEntry::State::Running || e->state == VmEntry::State::Deploying)
            hasRunning = true;
    }
    if (hasShrinking) {
        auto result = KMessageBox::warningTwoActions(
            this,
            i18n("Disk compaction is still in progress. "
                 "If you close the window, the operation(s) will be aborted "
                 "and the disk image(s) will remain unchanged.\n\n"
                 "Do you really want to proceed?"),
            i18nc("@title:window", "Confirm Exit"),
            KStandardGuiItem::close(),
            KStandardGuiItem::cancel());

        if (result != KMessageBox::PrimaryAction)
            return false;
    }

    if (hasRunning) {
        auto result = KMessageBox::warningTwoActions(
            this,
            i18n("Virtual machines are still running. If you close the window, "
                 "they will be terminated. Do you really want to proceed?"),
            i18nc("@title:window", "Confirm Exit"),
            KStandardGuiItem::close(),
            KStandardGuiItem::cancel());

        return result == KMessageBox::PrimaryAction;
    }
    return true;
}

QDir MainWindow::getVmBaseDir() const
{
    return QDir(SettingsDialog::getVmBasePath());
}

QDir MainWindow::getVmDir(const QString& vmName) const
{
    return getVmBaseDir().absoluteFilePath(vmName);
}

QString MainWindow::monitorSocketPath(const QString& vmName) const
{
    return getVmBaseDir().filePath(vmName) + u"/monitor.sock"_s;
}

void MainWindow::setupUi()
{
    setWindowTitle(i18n("KEMU – Virtual Machine Manager"));
    resize(1280, 800);

    auto* centralWidget = new QWidget(this);
    auto* mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto* splitter = new QSplitter(Qt::Horizontal, centralWidget);

    // --- LEFT ---
    auto* leftContainer = new QWidget(splitter);
    auto* leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(4, 4, 4, 4);

    // Header Layout: Label (left), Stretch (center), Button (right)
    auto* leftHeaderLayout = new QHBoxLayout();
    auto* titleLabel = new QLabel(i18n("<b>Virtual Machines</b>"));

    auto* addVmButton = new QToolButton();
    addVmButton->setIcon(QIcon::fromTheme(u"list-add"_s));
    addVmButton->setToolTip(i18n("Create a new Virtual Machine"));
    addVmButton->setAutoRaise(true);
    connect(addVmButton, &QToolButton::clicked, this, &MainWindow::createNewVM);

    leftHeaderLayout->addWidget(titleLabel);
    leftHeaderLayout->addStretch(); // Schiebt den Button nach rechts
    leftHeaderLayout->addWidget(addVmButton);

    // --- VM-Tree ---
    vmModel = new VmModel(&m_imageDatabase, this);

    vmTreeView = new VMTreeView();
    vmTreeView->setModel(vmModel);
    vmTreeView->setHeaderHidden(true);
    vmTreeView->setRootIsDecorated(true);
    vmTreeView->setIconSize(QSize(32, 32));
    vmTreeView->setItemDelegate(new VMItemDelegate(vmTreeView));
    vmTreeView->setAlternatingRowColors(true);
    vmTreeView->setMinimumWidth(220);
    vmTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
    vmTreeView->setSelectionMode(QAbstractItemView::SingleSelection);
    vmTreeView->setAnimated(true);
    vmTreeView->setIndentation(16);

    // Drag & Drop
    vmTreeView->setDragEnabled(true);
    vmTreeView->setAcceptDrops(true);
    vmTreeView->setDropIndicatorShown(true);
    vmTreeView->setDragDropMode(QAbstractItemView::DragDrop);
    vmTreeView->setDefaultDropAction(Qt::MoveAction);

    connect(vmTreeView->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onTreeSelectionChanged);
    connect(vmTreeView, &VMTreeView::doubleClicked, this, &MainWindow::onTreeDoubleClicked);
    connect(vmTreeView, &VMTreeView::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(vmTreeView, &VMTreeView::itemDropped, this, &MainWindow::onDropCompleted);

    connect(vmTreeView, &VMTreeView::mediaIconClicked, this, [this](const QString& vmName, VMItemDelegate::MediaType type, const QPoint& pos) {
        selectVmByName(vmName);

        QMenu menu(this);
        if (type == VMItemDelegate::MediaType::Cd) {
            menu.addAction(actionCollection()->action(u"vm_change_iso"_s));
            menu.addAction(actionCollection()->action(u"vm_eject_iso"_s));
        } else {
            menu.addAction(actionCollection()->action(u"vm_change_floppy"_s));
            menu.addAction(actionCollection()->action(u"vm_eject_floppy"_s));
        }
        menu.exec(pos);
    });

    leftLayout->addLayout(leftHeaderLayout);
    leftLayout->addWidget(vmTreeView);

    // --- RIGHT ---
    auto* rightContainer = new QWidget(splitter);
    auto* rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(4, 4, 4, 4);

    // Local toolbar
    vmActionBar = new QToolBar();
    vmActionBar->setIconSize(QSize(22, 22));
    vmActionBar->setMovable(false);

    detailsBrowser = new QTextBrowser();
    detailsBrowser->setOpenLinks(false);
    detailsBrowser->setFrameShape(QFrame::NoFrame);
    detailsBrowser->setStyleSheet(u"QTextBrowser { background: palette(window); padding: 12px; }"_s);
    detailsBrowser->setOpenLinks(false);
    connect(detailsBrowser, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        Q_UNUSED(this);
        if (url.scheme() == "copy-pid"_L1) {
            QString pidString = url.path();
            QGuiApplication::clipboard()->setText(pidString);
            QToolTip::showText(
                QCursor::pos(),
                i18n("PID %1 copied to clipboard", pidString),
                detailsBrowser,
                QRect(),
                2000);
        } else if (url.scheme() == "http"_L1 || url.scheme() == "https"_L1) {
            QDesktopServices::openUrl(url);
        }
    });

    rightLayout->addWidget(vmActionBar);

    rightLayout->addWidget(detailsBrowser);

    perfMonitor = new PerformanceMonitorWidget();
    perfMonitor->setActive(false);
    perfMonitor->setVisible(SettingsDialog::isPerformanceMonitorEnabled());
    rightLayout->addWidget(perfMonitor);

    // --- FINISH ---
    mainLayout->addWidget(splitter);
    setCentralWidget(centralWidget);

    splitter->setSizes({ 250, 500 });
    statusBar()->showMessage(i18n("Ready."));
}

bool MainWindow::startSwtpm(const QString& vmName)
{
    if (!requireTool(ExternalTools::Swtpm))
        return false;

    const QString vmPath = getVmDir(vmName).absolutePath();
    const QString sockPath = vmPath + u"/swtpm.sock"_s;

    QFile::remove(sockPath);

    QPointer<KProcess> process = new KProcess(this);
    process->setProgram(ExternalTools::instance().path(ExternalTools::Swtpm));
    process->setArguments({ u"socket"_s,
        u"--ctrl"_s, u"type=unixio,path=%1"_s.arg(sockPath),
        u"--terminate"_s,
        u"--tpmstate"_s, u"dir=%1"_s.arg(vmPath),
        u"--tpm2"_s });
    process->start();

    if (!process->waitForStarted(3000)) {
        KMessageBox::error(this, i18n("Failed to start swtpm: %1", process->errorString()));
        delete process;
        return false;
    }

    if (Utils::waitForSocket(sockPath, 5000)) {
        vmModel->setSwtpmProcess(vmName, process);
        return true;
    }

    KMessageBox::error(this, i18n("swtpm started but socket was not created in time."));
    process->terminate();
    process->waitForFinished(2000);
    delete process;

    QFile::remove(getVmBaseDir().filePath(vmName) + u"/swtpm.sock"_s);

    return false;
}

bool MainWindow::startVirtiofsd(const QString& vmName)
{
    if (!requireTool(ExternalTools::Virtiofsd))
        return false;

    const QString sockPath = getVmDir(vmName).absolutePath() + u"/virtiofsd.sock"_s;
    const QString sharedPath = currentConfig.sharedPath();

    QFile::remove(sockPath);

    auto* process = new KProcess(this);
    process->setProgram(ExternalTools::instance().path(ExternalTools::Virtiofsd));
    process->setArguments({ u"--socket-path=%1"_s.arg(sockPath),
        u"--shared-dir=%1"_s.arg(sharedPath),
        u"--cache=auto"_s,
        u"--sandbox=none"_s });
    process->start();

    if (!process->waitForStarted(3000)) {
        KMessageBox::error(this, i18n("Failed to start virtiofsd: %1", process->errorString()));
        delete process;
        return false;
    }

    if (Utils::waitForSocket(sockPath, 5000)) {
        vmModel->setVirtiofsdProcess(vmName, process);
        return true;
    }

    KMessageBox::error(this, i18n("virtiofsd started but socket was not created in time."));
    process->terminate();
    process->waitForFinished(2000);
    delete process;

    QFile::remove(getVmBaseDir().filePath(vmName) + u"/virtiofsd.sock"_s);
    QFile::remove(getVmBaseDir().filePath(vmName) + u"/virtiofsd.sock.pid"_s);

    return false;
}

bool MainWindow::startPasst(const QString& vmName, const QList<PortForwardRule>& portForwards)
{
    if (!requireTool(ExternalTools::Passt))
        return false;

    const QString sockPath = getVmDir(vmName).absolutePath() + u"/passt.sock"_s;

    QFile::remove(sockPath);

    QStringList passtArgs = { u"--socket"_s, sockPath, u"-f"_s, u"-q"_s };

    // Collect ports per protocol
    QStringList tcpPorts, udpPorts;
    for (const auto& rule : portForwards) {
        QString mapping = (rule.hostPort != rule.guestPort)
            ? u"%1:%2"_s.arg(rule.hostPort).arg(rule.guestPort)
            : QString::number(rule.hostPort);

        if (rule.protocol == PortForwardRule::Protocol::Tcp)
            tcpPorts << mapping;
        else
            udpPorts << mapping;
    }

    if (!tcpPorts.isEmpty())
        passtArgs << u"-t"_s << tcpPorts.join(u","_s);
    if (!udpPorts.isEmpty())
        passtArgs << u"-u"_s << udpPorts.join(u","_s);

    auto* process = new KProcess(this);
    process->setProgram(ExternalTools::instance().path(ExternalTools::Passt));
    process->setArguments(passtArgs);
    process->start();

    if (!process->waitForStarted(3000)) {
        KMessageBox::error(this, i18n("Failed to start passt: %1", process->errorString()));
        delete process;
        return false;
    }

    if (Utils::waitForSocket(sockPath, 5000)) {
        vmModel->setPasstProcess(vmName, process);
        return true;
    }

    KMessageBox::error(this, i18n("passt started but socket was not created in time."));
    process->terminate();
    process->waitForFinished(2000);
    delete process;

    QFile::remove(sockPath);

    return false;
}

bool MainWindow::startSpiceViewer(const QString& vmName)
{
    if (!requireTool(ExternalTools::RemoteViewer))
        return false;

    const QString vmPath = getVmDir(vmName).absolutePath();
    const QString spiceSockPath = vmPath + u"/spice.sock"_s;

    if (!Utils::waitForSocket(spiceSockPath, 3000)) {
        KMessageBox::error(this,
            i18n("SPICE socket was not created in time."),
            i18n("Viewer Error"));
        return false;
    }

    // Write a .vv connection file — avoids all URI encoding issues
    const QString vvFile = vmPath + u"/spice.vv"_s;
    QFile f(vvFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        KMessageBox::error(this,
            i18n("Failed to write SPICE connection file."),
            i18n("Viewer Error"));
        return false;
    }
    f.write("[virt-viewer]\n");
    f.write("type=spice\n");
    f.write(u"unix-path=%1\n"_s.arg(spiceSockPath).toUtf8());
    f.close();

    auto* viewer = new KProcess(this);
    viewer->setProgram(ExternalTools::instance().path(ExternalTools::RemoteViewer));
    viewer->setArguments({ vvFile });

    connect(viewer, &QProcess::finished, viewer, [this, vmName, viewer, vvFile]() {
        QFile::remove(vvFile);
        VmEntry* entry = vmModel->entryByName(vmName);
        if (entry && entry->spiceViewerProcess == viewer)
            entry->spiceViewerProcess = nullptr;
        viewer->deleteLater();

        if (!entry || entry->state != VmEntry::State::Running)
            return;

        const auto action = SettingsDialog::spiceViewerCloseAction();

        if (action == SettingsDialog::ShutdownVm) {
            if (entry->qmpClient && entry->qmpClient->isConnected()) {
                entry->qmpClient->sendCommand(u"system_powerdown"_s);
                statusBar()->showMessage(
                    i18n("SPICE viewer closed — shutting down '%1'.", vmName));
            }
            return;
        }

        // SwitchToHeadless (default)
        vmModel->setHeadless(vmName, true);
        statusBar()->showMessage(
            i18n("SPICE viewer closed — '%1' continues headless.", vmName));

        if (selectedVmName() == vmName) {
            updateDetails();
            updateActionStates();
        }
    });

    viewer->start();

    if (!viewer->waitForStarted(3000)) {
        KMessageBox::error(this,
            i18n("Failed to start SPICE viewer: %1", viewer->errorString()),
            i18n("Viewer Error"));
        QFile::remove(vvFile);
        delete viewer;
        return false;
    }

    vmModel->setSpiceViewerProcess(vmName, viewer);
    return true;
}

bool MainWindow::startDbusDaemon(const QString& vmName)
{
    if (!requireTool(ExternalTools::DbusDaemon))
        return false;

    VmEntry* entry = vmModel->entryByName(vmName);
    if (!entry)
        return false;

    const QString sockPath = getVmDir(vmName).absoluteFilePath(u"dbus-bus.sock"_s);
    qDebug() << "D-Bus raw sockPath:" << sockPath;
    qDebug() << "D-Bus escaped:" << QString::fromLatin1(sockPath.toUtf8().toPercentEncoding());

    // Stale socket cleanup
    QFile::remove(sockPath);

    // Per-VM dbus-daemon config: minimal session bus, no fork.
    // Written into the VM directory so it lives alongside other state.
    const QString configPath = getVmDir(vmName).absoluteFilePath(u"dbus-bus.conf"_s);

    QFile cfg(configPath);
    if (!cfg.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        KMessageBox::error(this,
            i18n("Failed to write D-Bus daemon config."));
        return false;
    }

    QString content = QStringLiteral(
        "<!DOCTYPE busconfig PUBLIC\n"
        " \"-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN\"\n"
        " \"http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd\">\n"
        "<busconfig>\n"
        "  <type>session</type>\n"
        "  <listen>unix:path=%1</listen>\n"
        "  <auth>EXTERNAL</auth>\n"
        "  <policy context=\"default\">\n"
        "    <allow send_destination=\"*\" eavesdrop=\"true\"/>\n"
        "    <allow eavesdrop=\"true\"/>\n"
        "    <allow own=\"*\"/>\n"
        "  </policy>\n"
        "</busconfig>\n")
                          .arg(QString::fromLatin1(sockPath.toUtf8().toPercentEncoding()));
    qDebug() << "D-Bus daemon config will be written:\n"
             << content;
    cfg.write(content.toUtf8());
    cfg.close();

    auto* daemon = new KProcess(this);
    daemon->setProgram(ExternalTools::instance().path(ExternalTools::DbusDaemon));
    daemon->setArguments({ u"--config-file=%1"_s.arg(configPath), u"--nofork"_s });

    daemon->start();
    if (!daemon->waitForStarted(3000)) {
        KMessageBox::error(this,
            i18n("Failed to start D-Bus daemon: %1",
                daemon->errorString()));
        delete daemon;
        return false;
    }

    // Wait for the socket to actually appear
    if (!Utils::waitForSocket(sockPath, 3000)) {
        KMessageBox::error(this,
            i18n("D-Bus daemon socket was not created in time."));
        daemon->terminate();
        if (!daemon->waitForFinished(2000))
            daemon->kill();
        delete daemon;
        return false;
    }

    vmModel->setDBusDaemonProcess(vmName, daemon);
    return true;
}

QString MainWindow::dbusBusAddress(const QString& vmName) const
{
    const QString sockPath = getVmDir(vmName).absoluteFilePath(u"dbus-bus.sock"_s);
    return u"unix:path=%1"_s.arg(QString::fromLatin1(sockPath.toUtf8().toPercentEncoding()));
}

bool MainWindow::startDbusViewer(const QString& vmName)
{
    // Already running? Just bring the existing window to the front.
    if (m_dbusViewers.contains(vmName)) {
        auto& existing = m_dbusViewers[vmName];
        if (existing.window) {
            existing.window->raise();
            existing.window->activateWindow();
            return true;
        }
    }

    // --- Connect to the per-VM private D-Bus ---
    const QString busAddress = dbusBusAddress(vmName);
    const QString connName = u"kemu-dbus-%1"_s.arg(vmName);

    QDBusConnection bus = QDBusConnection::connectToBus(busAddress, connName);
    if (!bus.isConnected()) {
        KMessageBox::error(this,
            i18n("Failed to connect to the D-Bus session for '%1': %2", vmName, bus.lastError().message()),
            i18n("Viewer Error"));
        QDBusConnection::disconnectFromBus(connName);
        return false;
    }

    // --- Locate QEMU's Console object ---
    // QEMU needs a brief moment after startup to claim the "org.qemu" name
    // on the bus. Retry for up to 3 seconds.
    const QString consolePath = KemuDbusDisplayConst::ConsolePathPrefix + u"0"_s;

    QScopedPointer<QDBusInterface> console(new QDBusInterface(
        KemuDbusDisplayConst::ServiceName, consolePath,
        KemuDbusDisplayConst::ConsoleIface, bus));

    for (int i = 0; i < 30 && !console->isValid(); ++i) {
        QThread::msleep(100);
        console.reset(new QDBusInterface(
            KemuDbusDisplayConst::ServiceName, consolePath,
            KemuDbusDisplayConst::ConsoleIface, bus));
    }

    if (!console->isValid()) {
        KMessageBox::error(this,
            i18n("QEMU console interface not available for '%1'.", vmName),
            i18n("Viewer Error"));
        QDBusConnection::disconnectFromBus(connName);
        return false;
    }

    DbusViewerState state;
    state.connectionName = connName;

    // --- Display listener ---
    state.displayListener = new QemuDisplayListener(this);
    int listenerFd = state.displayListener->createSocketPair();
    if (listenerFd < 0) {
        delete state.displayListener;
        QDBusConnection::disconnectFromBus(connName);
        return false;
    }

    QDBusUnixFileDescriptor fdWrapper(listenerFd);
    QDBusReply<void> reply = console->call(u"RegisterListener"_s, QVariant::fromValue(fdWrapper));
    ::close(listenerFd);

    if (!reply.isValid()) {
        qWarning() << "RegisterListener failed:" << reply.error().message();
        delete state.displayListener;
        QDBusConnection::disconnectFromBus(connName);
        return false;
    }

    if (!state.displayListener->connectListener()) {
        delete state.displayListener;
        QDBusConnection::disconnectFromBus(connName);
        return false;
    }

    // --- Audio listeners (created up front, registered async below) ---
    state.audioOut = new QemuAudioListener(this);
    state.audioIn = new QemuAudioInListener(this);

    // --- Viewer window ---
    state.window = new DbusViewerWindow(
        state.displayListener, bus, consolePath,
        state.audioOut, state.audioIn,
        false, false, nullptr);
    state.window->setWindowTitle(vmName);
    state.window->setAttribute(Qt::WA_DeleteOnClose, false);
    state.window->displayWidget()->watchGLibFd(state.displayListener->socketFd());
    state.window->show();

    // --- Async audio registration ---
    QDBusInterface* audioIface = new QDBusInterface(
        KemuDbusDisplayConst::ServiceName,
        KemuDbusDisplayConst::AudioPath,
        KemuDbusDisplayConst::AudioIface, bus);

    if (audioIface->isValid()) {
        // Audio output
        int outFd = state.audioOut->createSocketPair();
        if (outFd >= 0) {
            QDBusUnixFileDescriptor outFdW(outFd);
            QDBusPendingCall pending = audioIface->asyncCall(u"RegisterOutListener"_s, QVariant::fromValue(outFdW));
            ::close(outFd);

            auto* w = new QDBusPendingCallWatcher(pending, this);
            connect(w, &QDBusPendingCallWatcher::finished, this,
                [this, vmName](QDBusPendingCallWatcher* watcher) {
                    watcher->deleteLater();
                    QDBusPendingReply<> r = *watcher;
                    if (r.isError() || !m_dbusViewers.contains(vmName)) {
                        if (r.isError())
                            qWarning() << "RegisterOutListener failed:"
                                       << r.error().message();
                        return;
                    }
                    auto& s = m_dbusViewers[vmName];
                    if (!s.audioOut->connectListener())
                        return;
                    if (s.window) {
                        s.window->audioOutReady();
                        s.window->displayWidget()->watchGLibFd(s.audioOut->socketFd());
                    }
                });
        }

        // Audio input
        int inFd = state.audioIn->createSocketPair();
        if (inFd >= 0) {
            QDBusUnixFileDescriptor inFdW(inFd);
            QDBusPendingCall pending = audioIface->asyncCall(u"RegisterInListener"_s, QVariant::fromValue(inFdW));
            ::close(inFd);

            auto* w = new QDBusPendingCallWatcher(pending, this);
            connect(w, &QDBusPendingCallWatcher::finished, this,
                [this, vmName](QDBusPendingCallWatcher* watcher) {
                    watcher->deleteLater();
                    QDBusPendingReply<> r = *watcher;
                    if (r.isError() || !m_dbusViewers.contains(vmName)) {
                        if (r.isError())
                            qWarning() << "RegisterInListener failed:"
                                       << r.error().message();
                        return;
                    }
                    auto& s = m_dbusViewers[vmName];
                    if (!s.audioIn->connectListener())
                        return;
                    if (s.window) {
                        s.window->audioInReady();
                        s.window->displayWidget()->watchGLibFd(s.audioIn->socketFd());
                    }
                });
        }
    } else {
        qWarning() << "Audio interface not available on private bus";
    }
    delete audioIface;

    // Auto-cleanup when QEMU's display connection drops
    connect(state.displayListener, &QemuDisplayListener::connectionLost, this,
        [this, vmName]() { stopDbusViewer(vmName); });

    // Mirror SPICE behavior: when the user closes the viewer window,
    // switch the VM to headless instead of killing it.
    connect(state.window, &QObject::destroyed, this,
        [this, vmName](QObject*) {
            VmEntry* entry = vmModel->entryByName(vmName);
            if (entry && entry->state == VmEntry::State::Running)
                vmModel->setHeadless(vmName, true);
            if (selectedVmName() == vmName) {
                updateDetails();
                updateActionStates();
            }
        });

    m_dbusViewers.insert(vmName, state);
    return true;
}

void MainWindow::stopDbusViewer(const QString& vmName)
{
    if (!m_dbusViewers.contains(vmName))
        return;

    auto state = m_dbusViewers.take(vmName);

    if (state.window) {
        state.window->displayWidget()->stopAllGLibWatching();
        state.window->close();
        delete state.window;
    }
    delete state.audioOut;
    delete state.audioIn;
    delete state.displayListener;

    if (!state.connectionName.isEmpty())
        QDBusConnection::disconnectFromPeer(state.connectionName);
}

void MainWindow::cleanupDbusSocket(const QString& vmName)
{
    QFile::remove(getVmDir(vmName).absoluteFilePath(u"dbus-display.sock"_s));
}

bool MainWindow::requireTool(const ExternalTools::Tool tool)
{
    if (ExternalTools::instance().isAvailable(tool))
        return true;

    KMessageBox::error(this, i18n("%1 not found. Please set the path in Settings.", ExternalTools::displayName(tool)));
    return false;
}

bool MainWindow::isDisplayTypeAvailable(const VMSpecs::DisplayType type)
{
    switch (type) {
    case VMSpecs::DisplayType::Spice:
        return ExternalTools::instance().isAvailable(ExternalTools::spiceDisplayTools());
    case VMSpecs::DisplayType::Dbus:
        return ExternalTools::instance().isAvailable(ExternalTools::DbusDaemon) && SettingsDialog::enableDbusDisplay();
    case VMSpecs::DisplayType::Gtk:
    case VMSpecs::DisplayType::Sdl:
        return true;
    default:
        return false;
    }
}

void MainWindow::setupActions()
{
    KActionCollection* ac = actionCollection();

    auto* actionNew = ac->addAction(u"vm_new"_s);
    actionNew->setIcon(QIcon::fromTheme(u"list-add"_s));
    actionNew->setText(i18n("New Machine"));
    connect(actionNew, &QAction::triggered, this, &MainWindow::createNewVM);

    auto* actionStart = ac->addAction(u"vm_start"_s);
    actionStart->setIcon(QIcon::fromTheme(u"kt-start"_s));
    actionStart->setText(i18n("Start"));
    connect(actionStart, &QAction::triggered, this, &MainWindow::startVM);

    auto* actionStartHeadless = ac->addAction(u"vm_start_headless"_s);
    actionStartHeadless->setIcon(QIcon::fromTheme(u"system-run"_s));
    actionStartHeadless->setText(i18n("Start (Headless)"));
    connect(actionStartHeadless, &QAction::triggered, this, &MainWindow::startVMHeadless);

    auto* actionStop = ac->addAction(u"vm_shutdown"_s);
    actionStop->setIcon(QIcon::fromTheme(u"system-shutdown"_s));
    actionStop->setText(i18n("Shutdown"));
    connect(actionStop, &QAction::triggered, this, &MainWindow::shutdownVM);

    auto* actionChangeISO = ac->addAction(u"vm_change_iso"_s);
    actionChangeISO->setIcon(QIcon::fromTheme(u"media-optical"_s));
    actionChangeISO->setText(i18n("ISO Image…"));
    connect(actionChangeISO, &QAction::triggered, this, &MainWindow::changeISO);

    auto* actionEjectISO = ac->addAction(u"vm_eject_iso"_s);
    actionEjectISO->setIcon(QIcon::fromTheme(u"media-eject"_s));
    actionEjectISO->setText(i18n("Eject ISO Image"));
    connect(actionEjectISO, &QAction::triggered, this, &MainWindow::ejectISO);

    auto* actionChangeFloppy = ac->addAction(u"vm_change_floppy"_s);
    actionChangeFloppy->setIcon(QIcon::fromTheme(u"media-floppy"_s));
    actionChangeFloppy->setText(i18n("Floppy Image…"));
    connect(actionChangeFloppy, &QAction::triggered, this, &MainWindow::changeFloppy);

    auto* actionEjectFloppy = ac->addAction(u"vm_eject_floppy"_s);
    actionEjectFloppy->setIcon(QIcon::fromTheme(u"media-eject"_s));
    actionEjectFloppy->setText(i18n("Eject Floppy Image"));
    connect(actionEjectFloppy, &QAction::triggered, this, &MainWindow::ejectFloppy);

    auto* actionReset = ac->addAction(u"vm_reset"_s);
    actionReset->setIcon(QIcon::fromTheme(u"system-reboot"_s));
    actionReset->setText(i18n("Reset"));
    connect(actionReset, &QAction::triggered, this, &MainWindow::resetVM);

    auto* actionKill = ac->addAction(u"vm_kill"_s);
    actionKill->setIcon(QIcon::fromTheme(u"process-stop"_s));
    actionKill->setText(i18n("Kill"));
    connect(actionKill, &QAction::triggered, this, &MainWindow::killVM);

    auto* actionEdit = ac->addAction(u"vm_edit"_s);
    actionEdit->setIcon(QIcon::fromTheme(u"document-edit"_s));
    actionEdit->setText(i18n("Edit"));
    connect(actionEdit, &QAction::triggered, this, &MainWindow::editVM);

    auto* actionDelete = ac->addAction(u"vm_delete"_s);
    actionDelete->setIcon(QIcon::fromTheme(u"edit-delete"_s));
    actionDelete->setText(i18n("Delete"));
    connect(actionDelete, &QAction::triggered, this, &MainWindow::deleteVM);

    auto* actionClone = ac->addAction(u"vm_clone"_s);
    actionClone->setIcon(QIcon::fromTheme(u"edit-copy"_s));
    actionClone->setText(i18n("Clone VM…"));
    actionClone->setToolTip(i18n("Create a full copy of this virtual machine"));
    connect(actionClone, &QAction::triggered, this, &MainWindow::cloneVM);

    auto* actionShrink = ac->addAction(u"vm_shrink"_s);
    actionShrink->setIcon(QIcon::fromTheme(u"edit-clear-history"_s));
    actionShrink->setText(i18n("Compact Disk"));
    actionShrink->setToolTip(i18n("Compact the disk image by removing unused blocks. Please free blocks on guest OS level first."));
    connect(actionShrink, &QAction::triggered, this, &MainWindow::shrinkDisk);

    auto* snapshotsAction = actionCollection()->addAction(u"vm_manage_snapshots"_s);
    snapshotsAction->setIcon(QIcon::fromTheme(u"camera-photo"_s));
    snapshotsAction->setText(i18n("Manage Snapshots…"));
    snapshotsAction->setToolTip(i18n("Open the snapshot manager for this VM"));
    connect(snapshotsAction, &QAction::triggered, this, &MainWindow::manageSnapshots);

    auto* actionExport = ac->addAction(u"vm_export"_s);
    actionExport->setIcon(QIcon::fromTheme(u"document-export"_s));
    actionExport->setText(i18n("Export VM…"));
    actionExport->setToolTip(i18n("Export the virtual machine as a portable archive"));
    connect(actionExport, &QAction::triggered, this, &MainWindow::exportVM);

    auto* actionImport = ac->addAction(u"vm_import"_s);
    actionImport->setIcon(QIcon::fromTheme(u"document-import"_s));
    actionImport->setText(i18n("Import VM…"));
    actionImport->setToolTip(i18n("Import a virtual machine from an archive"));
    connect(actionImport, &QAction::triggered, this, &MainWindow::importVM);

    auto* actionShowCmd = ac->addAction(u"vm_show_cmd"_s);
    actionShowCmd->setIcon(QIcon::fromTheme(u"utilities-terminal"_s));
    actionShowCmd->setText(i18n("Show command…"));
    connect(actionShowCmd, &QAction::triggered, this, &MainWindow::showCommand);

    auto* actionScreenshot = ac->addAction(u"vm_screenshot"_s);
    actionScreenshot->setIcon(QIcon::fromTheme(u"spectacle"_s));
    actionScreenshot->setText(i18n("Save Screenshot…"));
    actionScreenshot->setToolTip(i18n("Capture and save a screenshot of the running VM"));
    connect(actionScreenshot, &QAction::triggered, this, &MainWindow::saveScreenshot);

    auto* actionNetShell = ac->addAction(u"vm_network_shell"_s);
    actionNetShell->setIcon(QIcon::fromTheme(u"utilities-terminal"_s));
    actionNetShell->setText(i18n("Open Network Shell…"));
    actionNetShell->setToolTip(i18n("Open a terminal (konsole) inside the virtual network namespace"));
    connect(actionNetShell, &QAction::triggered, this, &MainWindow::openNetworkShell);

    auto* actionReconnect = ac->addAction(u"vm_reconnect"_s);
    actionReconnect->setIcon(QIcon::fromTheme(u"view-fullscreen"_s));
    actionReconnect->setText(i18n("Reconnect Viewer"));
    actionReconnect->setToolTip(i18n("Re-open the viewer for a running headless VM"));
    connect(actionReconnect, &QAction::triggered, this, &MainWindow::reconnectViewer);

    // USB passthrough submenu
    usbActionMenu = new KActionMenu(QIcon::fromTheme(u"drive-removable-media-usb"_s), i18n("&USB Devices"), this);
    ac->addAction(u"vm_usb_devices"_s, usbActionMenu);
    connect(usbActionMenu->menu(), &QMenu::aboutToShow, this, &MainWindow::onUsbMenuAboutToShow);

    vmActionBar->addAction(actionStart);
    vmActionBar->addAction(actionStop);
    vmActionBar->addAction(actionReset);
    vmActionBar->addAction(actionKill);
    vmActionBar->addSeparator();
    vmActionBar->addAction(actionChangeISO);
    vmActionBar->addAction(actionChangeFloppy);
    vmActionBar->addSeparator();
    vmActionBar->addAction(snapshotsAction);
    auto* spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    vmActionBar->addWidget(spacer);
    vmActionBar->addAction(actionEdit);

    KStandardAction::preferences(this, &MainWindow::openSettings, ac);
    KStandardAction::quit(this, &QWidget::close, ac);
}

void MainWindow::updateActionStates()
{
    const QString name = selectedVmName();
    const bool hasSelection = !name.isEmpty();

    const VmEntry* entry = hasSelection ? vmModel->entryByName(name) : nullptr;
    const bool isRunning = entry && entry->state == VmEntry::State::Running;
    const bool isDeploying = entry && entry->state == VmEntry::State::Deploying;
    const bool isBusy = entry && entry->state != VmEntry::State::Stopped;
    const bool hasDisk = entry && !entry->config.diskPath().isEmpty();
    const bool canChangeMedia = hasSelection && entry && (entry->state == VmEntry::State::Stopped || entry->state == VmEntry::State::Running);

    actionCollection()->action(u"vm_edit"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_delete"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_clone"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_show_cmd"_s)->setEnabled(hasSelection);
    actionCollection()->action(u"vm_start"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_start_headless"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_shutdown"_s)->setEnabled(hasSelection && (isRunning || isDeploying));
    actionCollection()->action(u"vm_reset"_s)->setEnabled(hasSelection && isRunning);
    actionCollection()->action(u"vm_kill"_s)->setEnabled(hasSelection && (isRunning || isDeploying));
    actionCollection()->action(u"vm_change_iso"_s)->setEnabled(canChangeMedia);
    actionCollection()->action(u"vm_eject_iso"_s)->setEnabled(canChangeMedia && !currentConfig.isoPath().isEmpty());
    actionCollection()->action(u"vm_change_floppy"_s)->setEnabled(canChangeMedia);
    actionCollection()->action(u"vm_eject_floppy"_s)->setEnabled(canChangeMedia && !currentConfig.floppyPath().isEmpty());
    actionCollection()->action(u"vm_shrink"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_manage_snapshots"_s)->setEnabled(hasSelection && hasDisk && !isBusy);
    actionCollection()->action(u"vm_export"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_import"_s)->setEnabled(true);
    actionCollection()->action(u"vm_screenshot"_s)->setEnabled(hasSelection && isRunning && vmSupportsScreendump());
    bool hasUsbController = hasSelection && entry && entry->config.usbController() != VMSpecs::UsbController::None;
    usbActionMenu->setEnabled(hasUsbController && isRunning);

    const bool canReconnectSpice = isRunning && entry->isHeadless
        && entry->config.displayType() == VMSpecs::DisplayType::Spice
        && !entry->spiceViewerProcess;
    const bool canReconnectDbus = isRunning && entry->isHeadless
        && entry->config.displayType() == VMSpecs::DisplayType::Dbus
        && !m_dbusViewers.contains(entry->config.name());
    actionCollection()->action(u"vm_reconnect"_s)->setEnabled(hasSelection && (canReconnectSpice || canReconnectDbus));

    const bool hasActiveNamespace = hasSelection
        && (isRunning || isDeploying)
        && entry->config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork
        && !entry->config.virtualNetworkName().isEmpty()
        && m_networkManager
        && m_networkManager->namespacePid(entry->config.virtualNetworkName()) > 0;
    actionCollection()->action(u"vm_network_shell"_s)->setEnabled(hasActiveNamespace);
}

QStringList MainWindow::existingVmNames() const
{
    const QDir dir = getVmBaseDir();
    QStringList names;
    const QStringList dirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& d : dirs) {
        if (QFile::exists(dir.filePath(d) + u"/config.json"_s))
            names << d;
    }
    return names;
}

QStringList MainWindow::usedMacAddresses() const
{
    QStringList macs;
    for (const auto& info : macUsageInfo())
        macs << info.mac;
    return macs;
}

QList<MacUsageInfo> MainWindow::macUsageInfo() const
{
    QList<MacUsageInfo> result;
    for (const QString& name : existingVmNames()) {
        VMConfig cfg = loadVmConfig(getVmBaseDir().filePath(name));
        if (cfg.macAddress().isEmpty())
            continue;
        MacUsageInfo info;
        info.vmName = name;
        info.mac = cfg.macAddress();
        info.networkMode = cfg.networkMode();
        info.virtualNetworkName = cfg.virtualNetworkName();
        result << info;
    }
    return result;
}

void MainWindow::updateDetails()
{
    KColorScheme scheme(QPalette::Active, KColorScheme::View);

    const QString colNormal = scheme.foreground(KColorScheme::NormalText).color().name();
    const QString colInactive = scheme.foreground(KColorScheme::InactiveText).color().name();
    const QString colPositive = scheme.foreground(KColorScheme::PositiveText).color().name();
    const QString colNeutral = scheme.foreground(KColorScheme::NeutralText).color().name();
    const QString colLink = scheme.foreground(KColorScheme::LinkText).color().name();
    const QString colVisited = scheme.foreground(KColorScheme::VisitedText).color().name();
    QColor borderColor = scheme.foreground(KColorScheme::InactiveText).color();
    borderColor.setAlphaF(0.25);
    const QString colBorder = borderColor.name(QColor::HexArgb);

    if (currentConfig.name().isEmpty()) {
        detailsBrowser->setHtml(
            u"<div style='padding: 60px 40px; text-align: center; font-family: sans-serif;'>"
            "<div style='font-size: 48px; color: %1;'>&#x1F5A5;</div>"
            "<div style='font-size: 15px; color: %1; margin-top: 12px;'>"_s.arg(colInactive)
            + i18n("Select a virtual machine from the list or create a new one.")
            + u"</div></div>"_s);
        return;
    }

    const VmEntry* entry = vmModel->entryByName(currentConfig.name());
    const bool isRunning = entry && entry->state == VmEntry::State::Running;
    const bool isHeadless = entry && entry->isHeadless;
    const bool isShrinking = entry && entry->state == VmEntry::State::Shrinking;
    const bool isExporting = entry && entry->state == VmEntry::State::Exporting;
    const bool isDeploying = entry && entry->state == VmEntry::State::Deploying;
    const bool isCloning = entry && entry->state == VmEntry::State::Cloning;

    if (perfMonitor) {
        const bool vmLive = entry
            && entry->state == VmEntry::State::Running
            && entry->qmpClient
            && entry->qmpClient->isPerformanceTimerActive();
        perfMonitor->setActive(vmLive);
    }

    DiskInfo dInfo = getDiskInfo(currentConfig.diskPath());
    QString diskVal;
    if (dInfo.isValid) {
        QString diskInterfaceStr = VMConfig::toUIString(currentConfig.diskInterface());
        diskVal = i18n("%1 GB, %2 (%3 GB ", QString::number(dInfo.virtualSizeGb, 'f', 1),
                      diskInterfaceStr,
                      QString::number(dInfo.actualSizeGb, 'f', 1))
            + i18n("used")
            + u")"_s;
    } else {
        diskVal = i18n("Not available");
    }

    QString gpuVal = VMConfig::toUIString(currentConfig.gpuType());
    if (currentConfig.gpu3dAcceleration())
        gpuVal += i18n(" (3D)");
    QString audioVal = VMConfig::toUIString(currentConfig.audioDevice());
    if (currentConfig.audioDevice() == VMSpecs::AudioDevice::Sb16) {
        const auto& sb = currentConfig.sb16Config();
        audioVal += u" (IRQ %1, DMA %2/%3, I/O 0x%4)"_s
                        .arg(sb.irq)
                        .arg(sb.dma)
                        .arg(sb.dma16)
                        .arg(sb.iobase, 0, 16);
    }
    QString netVal;
    if (currentConfig.networkCard() == VMSpecs::NetworkCard::None) {
        netVal = VMConfig::toUIString(VMSpecs::NetworkCard::None);
    } else {
        netVal = VMConfig::toUIString(currentConfig.networkCard());
        netVal += u" (%1)"_s.arg(VMConfig::toUIString(currentConfig.networkMode()));
        if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork
            && !currentConfig.virtualNetworkName().isEmpty()) {
            netVal += u" [%1]"_s.arg(currentConfig.virtualNetworkName());
        }
    }

    QString portFwdVal;
    const auto& portForwards = currentConfig.portForwards();
    if (!portForwards.isEmpty()) {
        netVal += u"  [%1 port(s) forwarded]"_s.arg(portForwards.size());

        QStringList portLines;
        for (const auto& rule : portForwards) {
            portLines << u"%1 %2 → %3"_s
                             .arg(VMConfig::toString(rule.protocol).toUpper())
                             .arg(rule.hostPort)
                             .arg(rule.guestPort);
        }
        portFwdVal = portLines.join(u", "_s);
    }

    QString usbVal = VMConfig::toUIString(currentConfig.usbController());
    QString displayVal = VMConfig::toUIString(currentConfig.displayType());

    QString resolutionVal = u"%1×%2"_s.arg(currentConfig.displayResolution().width()).arg(currentConfig.displayResolution().height());

    QString firmwareVal = VMConfig::firmwareToUIString(currentConfig.useUefi(), currentConfig.useSecureBoot());
    QString chipsetVal = VMConfig::toUIString(currentConfig.machineType());

    QString osVal = VMConfig::toUIString(currentConfig.osType());
    if (!currentConfig.distroId().isEmpty()
        && currentConfig.distroId() != u"other"_s) {
        osVal += u" — "_s + m_imageDatabase.displayName(currentConfig.distroId());
    }

    QString memVal = i18n("%1 MB", currentConfig.ramMb());
    QStringList memFlags;
    if (currentConfig.useMemoryBallooning())
        memFlags << i18n("Balloon");
    if (currentConfig.preallocMemory())
        memFlags << i18n("Prealloc");
    if (!memFlags.isEmpty())
        memVal += i18n("  [%1]", memFlags.join(u", "));

    QString cpuVal = i18n("%1, %2 core(s), %3", VMConfig::toUIString(currentConfig.cpuModel()), currentConfig.cpuCoreCount(), currentConfig.useKvm() ? i18n("KVM") : i18n("TCG"));

    QString cdVal = currentConfig.isoPath().isEmpty()
        ? i18n("No disc inserted")
        : QFileInfo(currentConfig.isoPath()).fileName();

    QString floppyVal = currentConfig.floppyPath().isEmpty()
        ? i18n("No disk inserted")
        : QFileInfo(currentConfig.floppyPath()).fileName();

    QString sharedVal;
    if (currentConfig.sharedPath().isEmpty()) {
        sharedVal = i18n("Disabled");
    } else {
        QString protocol = (currentConfig.osType() == VMSpecs::OsType::Windows)
            ? i18n("virtio-fs")
            : i18n("9p (VirtFS)");
        QString tag = currentConfig.sharedName().isEmpty()
            ? u"share"_s
            : currentConfig.sharedName();
        sharedVal = i18n("%1  [%2, Tag: %3]", currentConfig.sharedPath(), protocol, tag);
    }

    QString statusDot, statusText, statusColor;
    if (isRunning) {
        auto pid = entry->qemuProcess ? entry->qemuProcess->processId() : 0;
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Powered On");
        if (isHeadless) {
            if (entry->config.displayType() == VMSpecs::DisplayType::Spice)
                statusText += i18n(" <b>Headless</b> (SPICE viewer disconnected)");
            else
                statusText += i18n(" <b>Headless</b>");
        }
        statusText += u"  <span class='pid'>[<a href='copy-pid:%1' title='%2'>PID %1</a>]</span>"_s.arg(QString::number(pid), i18n("Click to copy PID"));
        statusColor = colPositive;
    } else if (isShrinking) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Compacting Disk…");
        statusColor = colNeutral;
    } else if (isExporting) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Exporting…");
        statusColor = colNeutral;
    } else if (isDeploying) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Deploying…");
        statusColor = colNeutral;
    } else if (isCloning) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Cloning…");
        statusColor = colNeutral;
    } else {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Powered Off");
        statusColor = colInactive;
    }

    // Render icon as inline SVG or PNG for crisp display
    QString iconImgTag;
    {
        const int targetSize = KEMUConst::DetailViewIconSize;
        const qreal dpr = detailsBrowser->devicePixelRatioF();
        const int devPx = qRound(targetSize * dpr);

        // IconSet SVG: render at device-pixel resolution into PNG
        QString svgPath;
        if (currentConfig.iconSource() == VMConfig::IconSource::IconSet)
            svgPath = currentConfig.iconSetPath();

        if (!svgPath.isEmpty()
            && svgPath.endsWith(u".svg", Qt::CaseInsensitive)) {
            QSvgRenderer renderer(svgPath);
            if (renderer.isValid()) {
                QPixmap pix(devPx, devPx);
                pix.fill(Qt::transparent);
                QPainter painter(&pix);
                painter.setRenderHint(QPainter::Antialiasing);
                renderer.render(&painter);
                painter.end();

                QByteArray ba;
                QBuffer buffer(&ba);
                buffer.open(QIODevice::WriteOnly);
                pix.save(&buffer, "PNG");
                const QString b64 = QString::fromLatin1(ba.toBase64());
                iconImgTag = u"<img src='data:image/png;base64,%1' "
                             "width='%2' />"_s
                                 .arg(b64)
                                 .arg(targetSize);
            }
        }

        // Fallback: all other icon sources
        if (iconImgTag.isEmpty()) {
            const QIcon ic = currentConfig.icon(targetSize);
            const QPixmap pix = ic.pixmap(QSize(devPx, devPx));
            if (!pix.isNull()) {
                QByteArray ba;
                QBuffer buffer(&ba);
                buffer.open(QIODevice::WriteOnly);
                pix.save(&buffer, "PNG");
                const QString b64 = QString::fromLatin1(ba.toBase64());
                iconImgTag = u"<img src='data:image/png;base64,%1' "
                             "width='%2' />"_s
                                 .arg(b64)
                                 .arg(targetSize);
            } else {
                iconImgTag = u"<span style='font-size: 42px; "
                             "color: %1;'>&#x1F5B3;</span>"_s
                                 .arg(colInactive);
            }
        }
    }

    // --- CSS ---
    QString css = u"* { margin: 0; padding: 0; }"
                  "body { font-family: 'Segoe UI', 'Noto Sans', sans-serif; font-size: 13px; "
                  "       color: %1; background: transparent; padding: 16px 20px; }"
                  ".header { display: flex; margin-bottom: 18px; }"
                  ".vm-icon { width: 58px; vertical-align: top; padding-right: 10px; }"
                  ".header-info { }"
                  ".vm-name { font-size: 18px; font-weight: 600; color: %1; margin-bottom: 3px; }"
                  ".vm-status { font-size: 12px; color: %2; }"
                  ".vm-status .dot { font-size: 10px; }"
                  ".vm-os { font-size: 12px; color: %3; margin-top: 2px; }"
                  ".pid { color: %3; font-size: 11px; }"
                  ".pid a { color: %4; text-decoration: none; }"
                  ".pid a:hover { text-decoration: underline; color: %5; }"
                  ".hw-table { width: 100%%; border-collapse: collapse; margin-top: 4px; }"
                  ".hw-table td { padding: 6px 10px; vertical-align: top; border-bottom: 1px solid %6; }"
                  ".hw-table tr:last-child td { border-bottom: none; }"
                  ".hw-icon { width: 26px; text-align: center; font-size: 15px; color: %3; }"
                  ".hw-label { width: 160px; font-weight: 500; color: %3; }"
                  ".hw-value { color: %1; }"
                  ".hw-value.muted { color: %3; font-style: italic; }"
                  ".section-gap td { padding-top: 14px; }"_s
                      .arg(colNormal, statusColor, colInactive,
                          colLink, colVisited, colBorder);

    // --- HTML ---
    QString html = u"<html><head><style>%1</style></head><body>"_s.arg(css);

    html += u"<table class='header' cellpadding='0' cellspacing='0'><tr>"
            "<td class='vm-icon'>%1</td>"
            "<td class='header-info'>"
            "  <div class='vm-name'>%2</div>"
            "  <div class='vm-status'><span class='dot'>%3</span> %4</div>"
            "  <div class='vm-os'>%5</div>"
            "</td>"
            "</tr></table>"_s.arg(iconImgTag, currentConfig.name(), statusDot, statusText, osVal);

    struct HwRow {
        QString icon;
        QString label;
        QString value;
        bool sectionStart = false;
        bool muted = false;
    };

    QList<HwRow> rows = {
        { u"&#x2699;"_s, i18n("Firmware"), firmwareVal, false, false },
        { u"&#x2699;"_s, i18n("Chipset"), chipsetVal, false, false },

        { u"&#x1F512;"_s, i18n("TPM"), currentConfig.useTpm() ? i18n("TPM 2.0 (swtpm)") : i18n("None"), false, !currentConfig.useTpm() },

        { u"&#x1F552;"_s, i18n("Clock"), currentConfig.useLocalTime() ? i18n("Localtime") : i18n("UTC"), false, false },

        { u"&#x1F4BB;"_s, i18n("Memory"), memVal, true, false },
        { u"&#x1F4BB;"_s, i18n("Processors"), cpuVal, false, false },

        { u"&#x1F4BE;"_s, i18n("Hard Disk"), diskVal, true, false },
        { u"&#x1F4BF;"_s, i18n("CD/DVD"), cdVal, false, currentConfig.isoPath().isEmpty() },
        { u"&#x1F4BE;"_s, i18n("Floppy"), floppyVal, false, currentConfig.floppyPath().isEmpty() },

        { u"&#x1F5B5;"_s, i18n("Display"),
            gpuVal + u", "_s + displayVal + (currentConfig.displayType() != VMSpecs::DisplayType::Spice ? u" (%1)"_s.arg(resolutionVal) : QString()),
            true, false },

        { u"&#x1F50A;"_s, i18n("Sound Card"), audioVal, true, currentConfig.audioDevice() == VMSpecs::AudioDevice::None },

        { u"&#x1F310;"_s, i18n("Network Adapter"), netVal, true, currentConfig.networkCard() == VMSpecs::NetworkCard::None }

    };

    if (!portFwdVal.isEmpty())
        rows.append({ u"&#x21C4;"_s, i18n("Port Forwarding"), portFwdVal, false, false });

    // Show namespace PID for VMs on a virtual network (when running)
    if ((isRunning || isDeploying)
        && currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork
        && !currentConfig.virtualNetworkName().isEmpty()
        && m_networkManager) {
        const qint64 nsPid = m_networkManager->namespacePid(currentConfig.virtualNetworkName());
        if (nsPid > 0) {
            const int vmCount = m_networkManager->connectedVmCount(currentConfig.virtualNetworkName());
            QString nsVal = u"<a href='copy-pid:%1' title='%2'>PID %1</a>"_s
                                .arg(QString::number(nsPid), i18n("Click to copy namespace PID"));
            nsVal += u"  [%1, %2]"_s
                         .arg(currentConfig.virtualNetworkName())
                         .arg(i18np("%1 VM connected", "%1 VMs connected", vmCount));
            rows.append({ u"&#x1F310;"_s, i18n("Namespace"), nsVal, false, false });
        }
    }

    rows.append({ { u"&#x1F50C;"_s, i18n("USB Controller"), usbVal, true, false },
        { u"&#x1F4C1;"_s, i18n("Shared Folders"), sharedVal, true, currentConfig.sharedPath().isEmpty() } });

    html += u"<table class='hw-table'>"_s;

    for (const auto& row : rows) {
        QString trClass = row.sectionStart ? u" class='section-gap'"_s : QString();
        QString valClass = row.muted ? u"hw-value muted"_s : u"hw-value"_s;

        html += u"<tr%1>"
                "<td class='hw-icon'>%2</td>"
                "<td class='hw-label'>%3</td>"
                "<td class='%4'>%5</td>"
                "</tr>"_s.arg(trClass, row.icon, row.label, valClass, row.value);
    }

    html += u"</table></body></html>"_s;
    detailsBrowser->setHtml(html);
}

void MainWindow::onVmSelectionChanged()
{
    const QString name = selectedVmName();
    if (name.isEmpty()) {
        currentConfig = VMConfig();
    } else {
        currentConfig = loadVmConfig(getVmBaseDir().filePath(name));
    }
    updateDetails();
    updateActionStates();

    if (perfMonitor) {
        const VmEntry* entry = vmModel->entryByName(name);
        if (entry && entry->state == VmEntry::State::Running) {
            perfMonitor->setHistory(PerformanceMonitorWidget::Cpu, entry->cpuHistory);
            perfMonitor->setHistory(PerformanceMonitorWidget::Ram, entry->ramHistory);
            perfMonitor->setHistory(PerformanceMonitorWidget::Io, entry->ioHistory);
            perfMonitor->setActive(true);
        } else {
            perfMonitor->clearHistory();
            perfMonitor->setActive(false);
        }
    }
}

void MainWindow::openSettings()
{
    openSettingsPage(SettingsDialog::General);
}

void MainWindow::openSettingsPage(SettingsDialog::Page page)
{
    SettingsDialog dlg(this);
    dlg.setNetworkManager(m_networkManager);
    dlg.setPage(page);
    if (dlg.exec() == QDialog::Accepted) {
        currentConfig = VMConfig();
        QDir dir = getVmBaseDir();
        if (!dir.exists())
            dir.mkpath(u"."_s);
        loadVmList();
        onVmSelectionChanged();
        if (perfMonitor)
            perfMonitor->setVisible(SettingsDialog::isPerformanceMonitorEnabled());
    }
}

void MainWindow::saveVmConfig(const QString& vmPath, const VMConfig& config)
{
    QJsonDocument doc(config.toJson());
    QFile file(vmPath + u"/"_s + KEMUConst::VMConfigFileName);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson(QJsonDocument::Indented));
    } else {
        qWarning() << "Failed to write config.json:" << file.errorString();
    }

    if (!config.saveCustomIcon(vmPath)) {
        if (config.iconSource() == VMConfig::IconSource::Custom) {
            qWarning() << "Failed to save custom icon for" << config.name();
        }
    }

    ExternalTools::instance().refresh();
    generateShellScript(vmPath, config);
}

void MainWindow::generateShellScript(const QString& vmPath, const VMConfig& config)
{
    const QString sockPath = vmPath + u"/qemu-monitor.sock"_s;

    NetworkContext netCtx;
    if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        netCtx.tapDevice = u"tap0"_s;

    const VirtualNetwork net = (m_networkManager && config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        ? m_networkManager->network(config.virtualNetworkName())
        : VirtualNetwork { };

    const QString script = QemuCommandBuilder::buildShellScript(config, QDir(vmPath), sockPath, netCtx, net, true);

    QFile scriptFile(vmPath + u"/qemu.sh"_s);
    if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        scriptFile.write(script.toUtf8());
        scriptFile.setPermissions(scriptFile.permissions() | QFile::ExeOwner | QFile::ExeUser);
    } else {
        qWarning() << "Failed to write qemu.sh:" << scriptFile.errorString();
    }
}

VMConfig MainWindow::loadVmConfig(const QString& vmPath) const
{
    QFile file(vmPath + u"/"_s + KEMUConst::VMConfigFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "Failed to read config.json:" << file.errorString();
        return { };
    }

    VMConfig config = VMConfig::fromJson(QJsonDocument::fromJson(file.readAll()).object());
    config.normalizePaths(QDir(vmPath));

    if (config.iconSource() == VMConfig::IconSource::Custom) {
        if (!config.loadCustomIcon(vmPath)) {
            qWarning() << "Failed to load custom icon for" << config.name();
        }
    }

    return config;
}

void MainWindow::loadVmList()
{
    const QMap<QString, QString> assignments = loadFolderAssignments();

    vmModel->reload(getVmBaseDir(), assignments,
        [this](const QString& vmPath) {
            return loadVmConfig(vmPath);
        });

    QStringList macs = usedMacAddresses();
    for (const QString& name : vmModel->vmNames()) {
        const QString vmPath = getVmDir(name).absolutePath();
        VMConfig cfg = loadVmConfig(vmPath);
        if (cfg.networkCard() != VMSpecs::NetworkCard::None && cfg.macAddress().isEmpty()) {
            cfg.ensureMacAddress(macs);
            macs << cfg.macAddress();
            saveVmConfig(vmPath, cfg);
        }
    }

    KConfig kconfig(u"kemurc"_s);
    const QStringList collapsed = kconfig.group(u"Folders"_s).readEntry(u"collapsed"_s, QStringList());

    for (const QString& folder : vmModel->folderNames()) {
        QModelIndex idx = vmModel->indexForFolder(folder);
        if (idx.isValid() && !collapsed.contains(folder))
            vmTreeView->expand(idx);
    }

    statusBar()->showMessage(i18n("%1 virtual machine(s) found.", vmModel->vmNames().size()));
}

DiskInfo MainWindow::getDiskInfo(const QString& diskPath)
{
    DiskInfo info;
    if (!QFile::exists(diskPath))
        return info;

    if (!ExternalTools::instance().isAvailable(ExternalTools::QemuImg))
        return info;

    KProcess p;
    p.setOutputChannelMode(KProcess::SeparateChannels);
    p.setProgram(ExternalTools::instance().path(ExternalTools::QemuImg));
    p.setArguments({ u"info"_s, u"--output=json"_s, diskPath });
    p.start();
    p.waitForFinished(5000);

    if (p.exitCode() == 0) {
        QJsonDocument doc = QJsonDocument::fromJson(p.readAllStandardOutput());
        QJsonObject obj = doc.object();
        double virtBytes = obj[u"virtual-size"_s].toDouble();
        double actBytes = obj[u"actual-size"_s].toDouble();
        info.virtualSizeGb = virtBytes / (1024.0 * 1024.0 * 1024.0);
        info.actualSizeGb = actBytes / (1024.0 * 1024.0 * 1024.0);
        info.isValid = true;
    }
    return info;
}

bool MainWindow::vmSupportsScreendump() const
{
    return !currentConfig.gpu3dAcceleration();
}

QString MainWindow::ensureNetworkRunning(const QString& networkName, const QString& vmName)
{
    if (!VirtualNetworkManager::isUserNamespaceSupported()) {
        KMessageBox::error(this, i18n("User namespaces are not supported on this system. Virtual networking requires unprivileged user namespaces."));
        return { };
    }

    if (!ExternalTools::instance().isVirtualNetworkCapable()) {
        KMessageBox::error(this, i18n("Required tools for virtual networking are missing. Please check Settings → Executables."));
        return { };
    }

    const VirtualNetwork net = m_networkManager->network(networkName);
    if (!net.isValid()) {
        KMessageBox::error(this, i18n("Virtual network '%1' not found or invalid: %2", networkName, networkName, m_networkManager->lastError()));
        return { };
    }

    if (!m_networkManager->startNetwork(networkName)) {
        KMessageBox::error(this, i18n("Failed to start virtual network '%1': %2", networkName, m_networkManager->lastError()));
        return { };
    }

    const QString tapDevice = m_networkManager->attachVm(networkName, vmName);
    if (tapDevice.isEmpty()) {
        KMessageBox::error(this, i18n("Failed to create network interface for VM '%1': %2", vmName, networkName, m_networkManager->lastError()));
        return { };
    }

    return tapDevice;
}

void MainWindow::releaseNetwork(const VMConfig& config)
{
    if (!m_networkManager)
        return;
    if (config.networkMode() != VMSpecs::NetworkMode::VirtualNetwork)
        return;
    if (config.virtualNetworkName().isEmpty())
        return;
    m_networkManager->detachVm(config.virtualNetworkName(), config.name());
}

void MainWindow::changeISO()
{
    if (currentConfig.name().isEmpty())
        return;

    const QDir startDir = m_lastMediaDir.value(currentConfig.name(), QDir::homePath());
    const QString isoPath = QFileDialog::getOpenFileName(this, i18n("Select ISO Image"), startDir.absolutePath(), i18n("ISO Files (*.iso)"));
    if (isoPath.isEmpty())
        return;

    m_lastMediaDir[currentConfig.name()] = QFileInfo(isoPath).absolutePath();

    if (vmModel->isRunning(currentConfig.name())) {
        const VmEntry* entry = vmModel->entryByName(currentConfig.name());
        if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        QMPResult result = entry->qmpClient->sendCommandSync(
            u"blockdev-change-medium"_s,
            { { u"id"_s, u"cdrom0"_s },
                { u"filename"_s, isoPath },
                { u"format"_s, u"raw"_s } });

        if (!result.success) {
            KMessageBox::error(this, i18n("Changing ISO Failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("ISO changed successfully."));
    }

    m_lastMediaDir[currentConfig.name()] = QFileInfo(isoPath).absolutePath();
    currentConfig.setISOPath(isoPath);
    vmModel->updateConfig(currentConfig.name(), currentConfig);
    saveVmConfig(getVmDir(currentConfig.name()).absolutePath(), currentConfig);
    updateActionStates();
    updateDetails();
}

void MainWindow::ejectISO()
{
    if (currentConfig.name().isEmpty())
        return;

    if (vmModel->isRunning(currentConfig.name())) {
        const VmEntry* entry = vmModel->entryByName(currentConfig.name());
        if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        QJsonObject ejectArgs;
        ejectArgs.insert(u"id"_s, u"cdrom0"_s);
        QMPResult result = entry->qmpClient->sendCommandSync(u"eject"_s, ejectArgs);

        if (!result.success) {
            KMessageBox::error(this, i18n("Eject Failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("ISO ejected."));
    }

    currentConfig.clearISOPath();
    currentConfig.setBootFromIso(false);
    vmModel->updateConfig(currentConfig.name(), currentConfig);
    saveVmConfig(getVmDir(currentConfig.name()).absolutePath(), currentConfig);
    updateActionStates();
    updateDetails();
}

void MainWindow::changeFloppy()
{
    if (currentConfig.name().isEmpty())
        return;

    const QDir startDir = m_lastMediaDir.value(currentConfig.name(), QDir::homePath());
    const QString imagePath = QFileDialog::getOpenFileName(this, i18n("Select Floppy Image"), startDir.absolutePath(), i18n("Floppy Images (*.img *.ima *.flp *.vfd);;All Files (*)"));
    if (imagePath.isEmpty())
        return;

    m_lastMediaDir[currentConfig.name()] = QFileInfo(imagePath).absolutePath();

    if (vmModel->isRunning(currentConfig.name())) {
        const VmEntry* entry = vmModel->entryByName(currentConfig.name());
        if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        QMPResult result = entry->qmpClient->sendCommandSync(
            u"blockdev-change-medium"_s,
            { { u"id"_s, u"fda"_s },
                { u"filename"_s, imagePath },
                { u"format"_s, u"raw"_s } });

        if (!result.success) {
            KMessageBox::error(this, i18n("Changing floppy image failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("Floppy image changed successfully."));
    }

    m_lastMediaDir[currentConfig.name()] = QFileInfo(imagePath).absolutePath();
    currentConfig.setFloppyPath(imagePath);
    currentConfig.setBootFromFloppy(false);
    vmModel->updateConfig(currentConfig.name(), currentConfig);
    saveVmConfig(getVmDir(currentConfig.name()).absolutePath(), currentConfig);
    updateActionStates();
    updateDetails();
}

void MainWindow::ejectFloppy()
{
    if (currentConfig.name().isEmpty())
        return;

    if (vmModel->isRunning(currentConfig.name())) {
        const VmEntry* entry = vmModel->entryByName(currentConfig.name());
        if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        QJsonObject ejectArgs;
        ejectArgs.insert(u"id"_s, u"fda"_s);
        QMPResult result = entry->qmpClient->sendCommandSync(u"eject"_s, ejectArgs);

        if (!result.success) {
            KMessageBox::error(this, i18n("Eject floppy failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("Floppy ejected."));
    }

    currentConfig.clearFloppyPath();
    vmModel->updateConfig(currentConfig.name(), currentConfig);
    saveVmConfig(getVmDir(currentConfig.name()).absolutePath(), currentConfig);
    updateActionStates();
    updateDetails();
}

void MainWindow::shrinkDisk()
{
    if (currentConfig.name().isEmpty())
        return;

    if (vmModel->isBusy(currentConfig.name()))
        return;

    DiskInfo dInfo = getDiskInfo(currentConfig.diskPath());
    QString sizeInfo;
    if (dInfo.isValid) {
        sizeInfo = i18n("\n\nCurrent size: %1 GB (virtual), %2 GB (actual on disk)",
            QString::number(dInfo.virtualSizeGb, 'f', 1),
            QString::number(dInfo.actualSizeGb, 'f', 1));
    }

    auto answer = KMessageBox::questionTwoActions(
        this,
        i18n("Compact the disk image of VM '%1'?\n\n"
             "This will rewrite the disk image to reclaim unused space. "
             "The VM must remain stopped during the operation.%2",
            currentConfig.name(), sizeInfo),
        i18nc("@title:window", "Compact Disk Image"),
        KGuiItem(i18n("Compact"), u"edit-clear-history"_s),
        KStandardGuiItem::cancel());

    if (answer != KMessageBox::PrimaryAction)
        return;

    auto* job = new ShrinkJob(currentConfig.name(), currentConfig.diskPath(), this);

    if (jobTracker) {
        jobTracker->registerJob(job);
    }

    vmModel->setVmState(currentConfig.name(), VmEntry::State::Shrinking);

    connect(job, &ShrinkJob::finished, this, [this](KJob* kjob) {
        auto* shrinkJob = qobject_cast<ShrinkJob*>(kjob);
        if (!shrinkJob)
            return;

        QString vmName = shrinkJob->vmName();
        vmModel->setVmState(vmName, VmEntry::State::Stopped);

        if (shrinkJob->error()) {
            statusBar()->showMessage(i18n("Disk compaction of VM '%1' failed: %2", vmName, shrinkJob->errorText()));
        } else {
            VMConfig config = loadVmConfig(getVmDir(vmName).absolutePath());
            DiskInfo dInfo = getDiskInfo(config.diskPath());

            QString newSizeMsg = u"."_s;
            if (dInfo.isValid) {
                newSizeMsg = i18n(" (now %1 GB).", QString::number(dInfo.actualSizeGb, 'f', 1));
            }

            statusBar()->showMessage(i18n("Disk of VM '%1' compacted successfully%2", vmName, newSizeMsg));
        }

        if (currentConfig.name() == vmName) {
            updateDetails();
        }
        updateActionStates();

        shrinkJob->deleteLater();
    });

    job->start();

    statusBar()->showMessage(i18n("Compacting disk of VM '%1'…", currentConfig.name()));
    updateActionStates();
    updateDetails();
}

void MainWindow::manageSnapshots()
{
    const QString vmName = selectedVmName();
    if (vmName.isEmpty())
        return;

    const VMConfig cfg = loadVmConfig(getVmBaseDir().filePath(vmName));

    if (cfg.diskPath().isEmpty()) {
        KMessageBox::error(this, i18n("No disk image configured for \"%1\".", vmName));
        return;
    }

    const VmEntry* entry = vmModel->entryByName(vmName);
    QPointer<QMPClient> qmp = entry ? entry->qmpClient : nullptr;

    auto* dlg = new SnapshotDialog(vmName, cfg.diskPath(), qmp, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void MainWindow::exportVM()
{
    if (currentConfig.name().isEmpty())
        return;

    if (vmModel->isBusy(currentConfig.name())) {
        KMessageBox::error(this, i18n("The VM '%1' is currently busy. Please wait.", currentConfig.name()), i18n("Busy"));
        return;
    }

    QString filterKemu = i18n("KEMU Portable Archives (*.kemu.tar.zst)");
    QString filterOva = i18n("OVA Files (*.ova)");
    QString selectedFilter;

    QString filePath = QFileDialog::getSaveFileName(
        this,
        i18n("Export VM '%1'", currentConfig.name()),
        QDir::homePath() + u"/"_s + currentConfig.name(),
        filterKemu + u";;"_s + filterOva,
        &selectedFilter);

    if (filePath.isEmpty())
        return;

    if (selectedFilter == filterOva) {
        if (!filePath.endsWith(u".ova"_s, Qt::CaseInsensitive))
            filePath += u".ova"_s;
        exportOVA(filePath);
    } else {
        if (!filePath.endsWith(u".kemu.tar.zst"_s))
            filePath += u".kemu.tar.zst"_s;

        auto* job = new ExportJob(currentConfig.name(), getVmDir(currentConfig.name()).absolutePath(), filePath, this);
        if (jobTracker) {
            jobTracker->registerJob(job);
        }
        vmModel->setVmState(currentConfig.name(), VmEntry::State::Exporting);

        connect(job, &ExportJob::finished, this, [this](KJob* kjob) {
            auto* exportJob = qobject_cast<ExportJob*>(kjob);
            if (!exportJob)
                return;

            QString vmName = exportJob->vmName();
            vmModel->setVmState(vmName, VmEntry::State::Stopped);

            if (exportJob->error()) {
                statusBar()->showMessage(
                    i18n("Export of VM '%1' failed: %2", vmName, exportJob->errorText()));
                KMessageBox::error(this, i18n("Failed to export VM '%1':\n%2", vmName, exportJob->errorText()), i18n("Export Failed"));
            } else {
                statusBar()->showMessage(
                    i18n("VM '%1' exported successfully.", vmName));
            }

            if (currentConfig.name() == vmName) {
                updateDetails();
            }
            updateActionStates();

            exportJob->deleteLater();
        });

        job->start();

        statusBar()->showMessage(i18n("Exporting VM '%1'…", currentConfig.name()));
        updateActionStates();
        updateDetails();
    }
}

void MainWindow::importVM()
{
    const QString filePath = QFileDialog::getOpenFileName(
        this,
        i18n("Import Virtual Machine"),
        QDir::homePath(),
        i18n("KEMU Portable Archives (*.kemu.tar.zst);;OVA Files (*.ova);;All Files (*)"));

    if (filePath.isEmpty())
        return;

    if (filePath.endsWith(u".ova"_s, Qt::CaseInsensitive)) {
        importOVA(filePath);
        return;
    }

    const QString vmBaseDir = getVmBaseDir().absolutePath();

    auto* job = new ImportJob(filePath, vmBaseDir, this);
    if (jobTracker) {
        jobTracker->registerJob(job);
    }
    activeImports.append(job);

    connect(job, &ImportJob::finished, this, [this, vmBaseDir](KJob* kjob) {
        auto* importJob = qobject_cast<ImportJob*>(kjob);
        if (!importJob)
            return;

        activeImports.removeOne(importJob);

        if (importJob->error()) {

            statusBar()->showMessage(i18n("Import failed: %1", importJob->errorText()));
            KMessageBox::error(this, i18n("Failed to import VM:\n%1", importJob->errorText()), i18n("Import Failed"));

        } else {

            if (SettingsDialog::importRegenerateMac()) {
                VMConfig config = loadVmConfig(vmBaseDir);
                config.setMacAddress(Utils::generateRandomMac(usedMacAddresses()));
                saveVmConfig(vmBaseDir, config);
            }

            statusBar()->showMessage(i18n("VM '%1' imported successfully.", importJob->vmName()));
            loadVmList();
            selectVmByName(importJob->vmName());
        }

        importJob->deleteLater();
    });

    job->start();
    statusBar()->showMessage(i18n("Importing VM…"));
}

void MainWindow::saveScreenshot()
{
    const QString vmName = currentConfig.name();
    if (vmName.isEmpty() || !vmModel->isRunning(vmName))
        return;

    if (!vmSupportsScreendump()) {
        KMessageBox::information(this,
            i18n("Screenshots are not available for VMs using VirtIO-GL.\n\n"
                 "The GPU renders directly via OpenGL on the host, "
                 "so QEMU has no software framebuffer to capture.\n\n"
                 "To use this feature, switch the GPU type to "
                 "'VGA' or 'VirtIO' in the VM settings."),
            i18n("Screenshot Not Available"));
        return;
    }

    const VmEntry* entry = vmModel->entryByName(vmName);
    if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected()) {
        KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
        return;
    }

    const QString dumpPath = getVmBaseDir().filePath(vmName) + u"/screenshot_save.ppm"_s;

    QJsonObject args;
    args.insert(u"filename"_s, dumpPath);
    const QMPResult result = entry->qmpClient->sendCommandSync(u"screendump"_s, args, 5000);

    if (!result.success) {
        KMessageBox::error(this, i18n("Could not capture screenshot: %1", result.errorMessage), i18n("Screenshot Failed"));
        return;
    }

    const QImage image(dumpPath);
    QFile::remove(dumpPath);

    if (image.isNull()) {
        KMessageBox::error(this, i18n("The captured image could not be loaded."), i18n("Screenshot Failed"));
        return;
    }

    const QString picturesDir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString defaultName = u"%1/%2_%3.png"_s.arg(
        picturesDir,
        vmName,
        QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmmss"_s));

    const QString filePath = QFileDialog::getSaveFileName(
        this,
        i18n("Save Screenshot"),
        defaultName,
        i18n("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg)"));

    if (filePath.isEmpty())
        return;

    QByteArray format = "PNG";
    if (filePath.endsWith(u".jpg"_s, Qt::CaseInsensitive)
        || filePath.endsWith(u".jpeg"_s, Qt::CaseInsensitive)) {
        format = "JPEG";
    }

    if (image.save(filePath, format.constData())) {
        statusBar()->showMessage(i18n("Screenshot saved to %1", filePath));
    } else {
        KMessageBox::error(this, i18n("Failed to save screenshot to %1", filePath), i18n("Error"));
    }
}

void MainWindow::openNetworkShell()
{
    if (currentConfig.name().isEmpty())
        return;

    const QString netName = currentConfig.virtualNetworkName();
    if (netName.isEmpty() || !m_networkManager)
        return;

    const qint64 nsPid = m_networkManager->namespacePid(netName);
    if (nsPid <= 0)
        return;

    // Build the nsenter command as a single shell string
    const QString nsenterCmd = u"echo '-- KEMU Network Shell --'; "
                               u"echo 'Network: %1  (Namespace PID %2)'; "
                               u"echo 'Hint: Root privileges are mapped to the UID of the user executing KEMU.'; "
                               u"echo 'Type exit to leave.'; echo; "
                               u"exec nsenter -t %2 -U --preserve-credentials -n -- /bin/bash"_s
                                   .arg(netName)
                                   .arg(nsPid);

    // Try konsole first, then xterm as fallback
    const QString konsole = QStandardPaths::findExecutable(u"konsole"_s);
    const QString xterm = QStandardPaths::findExecutable(u"xterm"_s);

    auto* process = new KProcess(this);
    connect(process, &KProcess::finished, process, &KProcess::deleteLater);

    if (!konsole.isEmpty()) {
        process->setProgram(konsole);
        process->setArguments({ u"--separate"_s, u"--title"_s, i18n("KEMU Network Shell -- %1", netName), u"-e"_s, u"/bin/bash"_s, u"-c"_s, nsenterCmd });
    } else if (!xterm.isEmpty()) {
        process->setProgram(xterm);
        process->setArguments({ u"-title"_s, i18n("KEMU Network Shell -- %1", netName), u"-e"_s, u"/bin/bash"_s, u"-c"_s, nsenterCmd });
    } else {
        KMessageBox::error(this,
            i18n("No terminal emulator found.\n\n"
                 "You can enter the namespace manually:\n"
                 "nsenter -t %1 -U --preserve-credentials -n -- /bin/bash",
                nsPid));
        delete process;
        return;
    }

    process->start();

    if (!process->waitForStarted(3000)) {
        KMessageBox::error(this,
            i18n("Failed to open terminal: %1", process->errorString()));
        delete process;
    }
}

void MainWindow::reconnectViewer()
{
    const QString vmName = selectedVmName();
    if (vmName.isEmpty())
        return;

    VmEntry* entry = vmModel->entryByName(vmName);
    if (!entry || entry->state != VmEntry::State::Running)
        return;

    if (entry->config.displayType() == VMSpecs::DisplayType::Spice) {
        if (entry->spiceViewerProcess) {
            statusBar()->showMessage(i18n("Viewer is already running for '%1'.", vmName));
            return;
        }
        if (startSpiceViewer(vmName)) {
            vmModel->setHeadless(vmName, false);
            statusBar()->showMessage(i18n("Viewer reconnected to '%1'.", vmName));
            updateDetails();
            updateActionStates();
        }
        return;
    }

    if (entry->config.displayType() == VMSpecs::DisplayType::Dbus) {
        if (m_dbusViewers.contains(vmName)) {
            statusBar()->showMessage(i18n("Viewer is already running for '%1'.", vmName));
            return;
        }
        if (startDbusViewer(vmName)) {
            vmModel->setHeadless(vmName, false);
            statusBar()->showMessage(i18n("Viewer reconnected to '%1'.", vmName));
            updateDetails();
            updateActionStates();
        }
        return;
    }
}

void MainWindow::onTreeSelectionChanged()
{
    onVmSelectionChanged();
}

void MainWindow::onTreeDoubleClicked(const QModelIndex& index)
{
    const int itemType = index.data(VmModel::ItemTypeRole).toInt();

    if (itemType == VmModel::Folder) {
        vmTreeView->setExpanded(index, !vmTreeView->isExpanded(index));
    } else {
        const QString vmName = index.data(VmModel::VmNameRole).toString();
        const VmEntry* entry = vmModel->entryByName(vmName);

        const bool spiceReconnect = entry
            && entry->state == VmEntry::State::Running
            && entry->isHeadless
            && entry->config.displayType() == VMSpecs::DisplayType::Spice
            && !entry->spiceViewerProcess;

        const bool dbusReconnect = entry
            && entry->state == VmEntry::State::Running
            && entry->isHeadless
            && entry->config.displayType() == VMSpecs::DisplayType::Dbus
            && !m_dbusViewers.contains(vmName);

        if (spiceReconnect || dbusReconnect) {
            reconnectViewer();
        } else {
            startVM();
        }
    }
}

void MainWindow::onDropCompleted()
{
    saveFolderStructure();
}

void MainWindow::showContextMenu(const QPoint& pos)
{
    QModelIndex idx = vmTreeView->indexAt(pos);
    QMenu contextMenu(this);

    if (idx.isValid()) {
        int itemType = idx.data(VmModel::ItemTypeRole).toInt();

        if (itemType == VmModel::Folder) {
            auto* renameAction = contextMenu.addAction(QIcon::fromTheme(u"edit-rename"_s), i18n("Rename Folder"));
            connect(renameAction, &QAction::triggered, this, &MainWindow::renameFolder);

            auto* deleteAction = contextMenu.addAction(QIcon::fromTheme(u"edit-delete"_s), i18n("Delete Folder"));
            connect(deleteAction, &QAction::triggered, this, &MainWindow::deleteFolder);
            deleteAction->setEnabled(vmModel->rowCount(idx) == 0);

            contextMenu.addSeparator();
            contextMenu.addAction(actionCollection()->action(u"vm_new"_s));

        } else {
            const QString vmName = idx.data(VmModel::VmNameRole).toString();
            const VmEntry* entry = vmModel->entryByName(vmName);
            bool isRunning = entry && entry->state == VmEntry::State::Running;
            bool isDeploying = entry && entry->state == VmEntry::State::Deploying;
            bool isShrinking = entry && entry->state == VmEntry::State::Shrinking;

            if (isShrinking) {
                auto* infoAction = contextMenu.addAction(
                    QIcon::fromTheme(u"view-refresh"_s), i18n("Compacting disk…"));
                infoAction->setEnabled(false);
            } else if (isDeploying) {
                QAction* originalKillAction = actionCollection()->action(u"vm_kill"_s);
                QAction* customKillAction = contextMenu.addAction(
                    originalKillAction->icon(), i18n("Abort Deployment"));
                connect(customKillAction, &QAction::triggered, originalKillAction, &QAction::trigger);
                contextMenu.addAction(customKillAction);
            } else if (entry && entry->state == VmEntry::State::Cloning) {
                auto* infoAction = contextMenu.addAction(
                    QIcon::fromTheme(u"edit-copy"_s), i18n("Cloning…"));
                infoAction->setEnabled(false);
            } else if (!isRunning) {
                contextMenu.addAction(actionCollection()->action(u"vm_start"_s));
                contextMenu.addAction(actionCollection()->action(u"vm_start_headless"_s));
                contextMenu.addAction(actionCollection()->action(u"vm_edit"_s));
                contextMenu.addSeparator();
                contextMenu.addAction(actionCollection()->action(u"vm_clone"_s));
                contextMenu.addSeparator();
                contextMenu.addAction(actionCollection()->action(u"vm_shrink"_s));
                contextMenu.addAction(actionCollection()->action(u"vm_export"_s));
                contextMenu.addAction(actionCollection()->action(u"vm_manage_snapshots"_s));
                contextMenu.addSeparator();
                contextMenu.addAction(actionCollection()->action(u"vm_delete"_s));
            } else {
                if (actionCollection()->action(u"vm_reconnect"_s)->isEnabled())
                    contextMenu.addAction(actionCollection()->action(u"vm_reconnect"_s));

                contextMenu.addAction(actionCollection()->action(u"vm_shutdown"_s));
                contextMenu.addAction(actionCollection()->action(u"vm_reset"_s));

                contextMenu.addSeparator();

                contextMenu.addAction(actionCollection()->action(u"vm_change_iso"_s));
                contextMenu.addAction(actionCollection()->action(u"vm_change_floppy"_s));

                contextMenu.addSeparator();
                contextMenu.addAction(usbActionMenu);
                contextMenu.addSeparator();
                contextMenu.addAction(actionCollection()->action(u"vm_manage_snapshots"_s));
                contextMenu.addSeparator();
                if (actionCollection()->action(u"vm_network_shell"_s)->isEnabled())
                    contextMenu.addAction(actionCollection()->action(u"vm_network_shell"_s));
                contextMenu.addSeparator();

                contextMenu.addAction(actionCollection()->action(u"vm_kill"_s));
            }
        }
    } else {
        auto* newFolderAction = contextMenu.addAction(QIcon::fromTheme(u"folder-new"_s), i18n("New Folder"));
        connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
        contextMenu.addSeparator();
        contextMenu.addAction(actionCollection()->action(u"vm_new"_s));
        contextMenu.addSeparator();
        contextMenu.addAction(actionCollection()->action(u"vm_import"_s));
    }

    contextMenu.exec(vmTreeView->viewport()->mapToGlobal(pos));
}

void MainWindow::showCommand()
{
    if (currentConfig.name().isEmpty())
        return;

    NetworkContext netCtx;
    if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        netCtx.tapDevice = u"tap-<vm>"_s;

    const VirtualNetwork net = (m_networkManager && currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        ? m_networkManager->network(currentConfig.virtualNetworkName())
        : VirtualNetwork { };

    const QString sockPath = getVmBaseDir().absolutePath() + u"/qemu-monitor.sock"_s;
    const QString cmd = QemuCommandBuilder::buildShellScript(currentConfig, getVmDir(currentConfig.name()), sockPath, netCtx, net);

    QDialog d(this);
    d.setWindowTitle(i18n("QEMU Command Line"));
    auto* l = new QVBoxLayout(&d);

    auto* t = new QTextEdit();
    t->setPlainText(cmd);
    t->setReadOnly(true);
    t->setFont(QFont(u"monospace"_s, 10));
    l->addWidget(t);

    if (cmd.contains("UEFI_CODE_NOT_FOUND"_L1) || cmd.contains("VM_VARS_FILE_MISSING"_L1)) {
        QString warnText = currentConfig.useSecureBoot()
            ? i18n("WARNING: UEFI Secure Boot firmware not found! "
                   "Make sure a Secure Boot-capable OVMF package is installed "
                   "(e.g. 'edk2-ovmf' on Arch, 'ovmf' on Debian/Ubuntu) "
                   "or set the firmware path in Settings.")
            : i18n("WARNING: UEFI files missing! "
                   "Make sure 'edk2-ovmf' is installed or set the path in Settings.");
        auto* warn = new QLabel(warnText);
        warn->setWordWrap(true);
        l->addWidget(warn);
    }

    auto* btnLayout = new QHBoxLayout();
    auto* btnCopy = new QPushButton(QIcon::fromTheme(u"edit-copy"_s), i18n("Copy"));
    connect(btnCopy, &QPushButton::clicked, this, [cmd]() {
        QApplication::clipboard()->setText(cmd);
    });

    auto* btnClose = new QPushButton(QIcon::fromTheme(u"dialog-close"_s), i18n("Close"));
    connect(btnClose, &QPushButton::clicked, &d, &QDialog::accept);

    btnLayout->addStretch();
    btnLayout->addWidget(btnCopy);
    btnLayout->addWidget(btnClose);
    l->addLayout(btnLayout);

    d.resize(700, 400);
    d.exec();
}

void MainWindow::startVM(bool headless)
{
    const QString vmName = currentConfig.name();
    if (vmName.isEmpty())
        return;

    if (vmModel->isBusy(vmName)) {
        KMessageBox::error(this, i18n("The VM '%1' is currently busy.", vmName), i18n("Busy"));
        return;
    }

    if (currentConfig.useKvm() && !QFile::exists(u"/dev/kvm"_s))
        KMessageBox::error(this, i18n("/dev/kvm not found. Hardware virtualization may be disabled in BIOS."), i18n("KVM not available"));

    if (currentConfig.gpuType() == VMSpecs::GpuType::VgaISA) {
        const QString qemuDataDir = QFileInfo(ExternalTools::instance().path(ExternalTools::QemuSystem))
                                        .absoluteDir()
                                        .absoluteFilePath(u"../share/qemu"_s);
        const QString romPath = QDir(qemuDataDir).absoluteFilePath(u"vgabios.bin"_s);
        if (!QFile::exists(romPath)) {
            const QString isavgaRom = QDir(qemuDataDir).absoluteFilePath(u"vgabios-isavga.bin"_s);
            QString detail;
            if (QFile::exists(isavgaRom)) {
                detail = i18n("<p>A compatible ROM was found at:<br/>"
                              "<tt>%1</tt></p>"
                              "<p>To fix this, create a symbolic link:</p>"
                              "<pre>sudo ln -s vgabios-isavga.bin %2</pre>",
                    isavgaRom, romPath);
            } else {
                detail = i18n("Please install the QEMU VGA BIOS package for your distribution.");
            }
            KMessageBox::error(this,
                u"<p>"_s
                    + i18n("The ISA VGA device requires %1 which was not found in:",
                        u"<tt>vgabios.bin</tt><br/>"_s)
                    + u"<tt>"_s + qemuDataDir + u"</tt></p>"_s
                    + detail,
                i18n("Missing VGA BIOS ROM"));
            return;
        }
    }

    if (!isDisplayTypeAvailable(currentConfig.displayType())) {
        const auto configuredType = currentConfig.displayType();
        const auto fallbackType = VMSpecs::DisplayType::Gtk;

        const auto result = KMessageBox::warningTwoActions(this,
            i18n("The configured display type '%1' is currently not available.\n"
                 "KEMU will use '%2' instead.\n\n",
                VMConfig::toUIString(configuredType),
                VMConfig::toUIString(fallbackType)),
            i18n("Display Type Unavailable"),
            KStandardGuiItem::cont(),
            KStandardGuiItem::cancel());

        if (result != KMessageBox::PrimaryAction)
            return;
    }

    QFile::remove(monitorSocketPath(vmName));

    // Remove any stale D-Bus display socket from a previous run
    if (currentConfig.displayType() == VMSpecs::DisplayType::Dbus)
        cleanupDbusSocket(vmName);

    if (!headless && currentConfig.displayType() == VMSpecs::DisplayType::Dbus) {
        if (!startDbusDaemon(vmName))
            return;
    }

    if (currentConfig.useTpm()) {
        if (!startSwtpm(vmName))
            return;
    }

    if (!currentConfig.sharedPath().isEmpty()
        && currentConfig.osType() == VMSpecs::OsType::Windows) {
        if (!startVirtiofsd(vmName))
            return;
    }

    QString tapDevice;
    if (currentConfig.networkCard() != VMSpecs::NetworkCard::None) {
        if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            tapDevice = ensureNetworkRunning(currentConfig.virtualNetworkName(), vmName);
            if (tapDevice.isEmpty())
                return;
        } else {
            if (!startPasst(vmName, currentConfig.portForwards()))
                return;
        }
    }

    const QString sockPath = getVmDir(currentConfig.name()).absolutePath() + u"/qemu-monitor.sock"_s;

    NetworkContext netCtx;
    if (!tapDevice.isEmpty())
        netCtx.tapDevice = tapDevice;

    const QStringList args = QemuCommandBuilder::build(currentConfig, getVmDir(currentConfig.name()), sockPath, netCtx, false, headless);

    if (!requireTool(ExternalTools::QemuSystem))
        return;

    QPointer<KProcess> process = new KProcess(this);

    if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork
        && !tapDevice.isEmpty()) {
        const qint64 nsPid = m_networkManager->namespacePid(
            currentConfig.virtualNetworkName());
        NamespaceHelper::prepareProcessForNamespace(process, nsPid);
    }
    process->setProgram(ExternalTools::instance().path(ExternalTools::QemuSystem));
    process->setArguments(args);

    connect(process, QOverload<int, KProcess::ExitStatus>::of(&KProcess::finished), this, &MainWindow::onProcessFinished);

    process->start();

    if (process->waitForStarted()) {
        vmModel->setHeadless(vmName, headless);
        vmModel->setQemuProcess(vmName, process);
        vmModel->setVmState(vmName, VmEntry::State::Running);
        statusBar()->showMessage(i18n("VM '%1' started.", vmName));
        updateDetails();
        updateActionStates();

        // Launch SPICE viewer if configured
        if (!headless && currentConfig.displayType() == VMSpecs::DisplayType::Spice) {
            startSpiceViewer(vmName);
        }

        // Launch integrated D-Bus viewer if configured
        if (!headless && currentConfig.displayType() == VMSpecs::DisplayType::Dbus) {
            QTimer::singleShot(500, this, [this, vmName]() {
                startDbusViewer(vmName);
            });
        }

    } else {
        KMessageBox::error(this, i18n("Failed to start QEMU: %1", process->errorString()));
        delete process;
        return;
    }

    QPointer<QMPClient> client = new QMPClient(this);
    vmModel->setQmpClient(vmName, client);
    connect(client, &QMPClient::eventReceived, this,
        [this, vmName](const QString& eventName, const QJsonObject& data) {
            this->onQMPEvent(vmName, eventName, data);
        });

    connect(client, &QMPClient::connected, this, [this, client, vmName]() {
        client->startPerformanceTimer(1000);

        // Enable balloon guest statistics for actual RAM usage
        QJsonObject balloonArgs;
        balloonArgs[u"path"_s] = u"/machine/peripheral-anon/balloon0"_s;
        balloonArgs[u"property"_s] = u"guest-stats-polling-interval"_s;
        balloonArgs[u"value"_s] = 2;
        client->sendCommand(u"qom-set"_s, balloonArgs);

        if (selectedVmName() == vmName && perfMonitor) {
            const VmEntry* e = vmModel->entryByName(vmName);
            if (e) {
                perfMonitor->setHistory(PerformanceMonitorWidget::Cpu, e->cpuHistory);
                perfMonitor->setHistory(PerformanceMonitorWidget::Ram, e->ramHistory);
                perfMonitor->setHistory(PerformanceMonitorWidget::Io, e->ioHistory);
            }
            perfMonitor->setActive(true);
        }
    });

    connect(client, &QMPClient::cpuUsageUpdated, this,
        [this, vmName](const VmCpuUsage& usage) {
            if (!usage.isValid)
                return;
            vmModel->appendPerfSample(vmName, 0, usage.averagePercent);
            if (selectedVmName() == vmName && perfMonitor)
                perfMonitor->addSample(PerformanceMonitorWidget::Cpu,
                    usage.averagePercent);
        });

    connect(client, &QMPClient::memoryUsageUpdated, this,
        [this, vmName](const VmMemoryUsage& usage) {
            if (!usage.isValid)
                return;
            vmModel->appendPerfSample(vmName, 1, usage.usagePercent);
            if (selectedVmName() == vmName && perfMonitor)
                perfMonitor->addSample(PerformanceMonitorWidget::Ram,
                    usage.usagePercent);
        });

    connect(client, &QMPClient::blockStatsUpdated, this,
        [this, vmName](const VmBlockStats& stats) {
            if (!stats.isValid)
                return;
            VmEntry* e = vmModel->entryByName(vmName);
            if (!e)
                return;

            double totalBps = stats.totalRdBytesPerSec + stats.totalWrBytesPerSec;
            if (totalBps > e->ioPeakBytesPerSec)
                e->ioPeakBytesPerSec = totalBps;
            double pct = (e->ioPeakBytesPerSec > 0.0)
                ? (totalBps / e->ioPeakBytesPerSec * 100.0)
                : 0.0;

            vmModel->appendPerfSample(vmName, 2, pct);
            if (selectedVmName() == vmName && perfMonitor)
                perfMonitor->addSample(PerformanceMonitorWidget::Io, pct);
        });

    if (Utils::waitForSocket(sockPath, 2000)) {
        client->connectToVm(sockPath);
    } else {
        qWarning() << "Failed to connect to QMP: Socket was not created in time.";
    }
}

void MainWindow::startVMHeadless()
{
    startVM(true);
}

void MainWindow::shutdownVM()
{
    if (!vmModel->isRunning(currentConfig.name())
        && !(vmModel->entryByName(currentConfig.name())
            && vmModel->entryByName(currentConfig.name())->state == VmEntry::State::Deploying))
        return;

    // Handle deployment shutdown
    const VmEntry* deployEntry = vmModel->entryByName(currentConfig.name());
    if (deployEntry && deployEntry->state == VmEntry::State::Deploying) {
        auto answer = KMessageBox::questionTwoActionsCancel(this,
            i18n("VM '%1' is currently being deployed.\n\n"
                 "Stopping now will abort the installation. "
                 "Do you also want to delete the VM?",
                currentConfig.name()),
            i18n("Abort Deployment"),
            KGuiItem(i18n("Stop && Keep"), u"media-playback-stop"_s),
            KGuiItem(i18n("Stop && Delete"), u"edit-delete"_s),
            KStandardGuiItem::cancel());

        if (answer == KMessageBox::PrimaryAction)
            abortDeployment(currentConfig.name(), false);
        else if (answer == KMessageBox::SecondaryAction)
            abortDeployment(currentConfig.name(), true);
        return;
    }

    const VmEntry* entry = vmModel->entryByName(currentConfig.name());
    if (!entry)
        return;
    QPointer<QMPClient> client = entry->qmpClient;
    if (!client || !client->isConnected()) {
        KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
        return;
    }

    QMPResult result = client->sendCommandSync(u"system_powerdown"_s);

    if (!result.success) {
        KMessageBox::error(this, i18n("Shutdown Failed"), result.errorMessage);
        return;
    }

    statusBar()->showMessage(i18n("Shutting down VM '%1'…", currentConfig.name()));
}

void MainWindow::resetVM()
{
    if (!vmModel->isRunning(currentConfig.name()))
        return;
    const VmEntry* entry = vmModel->entryByName(currentConfig.name());
    if (!entry)
        return;
    QPointer<QMPClient> client = entry->qmpClient;
    if (!client || !client->isConnected()) {
        KMessageBox::error(this, i18n("Could not connect to QEMU monitor. Is the VM running?"));
        return;
    }

    QMPResult result = client->sendCommandSync(u"system_reset"_s);

    if (!result.success) {
        KMessageBox::error(this, i18n("Reset Failed"), result.errorMessage);
        return;
    }

    statusBar()->showMessage(i18n("VM '%1' reset.", currentConfig.name()));
}

void MainWindow::killVM()
{
    const VmEntry* killEntry = vmModel->entryByName(currentConfig.name());
    if (!killEntry || (killEntry->state != VmEntry::State::Running && killEntry->state != VmEntry::State::Deploying))
        return;

    if (killEntry->state == VmEntry::State::Deploying) {
        auto answer = KMessageBox::questionTwoActionsCancel(this,
            i18n("VM '%1' is currently being deployed.\n\n"
                 "Force-stopping now will abort the installation "
                 "and may leave the disk in an inconsistent state.\n\n"
                 "Do you also want to delete the VM?",
                currentConfig.name()),
            i18n("Abort Deployment"),
            KGuiItem(i18n("Stop && Keep"), u"media-playback-stop"_s),
            KGuiItem(i18n("Stop && Delete"), u"edit-delete"_s),
            KStandardGuiItem::cancel());

        if (answer == KMessageBox::PrimaryAction)
            abortDeployment(currentConfig.name(), false);
        else if (answer == KMessageBox::SecondaryAction)
            abortDeployment(currentConfig.name(), true);
        return;
    }

    const VmEntry* entry = vmModel->entryByName(currentConfig.name());
    if (!entry)
        return;

    const QString vmName = currentConfig.name();

    QPointer<KProcess> process = entry->qemuProcess;
    if (!process)
        return;

    KGuiItem stopItem(
        i18n("Force Stop"),
        u"kt-stop"_s,
        i18n("Forcefully stop the virtual machine"));

    auto result = KMessageBox::warningTwoActions(
        this,
        i18n("Are you sure you want to force a power-off for '%1'? "
             "This is equivalent to pulling the plug and may cause data loss.",
            vmName),
        i18nc("@title:window", "Confirm Force Stop"),
        stopItem,
        KStandardGuiItem::cancel());

    if (result == KMessageBox::PrimaryAction) {

        bool qmpSuccess = false;

        QPointer<QMPClient> client = entry->qmpClient;
        if (client && client->isConnected()) {
            QMPResult res = client->sendCommandSync(u"quit"_s, QJsonObject(), 2000);
            qmpSuccess = res.success;
        }

        if (qmpSuccess && process->state() == KProcess::Running) {
            process->waitForFinished(3000);
        }

        if (process->state() == KProcess::Running) {
            qWarning() << "VM" << vmName << "did not exit gracefully. Forcing terminate (SIGTERM)!";
            process->terminate();

            if (!process->waitForFinished(2000)) {
                qWarning() << "VM" << vmName << "is still hanging. Forcing kill (SIGKILL)!";
                process->kill();
            }
        }

        statusBar()->showMessage(i18n("Force stop initiated for VM '%1'.", vmName));
    }
}

void MainWindow::onProcessFinished(int, KProcess::ExitStatus)
{
    QPointer<KProcess> p = qobject_cast<KProcess*>(sender());
    if (!p)
        return;

    QString vmName;
    for (const QString& name : vmModel->vmNames()) {
        const VmEntry* e = vmModel->entryByName(name);
        if (e && e->qemuProcess == p) {
            vmName = name;
            break;
        }
    }

    if (vmName.isEmpty()) {
        p->deleteLater();
        return;
    }

    VmEntry* entry = vmModel->entryByName(vmName);

    // Deployment finalization
    if (entry->state == VmEntry::State::Deploying) {
        if (entry->qmpClient) {
            entry->qmpClient->disconnectFromVm();
            entry->qmpClient->deleteLater();
        }
        if (entry->swtpmProcess) {
            entry->swtpmProcess->terminate();
            if (!entry->swtpmProcess->waitForFinished(3000))
                entry->swtpmProcess->kill();
            delete entry->swtpmProcess;
            QFile::remove(getVmBaseDir().filePath(vmName) + u"/swtpm.sock"_s);
        }
        if (entry->virtiofsdProcess) {
            entry->virtiofsdProcess->terminate();
            if (!entry->virtiofsdProcess->waitForFinished(3000))
                entry->virtiofsdProcess->kill();
            delete entry->virtiofsdProcess;
            QFile::remove(getVmBaseDir().filePath(vmName) + u"/virtiofsd.sock.pid"_s);
        }
        if (entry->passtProcess) {
            entry->passtProcess->terminate();
            if (!entry->passtProcess->waitForFinished(3000))
                entry->passtProcess->kill();
            delete entry->passtProcess;
            QFile::remove(getVmBaseDir().filePath(vmName) + u"/passt.sock"_s);
        }
        if (entry->spiceViewerProcess) {
            entry->spiceViewerProcess->terminate();
            if (!entry->spiceViewerProcess->waitForFinished(2000))
                entry->spiceViewerProcess->kill();
            delete entry->spiceViewerProcess;
        }
        if (entry->dbusDaemonProcess) {
            entry->dbusDaemonProcess->terminate();
            if (!entry->dbusDaemonProcess->waitForFinished(2000))
                entry->dbusDaemonProcess->kill();
            delete entry->dbusDaemonProcess;
            QFile::remove(getVmBaseDir().filePath(vmName) + u"/dbus-bus.sock"_s);
            QFile::remove(getVmBaseDir().filePath(vmName) + u"/dbus-bus.conf"_s);
        }
        // D-Bus integrated viewer cleanup
        stopDbusViewer(vmName);
        cleanupDbusSocket(vmName);

        QFile::remove(monitorSocketPath(vmName));
        releaseNetwork(entry->config);
        vmModel->clearRuntimeState(vmName);

        finalizeDeployment(vmName);

        if (selectedVmName() == vmName) {
            updateDetails();
            updateActionStates();
            if (perfMonitor) {
                perfMonitor->clearHistory();
                perfMonitor->setActive(false);
            }
        }
        p->deleteLater();
        return; // Skip normal cleanup
    }

    if (entry->qmpClient) {
        entry->qmpClient->disconnectFromVm();
        entry->qmpClient->deleteLater();
    }

    if (entry->swtpmProcess) {
        entry->swtpmProcess->terminate();
        if (!entry->swtpmProcess->waitForFinished(3000))
            entry->swtpmProcess->kill();
        delete entry->swtpmProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/swtpm.sock"_s);
    }

    if (entry->virtiofsdProcess) {
        entry->virtiofsdProcess->terminate();
        if (!entry->virtiofsdProcess->waitForFinished(3000))
            entry->virtiofsdProcess->kill();
        delete entry->virtiofsdProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/virtiofsd.sock.pid"_s);
    }

    if (entry->passtProcess) {
        entry->passtProcess->terminate();
        if (!entry->passtProcess->waitForFinished(3000))
            entry->passtProcess->kill();
        delete entry->passtProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/passt.sock"_s);
    }

    if (entry->dbusDaemonProcess) {
        entry->dbusDaemonProcess->terminate();
        if (!entry->dbusDaemonProcess->waitForFinished(2000))
            entry->dbusDaemonProcess->kill();
        delete entry->dbusDaemonProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/dbus-bus.sock"_s);
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/dbus-bus.conf"_s);
    }

    stopDbusViewer(vmName);
    cleanupDbusSocket(vmName);

    QFile::remove(monitorSocketPath(vmName));
    releaseNetwork(entry->config);
    vmModel->clearRuntimeState(vmName);
    statusBar()->showMessage(i18n("VM '%1' stopped.", vmName));

    if (selectedVmName() == vmName) {
        updateDetails();
        updateActionStates();
        if (perfMonitor) {
            perfMonitor->clearHistory();
            perfMonitor->setActive(false);
        }
    }
    p->deleteLater();
}

void MainWindow::onQMPEvent(const QString& vmName, const QString& eventName, const QJsonObject& data)
{
    qDebug() << "QMP Event for" << vmName << ":" << eventName << data;

    if (eventName == u"SHUTDOWN"_s) {
        statusBar()->showMessage(i18n("VM '%1' is shutting down…", vmName));
    }

    else if (eventName == u"GUEST_CRASHED"_s) {
        KMessageBox::error(this, i18n("The virtual machine '%1' has crashed.", vmName), i18n("VM Crashed"));
    }

    else if (eventName == u"STOP"_s) {
        statusBar()->showMessage(i18n("VM '%1' paused.", vmName));
    }

    else if (eventName == u"DEVICE_TRAY_MOVED"_s) {
        if (data.value(u"id"_s).toString() == u"cdrom0"_s && data.value(u"tray-open"_s).toBool()) {
            if (currentConfig.name() == vmName && !currentConfig.isoPath().isEmpty()) {
                currentConfig.setISOPath({ });
                saveVmConfig(getVmDir(vmName).absolutePath(), currentConfig);
                updateDetails();
                statusBar()->showMessage(i18n("VM '%1' ejected the ISO.", vmName));
            }
        }
    }
}

void MainWindow::onUsbMenuAboutToShow()
{
    usbActionMenu->menu()->clear();

    QString vmName = selectedVmName();
    VmEntry* entry = vmModel ? vmModel->entryByName(vmName) : nullptr;

    if (!entry || entry->state != VmEntry::State::Running || !entry->qmpClient) {
        auto* a = usbActionMenu->menu()->addAction(i18n("VM not running"));
        a->setEnabled(false);
        return;
    }

    QList<UsbDeviceInfo> devices = UsbEnumerator::enumerate();

    if (devices.isEmpty()) {
        auto* a = usbActionMenu->menu()->addAction(i18n("No USB devices found"));
        a->setEnabled(false);
        return;
    }

    for (const auto& dev : devices) {
        QString label = QStringLiteral("%1  [%2]").arg(dev.displayName(), dev.idString());

        QAction* action = usbActionMenu->menu()->addAction(label);
        action->setCheckable(true);
        action->setChecked(entry->attachedUsb.contains(dev.busKey()));

        // Store device info for the handler
        action->setData(QVariantList { dev.busNum, dev.devAddr, dev.busKey(), dev.qemuId() });

        connect(action, &QAction::triggered, this, &MainWindow::onUsbActionTriggered);
    }
}

void MainWindow::onUsbActionTriggered()
{
    auto* action = qobject_cast<QAction*>(sender());
    if (!action)
        return;

    QVariantList d = action->data().toList();
    int busNum = d[0].toInt();
    int devAddr = d[1].toInt();
    QString busKey = d[2].toString();
    QString qemuId = d[3].toString();

    QString vmName = selectedVmName();
    VmEntry* entry = vmModel ? vmModel->entryByName(vmName) : nullptr;
    if (!entry || !entry->qmpClient)
        return;

    if (action->isChecked()) {
        // Attach
        QMPResult r = entry->qmpClient->attachUsbDevice(busNum, devAddr, qemuId);
        if (r.success) {
            entry->attachedUsb.insert(busKey, qemuId);
        } else {
            action->setChecked(false);
            KMessageBox::error(this,
                i18n("Could not attach USB device.\n\n%1\n\n"
                     "Make sure QEMU was started with -device qemu-xhci "
                     "and has permission to access /dev/bus/usb/.",
                    r.errorMessage),
                i18n("USB Passthrough"));
        }
    } else {
        // Detach
        QMPResult r = entry->qmpClient->detachUsbDevice(qemuId);
        if (r.success) {
            entry->attachedUsb.remove(busKey);
        } else {
            action->setChecked(true);
            KMessageBox::error(this, r.errorMessage, i18n("USB Passthrough"));
        }
    }
}

void MainWindow::importOVA(const QString& filePath)
{

    auto* job = new OvaImportJob(filePath, getVmBaseDir().absolutePath(), 0, this);

    if (jobTracker) {
        jobTracker->registerJob(job);
    }
    activeImports.append(job);

    connect(job, &OvaImportJob::finished, this, [this](KJob* kjob) {
        auto* importJob = qobject_cast<OvaImportJob*>(kjob);
        if (!importJob)
            return;

        activeImports.removeOne(importJob);

        if (importJob->error()) {

            statusBar()->showMessage(i18n("OVA import failed: %1", importJob->errorText()));
            KMessageBox::error(this, i18n("Failed to import OVA:\n%1", importJob->errorText()), i18n("Import Failed"));

        } else {

            VMConfig config = importJob->importedConfig();
            if (SettingsDialog::importRegenerateMac()) {
                config.setMacAddress(Utils::generateRandomMac(usedMacAddresses()));
            }
            saveVmConfig(getVmDir(config.name()).absolutePath(), config);

            statusBar()->showMessage(i18n("VM '%1' imported from OVA.", config.name()));
            loadVmList();

            selectVmByName(config.name());
        }

        importJob->deleteLater();
    });

    job->start();
    statusBar()->showMessage(i18n("Importing OVA…"));
}

void MainWindow::exportOVA(const QString& filePath)
{
    DiskInfo dInfo = getDiskInfo(currentConfig.diskPath());
    int diskGb = dInfo.isValid ? qRound(dInfo.virtualSizeGb) : currentConfig.diskSizeGb();

    VMConfig exportConfig = currentConfig;
    exportConfig.setDiskSizeGb(diskGb);

    auto* job = new OvaExportJob(currentConfig.name(),
        exportConfig,
        currentConfig.diskPath(),
        filePath, this);

    if (jobTracker) {
        jobTracker->registerJob(job);
    }
    vmModel->setVmState(currentConfig.name(), VmEntry::State::Exporting);

    connect(job, &OvaExportJob::finished, this, [this](KJob* kjob) {
        auto* exportJob = qobject_cast<OvaExportJob*>(kjob);
        if (!exportJob)
            return;

        QString vmName = exportJob->vmName();
        vmModel->setVmState(vmName, VmEntry::State::Stopped);

        if (exportJob->error()) {
            statusBar()->showMessage(
                i18n("OVA export of VM '%1' failed: %2", vmName, exportJob->errorText()));
            KMessageBox::error(this, i18n("Failed to export VM '%1' as OVA:\n%2", vmName, exportJob->errorText()), i18n("Export Failed"));
        } else {
            statusBar()->showMessage(i18n("VM '%1' exported as OVA.", vmName));
        }

        if (currentConfig.name() == vmName) {
            updateDetails();
        }
        updateActionStates();

        exportJob->deleteLater();
    });

    job->start();

    statusBar()->showMessage(i18n("Exporting VM '%1' as OVA…", currentConfig.name()));
    updateActionStates();
    updateDetails();
}

void MainWindow::createNewVM()
{
    CreateVmDialog dialog(this);

    dialog.setExistingVmNames(existingVmNames());
    dialog.setImageDatabase(&m_imageDatabase);
    dialog.setNetworkManager(m_networkManager);
    dialog.setMacUsageInfo(macUsageInfo());

    if (dialog.exec() != QDialog::Accepted)
        return;

    VMConfig newConfig = dialog.getResult();
    QDir baseDir = getVmBaseDir();

    if (newConfig.name().isEmpty() || baseDir.exists(newConfig.name())) {
        KMessageBox::error(this, i18n("Invalid name or VM already exists."));
        return;
    }

    if (!requireTool(ExternalTools::QemuImg))
        return;

    newConfig.ensureMacAddress(usedMacAddresses());

    baseDir.mkdir(newConfig.name());
    const QString vmPath = baseDir.filePath(newConfig.name());
    const QString diskPath = QDir(vmPath).filePath(KEMUConst::VMDiskFileName);

    if (!dialog.isCloudDeployRequested()) {
        KProcess imgProcess;
        imgProcess.setOutputChannelMode(KProcess::SeparateChannels);
        imgProcess.setProgram(ExternalTools::instance().path(ExternalTools::QemuImg));
        imgProcess.setArguments({ u"create"_s, u"-f"_s, u"qcow2"_s, diskPath, u"%1G"_s.arg(newConfig.diskSizeGb()) });
        imgProcess.start();
        imgProcess.waitForFinished(10000);

        if (imgProcess.exitCode() != 0) {
            KMessageBox::error(this, i18n("Failed to create disk image."));
            baseDir.rmdir(newConfig.name());
            return;
        }
    }

    // --- UEFI / OVMF ---
    if (newConfig.useUefi()) {
        const Utils::OvmfPaths ovmf = Utils::findOvmfPaths(newConfig.useSecureBoot());
        if (!ovmf.varsPath.isEmpty() && QFile::exists(ovmf.varsPath)) {
            QFile::copy(ovmf.varsPath, vmPath + u"/OVMF_VARS.fd"_s);
            QFile varsFile(vmPath + u"/OVMF_VARS.fd"_s);
            varsFile.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser);

            if (newConfig.useSecureBoot()
                && !ovmf.varsPath.contains(u"ms"_s, Qt::CaseInsensitive)
                && !ovmf.varsPath.contains(u"secboot"_s, Qt::CaseInsensitive)) {
                KMessageBox::information(this,
                    i18n("The OVMF VARS template found does not appear to contain "
                         "pre-enrolled Microsoft keys. Secure Boot may require "
                         "manual key enrollment in the UEFI firmware setup.\n\n"
                         "For pre-enrolled keys, install the appropriate package "
                         "(e.g. 'edk2-ovmf' on Arch, 'ovmf' on Debian/Ubuntu)."),
                    i18n("Note"));
            }
        } else {
            KMessageBox::error(this, i18n("Could not find OVMF_VARS template. Please install 'edk2-ovmf' or set the firmware path in Settings."), i18n("Warning"));
        }
    }

    // ======== CLOUD DEPLOY ========
    if (dialog.isCloudDeployRequested()) {
        const CloudImageInfo image = dialog.selectedCloudImage();
        const CloudDeployConfig cloudCfg = dialog.cloudConfig();

        QString imageLocalPath = ImageDatabase::localImagePath(image.url);
        bool needsDownload = !ImageDatabase::isImageCached(image.url);

        newConfig.setImageType(VMConfig::ImageType::CloudImage);
        newConfig.setDeployState(VMConfig::DeployState::Deploying);
        saveVmConfig(vmPath, newConfig);
        loadVmList();
        selectVmByName(newConfig.name());

        PendingDeployment pd;
        pd.type = PendingDeployment::Type::CloudImage;
        pd.imageUrl = image.url;
        pd.imageLocalPath = imageLocalPath;
        pd.cloudConfig = cloudCfg;
        m_pendingDeployments.insert(newConfig.name(), pd);

        vmModel->setVmState(newConfig.name(), VmEntry::State::Deploying);
        updateDetails();
        updateActionStates();

        if (needsDownload) {
            statusBar()->showMessage(i18n("Deploying '%1' — downloading cloud image…", newConfig.name()));

            auto* job = new DownloadJob(image.url, imageLocalPath, Utils::ChecksumType::NONE, { }, this);
            jobTracker->registerJob(job);

            connect(job, &DownloadJob::statusMessage, this, [this](const QString& msg) { statusBar()->showMessage(msg); });

            connect(job, &KJob::finished, this,
                [this, vmName = newConfig.name()](KJob* kjob) {
                    auto* dlJob = qobject_cast<DownloadJob*>(kjob);
                    if (!dlJob)
                        return;

                    if (dlJob->error()) {
                        statusBar()->showMessage(i18n("Image download for '%1' failed: %2", vmName, dlJob->errorText()));
                        KMessageBox::error(this, i18n("Image download failed:\n%1", dlJob->errorText()));
                        abortDeployment(vmName, false);
                    } else {
                        QTimer::singleShot(0, this, [this, vmName]() {
                            deployCloudImage(vmName);
                        });
                    }
                    dlJob->deleteLater();
                });

            job->start();
        } else {
            deployCloudImage(newConfig.name());
        }
        return;
    }

    // ======== MANUAL ISO DEPLOY ========
    if (dialog.isManualIsoDeployRequested()) {
        newConfig.setImageType(VMConfig::ImageType::InstallerIso);
        newConfig.setDeployState(VMConfig::DeployState::Deploying);
        newConfig.setBootFromIso(true);
        saveVmConfig(vmPath, newConfig);
        loadVmList();
        selectVmByName(newConfig.name());

        PendingDeployment pd;
        pd.type = PendingDeployment::Type::ManualIso;
        m_pendingDeployments.insert(newConfig.name(), pd);

        vmModel->setVmState(newConfig.name(), VmEntry::State::Deploying);
        startDeployment(newConfig.name());
        return;
    }
    // ======== End Deploy ========

    saveVmConfig(vmPath, newConfig);
    loadVmList();

    selectVmByName(newConfig.name());
    statusBar()->showMessage(i18n("VM '%1' created.", newConfig.name()));
}

void MainWindow::editVM()
{
    if (currentConfig.name().isEmpty())
        return;
    if (vmModel->isBusy(currentConfig.name())) {
        KMessageBox::error(this, i18n("Please stop the VM before editing."), i18n("Busy"));
        return;
    }

    DiskInfo dInfo = getDiskInfo(currentConfig.diskPath());
    if (dInfo.isValid)
        currentConfig.setDiskSizeGb(qRound(dInfo.virtualSizeGb));

    CreateVmDialog dialog(this);

    dialog.setExistingVmNames(existingVmNames());
    dialog.setImageDatabase(&m_imageDatabase);
    dialog.setNetworkManager(m_networkManager);
    dialog.setMacUsageInfo(macUsageInfo());

    dialog.setupForEdit(currentConfig);

    if (dialog.exec() == QDialog::Accepted) {
        VMConfig newConfig = dialog.getResult();

        newConfig.ensureMacAddress(usedMacAddresses());

        const QString oldName = currentConfig.name();
        const QString newName = newConfig.name();

        // Rename?
        if (newName != oldName) {
            QDir baseDir = getVmBaseDir();
            const QString oldPath = baseDir.filePath(oldName);
            const QString newPath = baseDir.filePath(newName);

            if (!baseDir.rename(oldName, newName)) {
                KMessageBox::error(this,
                    i18n("Could not rename VM folder from '%1' to '%2'.\n"
                         "Check file permissions and try again.",
                        oldName, newName),
                    i18n("Rename Failed"));
                return;
            }

            QString oldDiskPath = newConfig.diskPath();
            if (oldDiskPath.contains(oldName)) {
                newConfig.setDiskPath(oldDiskPath.replace(oldPath, newPath));
            }

            KConfig kconfig(u"kemurc"_s);
            KConfigGroup grp = kconfig.group(u"Folders"_s);
            QString folder = grp.readEntry(oldName, { });
            if (!folder.isEmpty()) {
                grp.deleteEntry(oldName);
                grp.writeEntry(newName, folder);
                grp.sync();
            }
        }

        QString vmPath = getVmDir(newConfig.name()).absolutePath();

        if (newConfig.useUefi() && !QFile::exists(vmPath + u"/OVMF_VARS.fd"_s))
            KMessageBox::information(this, i18n("UEFI enabled. A VARS file will be required on next start."));

        saveVmConfig(vmPath, newConfig);
        currentConfig = newConfig;
        loadVmList();
        selectVmByName(newConfig.name());
        updateDetails();
        statusBar()->showMessage(i18n("VM '%1' updated.", newConfig.name()));
    }
}

void MainWindow::deleteVM()
{
    if (currentConfig.name().isEmpty())
        return;
    if (vmModel->isBusy(currentConfig.name()))
        return;

    auto answer = KMessageBox::questionTwoActions(
        this,
        i18n("Delete VM '%1' and all associated files?", currentConfig.name()),
        i18n("Delete VM"),
        KGuiItem(i18n("Delete"), QStringLiteral("edit-delete")),
        KStandardGuiItem::cancel());

    if (answer == KMessageBox::PrimaryAction) {
        getVmDir(currentConfig.name()).removeRecursively();
        currentConfig = VMConfig();
        loadVmList();
        onVmSelectionChanged();
        statusBar()->showMessage(i18n("VM deleted."));
    }
}

void MainWindow::cloneVM()
{
    const QString srcName = currentConfig.name();
    if (srcName.isEmpty())
        return;

    if (vmModel->isBusy(srcName)) {
        KMessageBox::error(this, i18n("The VM '%1' is currently busy.", srcName), i18n("Busy"));
        return;
    }

    QString suggestion = srcName + u"_clone"_s;
    int suffix = 1;
    while (existingVmNames().contains(suggestion))
        suggestion = u"%1_clone%2"_s.arg(srcName).arg(++suffix);

    bool ok = false;
    QString cloneName = QInputDialog::getText(this,
        i18n("Clone VM"),
        i18n("Name for the cloned VM:"),
        QLineEdit::Normal, suggestion, &ok);

    if (!ok || cloneName.trimmed().isEmpty())
        return;

    cloneName = cloneName.trimmed();

    if (existingVmNames().contains(cloneName)) {
        KMessageBox::error(this, i18n("A VM named '%1' already exists.", cloneName), i18n("Error"));
        return;
    }

    const QDir baseDir = getVmBaseDir();
    const QString srcPath = baseDir.filePath(srcName);
    const QString dstPath = baseDir.filePath(cloneName);

    auto* job = new CloneJob(srcName, srcPath, dstPath, this);

    if (jobTracker)
        jobTracker->registerJob(job);

    vmModel->setVmState(srcName, VmEntry::State::Cloning);

    connect(job, &CloneJob::finished, this,
        [this, srcName, cloneName, dstPath](KJob* kjob) {
            auto* cloneJob = qobject_cast<CloneJob*>(kjob);
            if (!cloneJob)
                return;

            vmModel->setVmState(srcName, VmEntry::State::Stopped);

            if (cloneJob->error()) {
                statusBar()->showMessage(i18n("Cloning of VM '%1' failed: %2", srcName, cloneJob->errorText()));
                KMessageBox::error(this, i18n("Failed to clone VM '%1':\n%2", srcName, cloneJob->errorText()), i18n("Clone Failed"));
            } else {

                // collect used macs to exclude random generation matches
                QStringList usedMacs;
                for (const QString& name : existingVmNames()) {
                    VMConfig cfg = loadVmConfig(getVmBaseDir().filePath(name));
                    if (!cfg.macAddress().isEmpty())
                        usedMacs << cfg.macAddress();
                }

                VMConfig cloneConfig = loadVmConfig(dstPath);
                cloneConfig.setName(cloneName);
                cloneConfig.setDiskPath(dstPath + u"/disk.qcow2"_s);
                cloneConfig.setDeployState(VMConfig::DeployState::Complete);

                if (SettingsDialog::cloneRegenerateMac()) {
                    QStringList macs = usedMacAddresses();

                    if (!cloneConfig.macAddress().isEmpty()) {
                        auto answer = KMessageBox::questionTwoActions(this,
                            i18n("The source VM has an explicit MAC address (%1).\n\n"
                                 "Using the same MAC on two VMs causes network "
                                 "conflicts. Would you like to enter a new MAC "
                                 "address, or clear it so QEMU generates a "
                                 "random one?",
                                cloneConfig.macAddress()),
                            i18n("MAC Address"),
                            KGuiItem(i18n("Enter New MAC…"), u"document-edit"_s),
                            KGuiItem(i18n("Generate Random"), u"roll"_s));

                        if (answer == KMessageBox::PrimaryAction) {
                            bool macOk = false;
                            QString newMac = QInputDialog::getText(this,
                                i18n("New MAC Address"),
                                i18n("MAC Address (e.g. 52:54:00:11:22:33):"),
                                QLineEdit::Normal, { }, &macOk);
                            cloneConfig.setMacAddress(
                                (macOk && !newMac.trimmed().isEmpty())
                                    ? newMac.trimmed()
                                    : Utils::generateRandomMac(macs));
                        } else {
                            cloneConfig.setMacAddress(Utils::generateRandomMac(macs));
                        }
                    } else {
                        cloneConfig.setMacAddress(Utils::generateRandomMac(macs));
                    }
                }

                saveVmConfig(dstPath, cloneConfig);
                loadVmList();
                selectVmByName(cloneName);
                statusBar()->showMessage(
                    i18n("VM '%1' cloned as '%2'.", srcName, cloneName));
            }

            if (currentConfig.name() == srcName)
                updateDetails();
            updateActionStates();

            cloneJob->deleteLater();
        });

    job->start();

    statusBar()->showMessage(i18n("Cloning VM '%1'…", srcName));
    updateActionStates();
    updateDetails();
}

void MainWindow::createFolder()
{
    bool ok = false;
    QString name = QInputDialog::getText(this, i18n("New Folder"), i18n("Folder name:"), QLineEdit::Normal, { }, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    QModelIndex idx = vmModel->createFolder(name.trimmed());
    if (idx.isValid()) {
        vmTreeView->expand(idx);
        saveFolderStructure();
    }
}

void MainWindow::deleteFolder()
{
    QModelIndex idx = vmTreeView->currentIndex();
    if (vmModel->removeFolder(idx))
        saveFolderStructure();
    else
        KMessageBox::error(this, i18n("The folder is not empty. Move all VMs out first."), i18n("Cannot Delete Folder"));
}

void MainWindow::renameFolder()
{
    QModelIndex idx = vmTreeView->currentIndex();
    if (!idx.isValid()
        || idx.data(VmModel::ItemTypeRole).toInt() != VmModel::Folder)
        return;

    bool ok = false;
    QString newName = QInputDialog::getText(this, i18n("Rename Folder"), i18n("New name:"), QLineEdit::Normal, idx.data(Qt::DisplayRole).toString(), &ok);
    if (ok && !newName.trimmed().isEmpty()) {
        vmModel->renameFolder(idx, newName.trimmed());
        saveFolderStructure();
    }
}

void MainWindow::selectVmByName(const QString& vmName)
{
    const QModelIndex idx = vmModel->indexForVm(vmName);
    if (!idx.isValid())
        return;
    const QModelIndex parentIdx = idx.parent();
    if (parentIdx.isValid())
        vmTreeView->expand(parentIdx);
    vmTreeView->setCurrentIndex(idx);
}

QString MainWindow::selectedVmName() const
{
    const QModelIndex idx = vmTreeView->currentIndex();
    if (!idx.isValid())
        return { };
    if (idx.data(VmModel::ItemTypeRole).toInt() != VmModel::Vm)
        return { };
    return idx.data(VmModel::VmNameRole).toString();
}

bool MainWindow::isCurrentItemFolder() const
{
    const QModelIndex idx = vmTreeView->currentIndex();
    if (!idx.isValid())
        return false;
    return idx.data(VmModel::ItemTypeRole).toInt() == VmModel::Folder;
}

QMap<QString, QString> MainWindow::loadFolderAssignments() const
{
    QMap<QString, QString> map;
    KConfig config(u"kemurc"_s);
    KConfigGroup grp = config.group(u"Folders"_s);

    const QStringList keys = grp.keyList();
    for (const QString& key : keys) {
        if (key != u"order"_s)
            map.insert(key, grp.readEntry(key, { }));
    }
    return map;
}

void MainWindow::saveFolderStructure() const
{
    KConfig config(u"kemurc"_s);
    KConfigGroup grp = config.group(u"Folders"_s);

    for (const QString& key : grp.keyList())
        grp.deleteEntry(key);

    const QMap<QString, QString> assignments = vmModel->folderAssignments();
    for (auto it = assignments.cbegin(); it != assignments.cend(); ++it)
        grp.writeEntry(it.key(), it.value());

    QStringList collapsedFolders;
    for (const QString& name : vmModel->folderNames()) {
        QModelIndex idx = vmModel->indexForFolder(name);
        if (idx.isValid() && !vmTreeView->isExpanded(idx))
            collapsedFolders << name;
    }
    grp.writeEntry(u"collapsed"_s, collapsedFolders);
    grp.sync();
}

void MainWindow::startDeployment(const QString& vmName, const bool disable_headless)
{
    VmEntry* entry = vmModel->entryByName(vmName);
    if (!entry || entry->state != VmEntry::State::Deploying)
        return;

    // Load current config from disk
    const QString vmPath = getVmDir(vmName).absolutePath();
    VMConfig config = loadVmConfig(vmPath);
    entry->config = config;

    if (config.useTpm()) {
        if (!startSwtpm(vmName)) {
            abortDeployment(vmName, false);
            return;
        }
    }

    if (!config.sharedPath().isEmpty()
        && config.osType() == VMSpecs::OsType::Windows) {
        if (!startVirtiofsd(vmName)) {
            abortDeployment(vmName, false);
            return;
        }
    }

    QString tapDevice;
    if (config.networkCard() != VMSpecs::NetworkCard::None) {
        if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            tapDevice = ensureNetworkRunning(config.virtualNetworkName(), vmName);
            if (tapDevice.isEmpty()) {
                abortDeployment(vmName, false);
                return;
            }
        } else {
            if (!startPasst(vmName, config.portForwards())) {
                abortDeployment(vmName, false);
                return;
            }
        }
    }

    QFile::remove(monitorSocketPath(vmName));

    const bool headless = disable_headless ? false : SettingsDialog::deployHeadless();
    const QString sockPath = vmPath + u"/qemu-monitor.sock"_s;
    NetworkContext netCtx;
    if (!tapDevice.isEmpty())
        netCtx.tapDevice = tapDevice;

    QStringList args = QemuCommandBuilder::build(config, getVmDir(vmName), sockPath, netCtx, false, headless);

    const bool serialLog = SettingsDialog::deploySerialLog();
    if (serialLog) {
        const QString logFile = vmPath + u"/deploy-serial.log"_s;
        args << u"-serial"_s << u"file:%1"_s.arg(logFile);
    }

    if (!requireTool(ExternalTools::QemuSystem)) {
        abortDeployment(vmName, false);
        return;
    }

    QPointer<KProcess> process = new KProcess(this);

    if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork
        && !tapDevice.isEmpty()) {
        const qint64 nsPid = m_networkManager->namespacePid(
            currentConfig.virtualNetworkName());
        NamespaceHelper::prepareProcessForNamespace(process, nsPid);
    }
    process->setProgram(ExternalTools::instance().path(ExternalTools::QemuSystem));
    process->setArguments(args);

    connect(process, QOverload<int, KProcess::ExitStatus>::of(&KProcess::finished), this, &MainWindow::onProcessFinished);

    process->start();

    if (!process->waitForStarted()) {
        KMessageBox::error(this, i18n("Failed to start QEMU for deployment: %1", process->errorString()));
        delete process;
        abortDeployment(vmName, false);
        return;
    }

    if (!headless && config.displayType() == VMSpecs::DisplayType::Spice) {
        startSpiceViewer(vmName);
    }

    vmModel->setHeadless(vmName, headless);
    vmModel->setQemuProcess(vmName, process);
    // State stays Deploying (not Running)

    statusBar()->showMessage(i18n("Deploying '%1' — unattended installation running…", vmName));
    updateDetails();
    updateActionStates();

    // QMP connection for monitoring shutdown
    QPointer<QMPClient> client = new QMPClient(this);
    vmModel->setQmpClient(vmName, client);

    connect(client, &QMPClient::eventReceived, this,
        [this, vmName](const QString& eventName, const QJsonObject& data) {
            this->onQMPEvent(vmName, eventName, data);
        });

    connect(client, &QMPClient::connected, this, [client]() {
        client->startPerformanceTimer(1000);
    });

    if (Utils::waitForSocket(sockPath, 2000))
        client->connectToVm(sockPath);
    else
        qWarning() << "Deployment QMP: socket not ready for" << vmName;
}

void MainWindow::finalizeDeployment(const QString& vmName)
{
    m_pendingDeployments.remove(vmName);

    const QString vmPath = getVmDir(vmName).absolutePath();
    QDir vmDir(vmPath);

    CloudImageDeployer::removeNoCloudIso(vmDir);

    VMConfig config = loadVmConfig(vmPath);
    config.setBootFromIso(false);
    config.clearISOPath();
    config.setDeployState(VMConfig::DeployState::Complete);
    saveVmConfig(vmPath, config);

    vmModel->updateConfig(vmName, config);

    if (selectedVmName() == vmName) {
        currentConfig = config;
        updateDetails();
    }

    statusBar()->showMessage(
        i18n("Deployment of '%1' complete. VM is ready.", vmName));
}

void MainWindow::abortDeployment(const QString& vmName, bool deleteVm)
{
    m_pendingDeployments.remove(vmName);

    VmEntry* entry = vmModel->entryByName(vmName);
    if (!entry)
        return;

    if (entry->qemuProcess) {
        entry->qemuProcess->disconnect(this);
        if (entry->qemuProcess->state() == KProcess::Running) {
            entry->qemuProcess->kill();
            entry->qemuProcess->waitForFinished(3000);
        }
        entry->qemuProcess->deleteLater();
    }

    // Clean up helper processes
    if (entry->qmpClient) {
        entry->qmpClient->disconnectFromVm();
        entry->qmpClient->deleteLater();
    }

    if (entry->swtpmProcess) {
        entry->swtpmProcess->terminate();
        if (!entry->swtpmProcess->waitForFinished(3000))
            entry->swtpmProcess->kill();
        delete entry->swtpmProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/swtpm.sock"_s);
    }

    if (entry->virtiofsdProcess) {
        entry->virtiofsdProcess->terminate();
        if (!entry->virtiofsdProcess->waitForFinished(3000))
            entry->virtiofsdProcess->kill();
        delete entry->virtiofsdProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/virtiofsd.sock.pid"_s);
    }

    if (entry->passtProcess) {
        entry->passtProcess->terminate();
        if (!entry->passtProcess->waitForFinished(3000))
            entry->passtProcess->kill();
        delete entry->passtProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u"/passt.sock"_s);
    }

    if (entry->spiceViewerProcess) {
        entry->spiceViewerProcess->terminate();
        if (!entry->spiceViewerProcess->waitForFinished(2000))
            entry->spiceViewerProcess->kill();
        delete entry->spiceViewerProcess;
    }

    QFile::remove(monitorSocketPath(vmName));

    releaseNetwork(entry->config);

    if (deleteVm) {
        getVmDir(vmName).removeRecursively();
        loadVmList();
        onVmSelectionChanged();
        statusBar()->showMessage(
            i18n("Deployment of '%1' cancelled. VM deleted.", vmName));
    } else {
        vmModel->clearRuntimeState(vmName);
        updateDetails();
        updateActionStates();
        statusBar()->showMessage(
            i18n("Deployment of '%1' cancelled.", vmName));
    }
}

void MainWindow::deployCloudImage(const QString& vmName)
{
    if (!m_pendingDeployments.contains(vmName))
        return;

    const auto pd = m_pendingDeployments[vmName];
    QDir vmDir = getVmDir(vmName);
    VMConfig config = loadVmConfig(vmDir.absolutePath());

    statusBar()->showMessage(i18n("Deploying '%1'. Preparing cloud image…", vmName));

    auto result = CloudImageDeployer::deploy(pd.imageLocalPath, vmDir, config.diskSizeGb(), pd.cloudConfig);

    if (!result.success) {
        m_pendingDeployments.remove(vmName);

        if (selectedVmName() == vmName) {
            vmTreeView->clearSelection();
            currentConfig = VMConfig();
        }

        vmDir.removeRecursively();
        loadVmList();
        updateDetails();
        updateActionStates();

        statusBar()->showMessage(i18n("Deployment of '%1' failed.", vmName));

        KMessageBox::error(this, i18n("Cloud image deployment failed:\n%1", result.errorMessage));
        return;
    }

    config.setDiskPath(result.diskPath);
    config.setBootFromIso(false);
    saveVmConfig(vmDir.absolutePath(), config);

    startDeployment(vmName);
}
