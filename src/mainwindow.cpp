/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "mainwindow.h"

#include "const.h"
#include "dbusdisplay/dbusdisplayconst.h"
#include "dialogs/editvmdialog.h"
#include "dialogs/missingtoolsdialog.h"
#include "dialogs/newvmdialog.h"
#include "dialogs/settingsdialog.h"
#include "dialogs/snapshotdialog.h"
#include "jobs/clonejob.h"
#include "jobs/downloadjob.h"
#include "jobs/exportjob.h"
#include "jobs/importjob.h"
#include "jobs/ovaexportjob.h"
#include "jobs/ovaimportjob.h"
#include "jobs/shrinkjob.h"
#include "kemu_debug.h"
#include "utils/asynchelperprocess.h"
#include "utils/qemucapabilities.h"
#include "utils/qemucommandbuilder.h"
#include "utils/sandboxcapabilities.h"
#include "utils/startupchain.h"
#include "utils/usbenumerator.h"
#include "utils/utils.h"
#include "utils/vmconfig.h"

extern "C" {
#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <unistd.h>
}

MainWindow::MainWindow(QWidget *parent)
    : KXmlGuiWindow(parent)
{
    setupUi();
    setupActions();

    qApp->installEventFilter(this);

    jobTracker = new KUiServerV2JobTracker(this);

    setupGUI(Default, u"kemuui.rc"_s);

    const QDir dir = getVmBaseDir();
    if (!dir.exists())
        dir.mkpath(u"."_s);

    loadVmList();

    // A .part file at startup means a previous save was interrupted.
    for (const QString &name : veModel->vmNames())
        SaveStateManager::cleanupStalePart(getVmDir(name).absolutePath(), name);

    m_sessionGuard = new SessionGuard(this);
    connect(m_sessionGuard, &SessionGuard::aboutToSleep, this, &MainWindow::onAboutToSleep);
    connect(m_sessionGuard, &SessionGuard::wokeUp, this, &MainWindow::onWokeUp);
    // Forced shutdown: same damage control as for sleep, nothing slower fits.
    connect(m_sessionGuard, &SessionGuard::aboutToShutdown, this, &MainWindow::onAboutToSleep);

    connect(veModel, &VirtualEnvironmentModel::vmStateChanged, this, [this]() {
        updateSessionInhibitors();
    });
    updateSessionInhibitors();

    // preselect first VM
    if (veModel->rowCount() > 0) {
        QModelIndex firstIndex = veModel->index(0, 0);

        if (veModel->hasChildren(firstIndex) && veModel->data(firstIndex, VirtualEnvironmentModel::ItemTypeRole).toInt() == VirtualEnvironmentModel::Folder) {
            if (veTreeView->isExpanded(firstIndex)) {
                const QModelIndex firstChild = veModel->index(0, 0, firstIndex);
                if (firstChild.isValid())
                    firstIndex = firstChild;
            }
        }

        veTreeView->setCurrentIndex(firstIndex);
    }

    updateActionStates();

    // coming from setupUi()
    connect(veTreeView, &VirtualEnvironmentTreeView::expanded, this, [this]() {
        saveFolderStructure();
    });
    connect(veTreeView, &VirtualEnvironmentTreeView::collapsed, this, [this]() {
        saveFolderStructure();
    });

    const QString templatesPath = QStandardPaths::locate(QStandardPaths::AppDataLocation, u"data/templates.json"_s);
    if (!m_imageDatabase.loadFromFile(templatesPath)) {
        // Non-fatal: VMs can still be created manually, only the OS presets
        // and automated deployment are unavailable.
        KMessageBox::information(this,
                                 i18n("The operating system template database could not be loaded.\n"
                                      "Creating virtual machines still works, but OS presets and "
                                      "automated deployment are unavailable."),
                                 i18n("Template Database Unavailable"),
                                 u"TemplateDbMissingWarning"_s);
    }

    m_networkManager = new VirtualNetworkManager(this);
    m_networkManager->load();

    QTimer::singleShot(0, this, [this]() {
        auto missing = ExternalTools::instance().missingFeatureGroups();

        // drop suppressible groups the user has silenced
        missing.erase(std::remove_if(missing.begin(),
                                     missing.end(),
                                     [](const ExternalTools::FeatureGroup &g) {
                                         return g.suppressible && ExternalTools::isWarningSuppressed(g.id);
                                     }),
                      missing.end());

        if (missing.isEmpty())
            return;

        MissingToolsDialog dlg(missing, this);
        dlg.exec();
        if (dlg.openSettingsRequested())
            openSettingsPage(SettingsDialog::Executables);
    });
}

MainWindow::~MainWindow()
{
    for (auto job : std::as_const(creatingVMs)) {
        if (job) {
            job->disconnect();
            job->kill();
        }
    }
    creatingVMs.clear();

    for (auto job : std::as_const(activeImports)) {
        if (job) {
            job->disconnect();
            job->kill();
        }
    }
    activeImports.clear();

    for (const QString &name : veModel->vmNames()) {
        const VmEntry *e = veModel->entryByName(name);
        if (!e)
            continue;

        // Stop the QEMU process; Qt's parent-child cleanup deletes it
        if (e->qemuProcess) {
            e->qemuProcess->disconnect();
            if (e->qemuProcess->state() == KProcess::Running) {
                e->qemuProcess->terminate();
                if (!e->qemuProcess->waitForFinished(2000))
                    e->qemuProcess->kill();
            }
        }

        tearDownVmRuntime(name);
    }
}

bool MainWindow::queryClose()
{
    bool hasShrinking = false, hasRunning = false;
    for (const QString &name : veModel->vmNames()) {
        const VmEntry *e = veModel->entryByName(name);
        if (!e)
            continue;
        if (e->state == VmEntry::State::Shrinking)
            hasShrinking = true;
        if (e->state == VmEntry::State::Running || e->state == VmEntry::State::Deploying)
            hasRunning = true;
    }
    if (hasShrinking) {
        auto result = KMessageBox::warningTwoActions(this,
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

    if (hasRunning && !m_exitPolicyDone) {
        const ExitChoice choice = m_closingFromSession ? ExitChoice::ApplyPolicy : askExitChoice();

        if (choice == ExitChoice::Cancel)
            return false;

        if (choice == ExitChoice::ApplyPolicy) {
            statusBar()->showMessage(i18n("Shutting down virtual machines..."));
            applyExitPolicy();
            return false; // the watcher calls close() again once we are done
        }
        // QuitNow falls through: viewers are torn down and QEMU dies with us.
    }

    const auto viewerNames = m_dbusViewers.keys();
    for (const QString &name : viewerNames)
        stopDbusViewer(name);

    return true;
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == qApp) {
        const QEvent::Type t = event->type();
        if (t == QEvent::ApplicationPaletteChange || t == QEvent::ThemeChange || t == QEvent::StyleChange) {
            qCDebug(KEMU) << "Theme/palette change detected" << t;

            QTimer::singleShot(0, this, [this]() {
                if (detailsBrowser) {
                    const QString ss = detailsBrowser->styleSheet();
                    detailsBrowser->setStyleSheet(QString());
                    detailsBrowser->setStyleSheet(ss);
                }
                updateDetails();
            });
        }
    }
    return KXmlGuiWindow::eventFilter(watched, event);
}

QDir MainWindow::getVmBaseDir() const
{
    return QDir(SettingsDialog::getVmBasePath());
}

QDir MainWindow::getVmDir(const QString &vmName) const
{
    return getVmBaseDir().absoluteFilePath(vmName);
}

// Defence in depth: only a direct child of the VM store may ever be removed.
bool MainWindow::isManagedVmDir(const QDir &dir) const
{
    const QString base = QDir::cleanPath(getVmBaseDir().absolutePath());
    const QString path = QDir::cleanPath(dir.absolutePath());

    if (base.isEmpty() || path == base)
        return false;

    return QFileInfo(path).absolutePath() == base;
}

QString MainWindow::monitorSocketPath(const QString &vmName) const
{
    return getVmDir(vmName).absoluteFilePath(KEMUConst::QemuMonitorSocketFileName);
}

void MainWindow::setupUi()
{
    setWindowTitle(i18n("KEMU – Virtual Machine Manager"));
    resize(1280, 800);

    auto *centralWidget = new QWidget(this);
    auto *mainLayout = new QVBoxLayout(centralWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);

    auto *splitter = new QSplitter(Qt::Horizontal, centralWidget);

    // --- LEFT ---
    auto *leftContainer = new QWidget(splitter);
    auto *leftLayout = new QVBoxLayout(leftContainer);
    leftLayout->setContentsMargins(4, 4, 4, 4);

    // Header Layout: Label (left), Stretch (center), Button (right)
    auto *leftHeaderLayout = new QHBoxLayout();
    auto *titleLabel = new QLabel(i18n("<b>Virtual Machines</b>"));

    auto *addVmButton = new QToolButton();
    addVmButton->setIcon(QIcon::fromTheme(u"list-add"_s));
    addVmButton->setToolTip(i18n("Create a new Virtual Machine"));
    addVmButton->setAutoRaise(true);
    connect(addVmButton, &QToolButton::clicked, this, &MainWindow::createNewVM);

    leftHeaderLayout->addWidget(titleLabel);
    leftHeaderLayout->addStretch(); // Schiebt den Button nach rechts
    leftHeaderLayout->addWidget(addVmButton);

    // --- VM-Tree ---
    veModel = new VirtualEnvironmentModel(&m_imageDatabase, this);

    veTreeView = new VirtualEnvironmentTreeView();
    veTreeView->setModel(veModel);
    veTreeView->setHeaderHidden(true);
    veTreeView->setRootIsDecorated(true);
    veTreeView->setIconSize(QSize(32, 32));
    veTreeView->setItemDelegate(new VirtualEnvironmentItemDelegate(veTreeView));
    veTreeView->setAlternatingRowColors(true);
    veTreeView->setMinimumWidth(220);
    veTreeView->setContextMenuPolicy(Qt::CustomContextMenu);
    veTreeView->setSelectionMode(QAbstractItemView::SingleSelection);
    veTreeView->setAnimated(true);
    veTreeView->setIndentation(16);

    // Drag & Drop
    veTreeView->setDragEnabled(true);
    veTreeView->setAcceptDrops(true);
    veTreeView->setDropIndicatorShown(true);
    veTreeView->setDragDropMode(QAbstractItemView::DragDrop);
    veTreeView->setDefaultDropAction(Qt::MoveAction);

    connect(veTreeView->selectionModel(), &QItemSelectionModel::currentChanged, this, &MainWindow::onTreeSelectionChanged);
    connect(veTreeView, &VirtualEnvironmentTreeView::doubleClicked, this, &MainWindow::onTreeDoubleClicked);
    connect(veTreeView, &VirtualEnvironmentTreeView::customContextMenuRequested, this, &MainWindow::showContextMenu);
    connect(veTreeView, &VirtualEnvironmentTreeView::itemDropped, this, &MainWindow::onDropCompleted);

    connect(veTreeView,
            &VirtualEnvironmentTreeView::mediaIconClicked,
            this,
            [this](const QString &vmName, VirtualEnvironmentItemDelegate::MediaType type, const QPoint &pos) {
                selectVmByName(vmName);

                QMenu menu(this);
                if (type == VirtualEnvironmentItemDelegate::MediaType::Cd) {
                    menu.addAction(actionCollection()->action(u"vm_change_iso"_s));
                    menu.addAction(actionCollection()->action(u"vm_eject_iso"_s));
                } else {
                    menu.addAction(actionCollection()->action(u"vm_change_floppy"_s));
                    menu.addAction(actionCollection()->action(u"vm_eject_floppy"_s));
                }
                menu.exec(pos);
            });

    leftLayout->addLayout(leftHeaderLayout);
    leftLayout->addWidget(veTreeView);

    // --- RIGHT ---
    auto *rightContainer = new QWidget(splitter);
    auto *rightLayout = new QVBoxLayout(rightContainer);
    rightLayout->setContentsMargins(4, 4, 4, 4);

    // Local toolbar
    vmActionBar = new QToolBar();
    vmActionBar->setIconSize(QSize(22, 22));
    vmActionBar->setMovable(false);

    detailsBrowser = new QTextBrowser();
    detailsBrowser->setFrameShape(QFrame::NoFrame);
    detailsBrowser->setStyleSheet(u"QTextBrowser { background: palette(window); padding: 12px; }"_s);
    detailsBrowser->setOpenLinks(false);
    connect(detailsBrowser, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        Q_UNUSED(this);
        if (url.scheme() == "copy-pid"_L1) {
            const QString pidString = url.path();
            QGuiApplication::clipboard()->setText(pidString);
            QToolTip::showText(QCursor::pos(), i18n("PID %1 copied to clipboard", pidString), detailsBrowser, QRect(), 2000);
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

    splitter->setSizes({250, 500});
    statusBar()->showMessage(i18n("Ready."));
}

void MainWindow::startSwtpmAsync(const QString &vmName, std::function<void(bool)> done)
{
    if (!requireTool(ExternalTools::Swtpm)) {
        done(false);
        return;
    }

    const QString vmPath = getVmDir(vmName).absolutePath();
    const QString sockPath = vmPath + u'/' + KEMUConst::SwtpmSocketFileName;
    QFile::remove(sockPath);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Swtpm);
    opts.arguments =
        {u"socket"_s, u"--ctrl"_s, u"type=unixio,path=%1"_s.arg(sockPath), u"--terminate"_s, u"--tpmstate"_s, u"dir=%1"_s.arg(vmPath), u"--tpm2"_s};
    opts.socketPath = sockPath;

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            KMessageBox::error(this, i18n("Failed to start swtpm: %1", err));
            QFile::remove(sockPath);
            done(false);
            return;
        }
        veModel->setSwtpmProcess(vmName, process);
        done(true);
    });
}

void MainWindow::startVirtiofsdAsync(const QString &vmName, std::function<void(bool)> done)
{
    if (!requireTool(ExternalTools::Virtiofsd)) {
        done(false);
        return;
    }

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry) {
        done(false);
        return;
    }

    const QString sockPath = getVmDir(vmName).absolutePath() + u'/' + KEMUConst::VirtiofsdSocketFileName;
    const QString sharedPath = entry->config.sharedPath();
    QFile::remove(sockPath);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Virtiofsd);
    opts.arguments = {u"--socket-path=%1"_s.arg(sockPath), u"--shared-dir=%1"_s.arg(sharedPath), u"--cache=auto"_s, u"--sandbox=none"_s};
    opts.socketPath = sockPath;

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            KMessageBox::error(this, i18n("Failed to start virtiofsd: %1", err));
            QFile::remove(sockPath);
            QFile::remove(getVmDir(vmName).absolutePath() + u'/' + KEMUConst::VirtiofsdPidFileName);
            done(false);
            return;
        }
        veModel->setVirtiofsdProcess(vmName, process);
        done(true);
    });
}

void MainWindow::startPasstAsync(const QString &vmName, const QList<PortForwardRule> &portForwards, std::function<void(bool)> done)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (entry && !usePasstFor(entry->config)) {
        // Not reachable via the normal start paths; the VM is on SLIRP and
        // its command line has no passt netdev, so continuing is safe.
        qCWarning(KEMU) << "startPasstAsync() called for" << vmName << "which does not use passt - skipping";
        done(true);
        return;
    }

    if (!requireTool(ExternalTools::Passt)) {
        done(false);
        return;
    }

    const QString sockPath = getVmDir(vmName).absolutePath() + u'/' + KEMUConst::PasstSocketFileName;
    QFile::remove(sockPath);

    QStringList passtArgs = {u"--socket"_s, sockPath, u"-f"_s, u"-q"_s};

    QStringList tcpPorts, udpPorts;
    for (const auto &rule : portForwards) {
        const QString mapping = (rule.hostPort != rule.guestPort) ? u"%1:%2"_s.arg(rule.hostPort).arg(rule.guestPort) : QString::number(rule.hostPort);
        if (rule.protocol == PortForwardRule::Protocol::Tcp)
            tcpPorts << mapping;
        else
            udpPorts << mapping;
    }
    if (!tcpPorts.isEmpty())
        passtArgs << u"-t"_s << tcpPorts.join(u","_s);
    if (!udpPorts.isEmpty())
        passtArgs << u"-u"_s << udpPorts.join(u","_s);

    AsyncHelperProcess::Options opts;
    opts.program = ExternalTools::instance().path(ExternalTools::Passt);
    opts.arguments = passtArgs;
    opts.socketPath = sockPath;

    AsyncHelperProcess::run(this, opts, [this, vmName, sockPath, done = std::move(done)](KProcess *process, bool ok, const QString &err) {
        if (!ok) {
            KMessageBox::error(this, i18n("Failed to start passt: %1", err));
            QFile::remove(sockPath);
            done(false);
            return;
        }
        veModel->setPasstProcess(vmName, process);
        done(true);
    });
}

bool MainWindow::usePasstFor(const VMConfig &config) const
{
    if (config.networkCard() == VMSpecs::NetworkCard::None)
        return false;
    if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        return false;
    if (!ExternalTools::instance().isPasstUsable())
        return false;
    return !SettingsDialog::useSlirpNetworking();
}

NetworkContext MainWindow::networkContextFor(const VMConfig &config, const QString &tapDevice, int tapFd) const
{
    NetworkContext ctx;
    if (!tapDevice.isEmpty()) {
        ctx.tapDevice = tapDevice;
        ctx.tapFd = tapFd;
        return ctx;
    }
    ctx.useSlirp = !usePasstFor(config);
    return ctx;
}

bool MainWindow::openTapFdFor(const VMConfig &config, const QString &vmName, const QString &tapDevice, int *tapFd)
{
    *tapFd = -1;

    if (config.networkMode() != VMSpecs::NetworkMode::VirtualNetwork || tapDevice.isEmpty() || !m_networkManager)
        return true;

    // Opening the tap inside the namespace and handing QEMU the descriptor
    // keeps QEMU in our namespace, and thus at our UID - the D-Bus display
    // authenticates over the socket's peer credentials.
    const int fd = m_networkManager->openTapFd(config.virtualNetworkName(), vmName);
    if (fd < 0) {
        KMessageBox::error(dialogParentFor(vmName), i18n("Could not open the network interface for '%1': %2", vmName, m_networkManager->lastError()));
        return false;
    }

    *tapFd = fd;
    return true;
}

KProcess *MainWindow::createQemuProcess(const QString &vmName, const QStringList &args, int tapFd)
{
    auto *process = new KProcess(this);
    process->setOutputChannelMode(KProcess::SeparateChannels);

    constexpr int kStderrTailLimit = 65536;
    connect(process, &QProcess::readyReadStandardError, this, [this, process, vmName]() {
        Utils::appendTail(m_qemuStderrTails[vmName], process->readAllStandardError(), kStderrTailLimit);
    });

    Utils::setDiesWithParent(process, tapFd);

    // Never combine this with QProcess::UnixProcessParameters::CloseFileDescriptors:
    // it would close the tap descriptor despite the cleared CLOEXEC flag.
    process->setChildProcessModifier([tapFd] {
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (tapFd >= 0)
            fcntl(tapFd, F_SETFD, 0);
    });

    process->setProgram(ExternalTools::instance().path(ExternalTools::QemuSystem));
    process->setArguments(args);

    return process;
}

void MainWindow::launchQemuProcess(const QString &vmName, bool headless, const QString &tapDevice)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    // Copy: the model entry is mutated below and torn down on the failure path.
    const VMConfig config = entry->config;

    const QString vmDirPath = getVmDir(vmName).absolutePath();
    const QString sockPath = vmDirPath + u'/' + KEMUConst::QemuMonitorSocketFileName;

    if (!requireTool(ExternalTools::QemuSystem))
        return;

    int tapFd = -1;
    if (!openTapFdFor(config, vmName, tapDevice, &tapFd)) {
        tearDownVmRuntime(vmName);
        return;
    }

    const NetworkContext netCtx = networkContextFor(config, tapDevice, tapFd);

    const QString savestatePath = SaveStateManager::hasSavestate(vmDirPath, vmName) ? SaveStateManager::savestateFilePath(vmDirPath, vmName) : QString{};
    const bool isResume = !savestatePath.isEmpty();

    const QStringList args = QemuCommandBuilder::build(config,
                                                       getVmDir(vmName),
                                                       sockPath,
                                                       netCtx,
                                                       /*relativePaths=*/false,
                                                       headless,
                                                       savestatePath);

    KProcess *process = createQemuProcess(vmName, args, tapFd);

    connect(process, QOverload<int, KProcess::ExitStatus>::of(&KProcess::finished), this, [this, vmName](int code, KProcess::ExitStatus status) {
        handleQemuExit(vmName, code, status);
    });

    // Error-during-start path (e.g. exec failure). We never reach the probe
    // in that case.
    connect(process, &QProcess::errorOccurred, this, [this, process, vmName](QProcess::ProcessError) {
        const VmEntry *e = veModel->entryByName(vmName);
        if (e && e->qemuProcess == process)
            return; // handleQemuExit will own this.
        const QString err = process->errorString();
        m_startupFailureShown.insert(vmName);
        showQemuFailureDialog(vmName, true, -1, KProcess::CrashExit, err.toUtf8());
        process->deleteLater();
        tearDownVmRuntime(vmName);
    });

    process->start();

    // QEMU holds its own copy of the descriptor now.
    if (tapFd >= 0)
        ::close(tapFd);

    // start() can fail synchronously; errorOccurred has already cleaned up.
    if (process->state() == QProcess::NotRunning)
        return;

    // Optimistic registration: if QEMU dies inside the probe window, the
    // probe will surface a startup-failure dialog and dedup against
    // handleQemuExit via m_startupFailureShown.
    veModel->setHeadless(vmName, headless);
    veModel->setQemuProcess(vmName, process);
    veModel->setVmState(vmName, VmEntry::State::Running);
    statusBar()->showMessage(i18n("VM '%1' started.", vmName));
    updateDetails();
    updateActionStates();

    scheduleQemuStartupProbe(process, vmName);

    // --- QMP ---
    QMPClient *client = new QMPClient(this);
    veModel->setQmpClient(vmName, client);

    connect(client, &QMPClient::eventReceived, this, [this, vmName](const QString &eventName, const QJsonObject &data) {
        this->onQMPEvent(vmName, eventName, data);
    });

    connect(client, &QMPClient::connected, this, [this, client, vmName, isResume]() {
        client->startPerformanceTimer(1000);

        // Enable balloon guest statistics for actual RAM usage
        QJsonObject balloonArgs;
        balloonArgs[u"path"_s] = u"/machine/peripheral-anon/balloon0"_s;
        balloonArgs[u"property"_s] = u"guest-stats-polling-interval"_s;
        balloonArgs[u"value"_s] = 2;
        client->sendCommand(u"qom-set"_s, balloonArgs);

        // QEMU always starts with the link up; restore a persisted "cable out".
        {
            const VmEntry *netEntry = veModel->entryByName(vmName);
            if (netEntry && netEntry->config.networkCard() != VMSpecs::NetworkCard::None && !loadViewerPrefs(vmName).networkLinkUp)
                client->setNetworkLink(KEMUConst::NetdevId, false);
        }

        if (selectedVmName() == vmName && perfMonitor) {
            const VmEntry *e = veModel->entryByName(vmName);
            if (e) {
                perfMonitor->setHistory(PerformanceMonitorWidget::Cpu, e->cpuHistory);
                perfMonitor->setHistory(PerformanceMonitorWidget::Ram, e->ramHistory);
                perfMonitor->setHistory(PerformanceMonitorWidget::Io, e->ioHistory);
            }
            perfMonitor->setActive(true);
        }

        // Successful resume: the savestate file has done its job, drop it so
        // a subsequent normal start does not try to resume again.
        const QString dir = getVmDir(vmName).absolutePath();
        if (SaveStateManager::hasSavestate(dir, vmName)) {
            SaveStateManager::discardSavestate(dir, vmName);
            veModel->setHasSavestate(vmName, false);
        }

        if (isResume) {
            // Do NOT 'cont' here: QMP is up long before the incoming migration
            // has finished loading the (paused) state. Enable MIGRATION events
            // and let onQMPEvent() resume once status == "completed".
            m_resumingFromSavestate.insert(vmName);

            QJsonArray caps;
            QJsonObject capEvents;
            capEvents.insert(u"capability"_s, u"events"_s);
            capEvents.insert(u"state"_s, true);
            caps.append(capEvents);
            client->sendCommand(u"migrate-set-capabilities"_s, {{u"capabilities"_s, caps}}, nullptr);
        }
    });

    connect(client, &QMPClient::cpuUsageUpdated, this, [this, vmName](const VmCpuUsage &usage) {
        if (!usage.isValid)
            return;
        veModel->appendPerfSample(vmName, 0, usage.averagePercent);
        if (selectedVmName() == vmName && perfMonitor)
            perfMonitor->addSample(PerformanceMonitorWidget::Cpu, usage.averagePercent);
    });

    connect(client, &QMPClient::memoryUsageUpdated, this, [this, vmName](const VmMemoryUsage &usage) {
        if (!usage.isValid)
            return;
        veModel->appendPerfSample(vmName, 1, usage.usagePercent);
        if (selectedVmName() == vmName && perfMonitor)
            perfMonitor->addSample(PerformanceMonitorWidget::Ram, usage.usagePercent);
    });

    connect(client, &QMPClient::blockStatsUpdated, this, [this, vmName](const VmBlockStats &stats) {
        if (!stats.isValid)
            return;
        VmEntry *e = veModel->entryByName(vmName);
        if (!e)
            return;

        const double totalBps = stats.totalRdBytesPerSec + stats.totalWrBytesPerSec;
        if (totalBps > e->ioPeakBytesPerSec)
            e->ioPeakBytesPerSec = totalBps;
        const double pct = (e->ioPeakBytesPerSec > 0.0) ? (totalBps / e->ioPeakBytesPerSec * 100.0) : 0.0;

        veModel->appendPerfSample(vmName, 2, pct);
        if (selectedVmName() == vmName && perfMonitor)
            perfMonitor->addSample(PerformanceMonitorWidget::Io, pct);
    });

    QPointer<QMPClient> safeClient(client);
    AsyncHelperProcess::waitForSocket(this, sockPath, 2000, 50, [safeClient, vmName, sockPath](bool ok) {
        if (!ok) {
            qCWarning(KEMU) << "QMP: socket not ready for" << vmName;
            return;
        }
        if (safeClient)
            safeClient->connectToVm(sockPath);
    });

    // Last: startDbusViewer() needs the QMP client to exist. Its retry loop
    // covers the asynchronous socket wait above.
    if (!headless && config.displayType() == VMSpecs::DisplayType::Dbus) {
        QTimer::singleShot(500, this, [this, vmName]() {
            startDbusViewer(vmName);
        });
    }
}

void MainWindow::launchQemuForDeployment(const QString &vmName, bool headless, const QString &tapDevice)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry || entry->state != VmEntry::State::Deploying)
        return;

    // Copy: the model entry is mutated below and torn down on the failure path.
    const VMConfig config = entry->config;

    const QString vmPath = getVmDir(vmName).absolutePath();
    const QString sockPath = vmPath + u'/' + KEMUConst::QemuMonitorSocketFileName;

    QFile::remove(sockPath);

    int tapFd = -1;
    if (!openTapFdFor(config, vmName, tapDevice, &tapFd)) {
        abortDeployment(vmName, false);
        return;
    }

    const NetworkContext netCtx = networkContextFor(config, tapDevice, tapFd);

    QStringList args = QemuCommandBuilder::build(config,
                                                 getVmDir(vmName),
                                                 sockPath,
                                                 netCtx,
                                                 /*relativePaths=*/false,
                                                 headless);

    // Unattended runs only: a serial console makes some installers move their
    // UI off the display, which would defeat an attended installation.
    if (headless && SettingsDialog::deploySerialLog()) {
        const QString logFile = vmPath + u'/' + KEMUConst::DeploySerialLogFileName;
        args << u"-serial"_s << u"file:%1"_s.arg(logFile);
    }

    KProcess *process = createQemuProcess(vmName, args, tapFd);

    connect(process, QOverload<int, KProcess::ExitStatus>::of(&KProcess::finished), this, [this, vmName](int code, KProcess::ExitStatus status) {
        handleQemuExit(vmName, code, status);
    });

    // Failure-on-start path (exec failure etc.). Once QEMU has been registered
    // as the entry's qemuProcess, handleQemuExit() owns the failure handling.
    connect(process, &QProcess::errorOccurred, this, [this, process, vmName](QProcess::ProcessError) {
        const VmEntry *e = veModel->entryByName(vmName);
        if (e && e->qemuProcess == process)
            return;
        KMessageBox::error(dialogParentFor(vmName), i18n("Failed to start QEMU for deployment: %1", process->errorString()));
        process->deleteLater();
        abortDeployment(vmName, false);
    });

    process->start();

    // QEMU holds its own copy of the descriptor now.
    if (tapFd >= 0)
        ::close(tapFd);

    // start() can fail synchronously; errorOccurred has already cleaned up.
    if (process->state() == QProcess::NotRunning)
        return;

    // State stays Deploying (NOT Running). handleQemuExit() detects this on
    // QEMU exit and routes to finalizeDeployment() or abortDeployment().
    veModel->setHeadless(vmName, headless);
    veModel->setQemuProcess(vmName, process);

    statusBar()->showMessage(headless ? i18n("Deploying '%1' - unattended installation running...", vmName)
                                      : i18n("Installing '%1' - finish the installation in the VM window, then shut the guest down.", vmName));
    updateDetails();
    updateActionStates();

    // --- QMP (monitors the deployment's shutdown signal) ---
    QMPClient *client = new QMPClient(this);
    veModel->setQmpClient(vmName, client);

    connect(client, &QMPClient::eventReceived, this, [this, vmName](const QString &eventName, const QJsonObject &data) {
        onQMPEvent(vmName, eventName, data);
    });
    connect(client, &QMPClient::connected, this, [client]() {
        client->startPerformanceTimer(1000);
    });

    // QPointer protects against the (unlikely) case where the client is torn
    // down before the socket becomes available.
    QPointer<QMPClient> safeClient(client);
    AsyncHelperProcess::waitForSocket(this, sockPath, 2000, 50, [safeClient, vmName, sockPath](bool ok) {
        if (!ok) {
            qCWarning(KEMU) << "Deployment QMP: socket not ready for" << vmName;
            return;
        }
        if (safeClient)
            safeClient->connectToVm(sockPath);
    });

    // Attended installs need a window. Gtk/SDL open their own, the D-Bus
    // display is ours to connect. Last, so the QMP client exists.
    if (!headless && config.displayType() == VMSpecs::DisplayType::Dbus) {
        QTimer::singleShot(500, this, [this, vmName]() {
            startDbusViewer(vmName);
        });
    }
}

bool MainWindow::startDbusViewer(const QString &vmName)
{
    // Already running? Just bring the existing window to the front.
    if (m_dbusViewers.contains(vmName)) {
        auto &existing = m_dbusViewers[vmName];
        if (existing.window) {
            existing.window->raise();
            existing.window->activateWindow();
            return true;
        }
        // Stale entry without a window: drop it, otherwise the re-validation
        // below would discard the connection we are about to establish.
        stopDbusViewer(vmName);
    }

    // --- Rendezvous via QMP ---
    // The p2p display connection is handed to QEMU through QMP, so a live
    // QMP session is the precondition. Right after VM start it may not be
    // up yet; retry briefly instead of blocking.
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!hasLiveQemu(vmName)) {
        // VM died or was stopped while we were (re)trying: abort silently.
        m_viewerConnectAttempts.remove(vmName);
        return false;
    }

    QMPClient *qmp = entry->qmpClient.data();
    if (!qmp || !qmp->isConnected()) {
        const int attempt = m_viewerConnectAttempts.value(vmName, 0);
        if (attempt < kViewerConnectMaxAttempts) {
            m_viewerConnectAttempts[vmName] = attempt + 1;
            QTimer::singleShot(kViewerConnectRetryMs, this, [this, vmName] {
                startDbusViewer(vmName);
            });
            return true; // handshake initiated, completion is async
        }
        m_viewerConnectAttempts.remove(vmName);
        KMessageBox::error(this,
                           i18n("Display connection for '%1' not available (no QMP after %2 attempts).", vmName, kViewerConnectMaxAttempts),
                           i18n("Viewer Error"));
        return false;
    }
    m_viewerConnectAttempts.remove(vmName);

    // --- Socketpair: one end for QEMU, one end for us ---
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
        KMessageBox::error(this, i18n("socketpair() failed: %1", QString::fromLocal8Bit(strerror(errno))), i18n("Viewer Error"));
        return false;
    }

    // Hand sv[0] to QEMU. Pipelined on purpose: QMP executes commands in
    // order, so add_client may reference the fd name without waiting for
    // the getfd reply. Waiting here would deadlock: our GDBus handshake
    // below cannot complete before add_client is processed.
    const QString fdname = u"dbus-display-%1"_s.arg(vmName);
    qmp->sendFileDescriptor(fdname, sv[0], [](QMPResult r) {
        if (!r.success)
            qCWarning(KEMU) << "getfd failed:" << r.errorMessage;
    });

    QJsonObject args;
    args[u"protocol"_s] = u"@dbus-display"_s;
    args[u"fdname"_s] = fdname;
    qmp->sendCommand(u"add_client"_s, args, [vmName](QMPResult r) {
        if (!r.success)
            qCWarning(KEMU) << "add_client failed for" << vmName << ":" << r.errorMessage;
    });

    // --- Our end: asynchronous GDBus handshake with timeout ---
    QemuControlConnection::createAsync(sv[1], this, kViewerHandshakeTimeoutMs, [this, vmName](QemuControlConnection *ctrl, const QString &err) {
        if (!ctrl) {
            KMessageBox::error(this,
                               i18n("Failed to establish the display connection for '%1': %2\n\n"
                                    "This requires QEMU 7.0 or newer with D-Bus display support.",
                                    vmName,
                                    err),
                               i18n("Viewer Error"));
            return;
        }

        // The handshake ran through the event loop; re-validate the world.
        if (!hasLiveQemu(vmName) || m_dbusViewers.contains(vmName)) {
            ctrl->deleteLater(); // never delete synchronously: we are inside the bridge's dispatch
            return;
        }

        finishDbusViewer(vmName, ctrl);
    });

    return true; // handshake initiated, completion is async
}

void MainWindow::finishDbusViewer(const QString &vmName, QemuControlConnection *ctrl)
{
    const QString consolePath = KemuDbusDisplayConst::ConsolePathPrefix + u"0"_s;

    // --- Display listener ---
    auto *displayListener = new QemuDisplayListener(this);
    const int listenerFd = displayListener->createSocketPair();
    if (listenerFd < 0) {
        delete displayListener;
        ctrl->deleteLater();
        return;
    }

    // Asynchronous on purpose: QEMU issues synchronous counter-queries
    // (proxy GetAll) on the fresh listener connection that only our running
    // event loop can answer. Never wait synchronously on this connection.
    ctrl->callWithFd(consolePath,
                     KemuDbusDisplayConst::ConsoleIface,
                     u"RegisterListener"_s,
                     listenerFd,
                     [this, vmName, ctrl, displayListener, consolePath, listenerFd](bool ok, const QString &err) {
                         ::close(listenerFd); // QEMU received its own duplicate via the fd list

                         // The reply went through the event loop; re-validate the world.
                         const bool vmAlive = hasLiveQemu(vmName) && !m_dbusViewers.contains(vmName);

                         if (!ok || !vmAlive || !displayListener->connectListener()) {
                             if (!ok) {
                                 qCWarning(KEMU) << "RegisterListener failed:" << err;
                                 KMessageBox::error(this, i18n("Display registration for '%1' failed: %2", vmName, err), i18n("Viewer Error"));
                             }
                             delete displayListener;
                             ctrl->deleteLater();
                             return;
                         }

                         DbusViewerState state;
                         state.ctrl = ctrl;
                         state.displayListener = displayListener;

                         const VmEntry *cfgEntry = veModel->entryByName(vmName);
                         const bool hasUsbController = cfgEntry && cfgEntry->config.usbController() != VMSpecs::UsbController::None;
                         const ViewerDevicePrefs viewerPrefs = loadViewerPrefs(vmName);

                         // --- Audio listeners (created up front, registered async below) ---
                         state.audioOut = new QemuAudioListener(this);
                         state.audioIn = new QemuAudioInListener(this);

                         // --- Viewer window ---
                         state.window = new DbusViewerWindow(state.displayListener,
                                                             ctrl,
                                                             consolePath,
                                                             state.audioOut,
                                                             state.audioIn,
                                                             false,
                                                             false,
                                                             hasUsbController,
                                                             viewerPrefs,
                                                             nullptr);
                         state.window->setWindowTitle(vmName);
                         state.window->setAttribute(Qt::WA_DeleteOnClose, false);
                         state.window->displayWidget()->watchGLibFd(state.displayListener->socketFd(), state.displayListener->mainContext());
                         state.window->show();

                         // --- Teardown triggers ---
                         // Auto-cleanup when QEMU's display connection drops.
                         connect(state.displayListener, &QemuDisplayListener::connectionLost, this, [this, vmName]() {
                             stopDbusViewer(vmName);
                         });

                         // Second trigger: the control connection dropped. Deferred,
                         // because the signal is emitted from within the connection's
                         // own dispatch.
                         connect(ctrl, &QemuControlConnection::connectionLost, this, [this, vmName]() {
                             QTimer::singleShot(0, this, [this, vmName] {
                                 stopDbusViewer(vmName);
                             });
                         });

                         // When the user closes the viewer window, keep QEMU running,
                         // mark the VM as headless and tear down the per-viewer D-Bus
                         // listeners so the user can reconnect later.
                         //
                         // QueuedConnection: the slot must run AFTER closeEvent() has
                         // fully returned, otherwise stopDbusViewer() would delete the
                         // window from within its own event handler.
                         connect(
                             state.window,
                             &DbusViewerWindow::viewerClosed,
                             this,
                             [this, vmName]() {
                                 if (!m_dbusViewers.contains(vmName))
                                     return; // already torn down (shutdown path / connectionLost)

                                 if (hasLiveQemu(vmName))
                                     veModel->setHeadless(vmName, true);

                                 stopDbusViewer(vmName);

                                 if (selectedVmName() == vmName) {
                                     updateDetails();
                                     updateActionStates();
                                 }
                             },
                             Qt::QueuedConnection);

                         connect(state.window, &DbusViewerWindow::shutdownRequested, this, [this, vmName] {
                             shutdownVMByName(vmName);
                         });

                         connect(state.window, &DbusViewerWindow::resetRequested, this, [this, vmName] {
                             resetVMByName(vmName);
                         });

                         connect(state.window, &DbusViewerWindow::powerOffRequested, this, [this, vmName] {
                             powerOffVMByName(vmName);
                         });

                         connect(state.window, &DbusViewerWindow::snapshotManagerRequested, this, [this, vmName] {
                             manageSnapshotsForVm(vmName);
                         });

                         connect(state.window, &DbusViewerWindow::inhibitStateChanged, this, [this]() {
                             updateSessionInhibitors();
                         });

                         connect(state.window, &DbusViewerWindow::devicePrefsChanged, this, [this, vmName](const ViewerDevicePrefs &p) {
                             saveViewerPrefs(vmName, p);
                         });

                         connect(state.window, &DbusViewerWindow::networkLinkChangeRequested, this, [this, vmName](bool up) {
                             auto it = m_dbusViewers.find(vmName);
                             if (it == m_dbusViewers.end() || !it->window)
                                 return;
                             QPointer<DbusViewerWindow> viewer = it->window;

                             const VmEntry *entry = veModel->entryByName(vmName);
                             if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected()) {
                                 viewer->setNetworkLinkState(!up, false); // revert, keep the stored pref
                                 KMessageBox::error(viewer, i18n("Could not connect to QEMU monitor. Is the VM running?"));
                                 return;
                             }

                             entry->qmpClient->setNetworkLink(KEMUConst::NetdevId, up, [viewer, up](QMPResult res) {
                                 if (!viewer)
                                     return; // viewer closed while QEMU was answering
                                 if (!res.success) {
                                     viewer->setNetworkLinkState(!up, false);
                                     KMessageBox::error(viewer, i18n("Could not change the network link state: %1", res.errorMessage));
                                     return;
                                 }
                                 viewer->setNetworkLinkState(up);
                             });
                         });

                         connect(state.window, &DbusViewerWindow::changeIsoRequested, this, [this, vmName] {
                             changeISOForVm(vmName);
                         });
                         connect(state.window, &DbusViewerWindow::ejectIsoRequested, this, [this, vmName] {
                             ejectISOForVm(vmName);
                         });
                         connect(state.window, &DbusViewerWindow::changeFloppyRequested, this, [this, vmName] {
                             changeFloppyForVm(vmName);
                         });
                         connect(state.window, &DbusViewerWindow::ejectFloppyRequested, this, [this, vmName] {
                             ejectFloppyForVm(vmName);
                         });

                         // USB submenu population: reuse the existing enumerator path.
                         if (QMenu *usbMenu = state.window->usbMenu()) {
                             connect(usbMenu, &QMenu::aboutToShow, this, [this, vmName, usbMenu] {
                                 populateUsbMenuFor(vmName, usbMenu);
                             });
                         }

                         m_dbusViewers.insert(vmName, state);

                         pushMediaStateToViewer(vmName);
                         pushNetworkStateToViewer(vmName);

                         // Audio: asynchronous registration chain (see
                         // registerAudioListeners); runs against the map entry, so it
                         // must come after the insert above.
                         registerAudioListeners(vmName);
                     });
}

void MainWindow::stopDbusViewer(const QString &vmName)
{
    if (!m_dbusViewers.contains(vmName))
        return;

    auto state = m_dbusViewers.take(vmName);

    if (state.window) {
        state.window->displayWidget()->stopAllGLibWatching();
        disconnect(state.window, &DbusViewerWindow::viewerClosed, this, nullptr);
        state.window->close();
        state.window->deleteLater();
    }
    if (state.audioOut)
        state.audioOut->deleteLater();
    if (state.audioIn)
        state.audioIn->deleteLater();
    if (state.displayListener)
        state.displayListener->deleteLater();

    if (state.ctrl)
        state.ctrl->deleteLater();
}

void MainWindow::registerAudioListeners(const QString &vmName)
{
    auto it = m_dbusViewers.find(vmName);
    if (it == m_dbusViewers.end() || !it->ctrl)
        return;
    QemuControlConnection *ctrl = it->ctrl.data();

    const int outFd = it->audioOut->createSocketPair();
    if (outFd >= 0) {
        ctrl->callWithFd(KemuDbusDisplayConst::AudioPath,
                         KemuDbusDisplayConst::AudioIface,
                         u"RegisterOutListener"_s,
                         outFd,
                         [this, vmName, outFd](bool ok, const QString &err) {
                             ::close(outFd);
                             auto it = m_dbusViewers.find(vmName);
                             if (it == m_dbusViewers.end())
                                 return; // viewer torn down meanwhile
                             if (!ok) {
                                 qCDebug(KEMU) << "RegisterOutListener not available:" << err;
                                 return;
                             }
                             if (it->audioOut->connectListener() && it->window)
                                 it->window->audioOutReady();
                         });
    }

    const int inFd = it->audioIn->createSocketPair();
    if (inFd >= 0) {
        ctrl->callWithFd(KemuDbusDisplayConst::AudioPath,
                         KemuDbusDisplayConst::AudioIface,
                         u"RegisterInListener"_s,
                         inFd,
                         [this, vmName, inFd](bool ok, const QString &err) {
                             ::close(inFd);
                             auto it = m_dbusViewers.find(vmName);
                             if (it == m_dbusViewers.end())
                                 return;
                             if (!ok) {
                                 qCDebug(KEMU) << "RegisterInListener not available:" << err;
                                 return;
                             }
                             if (it->audioIn->connectListener() && it->window)
                                 it->window->audioInReady();
                         });
    }
}

void MainWindow::cleanupDbusSocket(const QString &vmName)
{
    const QDir vmDir = getVmDir(vmName);
    QFile::remove(vmDir.absoluteFilePath(KEMUConst::DBUSSocketFileName));
    QFile::remove(vmDir.absoluteFilePath(KEMUConst::DBUSConfigFileName));
}

bool MainWindow::requireTool(const ExternalTools::Tool tool)
{
    if (ExternalTools::instance().isAvailable(tool))
        return true;

    KMessageBox::error(this, i18n("%1 not found. Please set the path in Settings.", ExternalTools::displayName(tool)));
    return false;
}

void MainWindow::setupActions()
{
    KActionCollection *ac = actionCollection();

    auto *actionNew = ac->addAction(u"vm_new"_s);
    actionNew->setIcon(QIcon::fromTheme(u"list-add"_s));
    actionNew->setText(i18n("New Machine"));
    connect(actionNew, &QAction::triggered, this, &MainWindow::createNewVM);

    auto *actionStart = ac->addAction(u"vm_start"_s);
    actionStart->setIcon(QIcon::fromTheme(u"kt-start"_s));
    actionStart->setText(i18n("Start"));
    connect(actionStart, &QAction::triggered, this, &MainWindow::startVM);

    auto *actionStartHeadless = ac->addAction(u"vm_start_headless"_s);
    actionStartHeadless->setIcon(QIcon::fromTheme(u"system-run"_s));
    actionStartHeadless->setText(i18n("Start (Headless)"));
    connect(actionStartHeadless, &QAction::triggered, this, &MainWindow::startVMHeadless);

    auto *actionStop = ac->addAction(u"vm_shutdown"_s);
    actionStop->setIcon(QIcon::fromTheme(u"system-shutdown"_s));
    actionStop->setText(i18n("Shutdown"));
    connect(actionStop, &QAction::triggered, this, &MainWindow::shutdownVM);

    auto *actionChangeISO = ac->addAction(u"vm_change_iso"_s);
    actionChangeISO->setIcon(QIcon::fromTheme(u"media-optical"_s));
    actionChangeISO->setText(i18n("ISO Image..."));
    connect(actionChangeISO, &QAction::triggered, this, &MainWindow::changeISO);

    auto *actionEjectISO = ac->addAction(u"vm_eject_iso"_s);
    actionEjectISO->setIcon(QIcon::fromTheme(u"media-eject"_s));
    actionEjectISO->setText(i18n("Eject ISO Image"));
    connect(actionEjectISO, &QAction::triggered, this, &MainWindow::ejectISO);

    auto *actionChangeFloppy = ac->addAction(u"vm_change_floppy"_s);
    actionChangeFloppy->setIcon(QIcon::fromTheme(u"media-floppy"_s));
    actionChangeFloppy->setText(i18n("Floppy Image..."));
    connect(actionChangeFloppy, &QAction::triggered, this, &MainWindow::changeFloppy);

    auto *actionEjectFloppy = ac->addAction(u"vm_eject_floppy"_s);
    actionEjectFloppy->setIcon(QIcon::fromTheme(u"media-eject"_s));
    actionEjectFloppy->setText(i18n("Eject Floppy Image"));
    connect(actionEjectFloppy, &QAction::triggered, this, &MainWindow::ejectFloppy);

    auto *actionReset = ac->addAction(u"vm_reset"_s);
    actionReset->setIcon(QIcon::fromTheme(u"system-reboot"_s));
    actionReset->setText(i18n("Reset"));
    connect(actionReset, &QAction::triggered, this, &MainWindow::resetVM);

    auto *actionPowerOff = ac->addAction(u"vm_poweroff"_s);
    actionPowerOff->setIcon(QIcon::fromTheme(u"process-stop"_s));
    actionPowerOff->setText(i18n("Power-off"));
    connect(actionPowerOff, &QAction::triggered, this, &MainWindow::powerOffVM);

    auto *actionSaveState = ac->addAction(u"vm_save_state"_s);
    actionSaveState->setIcon(QIcon::fromTheme(u"document-save"_s));
    actionSaveState->setText(i18n("Save State"));
    actionSaveState->setToolTip(i18n("Save the complete running state to disk and stop the VM"));
    connect(actionSaveState, &QAction::triggered, this, &MainWindow::saveStateVM);

    auto *actionDiscardSavedState = ac->addAction(u"vm_discard_saved_state"_s);
    actionDiscardSavedState->setIcon(QIcon::fromTheme(u"edit-clear"_s));
    actionDiscardSavedState->setText(i18n("Discard Saved State"));
    actionDiscardSavedState->setToolTip(i18n("Delete the saved VM state so the VM boots normally next time"));
    connect(actionDiscardSavedState, &QAction::triggered, this, &MainWindow::discardSavedState);

    auto *actionEdit = ac->addAction(u"vm_edit"_s);
    actionEdit->setIcon(QIcon::fromTheme(u"document-edit"_s));
    actionEdit->setText(i18n("Edit"));
    connect(actionEdit, &QAction::triggered, this, &MainWindow::editVM);

    auto *actionDelete = ac->addAction(u"vm_delete"_s);
    actionDelete->setIcon(QIcon::fromTheme(u"edit-delete"_s));
    actionDelete->setText(i18n("Delete"));
    connect(actionDelete, &QAction::triggered, this, &MainWindow::deleteVM);

    auto *actionClone = ac->addAction(u"vm_clone"_s);
    actionClone->setIcon(QIcon::fromTheme(u"edit-copy"_s));
    actionClone->setText(i18n("Clone VM..."));
    actionClone->setToolTip(i18n("Create a full copy of this virtual machine"));
    connect(actionClone, &QAction::triggered, this, &MainWindow::cloneVM);

    auto *actionShrink = ac->addAction(u"vm_shrink"_s);
    actionShrink->setIcon(QIcon::fromTheme(u"edit-clear-history"_s));
    actionShrink->setText(i18n("Compact Disk"));
    actionShrink->setToolTip(
        i18n("Compact the disk image by removing unused blocks. Please free "
             "blocks on guest OS level first."));
    connect(actionShrink, &QAction::triggered, this, &MainWindow::shrinkDisk);

    auto *snapshotsAction = actionCollection()->addAction(u"vm_manage_snapshots"_s);
    snapshotsAction->setIcon(QIcon::fromTheme(u"camera-photo"_s));
    snapshotsAction->setText(i18n("Manage Snapshots..."));
    snapshotsAction->setToolTip(i18n("Open the snapshot manager for this VM"));
    connect(snapshotsAction, &QAction::triggered, this, &MainWindow::manageSnapshots);

    auto *actionExport = ac->addAction(u"vm_export"_s);
    actionExport->setIcon(QIcon::fromTheme(u"document-export"_s));
    actionExport->setText(i18n("Export VM..."));
    actionExport->setToolTip(i18n("Export the virtual machine as a portable archive"));
    connect(actionExport, &QAction::triggered, this, &MainWindow::exportVM);

    auto *actionImport = ac->addAction(u"vm_import"_s);
    actionImport->setIcon(QIcon::fromTheme(u"document-import"_s));
    actionImport->setText(i18n("Import VM..."));
    actionImport->setToolTip(i18n("Import a virtual machine from an archive"));
    connect(actionImport, &QAction::triggered, this, &MainWindow::importVM);

    auto *actionShowCmd = ac->addAction(u"vm_show_cmd"_s);
    actionShowCmd->setIcon(QIcon::fromTheme(u"utilities-terminal"_s));
    actionShowCmd->setText(i18n("Show command..."));
    connect(actionShowCmd, &QAction::triggered, this, &MainWindow::showCommand);

    auto *actionScreenshot = ac->addAction(u"vm_screenshot"_s);
    actionScreenshot->setIcon(QIcon::fromTheme(u"spectacle"_s));
    actionScreenshot->setText(i18n("Save Screenshot..."));
    actionScreenshot->setToolTip(i18n("Capture and save a screenshot of the running VM"));
    connect(actionScreenshot, &QAction::triggered, this, &MainWindow::saveScreenshot);

    auto *actionNetShell = ac->addAction(u"vm_network_shell"_s);
    actionNetShell->setIcon(QIcon::fromTheme(u"utilities-terminal"_s));
    actionNetShell->setText(i18n("Open Network Shell..."));
    actionNetShell->setToolTip(i18n("Open a terminal (konsole) inside the virtual network namespace"));
    connect(actionNetShell, &QAction::triggered, this, &MainWindow::openNetworkShell);
    actionNetShell->setVisible(SandboxCapabilities::instance().has(SandboxCapabilities::HostTerminal));

    auto *actionReconnect = ac->addAction(u"vm_reconnect"_s);
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
    vmActionBar->addAction(actionPowerOff);
    vmActionBar->addSeparator();
    vmActionBar->addAction(actionChangeISO);
    vmActionBar->addAction(actionChangeFloppy);
    vmActionBar->addSeparator();
    vmActionBar->addAction(snapshotsAction);
    auto *spacer = new QWidget();
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

    const VmEntry *entry = hasSelection ? veModel->entryByName(name) : nullptr;
    const bool isRunning = entry && entry->state == VmEntry::State::Running;
    const bool isDeploying = entry && entry->state == VmEntry::State::Deploying;
    const bool isBusy = entry && entry->state != VmEntry::State::Stopped;

    const bool hasSaved = hasSelection && entry && entry->hasSavestateFile;
    const bool isSaving = entry && entry->state == VmEntry::State::Saving;
    const bool isSavedOnly = entry && entry->state == VmEntry::State::Saved;
    const bool hasDisk = entry && !entry->config.diskPath().isEmpty();
    const bool canChangeMedia = hasSelection && entry && (entry->state == VmEntry::State::Stopped || entry->state == VmEntry::State::Running);

    // --- Configure action states ---
    // Always available: read-only while the VM is busy.
    QAction *editAction = actionCollection()->action(u"vm_edit"_s);
    editAction->setEnabled(hasSelection);
    editAction->setToolTip(isBusy ? i18n("Show the specification (read-only while the VM is busy)") : i18n("Edit the virtual machine's specification"));
    actionCollection()->action(u"vm_delete"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_clone"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_show_cmd"_s)->setEnabled(hasSelection);

    // A VM can be started if it is not busy OR if it is currently in 'Saved' state (it will resume).
    actionCollection()->action(u"vm_start"_s)->setEnabled(hasSelection && (!isBusy || isSavedOnly));
    actionCollection()->action(u"vm_start_headless"_s)->setEnabled(hasSelection && (!isBusy || isSavedOnly));

    actionCollection()->action(u"vm_shutdown"_s)->setEnabled(hasSelection && (isRunning || isDeploying));
    actionCollection()->action(u"vm_reset"_s)->setEnabled(hasSelection && isRunning);
    actionCollection()->action(u"vm_poweroff"_s)->setEnabled(hasSelection && (isRunning || isDeploying));
    actionCollection()->action(u"vm_save_state"_s)->setEnabled(isRunning && !isSaving);
    actionCollection()->action(u"vm_discard_saved_state"_s)->setEnabled(hasSaved && !isRunning && !isSaving);
    actionCollection()->action(u"vm_change_iso"_s)->setEnabled(canChangeMedia);
    actionCollection()->action(u"vm_eject_iso"_s)->setEnabled(canChangeMedia && !currentConfig.isoPath().isEmpty());
    actionCollection()->action(u"vm_change_floppy"_s)->setEnabled(canChangeMedia);
    actionCollection()->action(u"vm_eject_floppy"_s)->setEnabled(canChangeMedia && !currentConfig.floppyPath().isEmpty());
    actionCollection()->action(u"vm_shrink"_s)->setEnabled(hasSelection && !isBusy);

    // Snapshots must not be managed if the VM is in 'Saved' state.
    actionCollection()->action(u"vm_manage_snapshots"_s)->setEnabled(hasSelection && hasDisk && !isDeploying && !isSavedOnly);

    actionCollection()->action(u"vm_export"_s)->setEnabled(hasSelection && !isBusy);
    actionCollection()->action(u"vm_import"_s)->setEnabled(true);
    actionCollection()->action(u"vm_screenshot"_s)->setEnabled(hasSelection && isRunning && vmSupportsScreendump());

    // --- Additional UI states ---
    const bool hasUsbController = hasSelection && entry && entry->config.usbController() != VMSpecs::UsbController::None;
    usbActionMenu->setEnabled(hasUsbController && isRunning);

    const bool canReconnect = (isRunning || isDeploying) && entry->isHeadless && entry->config.displayType() == VMSpecs::DisplayType::Dbus
        && !m_dbusViewers.contains(entry->config.name());
    actionCollection()->action(u"vm_reconnect"_s)->setEnabled(hasSelection && canReconnect);

    const bool hasActiveNamespace = hasSelection && (isRunning || isDeploying) && entry->config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork
        && !entry->config.virtualNetworkName().isEmpty() && m_networkManager && m_networkManager->namespacePid(entry->config.virtualNetworkName()) > 0;

    actionCollection()->action(u"vm_network_shell"_s)->setEnabled(hasActiveNamespace && SandboxCapabilities::instance().has(SandboxCapabilities::HostTerminal));
}

QString MainWindow::encodeThemeIcon(const QString &name, int sizePx) const
{
    const QIcon icon = QIcon::fromTheme(name);
    if (icon.isNull() || !detailsBrowser)
        return {};

    const qreal dpr = detailsBrowser->devicePixelRatioF();
    const QPixmap pix = icon.pixmap(QSize(sizePx, sizePx) * dpr);
    if (pix.isNull())
        return {};

    QByteArray ba;
    QBuffer buf(&ba);
    buf.open(QIODevice::WriteOnly);
    pix.save(&buf, "PNG");
    return u"<img src='data:image/png;base64,%1' width='%2' height='%2' "
           "style='vertical-align: middle;'/>"_s.arg(QString::fromLatin1(ba.toBase64()))
               .arg(sizePx);
}

QStringList MainWindow::existingVmNames() const
{
    const QDir dir = getVmBaseDir();
    QStringList names;
    const QStringList dirs = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &d : dirs) {
        if (QFile::exists(dir.filePath(d) + u"/config.json"_s))
            names << d;
    }
    return names;
}

void MainWindow::updateDetails()
{
    const QPalette pal = detailsBrowser->palette();
    const KColorScheme scheme(QPalette::Active, KColorScheme::Window);

    const QString colNormal = pal.color(QPalette::Active, QPalette::WindowText).name();
    const QString colInactive = pal.color(QPalette::Disabled, QPalette::WindowText).name();
    const QString colLink = pal.color(QPalette::Active, QPalette::Link).name();
    const QString colVisited = pal.color(QPalette::Active, QPalette::LinkVisited).name();
    const QString colPositive = scheme.foreground(KColorScheme::PositiveText).color().name();
    const QString colNegative = scheme.foreground(KColorScheme::NegativeText).color().name();
    const QString colNeutral = scheme.foreground(KColorScheme::NeutralText).color().name();
    const QString colTrack = scheme.shade(KColorScheme::MidShade).name();

    QColor borderColor = pal.color(QPalette::Disabled, QPalette::WindowText);
    borderColor.setAlphaF(0.3);
    const QString colBorder = borderColor.name(QColor::HexArgb);

    // System font: respects the user's KDE font settings
    const QFont sysFont = QApplication::font();
    const QString fontFamily = sysFont.family();
    const int basePtSize = sysFont.pointSize() > 0 ? sysFont.pointSize() : qMax(8, QFontInfo(sysFont).pixelSize() * 72 / 96);

    if (currentConfig.name().isEmpty()) {
        detailsBrowser->setHtml(
            u"<div style='padding: 60px 40px; text-align: center; "
            "font-family: \"%1\"; font-size: %2pt; color: %3;'>"
            "<div style='font-size: 3em;'>&#x1F5A5;</div>"
            "<div style='margin-top: 0.8em;'>"_s.arg(fontFamily)
                .arg(basePtSize)
                .arg(colInactive)
            + i18n("Select a virtual machine from the list or create a new one.") + u"</div></div>"_s);
        return;
    }

    const VmEntry *entry = veModel->entryByName(currentConfig.name());
    const bool isRunning = entry && entry->state == VmEntry::State::Running;
    const bool isHeadless = entry && entry->isHeadless;
    const bool isShrinking = entry && entry->state == VmEntry::State::Shrinking;
    const bool isExporting = entry && entry->state == VmEntry::State::Exporting;
    const bool isDeploying = entry && entry->state == VmEntry::State::Deploying;
    const bool isCloning = entry && entry->state == VmEntry::State::Cloning;

    if (perfMonitor) {
        const bool vmLive = entry && entry->state == VmEntry::State::Running && entry->qmpClient && entry->qmpClient->isPerformanceTimerActive();
        perfMonitor->setActive(vmLive);
    }

    const QemuImgProcess::ImageInfo dInfo = QemuImgProcess::inspect(currentConfig.diskPath());
    QString diskVal;
    if (dInfo.isValid) {
        const QString diskInterfaceStr = VMConfig::toUIString(currentConfig.diskInterface());

        double usagePercent = 0.0;
        if (dInfo.virtualSizeBytes > 0.0)
            usagePercent = (static_cast<double>(dInfo.actualSizeBytes) / static_cast<double>(dInfo.virtualSizeBytes)) * 100.0;
        usagePercent = qBound(0.0, usagePercent, 100.0);
        const int filledPct = qRound(usagePercent);

        QString barColor;
        if (usagePercent < 80.0)
            barColor = colPositive;
        else if (usagePercent < 90.0)
            barColor = colNeutral;
        else
            barColor = colNegative;

        const QString bgColor = colTrack;

        const int barWidthPx = 140;
        const int barHeightPx = 6;
        const int filledPx = qBound(0, (barWidthPx * filledPct) / 100, barWidthPx);

        const QString svg =
            u"<svg xmlns='http://www.w3.org/2000/svg' width='%1' height='%2' "
            "shape-rendering='crispEdges'>"
            "<rect width='%1' height='%2' fill='%3'/>"
            "<rect width='%4' height='%2' fill='%5'/>"
            "</svg>"_s.arg(barWidthPx)
                .arg(barHeightPx)
                .arg(bgColor)
                .arg(filledPx)
                .arg(barColor);

        const QByteArray svgB64 = svg.toUtf8().toBase64();
        const QString barHtml =
            u"<img src='data:image/svg+xml;base64,%1' width='%2' height='%3' "
            "style='vertical-align:middle;'/>"_s.arg(QString::fromLatin1(svgB64))
                .arg(barWidthPx)
                .arg(barHeightPx);

        diskVal = u"<span style='white-space:nowrap;'>%1&nbsp;&nbsp;%2&nbsp;&nbsp;%3</span>"_s
                      .arg(i18n("%1 GB, %2", QString::number(dInfo.virtualSizeGb(), 'f', 1), diskInterfaceStr))
                      .arg(barHtml)
                      .arg(i18n("%1 GB used (%2%)", QString::number(dInfo.actualSizeGb(), 'f', 1), QString::number(usagePercent, 'f', 0)));
    } else {
        diskVal = i18n("Not available");
    }

    QString gpuVal = VMConfig::toUIString(currentConfig.gpuType());
    if (currentConfig.gpu3dAcceleration())
        gpuVal += i18n(" (3D)");
    QString audioVal = VMConfig::toUIString(currentConfig.audioDevice());
    if (currentConfig.audioDevice() == VMSpecs::AudioDevice::Sb16) {
        const auto &sb = currentConfig.sb16Config();
        audioVal += u" (IRQ %1, DMA %2/%3, I/O 0x%4)"_s.arg(sb.irq).arg(sb.dma).arg(sb.dma16).arg(sb.iobase, 0, 16);
    }
    QString netVal;
    if (currentConfig.networkCard() == VMSpecs::NetworkCard::None) {
        netVal = VMConfig::toUIString(VMSpecs::NetworkCard::None);
    } else {
        netVal = VMConfig::toUIString(currentConfig.networkCard());
        netVal += u" (%1)"_s.arg(VMConfig::toUIString(currentConfig.networkMode()));
        if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork && !currentConfig.virtualNetworkName().isEmpty()) {
            netVal += u" [%1]"_s.arg(currentConfig.virtualNetworkName());
        }
    }

    QString portFwdVal;
    const auto &portForwards = currentConfig.portForwards();
    if (!portForwards.isEmpty()) {
        netVal += u"  [%1 port(s) forwarded]"_s.arg(portForwards.size());
        QStringList portLines;
        for (const auto &rule : portForwards) {
            portLines << u"%1 %2 → %3"_s.arg(VMConfig::toString(rule.protocol).toUpper()).arg(rule.hostPort).arg(rule.guestPort);
        }
        portFwdVal = portLines.join(u", "_s);
    }

    const QString usbVal = VMConfig::toUIString(currentConfig.usbController());
    QString displayVal = VMConfig::toUIString(currentConfig.displayType());
    const QString resolutionVal = u"%1×%2"_s.arg(currentConfig.displayResolution().width()).arg(currentConfig.displayResolution().height());
    QString firmwareVal = VMConfig::firmwareToUIString(currentConfig.useUefi(), currentConfig.useSecureBoot());
    QString chipsetVal = VMConfig::toUIString(currentConfig.machineType());

    QString osVal = VMConfig::toUIString(currentConfig.osType());
    if (!currentConfig.distroId().isEmpty() && currentConfig.distroId() != u"other"_s) {
        osVal += u" / "_s + m_imageDatabase.displayName(currentConfig.distroId());
    }
    osVal += u" / "_s + VMConfig::toUIString(currentConfig.architecture());

    QString memVal = i18n("%1 MB", currentConfig.ramMb());
    QStringList memFlags;
    if (currentConfig.useMemoryBallooning())
        memFlags << i18n("Balloon");
    if (currentConfig.preallocMemory())
        memFlags << i18n("Prealloc");
    if (!memFlags.isEmpty())
        memVal += i18n("  [%1]", memFlags.join(u", "));

    QString cpuVal = i18n("%1, %2 core(s), %3",
                          VMConfig::toUIString(currentConfig.cpuModel()),
                          currentConfig.cpuCoreCount(),
                          currentConfig.useKvm() ? i18n("KVM") : i18n("TCG"));

    QString cdVal = currentConfig.isoPath().isEmpty() ? i18n("No disc inserted") : QFileInfo(currentConfig.isoPath()).fileName();
    QString floppyVal = currentConfig.floppyPath().isEmpty() ? i18n("No disk inserted") : QFileInfo(currentConfig.floppyPath()).fileName();

    QString sharedVal;
    if (currentConfig.sharedPath().isEmpty()) {
        sharedVal = i18n("Disabled");
    } else {
        const QString protocol = (currentConfig.osType() == VMSpecs::OsType::Windows) ? i18n("virtio-fs") : i18n("9p (VirtFS)");
        const QString tag = currentConfig.sharedName().isEmpty() ? u"share"_s : currentConfig.sharedName();
        sharedVal = i18n("%1  [%2, Tag: %3]", currentConfig.sharedPath(), protocol, tag);
    }

    QString statusDot, statusText, statusColor;
    if (isRunning) {
        auto pid = entry->qemuProcess ? entry->qemuProcess->processId() : 0;
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Powered On");
        if (isHeadless)
            statusText += i18n(" <b>Headless</b>");
        statusText += u"  <span class='pid'>[<a href='copy-pid:%1' title='%2'>PID %1</a>]</span>"_s.arg(QString::number(pid), i18n("Click to copy PID"));
        statusColor = colPositive;
    } else if (isShrinking) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Compacting Disk...");
        statusColor = colNeutral;
    } else if (isExporting) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Exporting...");
        statusColor = colNeutral;
    } else if (isDeploying) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Deploying...");
        statusColor = colNeutral;
    } else if (isCloning) {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Cloning...");
        statusColor = colNeutral;
    } else {
        statusDot = u"&#x25CF;"_s;
        statusText = i18n("Powered Off");
        statusColor = colInactive;
    }

    QColor badgeBg = QColor(statusColor);
    badgeBg.setAlphaF(0.15);
    const QString colBadgeBg = badgeBg.name(QColor::HexArgb);

    // VM header icon
    QString iconImgTag;
    {
        const int targetSize = KEMUConst::DetailViewIconSize;
        const qreal dpr = detailsBrowser->devicePixelRatioF();
        const int devPx = qRound(targetSize * dpr);

        auto pixmapToImgTag = [&](const QPixmap &src) -> QString {
            QImage srcImg = src.toImage();
            srcImg.setDevicePixelRatio(1.0);

            const QImage scaled = srcImg.scaled(devPx, devPx, Qt::KeepAspectRatio, Qt::SmoothTransformation);

            QImage canvas(devPx, devPx, QImage::Format_ARGB32_Premultiplied);
            canvas.setDevicePixelRatio(1.0);
            canvas.fill(Qt::transparent);
            QPainter p(&canvas);
            p.drawImage((devPx - scaled.width()) / 2, (devPx - scaled.height()) / 2, scaled);
            p.end();

            QByteArray ba;
            QBuffer buf(&ba);
            buf.open(QIODevice::WriteOnly);
            canvas.save(&buf, "PNG");
            return u"<img src='data:image/png;base64,%1' width='%2' height='%2'/>"_s.arg(QString::fromLatin1(ba.toBase64())).arg(targetSize);
        };

        QString svgPath;
        if (currentConfig.iconSource() == VMConfig::IconSource::IconSet)
            svgPath = currentConfig.iconSetPath();

        if (!svgPath.isEmpty() && svgPath.endsWith(u".svg", Qt::CaseInsensitive)) {
            QSvgRenderer renderer(svgPath);
            if (renderer.isValid()) {
                QPixmap pix(devPx, devPx);
                pix.fill(Qt::transparent);
                QPainter painter(&pix);
                painter.setRenderHint(QPainter::Antialiasing);
                renderer.render(&painter);
                painter.end();
                iconImgTag = pixmapToImgTag(pix);
            }
        }

        if (iconImgTag.isEmpty()) {
            const QIcon ic = currentConfig.icon(targetSize);
            const QPixmap pix = ic.pixmap(QSize(devPx, devPx));
            if (!pix.isNull()) {
                iconImgTag = pixmapToImgTag(pix);
            } else {
                QPixmap empty(devPx, devPx);
                empty.fill(Qt::transparent);
                iconImgTag = pixmapToImgTag(empty);
            }
        }
    }

    const int nameSize = basePtSize + 3;

    // Theme-aware CSS: system font, em-relative sizing, palette colors only
    const QString css =
        u"* { margin: 0; padding: 0; }"
        "body { font-family: '%1'; font-size: %2pt; color: %3; background: "
        "transparent; padding: 1.2em 1.5em; }"
        ".header { margin-bottom: 24px; }"
        ".vm-icon { width: 58px; height: 58px; vertical-align: top; "
        "padding-right: 16px; padding-bottom: 8px; }"
        ".header-info { vertical-align: top; padding-top: 2px; }"
        ".vm-name { font-size: %10pt; font-weight: 700; color: %3; "
        "margin-bottom: 0.15em; letter-spacing: -0.01em; }"
        ".vm-status { margin-bottom: 0.15em; }"
        ".vm-status .badge { color: %4; font-weight: 600; padding: 2px 8px; "
        "background-color: %9; border-radius: 3px; }"
        ".vm-status .dot { vertical-align: middle; }"
        ".vm-os { color: %5; }"
        ".pid { color: %5; margin-left: 8px; }"
        ".pid a { color: %6; text-decoration: none; }"
        ".pid a:hover { text-decoration: underline; color: %7; }"
        ".hw-table { width: 100%; border-collapse: collapse; margin-top: 4px; "
        "line-height: 1.0; }"
        ".hw-table td { padding: 2px 10px; vertical-align: middle; }"
        ".hw-icon { width: 22px; text-align: center; }"
        ".hw-label { color: %3; }"
        ".hw-value { color: %3; }"
        ".hw-value.muted { color: %5; font-style: italic; }"
        ".section-gap td { border-top: 1px solid %8; padding-top: 8px; }"_s.arg(fontFamily)
            .arg(basePtSize)
            .arg(colNormal, statusColor, colInactive, colLink, colVisited, colBorder, colBadgeBg)
            .arg(nameSize);

    QString html = u"<html><head><style>%1</style></head><body>"_s.arg(css);

    html +=
        u"<table class='header' cellpadding='0' cellspacing='0'><tr>"
        "<td class='vm-icon'>%1</td>"
        "<td class='header-info'>"
        "  <div class='vm-name'>%2</div>"
        "  <div class='vm-status'>"
        "    <span class='badge'><span class='dot'>%3</span> %4</span>"
        "  </div>"
        "  <div class='vm-os'>%5</div>"
        "</td></tr></table>"_s.arg(iconImgTag, currentConfig.name(), statusDot, statusText, osVal);

    const QString displayStr = (currentConfig.displayType() == VMSpecs::DisplayType::Dbus)
        ? QString(gpuVal + u" (%1)"_s.arg(resolutionVal))
        : QString(gpuVal + u", "_s + displayVal + u" (%1)"_s.arg(resolutionVal));

    struct HwRow {
        QString iconName; // freedesktop / Breeze theme icon name
        QString label;
        QString value;
        bool sectionStart = false;
        bool muted = false;
    };

    QList<HwRow> rows = {{u"preferences-system"_s, i18n("Firmware"), firmwareVal, false, false},
                         {u"computer"_s, i18n("Chipset"), chipsetVal, false, false},
                         {u"security-high"_s, i18n("TPM"), currentConfig.useTpm() ? i18n("TPM 2.0 (swtpm)") : i18n("None"), false, !currentConfig.useTpm()},
                         {u"clock"_s, i18n("Clock"), currentConfig.useLocalTime() ? i18n("Localtime") : i18n("UTC"), false, false},
                         {u"media-flash"_s, i18n("Memory"), memVal, true, false},
                         {u"cpu"_s, i18n("Processors"), cpuVal, false, false},
                         {u"drive-harddisk"_s, i18n("Hard Disk"), diskVal, true, false},
                         {u"media-optical"_s, i18n("CD/DVD"), cdVal, false, currentConfig.isoPath().isEmpty()},
                         {u"media-floppy"_s, i18n("Floppy"), floppyVal, false, currentConfig.floppyPath().isEmpty()},
                         {u"video-display"_s, i18n("Display"), displayStr, true, false},
                         {u"audio-card"_s, i18n("Sound Card"), audioVal, true, currentConfig.audioDevice() == VMSpecs::AudioDevice::None},
                         {u"network-wired"_s, i18n("Network Adapter"), netVal, true, currentConfig.networkCard() == VMSpecs::NetworkCard::None}};

    if (!portFwdVal.isEmpty())
        rows.append({u"network-connect"_s, i18n("Port Forwarding"), portFwdVal, false, false});

    if ((isRunning || isDeploying) && currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork && !currentConfig.virtualNetworkName().isEmpty()
        && m_networkManager) {
        const qint64 nsPid = m_networkManager->namespacePid(currentConfig.virtualNetworkName());
        if (nsPid > 0) {
            const int vmCount = m_networkManager->connectedVmCount(currentConfig.virtualNetworkName());
            QString nsVal = u"<a href='copy-pid:%1' title='%2'>PID %1</a>"_s.arg(QString::number(nsPid), i18n("Click to copy namespace PID"));
            nsVal += u"  [%1, %2]"_s.arg(currentConfig.virtualNetworkName()).arg(i18np("%1 VM connected", "%1 VMs connected", vmCount));
            rows.append({u"network-server"_s, i18n("Namespace"), nsVal, false, false});
        }
    }

    rows.append({u"drive-removable-media-usb"_s, i18n("USB Controller"), usbVal, true, false});
    rows.append({u"folder-network"_s, i18n("Shared Folders"), sharedVal, true, currentConfig.sharedPath().isEmpty()});

    const int hwIconPx = 14;

    html += u"<table class='hw-table' cellspacing='0' cellpadding='0'>"_s;
    for (const auto &row : rows) {
        const QString trClass = row.sectionStart ? u" class='section-gap'"_s : QString();
        const QString valClass = row.muted ? u"hw-value muted"_s : u"hw-value"_s;
        const QString iconImg = encodeThemeIcon(row.iconName, hwIconPx);

        html +=
            u"<tr%1>"
            "<td class='hw-icon'>%2</td>"
            "<td class='hw-label'>%3</td>"
            "<td class='%4'>%5</td>"
            "</tr>"_s.arg(trClass, iconImg, row.label, valClass, row.value);
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
        const VmEntry *entry = veModel->entryByName(name);
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
        const QDir dir = getVmBaseDir();
        if (!dir.exists())
            dir.mkpath(u"."_s);
        loadVmList();
        onVmSelectionChanged();
        if (perfMonitor)
            perfMonitor->setVisible(SettingsDialog::isPerformanceMonitorEnabled());
        updateSessionInhibitors();
    }
}

void MainWindow::saveVmConfig(const QString &vmPath, const VMConfig &config)
{
    const QJsonDocument doc(config.toJson());
    QFile file(vmPath + u"/"_s + KEMUConst::VMConfigFileName);
    if (file.open(QIODevice::WriteOnly)) {
        file.write(doc.toJson(QJsonDocument::Indented));
    } else {
        qCWarning(KEMU) << "Failed to write config.json:" << file.errorString();
    }

    if (!config.saveCustomIcon(vmPath)) {
        if (config.iconSource() == VMConfig::IconSource::Custom) {
            qCWarning(KEMU) << "Failed to save custom icon for" << config.name();
        }
    }

    generateShellScript(vmPath, config);
}

void MainWindow::generateShellScript(const QString &vmPath, const VMConfig &config)
{
    const QString sockPath = vmPath + u'/' + KEMUConst::QemuMonitorSocketFileName;

    const QString tap = config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork ? u"tap0"_s : QString{};
    const NetworkContext netCtx = networkContextFor(config, tap);

    const VirtualNetwork net = (m_networkManager && config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        ? m_networkManager->network(config.virtualNetworkName())
        : VirtualNetwork{};

    const QString script = QemuCommandBuilder::buildShellScript(config, QDir(vmPath), sockPath, netCtx, net, true);

    QFile scriptFile(vmPath + u"/qemu.sh"_s);
    if (scriptFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        scriptFile.write(script.toUtf8());
        scriptFile.setPermissions(scriptFile.permissions() | QFile::ExeOwner | QFile::ExeUser);
    } else {
        qCWarning(KEMU) << "Failed to write qemu.sh:" << scriptFile.errorString();
    }
}

VMConfig MainWindow::loadVmConfig(const QString &vmPath) const
{
    QFile file(vmPath + u"/"_s + KEMUConst::VMConfigFileName);
    if (!file.open(QIODevice::ReadOnly)) {
        qCWarning(KEMU) << "Failed to read config.json:" << file.errorString();
        return {};
    }

    VMConfig config = VMConfig::fromJson(QJsonDocument::fromJson(file.readAll()).object());
    config.normalizePaths(QDir(vmPath));

    if (config.iconSource() == VMConfig::IconSource::Custom) {
        if (!config.loadCustomIcon(vmPath)) {
            qCWarning(KEMU) << "Failed to load custom icon for" << config.name();
        }
    }

    return config;
}

void MainWindow::loadVmList()
{
    const QMap<QString, QString> assignments = loadFolderAssignments();

    veModel->reload(getVmBaseDir(), assignments, [this](const QString &vmPath) {
        return loadVmConfig(vmPath);
    });

    QStringList macs = veModel->usedMacAddresses();
    for (const QString &name : veModel->vmNames()) {
        VmEntry *e = veModel->entryByName(name);
        if (!e)
            continue;
        if (e->config.networkCard() == VMSpecs::NetworkCard::None || !e->config.macAddress().isEmpty())
            continue;

        VMConfig healed = e->config;
        healed.ensureMacAddress(macs);
        macs << healed.macAddress();

        const QString vmPath = getVmDir(name).absolutePath();
        saveVmConfig(vmPath, healed);
        veModel->updateConfig(name, healed);
    }

    KConfig kconfig(u"kemurc"_s);
    const QStringList collapsed = kconfig.group(u"Folders"_s).readEntry(u"collapsed"_s, QStringList());

    for (const QString &folder : veModel->folderNames()) {
        const QModelIndex idx = veModel->indexForFolder(folder);
        if (idx.isValid() && !collapsed.contains(folder))
            veTreeView->expand(idx);
    }

    statusBar()->showMessage(i18n("%1 virtual machine(s) found.", veModel->vmNames().size()));
}

bool MainWindow::vmSupportsScreendump() const
{
    return !currentConfig.gpu3dAcceleration();
}

QString MainWindow::ensureNetworkRunning(const QString &networkName, const QString &vmName)
{
    if (!SandboxCapabilities::instance().has(SandboxCapabilities::UserNamespaces)) {
        KMessageBox::error(this,
                           i18n("User namespaces are not supported on this system. Virtual "
                                "networking requires unprivileged user namespaces."));
        return {};
    }

    if (!ExternalTools::instance().isVirtualNetworkCapable()) {
        KMessageBox::error(this,
                           i18n("Required tools for virtual networking are "
                                "missing. Please check Settings → Executables."));
        return {};
    }

    const VirtualNetwork net = m_networkManager->network(networkName);
    if (!net.isValid()) {
        KMessageBox::error(this, i18n("Virtual network '%1' not found or invalid: %2", networkName, m_networkManager->lastError()));
        return {};
    }

    if (!m_networkManager->startNetwork(networkName)) {
        KMessageBox::error(this, i18n("Failed to start virtual network '%1': %2", networkName, m_networkManager->lastError()));
        return {};
    }

    const QString tapDevice = m_networkManager->attachVm(networkName, vmName);
    if (tapDevice.isEmpty()) {
        KMessageBox::error(this, i18n("Failed to create network interface for VM '%1': %2", vmName, m_networkManager->lastError()));
        return {};
    }

    return tapDevice;
}

void MainWindow::releaseNetwork(const VMConfig &config)
{
    if (!m_networkManager)
        return;
    if (config.networkMode() != VMSpecs::NetworkMode::VirtualNetwork)
        return;
    if (config.virtualNetworkName().isEmpty())
        return;
    m_networkManager->detachVm(config.virtualNetworkName(), config.name());
}

void MainWindow::tearDownVmRuntime(const QString &vmName)
{
    VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    // Detach signal sources first so no slot fires during teardown.
    if (entry->qmpClient) {
        entry->qmpClient->disconnect();
        entry->qmpClient->disconnectFromVm();
        entry->qmpClient->deleteLater();
    }

    // Detach the integrated D-Bus viewer (also drops its listeners).
    stopDbusViewer(vmName);

    auto stopHelper = [](QPointer<KProcess> &p, int waitMs) {
        if (!p)
            return;
        p->disconnect();
        p->terminate();
        if (!p->waitForFinished(waitMs))
            p->kill();
        delete p; // QPointer auto-nullifies
    };

    stopHelper(entry->swtpmProcess, 3000);
    stopHelper(entry->virtiofsdProcess, 3000);
    stopHelper(entry->passtProcess, 3000);

    const QDir vmDir = getVmDir(vmName);
    QFile::remove(vmDir.filePath(KEMUConst::SwtpmSocketFileName));
    QFile::remove(vmDir.filePath(KEMUConst::VirtiofsdPidFileName));
    QFile::remove(vmDir.filePath(KEMUConst::PasstSocketFileName));
    QFile::remove(monitorSocketPath(vmName));

    releaseNetwork(entry->config);
    veModel->clearRuntimeState(vmName);

    m_qemuStderrTails.remove(vmName);
    m_userInitiatedShutdowns.remove(vmName);
    m_resumingFromSavestate.remove(vmName);
}

void MainWindow::changeISO()
{
    if (!currentConfig.name().isEmpty())
        changeISOForVm(currentConfig.name());
}

void MainWindow::ejectISO()
{
    if (!currentConfig.name().isEmpty())
        ejectISOForVm(currentConfig.name());
}

void MainWindow::changeFloppy()
{
    if (!currentConfig.name().isEmpty())
        changeFloppyForVm(currentConfig.name());
}

void MainWindow::ejectFloppy()
{
    if (!currentConfig.name().isEmpty())
        ejectFloppyForVm(currentConfig.name());
}

void MainWindow::changeISOForVm(const QString &vmName)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    QWidget *parentW = dialogParentFor(vmName);

    const QDir startDir = m_lastMediaDir.value(vmName, QDir::homePath());
    const QString isoPath = QFileDialog::getOpenFileName(parentW, i18n("Select ISO Image"), startDir.absolutePath(), i18n("ISO Files (*.iso)"));
    if (isoPath.isEmpty())
        return;

    m_lastMediaDir[vmName] = QFileInfo(isoPath).absolutePath();

    if (veModel->isRunning(vmName)) {
        if (!entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(parentW, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        const QMPResult result =
            entry->qmpClient->sendCommandSync(u"blockdev-change-medium"_s, {{u"id"_s, u"cdrom0"_s}, {u"filename"_s, isoPath}, {u"format"_s, u"raw"_s}});

        if (!result.success) {
            KMessageBox::error(parentW, i18n("Changing ISO Failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("ISO changed successfully."));
    }

    // Base on currentConfig for the selected VM to preserve existing behaviour,
    // otherwise take the model's stored config.
    VMConfig cfg = (vmName == currentConfig.name()) ? currentConfig : entry->config;
    cfg.setISOPath(isoPath);
    veModel->updateConfig(vmName, cfg);
    saveVmConfig(getVmDir(vmName).absolutePath(), cfg);

    if (vmName == currentConfig.name()) {
        currentConfig = cfg;
        updateActionStates();
        updateDetails();
    }
    pushMediaStateToViewer(vmName);
}

void MainWindow::ejectISOForVm(const QString &vmName)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    QWidget *parentW = dialogParentFor(vmName);

    if (veModel->isRunning(vmName)) {
        if (!entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(parentW, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        QJsonObject ejectArgs;
        ejectArgs.insert(u"id"_s, u"cdrom0"_s);
        const QMPResult result = entry->qmpClient->sendCommandSync(u"eject"_s, ejectArgs);

        if (!result.success) {
            KMessageBox::error(parentW, i18n("Eject Failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("ISO ejected."));
    }

    VMConfig cfg = (vmName == currentConfig.name()) ? currentConfig : entry->config;
    cfg.clearISOPath();
    cfg.setBootFromIso(false);
    veModel->updateConfig(vmName, cfg);
    saveVmConfig(getVmDir(vmName).absolutePath(), cfg);

    if (vmName == currentConfig.name()) {
        currentConfig = cfg;
        updateActionStates();
        updateDetails();
    }
    pushMediaStateToViewer(vmName);
}

void MainWindow::changeFloppyForVm(const QString &vmName)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    QWidget *parentW = dialogParentFor(vmName);

    const QDir startDir = m_lastMediaDir.value(vmName, QDir::homePath());
    const QString imagePath = QFileDialog::getOpenFileName(parentW,
                                                           i18n("Select Floppy Image"),
                                                           startDir.absolutePath(),
                                                           i18n("Floppy Images (*.img *.ima *.flp *.vfd);;All Files (*)"));
    if (imagePath.isEmpty())
        return;

    m_lastMediaDir[vmName] = QFileInfo(imagePath).absolutePath();

    if (veModel->isRunning(vmName)) {
        if (!entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(parentW, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        const QMPResult result =
            entry->qmpClient->sendCommandSync(u"blockdev-change-medium"_s, {{u"id"_s, u"fda"_s}, {u"filename"_s, imagePath}, {u"format"_s, u"raw"_s}});

        if (!result.success) {
            KMessageBox::error(parentW, i18n("Changing floppy image failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("Floppy image changed successfully."));
    }

    VMConfig cfg = (vmName == currentConfig.name()) ? currentConfig : entry->config;
    cfg.setFloppyPath(imagePath);
    cfg.setBootFromFloppy(false);
    veModel->updateConfig(vmName, cfg);
    saveVmConfig(getVmDir(vmName).absolutePath(), cfg);

    if (vmName == currentConfig.name()) {
        currentConfig = cfg;
        updateActionStates();
        updateDetails();
    }
    pushMediaStateToViewer(vmName);
}

void MainWindow::ejectFloppyForVm(const QString &vmName)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    QWidget *parentW = dialogParentFor(vmName);

    if (veModel->isRunning(vmName)) {
        if (!entry->qmpClient || !entry->qmpClient->isConnected()) {
            KMessageBox::error(parentW, i18n("Could not connect to QEMU monitor. Is the VM running?"));
            return;
        }

        QJsonObject ejectArgs;
        ejectArgs.insert(u"id"_s, u"fda"_s);
        const QMPResult result = entry->qmpClient->sendCommandSync(u"eject"_s, ejectArgs);

        if (!result.success) {
            KMessageBox::error(parentW, i18n("Eject floppy failed"), result.errorMessage);
            return;
        }
        statusBar()->showMessage(i18n("Floppy ejected."));
    }

    VMConfig cfg = (vmName == currentConfig.name()) ? currentConfig : entry->config;
    cfg.clearFloppyPath();
    veModel->updateConfig(vmName, cfg);
    saveVmConfig(getVmDir(vmName).absolutePath(), cfg);

    if (vmName == currentConfig.name()) {
        currentConfig = cfg;
        updateActionStates();
        updateDetails();
    }
    pushMediaStateToViewer(vmName);
}

void MainWindow::pushMediaStateToViewer(const QString &vmName)
{
    auto it = m_dbusViewers.find(vmName);
    if (it == m_dbusViewers.end() || !it->window)
        return;

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    const QString isoPath = entry->config.isoPath();
    const QString floppyPath = entry->config.floppyPath();

    it->window->setMediaState(!isoPath.isEmpty(),
                              isoPath.isEmpty() ? QString() : QFileInfo(isoPath).fileName(),
                              !floppyPath.isEmpty(),
                              floppyPath.isEmpty() ? QString() : QFileInfo(floppyPath).fileName());
}

void MainWindow::pushNetworkStateToViewer(const QString &vmName)
{
    auto it = m_dbusViewers.find(vmName);
    if (it == m_dbusViewers.end() || !it->window)
        return;

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    const VMConfig &cfg = entry->config;
    if (cfg.networkCard() == VMSpecs::NetworkCard::None) {
        it->window->setNetworkState(QString(), false);
        return;
    }

    const QString label = u"%1 (%2)"_s.arg(VMConfig::toUIString(cfg.networkCard()), VMConfig::toUIString(cfg.networkMode()));
    it->window->setNetworkState(label, loadViewerPrefs(vmName).networkLinkUp);
}

void MainWindow::shrinkDisk()
{
    if (currentConfig.name().isEmpty())
        return;

    if (veModel->isBusy(currentConfig.name()))
        return;

    const QemuImgProcess::ImageInfo dInfo = QemuImgProcess::inspect(currentConfig.diskPath());
    QString sizeInfo;
    if (dInfo.isValid) {
        sizeInfo = i18n("\n\nCurrent size: %1 GB (virtual), %2 GB (actual on disk)",
                        QString::number(dInfo.virtualSizeGb(), 'f', 1),
                        QString::number(dInfo.actualSizeGb(), 'f', 1));
    }

    auto answer = KMessageBox::questionTwoActions(this,
                                                  i18n("Compact the disk image of VM '%1'?\n\n"
                                                       "This will rewrite the disk image to reclaim unused space. "
                                                       "The VM must remain stopped during the operation.%2",
                                                       currentConfig.name(),
                                                       sizeInfo),
                                                  i18nc("@title:window", "Compact Disk Image"),
                                                  KGuiItem(i18n("Compact"), u"edit-clear-history"_s),
                                                  KStandardGuiItem::cancel());

    if (answer != KMessageBox::PrimaryAction)
        return;

    auto *job = new ShrinkJob(currentConfig.name(), currentConfig.diskPath(), this);

    if (jobTracker) {
        jobTracker->registerJob(job);
    }

    veModel->setVmState(currentConfig.name(), VmEntry::State::Shrinking);

    connect(job, &ShrinkJob::finished, this, [this](KJob *kjob) {
        auto *shrinkJob = qobject_cast<ShrinkJob *>(kjob);
        if (!shrinkJob)
            return;

        const QString vmName = shrinkJob->vmName();
        veModel->setVmState(vmName, VmEntry::State::Stopped);

        if (shrinkJob->error()) {
            statusBar()->showMessage(i18n("Disk compaction of VM '%1' failed: %2", vmName, shrinkJob->errorText()));
        } else {
            const VMConfig config = loadVmConfig(getVmDir(vmName).absolutePath());
            const QemuImgProcess::ImageInfo dInfo = QemuImgProcess::inspect(config.diskPath());

            QString newSizeMsg = u"."_s;
            if (dInfo.isValid) {
                newSizeMsg = i18n(" (now %1 GB).", QString::number(dInfo.actualSizeGb(), 'f', 1));
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

    statusBar()->showMessage(i18n("Compacting disk of VM '%1'...", currentConfig.name()));
    updateActionStates();
    updateDetails();
}

void MainWindow::manageSnapshots()
{
    manageSnapshotsForVm(selectedVmName());
}

void MainWindow::exportVM()
{
    if (currentConfig.name().isEmpty())
        return;

    if (veModel->isBusy(currentConfig.name())) {
        KMessageBox::error(this, i18n("The VM '%1' is currently busy. Please wait.", currentConfig.name()), i18n("Busy"));
        return;
    }

    QString filterKemu = i18n("KEMU Portable Archives (*.kemu.tar.zst)");
    QString filterOva = i18n("OVA Files (*.ova)");
    QString selectedFilter;

    QString filePath = QFileDialog::getSaveFileName(this,
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

        auto *job = new ExportJob(currentConfig.name(), getVmDir(currentConfig.name()).absolutePath(), filePath, this);
        if (jobTracker) {
            jobTracker->registerJob(job);
        }
        veModel->setVmState(currentConfig.name(), VmEntry::State::Exporting);

        connect(job, &ExportJob::finished, this, [this](KJob *kjob) {
            auto *exportJob = qobject_cast<ExportJob *>(kjob);
            if (!exportJob)
                return;

            const QString vmName = exportJob->vmName();
            veModel->setVmState(vmName, VmEntry::State::Stopped);

            if (exportJob->error() == KJob::KilledJobError) {
                statusBar()->showMessage(i18n("Operation cancelled."));
            } else if (exportJob->error()) {
                statusBar()->showMessage(i18n("Export of VM '%1' failed: %2", vmName, exportJob->errorText()));
                KMessageBox::error(this, i18n("Failed to export VM '%1':\n%2", vmName, exportJob->errorText()), i18n("Export Failed"));
            } else {
                statusBar()->showMessage(i18n("VM '%1' exported successfully.", vmName));
            }

            if (currentConfig.name() == vmName) {
                updateDetails();
            }
            updateActionStates();

            exportJob->deleteLater();
        });

        job->start();

        statusBar()->showMessage(i18n("Exporting VM '%1'...", currentConfig.name()));
        updateActionStates();
        updateDetails();
    }
}

void MainWindow::importVM()
{
    const QString filePath = QFileDialog::getOpenFileName(this,
                                                          i18n("Import Virtual Machine"),
                                                          QDir::homePath(),
                                                          i18n("KEMU Portable Archives (*.kemu.tar.zst);;OVA Files (*.ova);;All "
                                                               "Files (*)"));

    if (filePath.isEmpty())
        return;

    if (filePath.endsWith(u".ova"_s, Qt::CaseInsensitive)) {
        importOVA(filePath);
        return;
    }

    const QString vmBaseDir = getVmBaseDir().absolutePath();

    auto *job = new ImportJob(filePath, vmBaseDir, this);
    if (jobTracker) {
        jobTracker->registerJob(job);
    }
    activeImports.append(job);

    connect(job, &ImportJob::finished, this, [this](KJob *kjob) {
        auto *importJob = qobject_cast<ImportJob *>(kjob);
        if (!importJob)
            return;

        activeImports.removeOne(importJob);

        if (importJob->error() == KJob::KilledJobError) {
            statusBar()->showMessage(i18n("Operation cancelled."));
        } else if (importJob->error()) {
            statusBar()->showMessage(i18n("Import failed: %1", importJob->errorText()));
            KMessageBox::error(this, i18n("Failed to import VM:\n%1", importJob->errorText()), i18n("Import Failed"));

        } else {
            const QString vmDir = getVmDir(importJob->vmName()).absolutePath();

            VMConfig config = loadVmConfig(vmDir);
            if (SettingsDialog::importRegenerateMac())
                config.setMacAddress(Utils::generateRandomMac(veModel->usedMacAddresses()));

            // Always rewrite: this also regenerates qemu.sh, which is not part
            // of the archive because it contains host paths.
            saveVmConfig(vmDir, config);

            statusBar()->showMessage(i18n("VM '%1' imported successfully.", importJob->vmName()));
            loadVmList();
            selectVmByName(importJob->vmName());
        }

        importJob->deleteLater();
    });

    job->start();
    statusBar()->showMessage(i18n("Importing VM..."));
}

void MainWindow::saveScreenshot()
{
    const QString vmName = currentConfig.name();
    if (vmName.isEmpty() || !veModel->isRunning(vmName))
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

    const VmEntry *entry = veModel->entryByName(vmName);
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
    const QString defaultName = u"%1/%2_%3.png"_s.arg(picturesDir, vmName, QDateTime::currentDateTime().toString(u"yyyyMMdd_HHmmss"_s));

    const QString filePath = QFileDialog::getSaveFileName(this, i18n("Save Screenshot"), defaultName, i18n("PNG Image (*.png);;JPEG Image (*.jpg *.jpeg)"));

    if (filePath.isEmpty())
        return;

    QByteArray format = "PNG";
    if (filePath.endsWith(u".jpg"_s, Qt::CaseInsensitive) || filePath.endsWith(u".jpeg"_s, Qt::CaseInsensitive)) {
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
    const QString nsenterCmd =
        u"echo '-- KEMU Network Shell --'; "
        u"echo 'Network: %1  (Namespace PID %2)'; "
        u"echo 'Hint: Root privileges are mapped to the UID of the user "
        u"executing KEMU.'; "
        u"echo 'Type exit to leave.'; echo; "
        u"exec nsenter -t %2 -U --preserve-credentials -n -- /bin/bash"_s.arg(netName)
            .arg(nsPid);

    const QString terminal = SandboxCapabilities::instance().terminalProgram();
    if (terminal.isEmpty()) {
        KMessageBox::error(this,
                           i18n("No terminal emulator found.\n\n"
                                "You can enter the namespace manually:\n"
                                "nsenter -t %1 -U --preserve-credentials -n -- /bin/bash",
                                nsPid));
        return;
    }

    auto *process = new KProcess(this);
    connect(process, &KProcess::finished, process, &KProcess::deleteLater);
    process->setProgram(terminal);

    const QString title = i18n("KEMU Network Shell -- %1", netName);
    if (terminal.endsWith(u"/konsole"_s))
        process->setArguments({u"--separate"_s, u"--title"_s, title, u"-e"_s, u"/bin/bash"_s, u"-c"_s, nsenterCmd});
    else
        process->setArguments({u"-title"_s, title, u"-e"_s, u"/bin/bash"_s, u"-c"_s, nsenterCmd});

    process->start();

    if (!process->waitForStarted(3000)) {
        KMessageBox::error(this, i18n("Failed to open terminal: %1", process->errorString()));
        delete process;
    }
}

void MainWindow::reconnectViewer()
{
    const QString vmName = selectedVmName();
    if (vmName.isEmpty())
        return;

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry || !hasLiveQemu(vmName))
        return;

    if (entry->config.displayType() == VMSpecs::DisplayType::Dbus) {
        if (m_dbusViewers.contains(vmName)) {
            statusBar()->showMessage(i18n("Viewer is already running for '%1'.", vmName));
            return;
        }
        if (startDbusViewer(vmName)) {
            veModel->setHeadless(vmName, false);
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

void MainWindow::onTreeDoubleClicked(const QModelIndex &index)
{
    const int itemType = index.data(VirtualEnvironmentModel::ItemTypeRole).toInt();

    if (itemType == VirtualEnvironmentModel::Folder) {
        veTreeView->setExpanded(index, !veTreeView->isExpanded(index));
        return;
    }

    const QString vmName = index.data(VirtualEnvironmentModel::VmNameRole).toString();
    const VmEntry *entry = veModel->entryByName(vmName);
    const bool isRunning = entry && entry->state == VmEntry::State::Running;

    // VM is not running yet: start normally
    if (!isRunning) {
        startVM();
        return;
    }

    // VM is running and the integrated D-Bus viewer window is already open:
    // simply bring it to the front instead of trying to start the VM again.
    if (entry->config.displayType() == VMSpecs::DisplayType::Dbus && m_dbusViewers.contains(vmName)) {
        if (auto *win = m_dbusViewers[vmName].window.data()) {
            if (win->isMinimized() || !win->isVisible())
                win->showNormal();
            win->raise();
            win->activateWindow();
            return;
        }
    }

    // VM is running headless: reconnect viewer
    if (entry->isHeadless && entry->config.displayType() == VMSpecs::DisplayType::Dbus && !m_dbusViewers.contains(vmName))
        reconnectViewer();
}

void MainWindow::onDropCompleted()
{
    saveFolderStructure();
}

void MainWindow::showContextMenu(const QPoint &pos)
{
    const QModelIndex idx = veTreeView->indexAt(pos);
    QMenu contextMenu(this);

    if (idx.isValid()) {
        const int itemType = idx.data(VirtualEnvironmentModel::ItemTypeRole).toInt();

        if (itemType == VirtualEnvironmentModel::Folder) {
            auto *renameAction = contextMenu.addAction(QIcon::fromTheme(u"edit-rename"_s), i18n("Rename Folder"));
            connect(renameAction, &QAction::triggered, this, &MainWindow::renameFolder);

            auto *deleteAction = contextMenu.addAction(QIcon::fromTheme(u"edit-delete"_s), i18n("Delete Folder"));
            connect(deleteAction, &QAction::triggered, this, &MainWindow::deleteFolder);
            deleteAction->setEnabled(veModel->rowCount(idx) == 0);

            contextMenu.addSeparator();
            contextMenu.addAction(actionCollection()->action(u"vm_new"_s));

        } else {
            const QString vmName = idx.data(VirtualEnvironmentModel::VmNameRole).toString();
            const VmEntry *entry = veModel->entryByName(vmName);
            const bool isRunning = entry && entry->state == VmEntry::State::Running;
            const bool isDeploying = entry && entry->state == VmEntry::State::Deploying;
            const bool isShrinking = entry && entry->state == VmEntry::State::Shrinking;

            if (isShrinking) {
                auto *infoAction = contextMenu.addAction(QIcon::fromTheme(u"view-refresh"_s), i18n("Compacting disk..."));
                infoAction->setEnabled(false);
            } else if (isDeploying) {
                const QAction *originalPoweroffAction = actionCollection()->action(u"vm_poweroff"_s);
                QAction *customPoweroffAction = contextMenu.addAction(originalPoweroffAction->icon(), i18n("Abort Deployment"));
                connect(customPoweroffAction, &QAction::triggered, originalPoweroffAction, &QAction::trigger);
                contextMenu.addAction(customPoweroffAction);
            } else if (entry && entry->state == VmEntry::State::Cloning) {
                auto *infoAction = contextMenu.addAction(QIcon::fromTheme(u"edit-copy"_s), i18n("Cloning..."));
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
                contextMenu.addAction(actionCollection()->action(u"vm_poweroff"_s));

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

                contextMenu.addAction(actionCollection()->action(u"vm_poweroff"_s));
            }
        }
    } else {
        auto *newFolderAction = contextMenu.addAction(QIcon::fromTheme(u"folder-new"_s), i18n("New Folder"));
        connect(newFolderAction, &QAction::triggered, this, &MainWindow::createFolder);
        contextMenu.addSeparator();
        contextMenu.addAction(actionCollection()->action(u"vm_new"_s));
        contextMenu.addSeparator();
        contextMenu.addAction(actionCollection()->action(u"vm_import"_s));
    }

    contextMenu.exec(veTreeView->viewport()->mapToGlobal(pos));
}

void MainWindow::showCommand()
{
    if (currentConfig.name().isEmpty())
        return;

    const QString tap = currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork ? u"tap-<vm>"_s : QString{};
    const NetworkContext netCtx = networkContextFor(currentConfig, tap);

    const VirtualNetwork net = (m_networkManager && currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        ? m_networkManager->network(currentConfig.virtualNetworkName())
        : VirtualNetwork{};

    const QString sockPath = monitorSocketPath(currentConfig.name());
    const bool portablePaths = SandboxCapabilities::instance().has(SandboxCapabilities::ExternalToolPaths);
    QString cmd = QemuCommandBuilder::buildShellScript(currentConfig, getVmDir(currentConfig.name()), sockPath, netCtx, net, !portablePaths);

    if (!portablePaths) {
        cmd.prepend(
            i18n("# NOTE: this build ships its own tools. The paths below are only\n"
                 "# valid inside the application; run this script from the VM\n"
                 "# directory with QEMU installed on your system.\n\n"));
    }

    QDialog d(this);
    d.setWindowTitle(i18n("QEMU Command Line"));
    auto *l = new QVBoxLayout(&d);

    auto *t = new QTextEdit();
    t->setPlainText(cmd);
    t->setReadOnly(true);
    t->setFont(QFont(u"monospace"_s, 10));
    l->addWidget(t);

    if (cmd.contains("UEFI_CODE_NOT_FOUND"_L1) || cmd.contains("VM_VARS_FILE_MISSING"_L1)) {
        const QString warnText = currentConfig.useSecureBoot() ? i18n(
                                                                     "WARNING: UEFI Secure Boot firmware not found! "
                                                                     "Make sure a Secure Boot-capable OVMF package is "
                                                                     "installed on the host "
                                                                     "or set a custom path in the settings.")
                                                               : i18n(
                                                                     "WARNING: UEFI files missing! "
                                                                     "Make sure 'edk2-ovmf' is installed on the host "
                                                                     "or set a custom path in the settings.");
        auto *warn = new QLabel(warnText);
        warn->setWordWrap(true);
        l->addWidget(warn);
    }

    auto *btnLayout = new QHBoxLayout();
    auto *btnCopy = new QPushButton(QIcon::fromTheme(u"edit-copy"_s), i18n("Copy"));
    connect(btnCopy, &QPushButton::clicked, this, [cmd]() {
        QApplication::clipboard()->setText(cmd);
    });

    auto *btnClose = new QPushButton(QIcon::fromTheme(u"dialog-close"_s), i18n("Close"));
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

    const QString vmDirPath = getVmDir(vmName).absolutePath();

    const bool isResume = SaveStateManager::hasSavestate(vmDirPath, vmName);
    if (veModel->isBusy(vmName) && !isResume) {
        KMessageBox::error(this, i18n("The VM '%1' is currently busy.", vmName), i18n("Busy"));
        return;
    }

    m_qemuStderrTails.remove(vmName);
    m_userInitiatedShutdowns.remove(vmName);
    m_resumingFromSavestate.remove(vmName);
    m_startupFailureShown.remove(vmName);

    if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork && !ExternalTools::instance().isVirtualNetworkCapable()) {
        const auto answer = KMessageBox::questionTwoActions(this,
                                                            i18n("'%1' is configured for a virtual network, which is not available "
                                                                 "on this system:\n%2\n\nSwitch this VM to user-mode networking?",
                                                                 vmName,
                                                                 SandboxCapabilities::instance().unavailableReason(SandboxCapabilities::UserNamespaces)),
                                                            i18n("Virtual Network Unavailable"),
                                                            KGuiItem(i18n("Switch and Start"), u"network-wired"_s),
                                                            KStandardGuiItem::cancel());
        if (answer != KMessageBox::PrimaryAction)
            return;

        currentConfig.setNetworkMode(VMSpecs::NetworkMode::Passt);
        saveVmConfig(vmDirPath, currentConfig);
    }

    if (!headless && !VMConfig::isDisplayTypeAvailable(currentConfig.displayType())) {
        const auto fallback = VMConfig::isDisplayTypeAvailable(VMSpecs::DisplayType::Dbus) ? VMSpecs::DisplayType::Dbus : VMSpecs::DisplayType::Gtk;
        const auto answer = KMessageBox::questionTwoActions(this,
                                                            i18n("The display type configured for '%1' is not available in this build. "
                                                                 "Switch this VM to '%2'?",
                                                                 vmName,
                                                                 VMConfig::toUIString(fallback)),
                                                            i18n("Display Type Unavailable"),
                                                            KGuiItem(i18n("Switch and Start"), u"video-display"_s),
                                                            KStandardGuiItem::cancel());
        if (answer != KMessageBox::PrimaryAction)
            return;

        currentConfig.setDisplayType(fallback);
        saveVmConfig(vmDirPath, currentConfig);
    }

    auto *chain = new StartupChain(this);

    if (currentConfig.useTpm()) {
        chain->then([this, vmName](auto done) {
            startSwtpmAsync(vmName, std::move(done));
        });
    }
    if (!currentConfig.sharedPath().isEmpty() && currentConfig.osType() == VMSpecs::OsType::Windows) {
        chain->then([this, vmName](auto done) {
            startVirtiofsdAsync(vmName, std::move(done));
        });
    }

    // Network: passt is async, virtual-network setup is sync (namespace
    // operations don't block long enough to matter).
    QString resolvedTap;
    if (currentConfig.networkCard() != VMSpecs::NetworkCard::None) {
        if (currentConfig.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            resolvedTap = ensureNetworkRunning(currentConfig.virtualNetworkName(), vmName);
            if (resolvedTap.isEmpty()) {
                delete chain;
                tearDownVmRuntime(vmName);
                return;
            }
        } else if (usePasstFor(currentConfig)) {
            const auto pf = currentConfig.portForwards();
            chain->then([this, vmName, pf](auto done) {
                startPasstAsync(vmName, pf, std::move(done));
            });
        }
    }

    chain->run([this, vmName, headless, resolvedTap](bool ok) {
        if (!ok) {
            tearDownVmRuntime(vmName);
            return;
        }
        launchQemuProcess(vmName, headless, resolvedTap);
    });
}

void MainWindow::startVMHeadless()
{
    startVM(true);
}

void MainWindow::shutdownVM()
{
    shutdownVMByName(currentConfig.name());
}

void MainWindow::resetVM()
{
    resetVMByName(currentConfig.name());
}

void MainWindow::powerOffVM()
{
    powerOffVMByName(currentConfig.name());
}

void MainWindow::saveStateVM()
{
    saveStateVMByName(currentConfig.name());
}

void MainWindow::discardSavedState()
{
    discardSavedStateForVm(currentConfig.name());
}

void MainWindow::handleQemuExit(const QString &vmName, int exitCode, KProcess::ExitStatus exitStatus)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    QPointer<KProcess> p = entry->qemuProcess;

    const bool wasDeploying = entry->state == VmEntry::State::Deploying;
    const bool userInitiated = m_userInitiatedShutdowns.contains(vmName);
    const bool startupAlreadyShown = m_startupFailureShown.contains(vmName);
    const bool abnormal = (exitStatus != KProcess::NormalExit) || (exitCode != 0);

    if (p) {
        QByteArray &tail = m_qemuStderrTails[vmName];
        tail.append(p->readAllStandardError());
    }
    const QByteArray stderrTail = m_qemuStderrTails.value(vmName);

    const bool savestatePresent = SaveStateManager::hasSavestate(getVmDir(vmName).absolutePath(), vmName);

    tearDownVmRuntime(vmName);

    if (wasDeploying && abnormal && !userInitiated) {
        showQemuFailureDialog(vmName, /*startupFailed=*/false, exitCode, exitStatus, stderrTail);
        abortDeployment(vmName, false);
    } else if (wasDeploying) {
        finalizeDeployment(vmName);
    } else if (savestatePresent && !abnormal) {
        veModel->setVmState(vmName, VmEntry::State::Saved);
        statusBar()->showMessage(i18n("VM '%1' state saved.", vmName));
    } else if (abnormal && !userInitiated && !startupAlreadyShown) {
        showQemuFailureDialog(vmName, /*startupFailed=*/false, exitCode, exitStatus, stderrTail);
    } else {
        statusBar()->showMessage(i18n("VM '%1' stopped.", vmName));
    }

    m_startupFailureShown.remove(vmName);

    if (selectedVmName() == vmName) {
        updateDetails();
        updateActionStates();
        if (perfMonitor) {
            perfMonitor->clearHistory();
            perfMonitor->setActive(false);
        }
    }

    if (p)
        p->deleteLater();
}

void MainWindow::onQMPEvent(const QString &vmName, const QString &eventName, const QJsonObject &data)
{
    qCDebug(KEMU) << "QMP Event for" << vmName << ":" << eventName << data;

    // Pause/resume events that occur as part of a save-state operation are
    // internal bookkeeping. The user already sees the "Saving state..."
    // subtitle and the Saved-style icon, so no extra status-bar noise.
    const VmEntry *entry = veModel->entryByName(vmName);
    const bool isSaving = entry && entry->state == VmEntry::State::Saving;

    if (eventName == u"MIGRATION"_s) {
        const QString status = data.value(u"status"_s).toString();
        // Incoming-migration completion on the resume path. The guest was
        // saved in a paused state (save() issued 'stop' before migrate), so it
        // stays paused until we 'cont'. Doing it here - not on QMP connect -
        // closes the race where QMP is up before the migration has finished.
        if (status == u"completed"_s && m_resumingFromSavestate.remove(vmName)) {
            if (const VmEntry *e = veModel->entryByName(vmName); e && e->qmpClient) {
                e->qmpClient->sendCommand(u"cont"_s, {}, [vmName](QMPResult r) {
                    if (!r.success)
                        qCWarning(KEMU) << "Could not resume" << vmName << "from savestate:" << r.errorMessage;
                });
            }
        }
        return;
    }

    if (eventName == u"SHUTDOWN"_s) {
        statusBar()->showMessage(i18n("VM '%1' is shutting down...", vmName));
        return;
    }

    if (eventName == u"GUEST_CRASHED"_s) {
        // During a save-state operation a "crash" event would be a confusing
        // duplicate of the save-state failure dialog. Suppress it; the save
        // path surfaces the real reason.
        if (isSaving)
            return;
        KMessageBox::error(this, i18n("The virtual machine '%1' has crashed.", vmName), i18n("VM Crashed"));
        return;
    }

    if (eventName == u"STOP"_s) {
        if (isSaving || m_suspendInProgress)
            return;
        statusBar()->showMessage(i18n("VM '%1' paused.", vmName));
        return;
    }

    if (eventName == u"RESUME"_s) {
        // Only happens as a rollback after a failed migration - in that
        // case the save-state failure dialog explains what's going on.
        if (isSaving || m_suspendInProgress)
            return;
        statusBar()->showMessage(i18n("VM '%1' resumed.", vmName));
        return;
    }

    if (eventName == u"DEVICE_TRAY_MOVED"_s) {
        if (data.value(u"id"_s).toString() != u"cdrom0"_s)
            return;
        if (!data.value(u"tray-open"_s).toBool())
            return;

        // Operate on the affected VM's own config, not on the selection.
        VmEntry *e = veModel->entryByName(vmName);
        if (!e || e->config.isoPath().isEmpty())
            return;

        VMConfig cfg = e->config;
        cfg.setISOPath({});
        saveVmConfig(getVmDir(vmName).absolutePath(), cfg);
        veModel->updateConfig(vmName, cfg);
        pushMediaStateToViewer(vmName);

        if (currentConfig.name() == vmName) {
            currentConfig = cfg;
            updateDetails();
        }

        statusBar()->showMessage(i18n("VM '%1' ejected the ISO.", vmName));
        return;
    }

    if (eventName == u"NETDEV_STREAM_CONNECTED"_s || eventName == u"NETDEV_STREAM_DISCONNECTED"_s) {
        if (data.value(u"netdev-id"_s).toString() != KEMUConst::NetdevId)
            return;
        auto it = m_dbusViewers.find(vmName);
        // Reported by QEMU, not requested by the user: do not persist.
        if (it != m_dbusViewers.end() && it->window)
            it->window->setNetworkLinkState(eventName == u"NETDEV_STREAM_CONNECTED"_s, false);
        return;
    }
}

void MainWindow::onUsbMenuAboutToShow()
{
    populateUsbMenuFor(selectedVmName(), usbActionMenu->menu());
}

void MainWindow::onUsbActionTriggered()
{
    auto *action = qobject_cast<QAction *>(sender());
    if (!action)
        return;

    const QVariantList d = action->data().toList();
    const int busNum = d[0].toInt();
    const int devAddr = d[1].toInt();
    const QString busKey = d[2].toString();
    const QString qemuId = d[3].toString();
    const QString vmName = d.size() > 4 ? d.value(4).toString() : selectedVmName();

    VmEntry *entry = veModel ? veModel->entryByName(vmName) : nullptr;
    if (!entry || !entry->qmpClient)
        return;

    if (action->isChecked()) {
        // Attach
        const QMPResult r = entry->qmpClient->attachUsbDevice(busNum, devAddr, qemuId);
        if (r.success) {
            entry->attachedUsb.insert(busKey, qemuId);
        } else {
            action->setChecked(false);
            KMessageBox::error(this,
                               i18n("Could not attach USB device.\n\n%1\n\n"
                                    "Make sure QEMU was started with a USB controller "
                                    "and has permission to access /dev/bus/usb/.",
                                    r.errorMessage),
                               i18n("USB Passthrough"));
        }
    } else {
        // Detach
        const QMPResult r = entry->qmpClient->detachUsbDevice(qemuId);
        if (r.success) {
            entry->attachedUsb.remove(busKey);
        } else {
            action->setChecked(true);
            KMessageBox::error(this, r.errorMessage, i18n("USB Passthrough"));
        }
    }
}

void MainWindow::importOVA(const QString &filePath)
{
    auto *job = new OvaImportJob(filePath, getVmBaseDir().absolutePath(), 0, this);

    if (jobTracker) {
        jobTracker->registerJob(job);
    }
    activeImports.append(job);

    connect(job, &OvaImportJob::finished, this, [this](KJob *kjob) {
        auto *importJob = qobject_cast<OvaImportJob *>(kjob);
        if (!importJob)
            return;

        activeImports.removeOne(importJob);

        if (importJob->error()) {
            statusBar()->showMessage(i18n("OVA import failed: %1", importJob->errorText()));
            KMessageBox::error(this, i18n("Failed to import OVA:\n%1", importJob->errorText()), i18n("Import Failed"));

        } else {
            VMConfig config = importJob->importedConfig();
            const QStringList used = veModel->usedMacAddresses();
            if (SettingsDialog::importRegenerateMac() || used.contains(config.macAddress(), Qt::CaseInsensitive))
                config.setMacAddress(Utils::generateRandomMac(used));
            saveVmConfig(getVmDir(config.name()).absolutePath(), config);

            statusBar()->showMessage(i18n("VM '%1' imported from OVA.", config.name()));
            loadVmList();

            selectVmByName(config.name());
        }

        importJob->deleteLater();
    });

    job->start();
    statusBar()->showMessage(i18n("Importing OVA..."));
}

void MainWindow::exportOVA(const QString &filePath)
{
    const QemuImgProcess::ImageInfo dInfo = QemuImgProcess::inspect(currentConfig.diskPath());

    const int diskGb = dInfo.isValid ? qRound(dInfo.virtualSizeGb()) : currentConfig.diskSizeGb();

    VMConfig exportConfig = currentConfig;
    exportConfig.setDiskSizeGb(diskGb);

    auto *job = new OvaExportJob(currentConfig.name(), currentConfig, currentConfig.diskPath(), dInfo.isValid ? dInfo.virtualSizeBytes : 0, filePath, this);

    if (jobTracker) {
        jobTracker->registerJob(job);
    }
    veModel->setVmState(currentConfig.name(), VmEntry::State::Exporting);

    connect(job, &OvaExportJob::finished, this, [this](KJob *kjob) {
        auto *exportJob = qobject_cast<OvaExportJob *>(kjob);
        if (!exportJob)
            return;

        const QString vmName = exportJob->vmName();
        veModel->setVmState(vmName, VmEntry::State::Stopped);

        if (exportJob->error() == KJob::KilledJobError) {
            statusBar()->showMessage(i18n("Operation cancelled."));
        } else if (exportJob->error()) {
            statusBar()->showMessage(i18n("OVA export of VM '%1' failed: %2", vmName, exportJob->errorText()));
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

    statusBar()->showMessage(i18n("Exporting VM '%1' as OVA...", currentConfig.name()));
    updateActionStates();
    updateDetails();
}

void MainWindow::onAboutToSleep()
{
    m_pausedBySuspend.clear();
    m_suspendInProgress = true;

    auto pending = std::make_shared<int>(0);
    for (const QString &name : veModel->vmNames()) {
        const VmEntry *e = veModel->entryByName(name);
        if (!e || e->state != VmEntry::State::Running || !e->qmpClient || !e->qmpClient->isConnected())
            continue;

        m_pausedBySuspend.insert(name);
        ++(*pending);

        if (auto it = m_dbusViewers.find(name); it != m_dbusViewers.end() && it->window)
            it->window->setSuspended(true);

        e->qmpClient->sendCommand(u"stop"_s, {}, [this, pending](QMPResult) {
            if (--(*pending) == 0)
                m_sessionGuard->prepareHandled();
        });
    }

    if (*pending == 0)
        m_sessionGuard->prepareHandled();
}

void MainWindow::onWokeUp()
{
    const QStringList names(m_pausedBySuspend.cbegin(), m_pausedBySuspend.cend());
    m_pausedBySuspend.clear();

    for (const QString &name : names) {
        const VmEntry *e = veModel->entryByName(name);
        if (!e || e->state != VmEntry::State::Running || !e->qmpClient || !e->qmpClient->isConnected())
            continue;

        e->qmpClient->sendCommand(u"cont"_s, {}, nullptr);
        if (auto it = m_dbusViewers.find(name); it != m_dbusViewers.end() && it->window)
            it->window->setSuspended(false);
    }
    m_suspendInProgress = false;
}

void MainWindow::shutdownVMByName(const QString &vmName)
{
    if (!veModel->isRunning(vmName) && !(veModel->entryByName(vmName) && veModel->entryByName(vmName)->state == VmEntry::State::Deploying))
        return;

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;
    QWidget *parentW = dialogParentFor(vmName);

    const QPointer<QMPClient> client = entry->qmpClient;
    if (!client || !client->isConnected()) {
        KMessageBox::error(parentW, i18n("Could not connect to QEMU monitor. Is the VM running?"));
        return;
    }

    // Handle deployment shutdown
    if (entry && entry->state == VmEntry::State::Deploying) {
        const auto answer = KMessageBox::questionTwoActionsCancel(parentW,
                                                                  i18n("VM '%1' is currently being deployed.\n\n"
                                                                       "Stopping now will abort the installation. "
                                                                       "Do you also want to delete the VM?",
                                                                       vmName),
                                                                  i18n("Abort Deployment"),
                                                                  KGuiItem(i18n("Stop && Keep"), u"media-playback-stop"_s),
                                                                  KGuiItem(i18n("Stop && Delete"), u"edit-delete"_s),
                                                                  KStandardGuiItem::cancel(),
                                                                  u"ConfirmVmShutdownDeployment"_s); // remembered via kemurc

        if (answer == KMessageBox::PrimaryAction)
            abortDeployment(vmName, false);
        else if (answer == KMessageBox::SecondaryAction)
            abortDeployment(vmName, true);
        return;
    }

    const KGuiItem shutdownItem(i18n("Shutdown"), u"system-shutdown"_s, i18n("Send ACPI power-off signal to the guest"));

    const auto answer = KMessageBox::questionTwoActions(parentW,
                                                        i18n("Are you sure you want to shut down '%1'?\n\n"
                                                             "The guest will receive an ACPI power-off signal. "
                                                             "Unsaved work inside the guest may be lost if the guest OS "
                                                             "does not handle the signal.",
                                                             vmName),
                                                        i18nc("@title:window", "Confirm Shutdown"),
                                                        shutdownItem,
                                                        KStandardGuiItem::cancel(),
                                                        u"ConfirmVmShutdown"_s); // remembered via kemurc
    if (answer != KMessageBox::PrimaryAction)
        return;

    m_userInitiatedShutdowns.insert(vmName);

    const QMPResult result = client->sendCommandSync(u"system_powerdown"_s);

    if (!result.success) {
        m_userInitiatedShutdowns.remove(vmName);
        m_resumingFromSavestate.remove(vmName);
        KMessageBox::error(parentW, result.errorMessage, i18n("Shutdown Failed"));
        return;
    }

    statusBar()->showMessage(i18n("Shutting down virtual machine '%1'...", vmName));
}

void MainWindow::resetVMByName(const QString &vmName)
{
    if (!veModel->isRunning(vmName))
        return;

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;
    QPointer<QWidget> safeParentW = dialogParentFor(vmName);

    const QPointer<QMPClient> client = entry->qmpClient;
    if (!client || !client->isConnected()) {
        KMessageBox::error(safeParentW, i18n("Could not connect to QEMU monitor. Is the VM running?"));
        return;
    }

    const KGuiItem resetItem(i18n("Reset"), u"system-reboot"_s, i18n("Hard-reset the virtual machine"));

    const auto answer = KMessageBox::warningTwoActions(safeParentW,
                                                       i18n("Are you sure you want to reset '%1'?\n\n"
                                                            "This is equivalent to pressing the hardware reset button. "
                                                            "Unsaved data inside the guest will be lost, "
                                                            "and the disk filesystem may become inconsistent.",
                                                            vmName),
                                                       i18nc("@title:window", "Confirm Reset"),
                                                       resetItem,
                                                       KStandardGuiItem::cancel(),
                                                       u"ConfirmVmReset"_s);

    if (answer != KMessageBox::PrimaryAction)
        return;

    const QMPResult result = client->sendCommandSync(u"system_reset"_s);

    if (!result.success) {
        KMessageBox::error(safeParentW, result.errorMessage, i18n("Reset Failed"));
        return;
    }

    statusBar()->showMessage(i18n("Virtual machine '%1' was successfully reset.", vmName));
}

void MainWindow::powerOffVMByName(const QString &vmName)
{
    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry || (entry->state != VmEntry::State::Running && entry->state != VmEntry::State::Deploying))
        return;
    QPointer<QWidget> safeParentW = dialogParentFor(vmName);

    // Deployment is a separate flow: an in-progress install cannot be saved.
    if (entry->state == VmEntry::State::Deploying) {
        const auto answer = KMessageBox::questionTwoActionsCancel(safeParentW,
                                                                  i18n("VM '%1' is currently being deployed.\n\n"
                                                                       "Forcing a power-off now will abort the installation "
                                                                       "and may leave the disk filesystem in an inconsistent state.\n\n"
                                                                       "Do you also want to delete the VM?",
                                                                       vmName),
                                                                  i18n("Abort Deployment"),
                                                                  KGuiItem(i18n("Power-off && Keep"), u"media-playback-stop"_s),
                                                                  KGuiItem(i18n("Power-off && Delete"), u"edit-delete"_s),
                                                                  KStandardGuiItem::cancel(),
                                                                  u"ConfirmVmPowerOffDeployment"_s);

        if (answer == KMessageBox::PrimaryAction)
            abortDeployment(vmName, false);
        else if (answer == KMessageBox::SecondaryAction)
            abortDeployment(vmName, true);
        return;
    }

    const QPointer<KProcess> process = entry->qemuProcess;
    if (!process)
        return;

    // Save State requires a working QMP channel. Without it we can only
    // offer the classic two-button dialog (Force power-off / Cancel).
    const QPointer<QMPClient> client = entry->qmpClient;
    const bool canSaveState = client && client->isConnected();

    const KGuiItem saveStateItem(i18n("Save State"), u"document-save"_s, i18n("Save the running state to disk; resume exactly here on next start"));
    const KGuiItem forceStopItem(i18n("Force power-off"), u"process-stop"_s, i18n("Forcefully stop the virtual machine (may cause data loss)"));

    KMessageBox::ButtonCode answer;
    if (canSaveState) {
        answer = KMessageBox::warningTwoActionsCancel(safeParentW,
                                                      i18n("How do you want to stop '%1'?<br/><br/>"
                                                           "<b>Save State</b> suspends the VM to disk so it can resume "
                                                           "from this exact point on the next start.<br/><br/>"
                                                           "<b>Force power-off</b> is equivalent to pulling the plug "
                                                           "and may cause data loss.<br/>",
                                                           vmName),
                                                      i18nc("@title:window", "Power-off Virtual Machine"),
                                                      saveStateItem,
                                                      forceStopItem,
                                                      KStandardGuiItem::cancel(),
                                                      {},
                                                      KMessageBox::AllowLink);
    } else {
        answer = KMessageBox::warningTwoActions(safeParentW,
                                                i18n("Are you sure you want to force a power-off for '%1'? "
                                                     "This is equivalent to pulling the plug and may cause data loss.",
                                                     vmName),
                                                i18nc("@title:window", "Confirm Force Power-off"),
                                                forceStopItem,
                                                KStandardGuiItem::cancel(),
                                                u"ConfirmVmPowerOff"_s);
    }

    if (canSaveState && answer == KMessageBox::PrimaryAction) {
        saveStateVMByName(vmName, true);
        return;
    }

    // three-button dialog -> SecondaryAction, two-button dialog   -> PrimaryAction
    const bool forcePowerOff = (canSaveState && answer == KMessageBox::SecondaryAction) || (!canSaveState && answer == KMessageBox::PrimaryAction);
    if (!forcePowerOff)
        return;

    m_userInitiatedShutdowns.insert(vmName);

    bool qmpSuccess = false;
    if (client && client->isConnected()) {
        const QMPResult res = client->sendCommandSync(u"quit"_s, QJsonObject(), 2000);
        qmpSuccess = res.success;
    }

    if (qmpSuccess && process->state() == KProcess::Running) {
        process->waitForFinished(3000);
    }

    if (process->state() == KProcess::Running) {
        qCWarning(KEMU) << "VM" << vmName << "did not exit gracefully. Forcing terminate (SIGTERM)!";
        process->terminate();

        if (!process->waitForFinished(2000)) {
            qCWarning(KEMU) << "VM" << vmName << "is still hanging. Forcing power-off (SIGKILL)!";
            process->kill();
        }
    }

    statusBar()->showMessage(i18n("Force power-off initiated for virtual machine '%1'...", vmName));
}

void MainWindow::saveStateVMByName(const QString &vmName, bool skipConfirm)
{
    if (vmName.isEmpty() || !veModel->isRunning(vmName))
        return;

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry || !entry->qmpClient || !entry->qmpClient->isConnected())
        return;

    const QPointer<KProcess> process = entry->qemuProcess;
    if (!process)
        return;

    QPointer<QWidget> safeParentW = dialogParentFor(vmName);

    if (!skipConfirm) {
        const auto answer = KMessageBox::questionTwoActions(safeParentW,
                                                            i18n("Save the running state of '%1' to disk?\n\n"
                                                                 "The VM will stop. On next start it will resume exactly "
                                                                 "from this point. The disk image must not be modified "
                                                                 "while a saved state exists.",
                                                                 vmName),
                                                            i18nc("@title:window", "Save VM State"),
                                                            KGuiItem(i18n("Save State"), u"document-save"_s),
                                                            KStandardGuiItem::cancel());
        if (answer != KMessageBox::PrimaryAction)
            return;
    }

    m_userInitiatedShutdowns.insert(vmName);
    veModel->setVmState(vmName, VmEntry::State::Saving);
    statusBar()->showMessage(i18n("Saving state of '%1'...", vmName));
    updateActionStates();

    // Visually mark the integrated D-Bus viewer as suspended.
    if (auto it = m_dbusViewers.find(vmName); it != m_dbusViewers.end() && it->window)
        it->window->setSuspended(true);

    const QString vmDirPath = getVmDir(vmName).absolutePath();

    m_saveStateManager.save(entry->qmpClient, vmDirPath, vmName, [this, vmName, safeParentW, process](bool ok, QString err) {
        if (!ok) {
            m_userInitiatedShutdowns.remove(vmName);
            m_resumingFromSavestate.remove(vmName);
            if (veModel->entryByName(vmName))
                veModel->setVmState(vmName, VmEntry::State::Running);

            // Restore the viewer to its normal, interactive state.
            if (auto it = m_dbusViewers.find(vmName); it != m_dbusViewers.end() && it->window)
                it->window->setSuspended(false);

            if (safeParentW) {
                KMessageBox::error(safeParentW, i18n("Could not save VM state: %1", err), i18n("Save State Failed"));
            }
            updateActionStates();
            return;
        }

        const VmEntry *currentEntry = veModel->entryByName(vmName);

        if (currentEntry && currentEntry->qmpClient && currentEntry->qmpClient->isConnected()) {
            currentEntry->qmpClient->sendCommand(u"quit"_s, {}, nullptr);
        }

        veModel->setHasSavestate(vmName, true);

        QTimer::singleShot(1000, this, [this, vmName, process]() {
            if (!process || process->state() != KProcess::Running)
                return;

            qCWarning(KEMU) << "VM" << vmName << "did not exit after savestate quit; sending SIGTERM.";
            process->terminate();

            QTimer::singleShot(1000, this, [process, vmName]() {
                if (process && process->state() == KProcess::Running) {
                    qCWarning(KEMU) << "VM" << vmName << "still alive after SIGTERM; sending SIGKILL.";
                    process->kill();
                }
            });
        });
    });
}

void MainWindow::discardSavedStateForVm(const QString &vmName)
{
    if (vmName.isEmpty())
        return;

    QPointer<QWidget> safeParentW = dialogParentFor(vmName);

    const auto answer = KMessageBox::warningTwoActions(safeParentW,
                                                       i18n("Discard the saved state of '%1'?\n\n"
                                                            "The VM will boot from scratch on the next start. "
                                                            "Any unsaved work inside the suspended guest will be lost.",
                                                            vmName),
                                                       i18nc("@title:window", "Discard Saved State"),
                                                       KGuiItem(i18n("Discard"), u"edit-delete"_s),
                                                       KStandardGuiItem::cancel());
    if (answer != KMessageBox::PrimaryAction)
        return;

    const QString dir = getVmDir(vmName).absolutePath();
    if (!SaveStateManager::discardSavestate(dir, vmName)) {
        KMessageBox::error(safeParentW, i18n("Could not delete the savestate file."));
        return;
    }

    veModel->setHasSavestate(vmName, false);

    if (veModel->entryByName(vmName) && veModel->entryByName(vmName)->state == VmEntry::State::Saved)
        veModel->setVmState(vmName, VmEntry::State::Stopped);

    statusBar()->showMessage(i18n("Discarded saved state for '%1'.", vmName));
    updateActionStates();
    updateDetails();
}

void MainWindow::manageSnapshotsForVm(const QString &vmName)
{
    if (vmName.isEmpty())
        return;

    const VMConfig cfg = loadVmConfig(getVmBaseDir().filePath(vmName));

    if (cfg.diskPath().isEmpty()) {
        KMessageBox::error(dialogParentFor(vmName), i18n("No disk image configured for \"%1\".", vmName));
        return;
    }

    const VmEntry *entry = veModel->entryByName(vmName);
    const QPointer<QMPClient> qmp = entry ? entry->qmpClient : nullptr;

    auto *dlg = new SnapshotDialog(vmName, cfg.diskPath(), qmp, dialogParentFor(vmName));
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->exec();
}

void MainWindow::createNewVM()
{
    NewVmDialog dialog(this);
    dialog.setExistingVmNames(existingVmNames());
    dialog.setImageDatabase(&m_imageDatabase);
    dialog.setNetworkManager(m_networkManager);
    dialog.setMacUsageInfo(veModel->macUsageInfo());

    if (dialog.exec() != QDialog::Accepted)
        return;

    VMConfig newConfig = dialog.getResult();
    const NewVmDialog::Source source = dialog.source();
    const CloudDeployConfig cloudCfg = dialog.cloudConfig();

    const QDir baseDir = getVmBaseDir();

    if (newConfig.name().isEmpty() || baseDir.exists(newConfig.name())) {
        KMessageBox::error(this, i18n("Invalid name or VM already exists."));
        return;
    }

    if (!requireTool(ExternalTools::QemuImg))
        return;

    newConfig.ensureMacAddress(veModel->usedMacAddresses());

    baseDir.mkdir(newConfig.name());
    const QString vmPath = baseDir.filePath(newConfig.name());
    const QString diskPath = QDir(vmPath).filePath(KEMUConst::VMDiskFileName);

    if (source != NewVmDialog::Source::CloudImage) {
        KProcess imgProcess;
        imgProcess.setOutputChannelMode(KProcess::SeparateChannels);
        imgProcess.setProgram(ExternalTools::instance().path(ExternalTools::QemuImg));
        imgProcess.setArguments({u"create"_s, u"-f"_s, u"qcow2"_s, diskPath, u"%1G"_s.arg(newConfig.diskSizeGb())});
        imgProcess.start();
        imgProcess.waitForFinished(10000);

        if (imgProcess.exitCode() != 0) {
            KMessageBox::error(this, i18n("Failed to create disk image."));
            QDir vmDir = baseDir.filePath(newConfig.name());
            if (!isManagedVmDir(vmDir)) {
                qCWarning(KEMU) << "Refusing to remove a directory outside the VM store:" << vmDir.absolutePath();
                return;
            }
            vmDir.removeRecursively();
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

            if (newConfig.useSecureBoot() && !ovmf.varsPath.contains(u"ms"_s, Qt::CaseInsensitive)
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
            KMessageBox::error(this,
                               i18n("Could not find OVMF_VARS template. Please install 'edk2-ovmf' "
                                    "or set the firmware path in Settings."),
                               i18n("Warning"));
        }
    }

    // ======== CLOUD DEPLOY ========
    if (source == NewVmDialog::Source::CloudImage) {
        const CloudImageInfo image = dialog.selectedCloudImage();

        const QString imageLocalPath = ImageDatabase::localImagePath(image.url);
        const bool needsDownload = !ImageDatabase::isImageCached(image.url);

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

        veModel->setVmState(newConfig.name(), VmEntry::State::Deploying);
        updateDetails();
        updateActionStates();

        if (needsDownload) {
            statusBar()->showMessage(i18n("Deploying '%1' - downloading cloud image...", newConfig.name()));

            auto *job = new DownloadJob(image.url, imageLocalPath, Utils::ChecksumType::NONE, {}, this);
            jobTracker->registerJob(job);

            connect(job, &DownloadJob::statusMessage, this, [this](const QString &msg) {
                statusBar()->showMessage(msg);
            });

            connect(job, &KJob::finished, this, [this, vmName = newConfig.name()](KJob *kjob) {
                auto *dlJob = qobject_cast<DownloadJob *>(kjob);
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
    if (source == NewVmDialog::Source::InstallerIso) {
        newConfig.setImageType(VMConfig::ImageType::InstallerIso);
        newConfig.setDeployState(VMConfig::DeployState::Deploying);
        saveVmConfig(vmPath, newConfig);
        loadVmList();
        selectVmByName(newConfig.name());

        PendingDeployment pd;
        pd.type = PendingDeployment::Type::ManualIso;
        m_pendingDeployments.insert(newConfig.name(), pd);

        veModel->setVmState(newConfig.name(), VmEntry::State::Deploying);
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
    // A busy VM cannot be reconfigured, but its specification is still worth
    // looking at, so open the dialog read-only instead of refusing.
    const bool readOnly = veModel->isBusy(currentConfig.name());

    const QemuImgProcess::ImageInfo dInfo = QemuImgProcess::inspect(currentConfig.diskPath());
    if (dInfo.isValid)
        currentConfig.setDiskSizeGb(qRound(dInfo.virtualSizeGb()));

    EditVmDialog dialog(EditVmDialog::Mode::Edit, this);

    dialog.setExistingVmNames(existingVmNames());
    dialog.setImageDatabase(&m_imageDatabase);
    dialog.setNetworkManager(m_networkManager);
    dialog.setMacUsageInfo(veModel->macUsageInfo());

    dialog.setConfig(currentConfig);

    if (readOnly) {
        dialog.setReadOnly(true);
        dialog.exec();
        return;
    }

    if (dialog.exec() == QDialog::Accepted) {
        VMConfig newConfig = dialog.getResult();

        newConfig.ensureMacAddress(veModel->usedMacAddresses());

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
                                        oldName,
                                        newName),
                                   i18n("Rename Failed"));
                return;
            }

            QString oldDiskPath = newConfig.diskPath();
            if (oldDiskPath.contains(oldName)) {
                newConfig.setDiskPath(oldDiskPath.replace(oldPath, newPath));
            }

            KConfig kconfig(u"kemurc"_s);
            KConfigGroup grp = kconfig.group(u"Folders"_s);
            const QString folder = grp.readEntry(oldName, {});
            if (!folder.isEmpty()) {
                grp.deleteEntry(oldName);
                grp.writeEntry(newName, folder);
                grp.sync();
            }
            // migrate per-VM viewer preferences to the new name
            if (kconfig.group(u"ViewerState"_s).hasGroup(oldName)) {
                const ViewerDevicePrefs vp = loadViewerPrefs(oldName);
                saveViewerPrefs(newName, vp);
                removeViewerPrefs(oldName);
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
    if (veModel->isBusy(currentConfig.name()))
        return;

    const auto answer = KMessageBox::questionTwoActions(this,
                                                        i18n("Delete VM '%1' and all associated files?", currentConfig.name()),
                                                        i18n("Delete VM"),
                                                        KGuiItem(i18n("Delete"), QStringLiteral("edit-delete")),
                                                        KStandardGuiItem::cancel());

    if (answer == KMessageBox::PrimaryAction) {
        {
            QDir vmDir = getVmDir(currentConfig.name());
            if (!isManagedVmDir(vmDir)) {
                qCWarning(KEMU) << "Refusing to remove a directory outside the VM store:" << vmDir.absolutePath();
                return;
            }
            vmDir.removeRecursively();
        }

        removeViewerPrefs(currentConfig.name());
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

    if (veModel->isBusy(srcName)) {
        KMessageBox::error(this, i18n("The VM '%1' is currently busy.", srcName), i18n("Busy"));
        return;
    }

    QString suggestion = srcName + u"_clone"_s;
    int suffix = 1;
    while (existingVmNames().contains(suggestion))
        suggestion = u"%1_clone%2"_s.arg(srcName).arg(++suffix);

    bool ok = false;
    QString cloneName = QInputDialog::getText(this, i18n("Clone VM"), i18n("Name for the cloned VM:"), QLineEdit::Normal, suggestion, &ok);

    if (!ok || cloneName.trimmed().isEmpty())
        return;

    cloneName = cloneName.trimmed();

    QString nameError;
    if (!VMConfig::isValidVmName(cloneName, &nameError)) {
        KMessageBox::error(this, nameError, i18n("Invalid Name"));
        return;
    }

    if (existingVmNames().contains(cloneName)) {
        KMessageBox::error(this, i18n("A VM named '%1' already exists.", cloneName), i18n("Error"));
        return;
    }

    const QDir baseDir = getVmBaseDir();
    const QString srcPath = baseDir.filePath(srcName);
    const QString dstPath = baseDir.filePath(cloneName);

    auto *job = new CloneJob(srcName, srcPath, dstPath, this);

    if (jobTracker)
        jobTracker->registerJob(job);

    veModel->setVmState(srcName, VmEntry::State::Cloning);

    connect(job, &CloneJob::finished, this, [this, srcName, cloneName, dstPath](KJob *kjob) {
        auto *cloneJob = qobject_cast<CloneJob *>(kjob);
        if (!cloneJob)
            return;

        veModel->setVmState(srcName, VmEntry::State::Stopped);

        if (cloneJob->error()) {
            statusBar()->showMessage(i18n("Cloning of VM '%1' failed: %2", srcName, cloneJob->errorText()));
            KMessageBox::error(this, i18n("Failed to clone VM '%1':\n%2", srcName, cloneJob->errorText()), i18n("Clone Failed"));
        } else {
            // collect used macs to exclude random generation matches
            const QStringList usedMacs = veModel->usedMacAddresses();

            VMConfig cloneConfig = loadVmConfig(dstPath);
            cloneConfig.setName(cloneName);
            cloneConfig.setDiskPath(dstPath + u"/disk.qcow2"_s);
            cloneConfig.setDeployState(VMConfig::DeployState::Complete);

            if (SettingsDialog::cloneRegenerateMac()) {
                const QStringList macs = veModel->usedMacAddresses();

                if (!cloneConfig.macAddress().isEmpty()) {
                    auto answer = KMessageBox::questionTwoActions(this,
                                                                  i18n("The source VM has an explicit MAC address (%1).\n\n"
                                                                       "Using the same MAC on two VMs causes network "
                                                                       "conflicts. Would you like to enter a new MAC "
                                                                       "address, or clear it so QEMU generates a "
                                                                       "random one?",
                                                                       cloneConfig.macAddress()),
                                                                  i18n("MAC Address"),
                                                                  KGuiItem(i18n("Enter New MAC..."), u"document-edit"_s),
                                                                  KGuiItem(i18n("Generate Random"), u"roll"_s));

                    if (answer == KMessageBox::PrimaryAction) {
                        bool macOk = false;
                        const QString newMac =
                            QInputDialog::getText(this, i18n("New MAC Address"), i18n("MAC Address (e.g. 52:54:00:11:22:33):"), QLineEdit::Normal, {}, &macOk);
                        cloneConfig.setMacAddress((macOk && !newMac.trimmed().isEmpty()) ? newMac.trimmed() : Utils::generateRandomMac(macs));
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
            statusBar()->showMessage(i18n("VM '%1' cloned as '%2'.", srcName, cloneName));
        }

        if (currentConfig.name() == srcName)
            updateDetails();
        updateActionStates();

        cloneJob->deleteLater();
    });

    job->start();

    statusBar()->showMessage(i18n("Cloning VM '%1'...", srcName));
    updateActionStates();
    updateDetails();
}

void MainWindow::createFolder()
{
    bool ok = false;
    const QString name = QInputDialog::getText(this, i18n("New Folder"), i18n("Folder name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.trimmed().isEmpty())
        return;

    const QModelIndex idx = veModel->createFolder(name.trimmed());
    if (idx.isValid()) {
        veTreeView->expand(idx);
        saveFolderStructure();
    }
}

void MainWindow::deleteFolder()
{
    const QModelIndex idx = veTreeView->currentIndex();
    if (veModel->removeFolder(idx))
        saveFolderStructure();
    else
        KMessageBox::error(this, i18n("The folder is not empty. Move all VMs out first."), i18n("Cannot Delete Folder"));
}

void MainWindow::renameFolder()
{
    const QModelIndex idx = veTreeView->currentIndex();
    if (!idx.isValid() || idx.data(VirtualEnvironmentModel::ItemTypeRole).toInt() != VirtualEnvironmentModel::Folder)
        return;

    bool ok = false;
    const QString newName = QInputDialog::getText(this, i18n("Rename Folder"), i18n("New name:"), QLineEdit::Normal, idx.data(Qt::DisplayRole).toString(), &ok);
    if (ok && !newName.trimmed().isEmpty()) {
        veModel->renameFolder(idx, newName.trimmed());
        saveFolderStructure();
    }
}

void MainWindow::selectVmByName(const QString &vmName)
{
    const QModelIndex idx = veModel->indexForVm(vmName);
    if (!idx.isValid())
        return;
    const QModelIndex parentIdx = idx.parent();
    if (parentIdx.isValid())
        veTreeView->expand(parentIdx);
    veTreeView->setCurrentIndex(idx);
}

QString MainWindow::selectedVmName() const
{
    const QModelIndex idx = veTreeView->currentIndex();
    if (!idx.isValid())
        return {};
    if (idx.data(VirtualEnvironmentModel::ItemTypeRole).toInt() != VirtualEnvironmentModel::Vm)
        return {};
    return idx.data(VirtualEnvironmentModel::VmNameRole).toString();
}

bool MainWindow::isCurrentItemFolder() const
{
    const QModelIndex idx = veTreeView->currentIndex();
    if (!idx.isValid())
        return false;
    return idx.data(VirtualEnvironmentModel::ItemTypeRole).toInt() == VirtualEnvironmentModel::Folder;
}

QMap<QString, QString> MainWindow::loadFolderAssignments() const
{
    QMap<QString, QString> map;
    KConfig config(u"kemurc"_s);
    const KConfigGroup grp = config.group(u"Folders"_s);

    const QStringList keys = grp.keyList();
    for (const QString &key : keys) {
        if (key != u"order"_s)
            map.insert(key, grp.readEntry(key, {}));
    }
    return map;
}

ViewerDevicePrefs MainWindow::loadViewerPrefs(const QString &vmName) const
{
    ViewerDevicePrefs prefs;
    KConfig config(u"kemurc"_s);
    const KConfigGroup grp = config.group(u"ViewerState"_s).group(vmName);

    prefs.audioOut = grp.readEntry(u"AudioOut"_s, prefs.audioOut);
    prefs.audioIn = grp.readEntry(u"AudioIn"_s, prefs.audioIn);
    prefs.clipboard = clipboardDirFromString(grp.readEntry(u"Clipboard"_s, clipboardDirToString(prefs.clipboard)));
    prefs.networkLinkUp = grp.readEntry(u"NetworkLinkUp"_s, prefs.networkLinkUp);
    return prefs;
}

void MainWindow::saveViewerPrefs(const QString &vmName, const ViewerDevicePrefs &prefs) const
{
    KConfig config(u"kemurc"_s);
    KConfigGroup grp = config.group(u"ViewerState"_s).group(vmName);

    grp.writeEntry(u"AudioOut"_s, prefs.audioOut);
    grp.writeEntry(u"AudioIn"_s, prefs.audioIn);
    grp.writeEntry(u"Clipboard"_s, clipboardDirToString(prefs.clipboard));
    grp.writeEntry(u"NetworkLinkUp"_s, prefs.networkLinkUp);

    grp.sync();
}

void MainWindow::removeViewerPrefs(const QString &vmName) const
{
    KConfig config(u"kemurc"_s);
    KConfigGroup parent = config.group(u"ViewerState"_s);
    parent.deleteGroup(vmName);
    parent.sync();
}

void MainWindow::saveFolderStructure() const
{
    KConfig config(u"kemurc"_s);
    KConfigGroup grp = config.group(u"Folders"_s);

    for (const QString &key : grp.keyList())
        grp.deleteEntry(key);

    const QMap<QString, QString> assignments = veModel->folderAssignments();
    for (auto it = assignments.cbegin(); it != assignments.cend(); ++it)
        grp.writeEntry(it.key(), it.value());

    QStringList collapsedFolders;
    for (const QString &name : veModel->folderNames()) {
        const QModelIndex idx = veModel->indexForFolder(name);
        if (idx.isValid() && !veTreeView->isExpanded(idx))
            collapsedFolders << name;
    }
    grp.writeEntry(u"collapsed"_s, collapsedFolders);
    grp.sync();
}

void MainWindow::showQemuFailureDialog(const QString &vmName, bool startupFailed, int exitCode, KProcess::ExitStatus exitStatus, const QByteArray &stderrTail)
{
    const QString title = startupFailed ? i18nc("@title:window", "Failed to Start VM") : i18nc("@title:window", "VM Terminated Unexpectedly");

    QString summary;
    if (startupFailed) {
        summary = i18n("QEMU failed to start the virtual machine '%1'.", vmName);
    } else if (exitStatus == KProcess::CrashExit) {
        summary = i18n("QEMU for VM '%1' was terminated by a signal.", vmName);
    } else {
        summary = i18n("QEMU for VM '%1' exited with code %2.", vmName, exitCode);
    }

    // Build the main body: summary + QEMU's stderr (the actually useful part)
    QString body = u"<p>"_s + summary.toHtmlEscaped() + u"</p>"_s;

    const QString stderrText = QString::fromUtf8(stderrTail).trimmed();
    if (!stderrText.isEmpty()) {
        body += u"<pre style='white-space: pre-wrap;'>"_s + stderrText.toHtmlEscaped() + u"</pre>"_s;
    } else {
        body += u"<p><i>"_s + i18n("(No diagnostic output captured.)").toHtmlEscaped() + u"</i></p>"_s;
    }

    KMessageBox::error(this, body, title);
}

void MainWindow::startDeployment(const QString &vmName)
{
    VmEntry *entry = veModel->entryByName(vmName);
    if (!entry || entry->state != VmEntry::State::Deploying)
        return;

    // The config may have been touched between VM creation and start (e.g.
    // CloudImageDeployer rewrote the disk path). Reload it and let the
    // model emit a refresh so the details view stays in sync.
    const QString vmPath = getVmDir(vmName).absolutePath();
    const VMConfig config = loadVmConfig(vmPath);
    veModel->updateConfig(vmName, config);

    // Reset failure-handling state from any previous attempt of this VM.
    m_qemuStderrTails.remove(vmName);
    m_userInitiatedShutdowns.remove(vmName);
    m_resumingFromSavestate.remove(vmName);
    m_startupFailureShown.remove(vmName);

    // Pre-flight: bail out before starting helpers if QEMU is missing,
    // so we don't leave half-set-up helpers running.
    if (!requireTool(ExternalTools::QemuSystem)) {
        abortDeployment(vmName, false);
        return;
    }

    // --- Helper chain ---
    auto *chain = new StartupChain(this);

    if (config.useTpm()) {
        chain->then([this, vmName](auto done) {
            startSwtpmAsync(vmName, std::move(done));
        });
    }
    if (!config.sharedPath().isEmpty() && config.osType() == VMSpecs::OsType::Windows) {
        chain->then([this, vmName](auto done) {
            startVirtiofsdAsync(vmName, std::move(done));
        });
    }

    // Network: passt is async, virtual-network setup is sync (namespace
    // operations are short and we need the tap device name before QEMU
    // starts).
    QString resolvedTap;
    if (config.networkCard() != VMSpecs::NetworkCard::None) {
        if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            resolvedTap = ensureNetworkRunning(config.virtualNetworkName(), vmName);
            if (resolvedTap.isEmpty()) {
                delete chain;
                abortDeployment(vmName, false);
                return;
            }
        } else if (usePasstFor(config)) {
            const auto pf = config.portForwards();
            chain->then([this, vmName, pf](auto done) {
                startPasstAsync(vmName, pf, std::move(done));
            });
        }
    }

    // Headless is a cloud-init concept: the guest configures itself and signals
    // completion by shutting down. An installer ISO needs a display.
    const auto pending = m_pendingDeployments.constFind(vmName);
    const bool unattended = pending != m_pendingDeployments.constEnd() && pending->type == PendingDeployment::Type::CloudImage;
    const bool headless = unattended && SettingsDialog::deployHeadless();

    chain->run([this, vmName, headless, resolvedTap](bool ok) {
        if (!ok) {
            abortDeployment(vmName, false);
            return;
        }
        launchQemuForDeployment(vmName, headless, resolvedTap);
    });
}

void MainWindow::finalizeDeployment(const QString &vmName)
{
    m_pendingDeployments.remove(vmName);

    const QString vmPath = getVmDir(vmName).absolutePath();
    const QDir vmDir(vmPath);

    CloudImageDeployer::removeNoCloudIso(vmDir);

    VMConfig config = loadVmConfig(vmPath);
    config.setBootFromIso(false);
    config.clearISOPath();
    config.setDeployState(VMConfig::DeployState::Complete);
    saveVmConfig(vmPath, config);

    veModel->updateConfig(vmName, config);

    if (selectedVmName() == vmName) {
        currentConfig = config;
        updateDetails();
    }

    statusBar()->showMessage(i18n("Deployment of '%1' complete. VM is ready.", vmName));
}

void MainWindow::abortDeployment(const QString &vmName, bool deleteVm)
{
    m_pendingDeployments.remove(vmName);

    const VmEntry *entry = veModel->entryByName(vmName);
    if (!entry)
        return;

    // Not routed through tearDownVmRuntime(), so drop the viewer explicitly.
    stopDbusViewer(vmName);

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
        QFile::remove(getVmBaseDir().filePath(vmName) + u'/' + KEMUConst::SwtpmSocketFileName);
    }

    if (entry->virtiofsdProcess) {
        entry->virtiofsdProcess->terminate();
        if (!entry->virtiofsdProcess->waitForFinished(3000))
            entry->virtiofsdProcess->kill();
        delete entry->virtiofsdProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u'/' + KEMUConst::VirtiofsdPidFileName);
    }

    if (entry->passtProcess) {
        entry->passtProcess->terminate();
        if (!entry->passtProcess->waitForFinished(3000))
            entry->passtProcess->kill();
        delete entry->passtProcess;
        QFile::remove(getVmBaseDir().filePath(vmName) + u'/' + KEMUConst::PasstSocketFileName);
    }

    QFile::remove(monitorSocketPath(vmName));

    releaseNetwork(entry->config);

    if (deleteVm) {
        {
            QDir vmDir = getVmDir(vmName);
            if (!isManagedVmDir(vmDir)) {
                qCWarning(KEMU) << "Refusing to remove a directory outside the VM store:" << vmDir.absolutePath();
            } else {
                vmDir.removeRecursively();
            }
        }
        loadVmList();
        onVmSelectionChanged();
        statusBar()->showMessage(i18n("Deployment of '%1' cancelled. VM deleted.", vmName));
    } else {
        veModel->clearRuntimeState(vmName);
        updateDetails();
        updateActionStates();
        statusBar()->showMessage(i18n("Deployment of '%1' cancelled.", vmName));
    }
}

void MainWindow::deployCloudImage(const QString &vmName)
{
    if (!m_pendingDeployments.contains(vmName))
        return;

    const auto pd = m_pendingDeployments[vmName];
    QDir vmDir = getVmDir(vmName);
    VMConfig config = loadVmConfig(vmDir.absolutePath());

    statusBar()->showMessage(i18n("Deploying '%1'. Preparing cloud image...", vmName));

    auto result = CloudImageDeployer::deploy(pd.imageLocalPath, vmDir, config.diskSizeGb(), pd.cloudConfig);

    if (!result.success) {
        m_pendingDeployments.remove(vmName);

        if (selectedVmName() == vmName) {
            veTreeView->clearSelection();
            currentConfig = VMConfig();
        }

        {
            if (!isManagedVmDir(vmDir)) {
                qCWarning(KEMU) << "Refusing to remove a directory outside the VM store:" << vmDir.absolutePath();
                return;
            } else {
                vmDir.removeRecursively();
            }
        }
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

void MainWindow::populateUsbMenuFor(const QString &vmName, QMenu *target)
{
    target->clear();

    VmEntry *entry = veModel ? veModel->entryByName(vmName) : nullptr;
    if (!entry || entry->state != VmEntry::State::Running || !entry->qmpClient) {
        auto *a = target->addAction(i18n("VM not running"));
        a->setEnabled(false);
        return;
    }

    const auto &caps = SandboxCapabilities::instance();
    if (!caps.has(SandboxCapabilities::UsbPassthrough)) {
        auto *a = target->addAction(caps.unavailableReason(SandboxCapabilities::UsbPassthrough));
        a->setEnabled(false);
        return;
    }

    const QList<UsbDeviceInfo> devices = UsbEnumerator::enumerate();
    if (devices.isEmpty()) {
        auto *a = target->addAction(i18n("No USB devices found"));
        a->setEnabled(false);
        return;
    }

    for (const auto &dev : devices) {
        const QString label = QStringLiteral("%1  [%2]").arg(dev.displayName(), dev.idString());
        QAction *action = target->addAction(label);
        action->setCheckable(true);
        action->setChecked(entry->attachedUsb.contains(dev.busKey()));
        action->setData(QVariantList{dev.busNum, dev.devAddr, dev.busKey(), dev.qemuId(), vmName}); // carry vmName
        connect(action, &QAction::triggered, this, &MainWindow::onUsbActionTriggered);
    }
}

MainWindow::ExitChoice MainWindow::askExitChoice()
{
    QStringList details;
    for (const QString &name : veModel->vmNames()) {
        const VmEntry *e = veModel->entryByName(name);
        if (!e || e->state != VmEntry::State::Running)
            continue;
        details << i18nc("@item VM name and what will happen to it", "%1 - %2", name, VMConfig::toUIString(e->config.exitAction()));
    }

    const auto answer = KMessageBox::warningTwoActionsCancelList(this,
                                                                 i18n("Virtual machines are still running. "
                                                                      "They will be handled as configured:"),
                                                                 details,
                                                                 i18nc("@title:window", "Confirm Exit"),
                                                                 KGuiItem(i18n("Apply and Quit"), u"application-exit"_s),
                                                                 KGuiItem(i18n("Quit Immediately"), u"process-stop"_s),
                                                                 KStandardGuiItem::cancel());

    switch (answer) {
    case KMessageBox::PrimaryAction:
        return ExitChoice::ApplyPolicy;
    case KMessageBox::SecondaryAction:
        return ExitChoice::QuitNow;
    default:
        return ExitChoice::Cancel;
    }
}

void MainWindow::updateSessionInhibitors()
{
    bool anyRunning = false;
    for (const QString &name : veModel->vmNames()) {
        const VmEntry *e = veModel->entryByName(name);
        if (e && (e->state == VmEntry::State::Running || e->state == VmEntry::State::Deploying))
            anyRunning = true;
    }
    m_sessionGuard->setShutdownBlocked(anyRunning, i18n("Virtual machines are running"));

    bool inhibitSaver = false;
    if (SettingsDialog::inhibitScreensaverAlways()) {
        inhibitSaver = !m_dbusViewers.isEmpty();
    } else {
        for (const auto &state : std::as_const(m_dbusViewers)) {
            if (!state.window)
                continue;
            if (SettingsDialog::inhibitScreensaverFullscreen() && state.window->isFullScreen())
                inhibitSaver = true;
            if (SettingsDialog::inhibitScreensaverPointerGrab() && state.window->displayWidget()->isPointerCaptured())
                inhibitSaver = true;
        }
    }
    m_sessionGuard->setScreensaverInhibited(inhibitSaver, i18n("A virtual machine is in use"));
}

void MainWindow::applyExitPolicy()
{
    for (const QString &name : veModel->vmNames()) {
        const VmEntry *e = veModel->entryByName(name);
        if (!e || e->state != VmEntry::State::Running)
            continue;

        switch (e->config.exitAction()) {
        case VMSpecs::ExitAction::SaveState:
            saveStateVMByName(name, true); // skipConfirm: already confirmed above
            break;
        case VMSpecs::ExitAction::Shutdown:
            shutdownVMByName(name);
            break;
        case VMSpecs::ExitAction::PowerOff:
            powerOffVMByName(name);
            break;
        }
    }

    // Poll until nothing is in flight, then close for real. Polling avoids
    // adding completion callbacks to three different teardown paths.
    auto *watch = new QTimer(this);
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + KEMUConst::kExitPolicyTimeoutMs;
    connect(watch, &QTimer::timeout, this, [this, watch, deadline]() {
        bool busy = false;
        for (const QString &name : veModel->vmNames()) {
            const VmEntry *e = veModel->entryByName(name);
            if (!e)
                continue;
            if (e->state == VmEntry::State::Running || e->state == VmEntry::State::Saving || e->state == VmEntry::State::ShuttingDown)
                busy = true;
        }
        if (busy && QDateTime::currentMSecsSinceEpoch() < deadline)
            return;

        watch->stop();
        watch->deleteLater();
        m_exitPolicyDone = true;
        close();
    });
    watch->start(500);
}

void MainWindow::scheduleQemuStartupProbe(KProcess *process, const QString &vmName)
{
    QPointer<KProcess> safeProcess(process);
    QTimer::singleShot(kQemuStartupProbeMs, this, [this, safeProcess, vmName]() {
        if (!safeProcess)
            return;
        if (safeProcess->state() == KProcess::Running)
            return; // Past the danger zone; handleQemuExit() owns any future exit.

        // If finished() has already been delivered, handleQemuExit() has run
        // and the entry no longer points at this process - nothing to do.
        const VmEntry *e = veModel->entryByName(vmName);
        if (!e || e->qemuProcess != safeProcess)
            return;

        m_startupFailureShown.insert(vmName);

        // Capture diagnostics BEFORE teardown clears the stderr tail.
        const int exitCode = safeProcess->exitCode();
        const auto exitStatus = safeProcess->exitStatus();
        const QByteArray tail = m_qemuStderrTails.value(vmName);

        // FailedToStart never emits finished(), so handleQemuExit() will
        // never run for this process. Clean up here instead.
        tearDownVmRuntime(vmName);
        veModel->setVmState(vmName, VmEntry::State::Stopped);
        safeProcess->deleteLater();

        showQemuFailureDialog(vmName, /*startupFailed=*/true, exitCode, exitStatus, tail);

        if (selectedVmName() == vmName) {
            updateDetails();
            updateActionStates();
        }
    });
}

QWidget *MainWindow::dialogParentFor(const QString &vmName)
{
    auto it = m_dbusViewers.constFind(vmName);
    if (it != m_dbusViewers.constEnd() && it->window && it->window->isVisible())
        return it->window;
    return this;
}

bool MainWindow::hasLiveQemu(const QString &vmName) const
{
    const VmEntry *entry = veModel->entryByName(vmName);
    return entry && (entry->state == VmEntry::State::Running || entry->state == VmEntry::State::Deploying);
}
