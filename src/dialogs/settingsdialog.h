/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPageDialog>
#include <KPageWidgetItem>

#include "utils/externaltools.h"
#include "utils/virtualnetworkmanager.h"

using namespace Qt::StringLiterals;

class SettingsDialog : public KPageDialog {

    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);

    enum Page { General,
        Firmware,
        Executables,
        Monitoring,
        Display,
        AutomatedDeploy,
        VirtualNetworks };
    void setPage(Page page);

    enum SpiceViewerCloseAction { SwitchToHeadless,
        ShutdownVm };

    static QString getVmBasePath();
    static QString getOvmfPath();
    static bool isPerformanceMonitorEnabled();
    static QSize getDefaultResolution();
    static bool enableDbusDisplay();
    static bool showAllArchitectures();
    static bool cloneRegenerateMac();
    static bool importRegenerateMac();
    static bool deployHeadless();
    static bool deploySerialLog();
    static SpiceViewerCloseAction spiceViewerCloseAction();

    void setNetworkManager(VirtualNetworkManager* mgr);

private Q_SLOTS:
    void browseVmDir();
    void browseOvmfDir();

    void saveSettings();

    void addNetwork();
    void editNetwork();
    void removeNetwork();
    void onNetworkSelectionChanged();

private:
    void setupGeneralPage();
    void setupFirmwarePage();
    void setupExecutablesPage();
    void setupMonitoringPage();
    void setupDisplayPage();
    void setupAutomatedDeployPage();

    void setupVirtualNetworksPage();
    void refreshNetworkTable();

    QMap<Page, KPageWidgetItem*> m_pages;

    QPointer<QLineEdit> pathEdit;
    QPointer<QLineEdit> ovmfEdit;

    struct ToolWidgets {
        ExternalTools::Tool tool;
        QPointer<QLineEdit> pathEdit;
        QPointer<QLabel> statusLabel;
    };
    QList<ToolWidgets> m_toolWidgets;
    void updateToolVersion(int index);

    QPointer<QCheckBox> deployHeadlessCheck;
    QPointer<QCheckBox> deploySerialLogCheck;
    QPointer<QComboBox> spiceCloseActionCombo;

    QPointer<QCheckBox> perfMonitorCheck;
    QPointer<QComboBox> resolutionCombo;
    QPointer<QCheckBox> dbusDisplayCheck;

    QPointer<QCheckBox> allArchCheck;
    QPointer<QCheckBox> cloneRegenerateMacCheck;
    QPointer<QCheckBox> importRegenerateMacCheck;

    VirtualNetworkManager* m_networkManager = nullptr;
    QPointer<QTableWidget> networkTable;
    QPointer<QPushButton> netAddButton;
    QPointer<QPushButton> netEditButton;
    QPointer<QPushButton> netRemoveButton;
};
