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
#include <QDebug>
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

#include "dialogs/settingsdialog.h"
#include "jobs/createvmjob.h"
#include "models/vmmodel.h"
#include "utils/cloudimagedeployer.h"
#include "utils/externaltools.h"
#include "utils/imagedatabase.h"
#include "utils/virtualnetworkmanager.h"
#include "utils/vmconfig.h"
#include "widgets/performancemonitorwidget.h"
#include "widgets/vmtreeview.h"

using namespace Qt::StringLiterals;

struct DiskInfo {
    double virtualSizeGb = 0.0;
    double actualSizeGb = 0.0;
    bool isValid = false;
};

class MainWindow : public KXmlGuiWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    bool queryClose() override;

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
    void killVM();
    void changeISO();
    void ejectISO();
    void shrinkDisk();
    void manageSnapshots();
    void exportVM();
    void importVM();
    void saveScreenshot();
    void openNetworkShell();
    void reconnectSpiceViewer();

    void onTreeSelectionChanged();
    void onTreeDoubleClicked(const QModelIndex& index);
    void onDropCompleted();

    void showContextMenu(const QPoint& pos);

    void onVmSelectionChanged();
    void onProcessFinished(int exitCode, KProcess::ExitStatus exitStatus);

    void onQMPEvent(const QString& vmName, const QString& eventName, const QJsonObject& data);

    void onUsbMenuAboutToShow();
    void onUsbActionTriggered();

    void importOVA(const QString& filePath);
    void exportOVA(const QString& filePath);

private:
    ImageDatabase m_imageDatabase;

    VirtualNetworkManager* m_networkManager = nullptr;

    void loadVmList();
    void setupUi();
    void setupActions();
    void updateDetails();
    void updateActionStates();

    QStringList existingVmNames() const;
    QStringList usedMacAddresses() const;
    QList<MacUsageInfo> macUsageInfo() const;

    void saveVmConfig(const QString& vmPath, const VMConfig& config);
    void generateShellScript(const QString& vmPath, const VMConfig& config);
    VMConfig loadVmConfig(const QString& vmPath) const;

    DiskInfo getDiskInfo(const QString& diskPath);
    bool vmSupportsScreendump() const;

    QString ensureNetworkRunning(const QString& networkName, const QString& vmName);
    void releaseNetwork(const VMConfig& config);

    QDir getVmBaseDir() const;
    QDir getVmDir(const QString& vmName) const;
    QString monitorSocketPath(const QString& vmName) const;

    bool startSwtpm(const QString& vmName);
    bool startVirtiofsd(const QString& vmName);
    bool startPasst(const QString& vmName, const QList<PortForwardRule>& portForwards = QList<PortForwardRule>());
    bool startSpiceViewer(const QString& vmName);

    bool requireTool(ExternalTools::Tool tool);

    QPointer<VMTreeView> vmTreeView;
    QPointer<VmModel> vmModel;

    QPointer<KActionMenu> usbActionMenu;

    // Folder handling
    void createFolder();
    void deleteFolder();
    void renameFolder();
    void selectVmByName(const QString& vmName);
    QString selectedVmName() const;
    bool isCurrentItemFolder() const;
    void saveFolderStructure() const;
    QMap<QString, QString> loadFolderAssignments() const;

    QPointer<QTextBrowser> detailsBrowser;
    QPointer<PerformanceMonitorWidget> perfMonitor;
    QPointer<QToolBar> vmActionBar;

    QList<CreateVmJob*> creatingVMs;
    QList<KJob*> activeImports;

    // Deployment handling
    struct PendingDeployment {
        enum class Type { CloudImage,
            ManualIso };
        Type type = Type::ManualIso;
        QString imageUrl;
        QString imageLocalPath;
        CloudDeployConfig cloudConfig;
    };
    QMap<QString, PendingDeployment> m_pendingDeployments;
    void startDeployment(const QString& vmName, const bool disable_headless = false);
    void finalizeDeployment(const QString& vmName);
    void abortDeployment(const QString& vmName, bool deleteVm);

    void deployCloudImage(const QString& vmName);

    QPointer<KUiServerV2JobTracker> jobTracker;

    VMConfig currentConfig;
};
