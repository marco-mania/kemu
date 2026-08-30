/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QCursor>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLocalSocket>
#include <QMap>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QSet>
#include <QSplitter>
#include <QStatusBar>
#include <QTextBrowser>
#include <QTextEdit>
#include <QToolBar>
#include <QToolTip>
#include <QTreeView>
#include <QVBoxLayout>

#include <KActionCollection>
#include <KActionMenu>
#include <KLocalizedString>
#include <KMessageBox>
#include <KProcess>
#include <KUiServerV2JobTracker>
#include <KXmlGuiWindow>

#include "dbusdisplay/dbusviewerwindow.h"
#include "dialogs/settingsdialog.h"
#include "jobs/createvmjob.h"
#include "models/virtualenvironmentmodel.h"
#include "runtime/vmruntimemanager.h"
#include "utils/externaltools.h"
#include "utils/imagedatabase.h"
#include "utils/qemucommandbuilder.h"
#include "utils/savestatemanager.h"
#include "utils/sessionguard.h"
#include "utils/virtualnetworkmanager.h"
#include "utils/vmconfig.h"
#include "widgets/performancemonitorwidget.h"
#include "widgets/virtualenvironmenttreeview.h"

using namespace Qt::StringLiterals;

class MainWindow : public KXmlGuiWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    // Set by the session-manager handler before close(): suppresses the
    // confirmation dialog, which nobody would see during logout.
    void setClosingFromSession(bool fromSession)
    {
        m_closingFromSession = fromSession;
    }

protected:
    bool queryClose() override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void openSettings();
    void openSettingsPage(SettingsDialog::Page page);
    void createNewVM();
    void editVM();
    void deleteVM();
    void cloneVM();
    void showCommand();
    void startVM(bool headless = false);
    void startVMHeadless();
    void shutdownVM();
    void resetVM();
    void powerOffVM();
    void saveStateVM();
    void discardSavedState();
    void changeISO();
    void ejectISO();
    void changeFloppy();
    void ejectFloppy();
    void shrinkDisk();
    void manageSnapshots();
    void exportVM();
    void importVM();
    void saveScreenshot();
    void openNetworkShell();
    void reconnectViewer();

    void onVmStartFailed(const QString &vmName, const QString &reason, int exitCode, KProcess::ExitStatus exitStatus, const QByteArray &stderrTail);
    void onVmExited(const VmRuntimeManager::ExitInfo &info);
    void onPerfSample(const QString &vmName, int channel, double value);

    void onTreeSelectionChanged();
    void onTreeDoubleClicked(const QModelIndex &index);
    void onDropCompleted();

    void showContextMenu(const QPoint &pos);

    void onVmSelectionChanged();

    void onQMPEvent(const QString &vmName, const QString &eventName, const QJsonObject &data);

    void onUsbMenuAboutToShow();
    void onUsbActionTriggered();

    void importOVA(const QString &filePath);
    void exportOVA(const QString &filePath);

    void onAboutToSleep();
    void onWokeUp();

private:
    void shutdownVMByName(const QString &vmName);
    void resetVMByName(const QString &vmName);
    void powerOffVMByName(const QString &vmName);
    void saveStateVMByName(const QString &vmName, bool skipConfirm = false);
    void discardSavedStateForVm(const QString &vmName);
    void manageSnapshotsForVm(const QString &vmName);
    void changeISOForVm(const QString &vmName);
    void ejectISOForVm(const QString &vmName);
    void changeFloppyForVm(const QString &vmName);
    void ejectFloppyForVm(const QString &vmName);
    void pushMediaStateToViewer(const QString &vmName);
    void pushNetworkStateToViewer(const QString &vmName);

    ImageDatabase m_imageDatabase;

    SaveStateManager m_saveStateManager;

    VirtualNetworkManager *m_networkManager = nullptr;

    void loadVmList();
    void setupUi();
    void setupActions();
    void updateDetails();
    void updateActionStates();

    VmRuntimeManager *m_runtimeManager = nullptr;
    // Adapts the manager's HelperResult to StartupChain's done(bool) and
    // shows the failure reason on the way through.
    VmRuntimeManager::HelperResult reportingDone(const QString &vmName, std::function<void(bool)> done);

    QString encodeThemeIcon(const QString &name, int sizePx) const;

    QStringList existingVmNames() const;

    void saveVmConfig(const QString &vmPath, const VMConfig &config);
    void generateShellScript(const QString &vmPath, const VMConfig &config);
    VMConfig loadVmConfig(const QString &vmPath) const;

    bool vmSupportsScreendump() const;

    // Empty when a save state is possible, otherwise the reason to show the
    // user. QEMU sets a migration blocker for these devices.
    [[nodiscard]] QString saveStateBlockReason(const VmEntry *entry) const;

    QDir getVmBaseDir() const;
    QDir getVmDir(const QString &vmName) const;
    [[nodiscard]] bool isManagedVmDir(const QDir &dir) const;

    // Returns false only when a tap was required but could not be opened.
    // tapFd stays -1 for every other network mode.
    bool openTapFdFor(const VMConfig &config, const QString &vmName, const QString &tapDevice, int *tapFd);
    void launchQemuProcess(const QString &vmName, bool headless, const QString &tapDevice);
    void launchQemuForDeployment(const QString &vmName, bool headless, const QString &tapDevice);

    // D-Bus integrated viewer lifecycle
    bool startDbusViewer(const QString &vmName);
    void stopDbusViewer(const QString &vmName);
    void finishDbusViewer(const QString &vmName, QemuControlConnection *ctrl);
    void registerAudioListeners(const QString &vmName);
    void cleanupDbusSocket(const QString &vmName);

    bool requireTool(const ExternalTools::Tool tool);

    VirtualEnvironmentTreeView *veTreeView = nullptr;
    VirtualEnvironmentModel *veModel = nullptr;

    QPointer<KActionMenu> usbActionMenu;

    // Folder handling
    void createFolder();
    void deleteFolder();
    void renameFolder();
    void selectVmByName(const QString &vmName);
    QString selectedVmName() const;
    bool isCurrentItemFolder() const;
    void saveFolderStructure() const;
    QMap<QString, QString> loadFolderAssignments() const;

    ViewerDevicePrefs loadViewerPrefs(const QString &vmName) const;
    void saveViewerPrefs(const QString &vmName, const ViewerDevicePrefs &prefs) const;
    void removeViewerPrefs(const QString &vmName) const;

    QMap<QString, QDir> m_lastMediaDir;

    // QEMU failure handling, keyed by VM name
    void showQemuFailureDialog(const QString &vmName, bool startupFailed, int exitCode, KProcess::ExitStatus exitStatus, const QByteArray &stderrTail);

    QTextBrowser *detailsBrowser = nullptr;
    PerformanceMonitorWidget *perfMonitor = nullptr;
    QToolBar *vmActionBar = nullptr;

    QList<QPointer<CreateVmJob>> creatingVMs;
    QList<QPointer<KJob>> activeImports;

    void startDeployment(const QString &vmName);
    void finalizeDeployment(const QString &vmName);
    void abortDeployment(const QString &vmName, bool deleteVm);

    void deployCloudImage(const QString &vmName);

    void populateUsbMenuFor(const QString &vmName, QMenu *target);

    KUiServerV2JobTracker *jobTracker = nullptr;

    VMConfig currentConfig;

    static constexpr int kViewerConnectMaxAttempts = 20;
    static constexpr int kViewerConnectRetryMs = 250;
    static constexpr int kViewerHandshakeTimeoutMs = 5000;

    enum class ExitChoice {
        Cancel,
        ApplyPolicy,
        QuitNow
    };
    ExitChoice askExitChoice();

    void updateSessionInhibitors();
    void applyExitPolicy();

    SessionGuard *m_sessionGuard = nullptr;
    bool m_suspendInProgress = false;
    bool m_exitPolicyDone = false;
    bool m_closingFromSession = false;

    QWidget *dialogParentFor(const QString &vmName);
};
