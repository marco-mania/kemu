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
#include <KSharedConfig>
#include <KUiServerV2JobTracker>
#include <KXmlGuiWindow>

#include "dbusdisplay/dbusviewerwindow.h"
#include "dialogs/deploymentlogwindow.h"
#include "dialogs/hostshutdowndialog.h"
#include "dialogs/settingsdialog.h"
#include "jobs/clouddeployjob.h"
#include "jobs/createvmjob.h"
#include "jobs/diskimportjob.h"
#include "models/virtualenvironmentmodel.h"
#include "runtime/vmruntimemanager.h"
#include "utils/externaltools.h"
#include "utils/imagedatabase.h"
#include "utils/qemuimgprocess.h"
#include "utils/savestatemanager.h"
#include "utils/sessionguard.h"
#include "utils/virtualnetworkmanager.h"
#include "utils/vmconfig.h"
#include "widgets/performancemonitorwidget.h"
#include "widgets/virtualenvironmenttreeview.h"

#include <functional>
#include <optional>

using namespace Qt::StringLiterals;

class DiskKeyStore;
class EditVmDialog;
class EncryptDiskJob;
class ShrinkJob;

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
    void makeIndependent();
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
    void reconnectViewer();

    void onVmStarted(const QString &vmName, bool headless, bool deployment);
    void onVmStartFailed(const QString &vmName, const QString &reason, int exitCode, KProcess::ExitStatus exitStatus, const QByteArray &stderrTail);
    void onVmExited(const VmRuntimeManager::ExitInfo &info);
    void onPerfSample(const QString &vmName, int channel, double value);

    void onTreeSelectionChanged();
    void onTreeDoubleClicked(const QModelIndex &index);
    void onDropCompleted();

    void showContextMenu(const QPoint &pos);

    void onVmSelectionChanged();

    void onQMPEvent(const QString &vmName, const QString &eventName, const QJsonObject &data);
    void onQmpReady(const QString &vmName);

    void onUsbMenuAboutToShow();
    void onUsbActionTriggered();

    void importOVA(const QString &filePath);
    void exportOVA(const QString &filePath);
    void importDisks(const QString &vmName, const QList<DiskImportJob::Item> &items);

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
    void activatePerfMonitorFor(const QString &vmName);

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

    // Empty when the VM may be started or changed. A frozen source of linked
    // clones and a clone that lost its source may not.
    [[nodiscard]] QString linkBlockReason(const VmEntry *entry) const;
    // Empty when a linked clone of the VM can be made.
    [[nodiscard]] QString linkedCloneBlockReason(const VmEntry *entry) const;

    // Encrypted disks: the password from the wallet, or asked with @p prompt
    // (and checked). nullopt if the user cancelled. Immediately {} for a VM
    // without encryption. @p offerStore shows "Store the password in KWallet".
    void requestDiskKey(const QString &vmName, const QString &prompt, std::function<void(std::optional<QByteArray> key)> done, bool offerStore = true);
    void continueStartVM(bool headless, const QByteArray &diskKey);
    // Encryption actions of the VM settings; @p dialog is refreshed when they are done.
    void encryptSystemDisk(const QString &vmName, EditVmDialog *dialog);
    void decryptSystemDisk(const QString &vmName, EditVmDialog *dialog);
    // Shows the progress over @p dialog; @p succeeded runs before warnings are shown.
    void runEncryptionJob(EncryptDiskJob *job, EditVmDialog *dialog, std::function<void()> succeeded);
    void changeDiskPassword(const QString &vmName, EditVmDialog *dialog);
    void storeDiskPassword(const QString &vmName, EditVmDialog *dialog);
    void forgetDiskPassword(const QString &vmName, EditVmDialog *dialog);
    // Stores in KWallet and tells the user (over @p parent) if that failed.
    void storeDiskKeyInWallet(const QString &vmName, const QString &keyId, const QString &password, QWidget *parent, std::function<void(bool ok)> done = {});
    void continueDeployment(const QString &vmName, const QByteArray &diskKey);
    // True if QEMU failed on the disk password; the user was told.
    bool handleWrongDiskPassword(const QString &vmName, const QByteArray &stderrTail);
    DiskKeyStore *m_diskKeyStore = nullptr;

    QDir getVmBaseDir() const;
    QDir getVmDir(const QString &vmName) const;
    [[nodiscard]] bool isManagedVmDir(const QDir &dir) const;

    // D-Bus integrated viewer lifecycle
    bool startDbusViewer(const QString &vmName);
    void stopDbusViewer(const QString &vmName);
    void finishDbusViewer(const QString &vmName, QemuControlConnection *ctrl);
    void registerAudioListeners(const QString &vmName);

    bool requireTool(const ExternalTools::Tool tool);
    // Through the guest agent if it answers, otherwise ACPI; the error, if any.
    QString requestGuestShutdown(const QString &vmName, QMPClient *client);
    // swtpm and swtpm_ioctl, if @p vmName has a TPM state to move to a new password.
    // Rewrites @p diskPath of @p vmName; asks first, over @p parent.
    // After the clone dialog; @p diskKey opens an encrypted system disk.
    void startClone(const QString &srcName, const QUuid &srcUuid, const QString &srcPath, const QString &dstPath, bool linked, const QByteArray &diskKey);
    void compactDisk(const QString &vmName, const QString &diskPath, QWidget *parent);
    void runCompactJob(ShrinkJob *job, QWidget *parent);
    // Grows @p diskPath to @p sizeGb after asking over @p parent; tells
    // @p dialog when done.
    void expandDisk(const QString &vmName, const QString &diskPath, int sizeGb, EditVmDialog *dialog, QWidget *parent);
    bool requireTpmRekeyTools(const QString &vmName, QWidget *parent);
    /// Creates the VM's UEFI variable store if needed and reports problems.
    /// False if the VM cannot boot with UEFI.
    bool requireUefiFirmware(const QString &vmPath, const VMConfig &config, QWidget *parent);

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
    QStringList loadFolderNames() const;

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
    void confirmDeploymentAbort(const QString &vmName);
    void shutdownDeployment(const QString &vmName);
    void abortDeployment(const QString &vmName, bool deleteVm);
    void finishDeploymentAbort(const QString &vmName, bool deleteVm);
    void showDeploymentLog(const QString &vmName, bool allowMissing = false);
    [[nodiscard]] QString deploymentLogPath(const QString &vmName) const;

    // Cloud-image runs configure themselves; an ISO install needs the user.
    [[nodiscard]] bool isUnattendedDeployment(const QString &vmName) const;

    // At most one log window per VM. QPointer clears the entry by itself when
    // the user closes the window (WA_DeleteOnClose).
    QMap<QString, QPointer<DeploymentLogWindow>> m_deploymentLogWindows;

    // Deployments whose QEMU process has been killed or asked to shut down and
    // whose cleanup is waiting for the exit signal. Value = delete the VM
    // directory as well.
    // Doubles as a reentrancy guard for abortDeployment().
    QMap<QString, bool> m_pendingDeploymentAbort;

    void deployCloudImage(const QString &vmName);

    // Cloud deployments whose image preparation is still running in a worker
    // thread, keyed by VM name.
    QMap<QString, QPointer<CloudDeployJob>> m_cloudDeployJobs;

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

    void setShutdownHostWithGuest(const QString &vmName, bool armed);
    void pushHostShutdownStateToViewer(const QString &vmName);
    [[nodiscard]] QStringList busyVmNames() const;
    void offerHostShutdown(const QString &vmName);
    QPointer<HostShutdownDialog> m_hostShutdownDialog;

    SessionGuard *m_sessionGuard = nullptr;
    bool m_suspendInProgress = false;
    bool m_exitPolicyDone = false;
    bool m_closingFromSession = false;

    QWidget *dialogParentFor(const QString &vmName);

    // qemu-img info is a fork per call. Keyed by the image's write time and
    // size, so it re-reads exactly when the image has actually changed.
    struct DiskInfoCacheEntry {
        QDateTime lastModified;
        qint64 fileSize = 0;
        QemuImgProcess::ImageInfo info;
    };
    mutable QHash<QString, DiskInfoCacheEntry> m_diskInfoCache;
    [[nodiscard]] QemuImgProcess::ImageInfo diskInfoFor(const QString &diskPath) const;
};
