/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimeZone>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KConfig>
#include <KConfigGroup>
#include <KLocalizedString>
#include <KMessageBox>
#include <KPageDialog>
#include <KPageWidgetItem>
#include <KSharedConfig>
#include <KUser>

#include "utils/cloudimagedeployer.h"
#include "utils/externaltools.h"
#include "utils/virtualnetworkmanager.h"

using namespace Qt::StringLiterals;

class SettingsDialog : public KPageDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget *parent = nullptr);

    enum Page {
        General,
        Firmware,
        Executables,
        Monitoring,
        Display,
        AutomatedDeploy,
        VirtualNetworks
    };
    void setPage(Page page);

    static QString getVmBasePath();
    static QString getOvmfPath();
    static bool isPerformanceMonitorEnabled();
    static QSize getDefaultResolution();
    static bool showAllArchitectures();
    static bool cloneRegenerateMac();
    static bool importRegenerateMac();

    static bool inhibitScreensaverAlways();
    static bool inhibitScreensaverFullscreen();
    static bool inhibitScreensaverPointerGrab();

    // Guest defaults for a new deployment. hostname, templateId and
    // networkTemplateId are left empty; the caller fills them in.
    [[nodiscard]] static CloudDeployConfig deploymentDefaults();

    void setNetworkManager(VirtualNetworkManager *mgr);

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

    QMap<Page, KPageWidgetItem *> m_pages;

    QLineEdit *pathEdit = nullptr;
    QLineEdit *ovmfEdit = nullptr;

    struct ToolWidgets {
        ExternalTools::Tool tool;
        QLineEdit *pathEdit = nullptr;
        QLabel *statusLabel = nullptr;
    };
    QList<ToolWidgets> m_toolWidgets;
    void updateToolVersion(int index);

    QLineEdit *deployUserEdit = nullptr;
    QPlainTextEdit *deploySshKeysEdit = nullptr;
    QCheckBox *deployPasswordLoginCheck = nullptr;
    QComboBox *deployTimezoneCombo = nullptr;
    QComboBox *deployLocaleCombo = nullptr;
    QComboBox *deployKbCombo = nullptr;
    QCheckBox *deployInstallDesktopCheck = nullptr;

    static QStringList detectHostSshKeys();

    QCheckBox *perfMonitorCheck = nullptr;
    QComboBox *resolutionCombo = nullptr;

    QCheckBox *ssAlwaysCheck = nullptr;
    QCheckBox *ssFullscreenCheck = nullptr;
    QCheckBox *ssGrabCheck = nullptr;

    QCheckBox *allArchCheck = nullptr;
    QCheckBox *cloneRegenerateMacCheck = nullptr;
    QCheckBox *importRegenerateMacCheck = nullptr;

    VirtualNetworkManager *m_networkManager = nullptr;
    QTableWidget *networkTable = nullptr;
    QPushButton *netAddButton = nullptr;
    QPushButton *netEditButton = nullptr;
    QPushButton *netRemoveButton = nullptr;
};
