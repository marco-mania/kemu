/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settingsdialog.h"

#include <algorithm>

#include "const.h"
#include "dialogs/templatessettingspage.h"
#include "utils/guestlocales.h"
#include "utils/qemucapabilities.h"
#include "utils/sandboxcapabilities.h"
#include "utils/utils.h"
#include "utils/vmconfig.h"

SettingsDialog::SettingsDialog(QWidget *parent)
    : KPageDialog(parent)
{
    setWindowTitle(i18n("Settings"));
    setFaceType(KPageDialog::List);
    resize(750, 500);

    setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    button(QDialogButtonBox::Ok)->setText(i18n("Save"));
    button(QDialogButtonBox::Ok)->setIcon(QIcon::fromTheme(u"document-save"_s));
    connect(button(QDialogButtonBox::Ok), &QPushButton::clicked, this, &SettingsDialog::saveSettings);

    setupGeneralPage();
    setupFirmwarePage();
    setupExecutablesPage();
    setupMonitoringPage();
    setupDisplayPage();
    setupAutomatedDeployPage();
    setupTemplatesPage();
    setupVirtualNetworksPage();
}

void SettingsDialog::setPage(Page page)
{
    if (m_pages.contains(page))
        setCurrentPage(m_pages.value(page));
}

void SettingsDialog::setupGeneralPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(new QLabel(i18n("Storage location for virtual machines:")));

    auto *row = new QHBoxLayout;
    pathEdit = new QLineEdit;
    pathEdit->setText(getVmBasePath());
    auto *btn = new QPushButton(QIcon::fromTheme(u"document-open"_s), {});
    connect(btn, &QPushButton::clicked, this, &SettingsDialog::browseVmDir);
    // Shown inside the field, like a clear button, while another folder is set.
    QAction *resetAction = pathEdit->addAction(QIcon::fromTheme(u"edit-undo"_s), QLineEdit::TrailingPosition);
    resetAction->setToolTip(i18n("Reset to the default location (%1)", defaultVmBasePath()));
    connect(resetAction, &QAction::triggered, this, [this]() {
        pathEdit->setText(defaultVmBasePath());
    });
    const auto updateResetAction = [this, resetAction]() {
        resetAction->setVisible(QDir::cleanPath(pathEdit->text().trimmed()) != defaultVmBasePath());
    };
    connect(pathEdit, &QLineEdit::textChanged, this, updateResetAction);
    updateResetAction();
    row->addWidget(pathEdit);
    row->addWidget(btn);
    layout->addLayout(row);

    auto *line = new QFrame(this);
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);

    layout->addWidget(line);

    cloneRegenerateMacCheck = new QCheckBox(i18n("Regenerate MAC address when cloning a VM"));
    cloneRegenerateMacCheck->setToolTip(
        i18n("When <b>enabled</b>, a cloned VM always receives a new random MAC "
             "address. "
             "If the source VM has an explicit MAC, a dialog lets you choose a "
             "replacement. When <b>disabled</b>, the MAC is copied as-is."));
    cloneRegenerateMacCheck->setChecked(cloneRegenerateMac());
    layout->addWidget(cloneRegenerateMacCheck);

    importRegenerateMacCheck = new QCheckBox(i18n("Regenerate MAC address when importing a VM"));
    importRegenerateMacCheck->setToolTip(
        i18n("When <b>enabled</b>, imported VMs always receive a new MAC address. "
             "This prevents address conflicts when running the original and "
             "the imported copy on the same network.\n"
             "When <b>disabled</b>, the original MAC address is preserved, which "
             "maintains DHCP leases and static IP assignments."));
    importRegenerateMacCheck->setChecked(importRegenerateMac());
    layout->addWidget(importRegenerateMacCheck);

    auto *sandboxLine = new QFrame(this);
    sandboxLine->setFrameShape(QFrame::HLine);
    sandboxLine->setFrameShadow(QFrame::Sunken);
    layout->addWidget(sandboxLine);

    seccompSandboxCheck = new QCheckBox(i18n("Confine QEMU with its seccomp sandbox"));
    seccompSandboxCheck->setToolTip(
        i18n("When <b>enabled</b>, QEMU is started with "
             "<tt>-sandbox on,obsolete=deny,elevateprivileges=deny,spawn=deny,"
             "resourcecontrol=deny</tt>. The seccomp filter limits what a guest "
             "could reach if it broke out of QEMU: it may neither start other "
             "programs nor change its privileges.\n"
             "The filter applies to QEMU itself, so a feature that needs one of "
             "those calls kills QEMU with SIGSYS, without a message on why. That "
             "makes a sandboxed QEMU harder to diagnose.\n"
             "When <b>disabled</b> (the default), QEMU runs unconfined. A VM whose "
             "extra arguments carry their own <tt>-sandbox</tt> is never changed by "
             "this setting."));
    seccompSandboxCheck->setChecked(qemuSeccompSandbox());

    // A -sandbox on a QEMU built without seccomp is fatal, so do not offer it.
    if (!QemuCapabilities::instance().hasSeccompSandbox()) {
        seccompSandboxCheck->setEnabled(false);
        seccompSandboxCheck->setChecked(false);
        seccompSandboxCheck->setToolTip(QemuCapabilities::instance().probeSucceeded()
                                            ? i18n("The QEMU binary in use was built without seccomp support.")
                                            : i18n("KEMU could not probe the QEMU binary in use for seccomp support."));
    }
    layout->addWidget(seccompSandboxCheck);

    layout->addStretch();

    auto *item = addPage(page, i18n("Virtual Machines"));
    item->setIcon(QIcon::fromTheme(u"computer"_s));
    m_pages[General] = item;
}

void SettingsDialog::setupFirmwarePage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    layout->addWidget(new QLabel(i18n("Custom firmware directory (leave empty for auto-detect):")));

    auto *row = new QHBoxLayout;
    ovmfEdit = new QLineEdit;
    ovmfEdit->setText(getOvmfPath());
    auto *btn = new QPushButton(QIcon::fromTheme(u"document-open"_s), {});
    connect(btn, &QPushButton::clicked, this, &SettingsDialog::browseOvmfDir);
    row->addWidget(ovmfEdit);
    row->addWidget(btn);
    layout->addLayout(row);

    layout->addStretch();

    auto *item = addPage(page, i18n("UEFI Firmware"));
    item->setIcon(QIcon::fromTheme(u"hwinfo"_s));
    m_pages[Firmware] = item;
}

void SettingsDialog::setupExecutablesPage()
{
    const bool editablePaths = SandboxCapabilities::instance().has(SandboxCapabilities::ExternalToolPaths);

    auto *page = new QWidget;
    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(4);

    auto *refreshLayout = new QHBoxLayout;
    auto *refreshBtn = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18n("Re-detect All"));
    refreshBtn->setEnabled(editablePaths);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        ExternalTools::instance().refresh();
        QemuCapabilities::instance().refresh();
        for (int i = 0; i < m_toolWidgets.size(); ++i) {
            const QString detected = ExternalTools::instance().detectedPath(m_toolWidgets[i].tool);
            m_toolWidgets[i].pathEdit->setPlaceholderText(detected.isEmpty() ? i18n("Not found - enter a path manually") : detected);
            updateToolVersion(i);
        }
    });
    refreshLayout->addWidget(refreshBtn);
    refreshLayout->addStretch();
    layout->addLayout(refreshLayout);

    const auto &tools = ExternalTools::instance();

    for (const ExternalTools::Tool tool : ExternalTools::allTools()) {
        const QString binary = tools.defaultBinary(tool);
        const QString description = ExternalTools::displayName(tool);
        const bool unused = ExternalTools::instance().unusedTools().contains(tool);

        auto *headerLayout = new QHBoxLayout;
        headerLayout->setContentsMargins(0, 8, 0, 0);
        auto *binaryLabel = new QLabel(u"<b>%1</b>"_s.arg(binary));
        binaryLabel->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        headerLayout->addWidget(binaryLabel);
        const QString midColor = palette().color(QPalette::Mid).name();
        auto *descLabel = new QLabel(u"<font color='%1'> (%2)</font>"_s.arg(midColor, description));
        headerLayout->addWidget(descLabel);
        headerLayout->addStretch();
        layout->addLayout(headerLayout);

        auto *pathRow = new QHBoxLayout;
        auto *pathEdit = new QLineEdit;
        const QString detected = tools.detectedPath(tool);
        pathEdit->setText(tools.configuredPath(tool));
        pathEdit->setPlaceholderText(detected.isEmpty() ? i18n("Not found - enter a path manually") : detected);
        pathRow->addWidget(pathEdit);
        auto *browseBtn = new QPushButton(QIcon::fromTheme(u"document-open"_s), {});
        pathRow->addWidget(browseBtn);
        layout->addLayout(pathRow);

        auto *statusLabel = new QLabel;
        layout->addWidget(statusLabel);

        pathEdit->setReadOnly(!editablePaths);
        browseBtn->setEnabled(editablePaths);
        if (unused)
            descLabel->setText(u"<font color='%1'> (%2 - %3)</font>"_s.arg(midColor, description, i18n("not used by this build")));

        ToolWidgets tw;
        tw.tool = tool;
        tw.pathEdit = pathEdit;
        tw.statusLabel = statusLabel;
        m_toolWidgets.append(tw);

        const int idx = m_toolWidgets.size() - 1;

        connect(browseBtn, &QPushButton::clicked, this, [this, idx]() {
            const auto &tw = m_toolWidgets[idx];
            const QString file = QFileDialog::getOpenFileName(this, i18n("Select Executable"), tw.pathEdit->text());
            if (!file.isEmpty())
                tw.pathEdit->setText(file);
        });

        connect(pathEdit, &QLineEdit::textChanged, this, [this, idx]() {
            updateToolVersion(idx);
        });
    }

    // Suppressed-warnings section
    auto *suppressedBox = new QGroupBox(i18n("Suppressed Startup Warnings"));
    auto *suppressedLayout = new QVBoxLayout(suppressedBox);

    auto *suppressedLabel = new QLabel;
    suppressedLabel->setWordWrap(true);
    suppressedLayout->addWidget(suppressedLabel);

    auto *resetBtn = new QPushButton(QIcon::fromTheme(u"edit-undo"_s), i18n("Reset All Suppressed Warnings"));
    auto *btnRow = new QHBoxLayout;
    btnRow->addWidget(resetBtn);
    btnRow->addStretch();
    suppressedLayout->addLayout(btnRow);

    auto refresh = [suppressedLabel, resetBtn]() {
        const auto ids = ExternalTools::suppressedFeatureGroups();
        if (ids.isEmpty()) {
            suppressedLabel->setText(i18n("No warnings are currently suppressed."));
            resetBtn->setEnabled(false);
        } else {
            QStringList names;
            for (const auto &g : ExternalTools::featureGroups()) {
                if (ids.contains(g.id))
                    names << g.name;
            }
            suppressedLabel->setText(i18n("Currently suppressed: %1", names.join(u", "_s)));
            resetBtn->setEnabled(true);
        }
    };
    refresh();

    connect(resetBtn, &QPushButton::clicked, this, [refresh]() {
        ExternalTools::resetAllSuppressions();
        refresh();
    });

    layout->addWidget(suppressedBox);

    layout->addStretch();
    scrollArea->setWidget(page);

    auto *wrapperWidget = new QWidget;
    auto *wrapperLayout = new QVBoxLayout(wrapperWidget);
    wrapperLayout->setContentsMargins(0, 0, 0, 0);
    wrapperLayout->addWidget(scrollArea);

    auto *item = addPage(wrapperWidget, i18n("Executables"));
    item->setIcon(QIcon::fromTheme(u"application-x-executable"_s));
    m_pages[Executables] = item;

    for (int i = 0; i < m_toolWidgets.size(); ++i)
        updateToolVersion(i);
}

void SettingsDialog::setupMonitoringPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    perfMonitorCheck = new QCheckBox(i18n("Show performance monitor (CPU, RAM, I/O)"));
    perfMonitorCheck->setChecked(isPerformanceMonitorEnabled());
    layout->addWidget(perfMonitorCheck);

    layout->addStretch();

    auto *item = addPage(page, i18n("Monitoring"));
    item->setIcon(QIcon::fromTheme(u"utilities-system-monitor"_s));
    m_pages[Monitoring] = item;
}

void SettingsDialog::setupDisplayPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    // --- Defaults ---
    auto *defaultsGroup = new QGroupBox(i18n("Defaults"));
    auto *defaultsLayout = new QFormLayout(defaultsGroup);

    resolutionCombo = new QComboBox;
    resolutionCombo->addItem(u"1280 × 800"_s, QSize(1280, 800));
    resolutionCombo->addItem(u"1280 × 720 (HD)"_s, QSize(1280, 720));
    resolutionCombo->addItem(u"1366 × 768"_s, QSize(1366, 768));
    resolutionCombo->addItem(u"1440 × 900"_s, QSize(1440, 900));
    resolutionCombo->addItem(u"1600 × 900"_s, QSize(1600, 900));
    resolutionCombo->addItem(u"1920 × 1080 (Full HD)"_s, QSize(1920, 1080));
    resolutionCombo->addItem(u"2560 × 1440 (QHD)"_s, QSize(2560, 1440));
    resolutionCombo->addItem(u"3840 × 2160 (4K)"_s, QSize(3840, 2160));

    const QSize currentRes = getDefaultResolution();
    for (int i = 0; i < resolutionCombo->count(); ++i) {
        if (resolutionCombo->itemData(i).toSize() == currentRes) {
            resolutionCombo->setCurrentIndex(i);
            break;
        }
    }
    // Fall back to the first entry when the stored size is not offered
    if (resolutionCombo->currentIndex() < 0)
        resolutionCombo->setCurrentIndex(0);

    defaultsLayout->addRow(i18n("Default resolution for new VMs:"), resolutionCombo);
    layout->addWidget(defaultsGroup);

    // --- Screen locking ---
    auto *lockGroup = new QGroupBox(i18n("Screen Locking"));
    auto *lockLayout = new QVBoxLayout(lockGroup);

    auto *lockHint = new QLabel(i18n("Keep the screen from locking or blanking while you are working in a guest."));
    lockHint->setWordWrap(true);
    lockLayout->addWidget(lockHint);

    ssAlwaysCheck = new QCheckBox(i18n("Whenever a viewer window is open"));
    ssAlwaysCheck->setToolTip(i18n("Includes windowed viewers you are not currently interacting with."));
    ssAlwaysCheck->setChecked(inhibitScreensaverAlways());
    lockLayout->addWidget(ssAlwaysCheck);

    ssFullscreenCheck = new QCheckBox(i18n("While a viewer is fullscreen"));
    ssFullscreenCheck->setChecked(inhibitScreensaverFullscreen());
    lockLayout->addWidget(ssFullscreenCheck);

    ssGrabCheck = new QCheckBox(i18n("While the pointer is captured by a guest"));
    ssGrabCheck->setChecked(inhibitScreensaverPointerGrab());
    lockLayout->addWidget(ssGrabCheck);

    // "Always" subsumes the other two.
    auto syncLockChecks = [this](bool always) {
        ssFullscreenCheck->setEnabled(!always);
        ssGrabCheck->setEnabled(!always);
    };
    connect(ssAlwaysCheck, &QCheckBox::toggled, this, syncLockChecks);
    syncLockChecks(ssAlwaysCheck->isChecked());

    layout->addWidget(lockGroup);
    layout->addStretch();

    auto *item = addPage(page, i18n("Display"));
    item->setIcon(QIcon::fromTheme(u"preferences-desktop-display"_s));
    m_pages[Display] = item;
}

void SettingsDialog::setupAutomatedDeployPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    // --- Behaviour ---
    auto *behaviourGroup = new QGroupBox(i18n("Behaviour"));
    auto *behaviourLayout = new QFormLayout(behaviourGroup);

    auto *archInfoLabel = new QLabel(i18n("Detected host architecture: <b>%1</b>", QSysInfo::currentCpuArchitecture()));
    behaviourLayout->addRow(archInfoLabel);

    allArchCheck = new QCheckBox(i18n("Show all CPU architectures in edition list"));
    allArchCheck->setToolTip(
        i18n("By default only editions matching your host architecture are shown. "
             "Enable this to see all architectures (note: non-native architectures "
             "use slow TCG emulation instead of KVM)."));
    allArchCheck->setChecked(showAllArchitectures());
    behaviourLayout->addRow(allArchCheck);

    layout->addWidget(behaviourGroup);

    // --- Guest Defaults ---
    auto *guestGroup = new QGroupBox(i18n("Guest Defaults"));
    auto *guestLayout = new QFormLayout(guestGroup);

    const CloudDeployConfig defaults = deploymentDefaults();

    deployUserEdit = new QLineEdit(defaults.username);
    deployUserEdit->setToolTip(i18n("Login name created inside new guests."));
    guestLayout->addRow(i18n("User name:"), deployUserEdit);

    deploySshKeysEdit = new QPlainTextEdit();
    deploySshKeysEdit->setPlaceholderText(i18n("One key per line (ssh-ed25519 AAAA...)"));
    deploySshKeysEdit->setMaximumHeight(80);
    deploySshKeysEdit->setPlainText(defaults.sshKeys.join(u"\n"_s));
    guestLayout->addRow(i18n("SSH keys:"), deploySshKeysEdit);

    deployPasswordLoginCheck = new QCheckBox(i18n("Allow password login over SSH"));
    deployPasswordLoginCheck->setToolTip(i18n("The guest password itself is set per virtual machine, not here."));
    deployPasswordLoginCheck->setChecked(defaults.passwordLogin);
    guestLayout->addRow({}, deployPasswordLoginCheck);

    deployTimezoneCombo = new QComboBox();
    deployTimezoneCombo->setEditable(true);
    for (const QByteArray &tzId : QTimeZone::availableTimeZoneIds())
        deployTimezoneCombo->addItem(QString::fromUtf8(tzId));
    deployTimezoneCombo->setCurrentText(defaults.timezone);
    guestLayout->addRow(i18n("Time zone:"), deployTimezoneCombo);

    deployLocaleCombo = new QComboBox();
    deployLocaleCombo->setEditable(true);
    deployLocaleCombo->completer()->setFilterMode(Qt::MatchContains);
    for (const auto &loc : availableLocales())
        deployLocaleCombo->addItem(loc.displayName, loc.code);
    if (const int idx = deployLocaleCombo->findData(defaults.locale); idx >= 0)
        deployLocaleCombo->setCurrentIndex(idx);
    else
        deployLocaleCombo->setCurrentText(defaults.locale);
    guestLayout->addRow(i18n("Language (locale):"), deployLocaleCombo);

    deployKbCombo = new QComboBox();
    const auto keyboards = availableKeyboards();
    for (int i = 0; i < keyboards.size(); ++i) {
        QString label = keyboards[i].displayName;
        if (!keyboards[i].variant.isEmpty())
            label += u" -- %1"_s.arg(keyboards[i].variant);
        deployKbCombo->addItem(label, i);
        if (keyboards[i].layout == defaults.kbLayout && keyboards[i].variant == defaults.kbVariant)
            deployKbCombo->setCurrentIndex(deployKbCombo->count() - 1);
    }
    guestLayout->addRow(i18n("Keyboard layout:"), deployKbCombo);

    deployInstallDesktopCheck = new QCheckBox(i18n("Install the default desktop environment (if available)"));
    deployInstallDesktopCheck->setToolTip(i18n("Each cloud-init template resolves this to the packages its distribution uses."));
    deployInstallDesktopCheck->setChecked(defaults.installDesktop);
    guestLayout->addRow({}, deployInstallDesktopCheck);

    layout->addWidget(guestGroup);

    layout->addStretch();

    auto *item = addPage(page, i18n("Deployment"));
    item->setIcon(QIcon::fromTheme(u"system-run"_s));
    m_pages[AutomatedDeploy] = item;
}

void SettingsDialog::setupVirtualNetworksPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    networkTable = new QTableWidget(0, 6);
    networkTable->setHorizontalHeaderLabels({i18n("Name"), i18n("Gateway"), i18n("DHCP"), i18n("DNS"), i18n("NAT"), i18n("Status")});
    networkTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    networkTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    networkTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    networkTable->setSelectionMode(QAbstractItemView::SingleSelection);
    networkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    networkTable->verticalHeader()->setVisible(false);
    layout->addWidget(networkTable);

    auto *buttonLayout = new QHBoxLayout();
    netAddButton = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18n("Add..."));
    netEditButton = new QPushButton(QIcon::fromTheme(u"document-edit"_s), i18n("Edit..."));
    netRemoveButton = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18n("Remove"));
    netEditButton->setEnabled(false);
    netRemoveButton->setEnabled(false);
    buttonLayout->addWidget(netAddButton);
    buttonLayout->addWidget(netEditButton);
    buttonLayout->addWidget(netRemoveButton);
    buttonLayout->addStretch();
    layout->addLayout(buttonLayout);

    connect(netAddButton, &QPushButton::clicked, this, &SettingsDialog::addNetwork);
    connect(netEditButton, &QPushButton::clicked, this, &SettingsDialog::editNetwork);
    connect(netRemoveButton, &QPushButton::clicked, this, &SettingsDialog::removeNetwork);
    connect(networkTable, &QTableWidget::itemSelectionChanged, this, &SettingsDialog::onNetworkSelectionChanged);
    connect(networkTable, &QTableWidget::cellDoubleClicked, this, &SettingsDialog::editNetwork);

    auto *item = addPage(page, i18n("Virtual Networks"));
    item->setIcon(QIcon::fromTheme(u"network-workgroup"_s));
    m_pages[VirtualNetworks] = item;
}

void SettingsDialog::setupTemplatesPage()
{
    m_templatesPage = new TemplatesSettingsPage;
    CloudDeployConfig preview = deploymentDefaults();
    preview.hostname = u"preview-vm"_s;
    m_templatesPage->setPreviewConfig(preview);
    auto *item = addPage(m_templatesPage, i18n("Templates"));
    item->setIcon(QIcon::fromTheme(u"document-new-from-template"_s));
    m_pages[Templates] = item;
}

void SettingsDialog::setImageDatabase(ImageDatabase *db)
{
    m_templatesPage->setImageDatabase(db);
}

void SettingsDialog::browseVmDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, i18n("Select VM storage location"), pathEdit->text());
    if (!dir.isEmpty())
        pathEdit->setText(dir);
}

void SettingsDialog::browseOvmfDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, i18n("Select OVMF firmware directory"), ovmfEdit->text());
    if (!dir.isEmpty())
        ovmfEdit->setText(dir);
}

void SettingsDialog::saveSettings()
{
    KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    KConfigGroup general = config->group(u"General"_s);
    // The default is not stored, so it follows XDG_DATA_HOME.
    if (QDir::cleanPath(pathEdit->text().trimmed()) == defaultVmBasePath())
        general.deleteEntry("VmBasePath");
    else
        general.writeEntry("VmBasePath", pathEdit->text());
    general.writeEntry("OvmfPath", ovmfEdit->text());
    // Never persist sandbox paths as user overrides; an empty entry means
    // auto-detection stays in charge.
    if (SandboxCapabilities::instance().has(SandboxCapabilities::ExternalToolPaths)) {
        for (const auto &tw : m_toolWidgets) {
            const QString key = ExternalTools::instance().configKey(tw.tool);
            general.writeEntry(key, tw.pathEdit->text().trimmed());
        }
    }
    general.writeEntry("PerformanceMonitorEnabled", perfMonitorCheck->isChecked());
    const QSize res = resolutionCombo->currentData().toSize();
    general.writeEntry("DisplayWidth", res.width());
    general.writeEntry("DisplayHeight", res.height());
    general.writeEntry("InhibitScreensaverAlways", ssAlwaysCheck->isChecked());
    general.writeEntry("InhibitScreensaverFullscreen", ssFullscreenCheck->isChecked());
    general.writeEntry("InhibitScreensaverPointerGrab", ssGrabCheck->isChecked());
    general.writeEntry("ShowAllArchitectures", allArchCheck->isChecked());
    general.writeEntry("CloneRegenerateMac", cloneRegenerateMacCheck->isChecked());
    general.writeEntry("ImportRegenerateMac", importRegenerateMacCheck->isChecked());
    // Only when the box was offered: a QEMU without seccomp shows it unchecked
    // and must not overwrite the choice the user made for another binary.
    if (seccompSandboxCheck->isEnabled())
        general.writeEntry("QemuSeccompSandbox", seccompSandboxCheck->isChecked());

    general.writeEntry("DeployUser", deployUserEdit->text().trimmed());
    general.writeEntry("DeploySshKeys", deploySshKeysEdit->toPlainText().trimmed());
    general.writeEntry("DeployPasswordLogin", deployPasswordLoginCheck->isChecked());
    general.writeEntry("DeployTimezone", deployTimezoneCombo->currentText());

    const QVariant localeData = deployLocaleCombo->currentData();
    general.writeEntry("DeployLocale", localeData.isValid() ? localeData.toString() : deployLocaleCombo->currentText().trimmed());

    const auto keyboards = availableKeyboards();
    if (const int kbIdx = deployKbCombo->currentData().toInt(); kbIdx >= 0 && kbIdx < keyboards.size()) {
        general.writeEntry("DeployKbLayout", keyboards[kbIdx].layout);
        general.writeEntry("DeployKbVariant", keyboards[kbIdx].variant);
    }

    general.writeEntry("DeployInstallDesktop", deployInstallDesktopCheck->isChecked());

    config->sync();

    ExternalTools::instance().refresh();
    QemuCapabilities::instance().refresh();

    accept();
}

QString SettingsDialog::getVmBasePath()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("VmBasePath", defaultVmBasePath());
}

QString SettingsDialog::defaultVmBasePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/kemu/vms"_s;
}

QString SettingsDialog::getOvmfPath()
{
    return Utils::configuredOvmfDir();
}

bool SettingsDialog::isPerformanceMonitorEnabled()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("PerformanceMonitorEnabled", true);
}

QSize SettingsDialog::getDefaultResolution()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    const int w = general.readEntry("DisplayWidth", KEMUConst::DefaultDisplaySize.width());
    const int h = general.readEntry("DisplayHeight", KEMUConst::DefaultDisplaySize.height());
    return QSize(w, h);
}

bool SettingsDialog::showAllArchitectures()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("ShowAllArchitectures", false);
}

bool SettingsDialog::cloneRegenerateMac()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("CloneRegenerateMac", true);
}

bool SettingsDialog::importRegenerateMac()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("ImportRegenerateMac", false);
}

bool SettingsDialog::qemuSeccompSandbox()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("QemuSeccompSandbox", false);
}

bool SettingsDialog::inhibitScreensaverAlways()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("InhibitScreensaverAlways", false);
}

bool SettingsDialog::inhibitScreensaverFullscreen()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("InhibitScreensaverFullscreen", true);
}

bool SettingsDialog::inhibitScreensaverPointerGrab()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);
    return general.readEntry("InhibitScreensaverPointerGrab", true);
}

CloudDeployConfig SettingsDialog::deploymentDefaults()
{
    const KSharedConfig::Ptr config = KSharedConfig::openConfig(u"kemurc"_s);
    const KConfigGroup general = config->group(u"General"_s);

    CloudDeployConfig cfg;
    cfg.username = general.readEntry("DeployUser", KUser().loginName());
    cfg.passwordLogin = general.readEntry("DeployPasswordLogin", false);
    cfg.timezone = general.readEntry("DeployTimezone", defaultTimezone());
    cfg.locale = general.readEntry("DeployLocale", defaultLocaleCode());
    cfg.installDesktop = general.readEntry("DeployInstallDesktop", false);

    const QString keys = general.readEntry("DeploySshKeys", QString());
    cfg.sshKeys = keys.isEmpty() ? detectHostSshKeys() : keys.split(u'\n', Qt::SkipEmptyParts);

    const auto keyboards = availableKeyboards();
    const int kbIdx = defaultKeyboardIndex();
    cfg.kbLayout = general.readEntry("DeployKbLayout", kbIdx >= 0 ? keyboards[kbIdx].layout : u"us"_s);
    cfg.kbVariant = general.readEntry("DeployKbVariant", QString());

    // hostname, templateId and networkTemplateId are the caller's job.
    return cfg;
}

void SettingsDialog::setNetworkManager(VirtualNetworkManager *mgr)
{
    m_networkManager = mgr;
    refreshNetworkTable();
}

static bool showNetworkForm(QWidget *parent, VirtualNetwork &net, const QString &title, bool isNew)
{
    QDialog dlg(parent);
    dlg.setWindowTitle(title);

    auto *form = new QFormLayout(&dlg);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    // --- Name ---
    auto *nameEdit = new QLineEdit(net.name);
    nameEdit->setPlaceholderText(u"lab-net"_s);
    if (!isNew)
        nameEdit->setReadOnly(true);
    form->addRow(i18n("Name:"), nameEdit);

    auto *nameHint = new QLabel;
    nameHint->setWordWrap(true);
    nameHint->setStyleSheet(u"color: palette(mid);"_s);
    form->addRow(u""_s, nameHint);

    // --- Gateway / Subnet ---
    auto *gatewayEdit = new QLineEdit(net.gateway);
    gatewayEdit->setPlaceholderText(u"192.168.100.1"_s);
    gatewayEdit->setToolTip(i18n("Gateway address of the virtual network."));
    form->addRow(i18n("Gateway:"), gatewayEdit);

    auto *prefixSpin = new QSpinBox;
    prefixSpin->setRange(8, 30);
    prefixSpin->setValue(net.prefixLength);
    prefixSpin->setSuffix(u" (CIDR)"_s);
    form->addRow(i18n("Prefix length:"), prefixSpin);

    auto *subnetInfoLabel = new QLabel;
    subnetInfoLabel->setStyleSheet(u"color: palette(mid);"_s);
    form->addRow(u""_s, subnetInfoLabel);

    auto updateSubnetLabel = [subnetInfoLabel, gatewayEdit, prefixSpin]() {
        const QString gw = gatewayEdit->text().trimmed();
        if (gw.isEmpty()) {
            subnetInfoLabel->setText(i18n("Subnet: (enter a gateway address)"));
            return;
        }
        QStringList parts = gw.split(u'.');
        if (parts.size() != 4) {
            subnetInfoLabel->setText(i18n("Subnet: (invalid gateway)"));
            return;
        }
        parts[3] = u"0"_s;
        subnetInfoLabel->setText(i18n("Subnet: %1/%2", parts.join(u'.'), QString::number(prefixSpin->value())));
    };
    updateSubnetLabel();
    QObject::connect(gatewayEdit, &QLineEdit::textChanged, &dlg, updateSubnetLabel);
    QObject::connect(prefixSpin, &QSpinBox::valueChanged, &dlg, updateSubnetLabel);

    // --- DHCP ---
    auto *dhcpCheck = new QCheckBox(i18n("Enable DHCP"));
    dhcpCheck->setChecked(net.enableDhcp);
    form->addRow(dhcpCheck);

    auto *dhcpStartEdit = new QLineEdit(net.dhcpRangeStart);
    dhcpStartEdit->setPlaceholderText(u"192.168.100.100"_s);
    auto *dhcpStartLabel = new QLabel(i18n("DHCP range start:"));
    form->addRow(dhcpStartLabel, dhcpStartEdit);

    auto *dhcpEndEdit = new QLineEdit(net.dhcpRangeEnd);
    dhcpEndEdit->setPlaceholderText(u"192.168.100.200"_s);
    auto *dhcpEndLabel = new QLabel(i18n("DHCP range end:"));
    form->addRow(dhcpEndLabel, dhcpEndEdit);

    // --- DNS ---
    auto *dnsCheck = new QCheckBox(i18n("Enable DNS server"));
    dnsCheck->setChecked(net.enableDns);
    dnsCheck->setToolTip(i18n("Resolves the VMs' hostnames inside this network "
                              "(\"<hostname>.<DNS domain>\"). Other queries are forwarded "
                              "to the host resolver only if NAT is enabled. Requires DHCP."));
    form->addRow(dnsCheck);

    auto *domainEdit = new QLineEdit(net.dnsDomain);
    auto *domainLabel = new QLabel(i18n("DNS domain:"));
    domainEdit->setToolTip(i18n("Domain of the VMs' names and search domain handed to guests. "
                                "Leave empty for \"<network>.internal\"."));
    form->addRow(domainLabel, domainEdit);
    const auto updateDomainPlaceholder = [=]() {
        domainEdit->setPlaceholderText(Utils::vmNameToHostname(nameEdit->text().trimmed()) + u".internal"_s);
    };
    updateDomainPlaceholder();
    QObject::connect(nameEdit, &QLineEdit::textChanged, &dlg, updateDomainPlaceholder);

    auto *domainHint = new QLabel;
    domainHint->setWordWrap(true);
    domainHint->setStyleSheet(u"color: palette(mid);"_s);
    form->addRow(u""_s, domainHint);

    auto updateDhcpWidgets = [=](bool checked) {
        dhcpStartEdit->setEnabled(checked);
        dhcpStartLabel->setEnabled(checked);
        dhcpEndEdit->setEnabled(checked);
        dhcpEndLabel->setEnabled(checked);
        dnsCheck->setEnabled(checked);
        domainEdit->setEnabled(checked && dnsCheck->isChecked());
        domainLabel->setEnabled(checked && dnsCheck->isChecked());
    };
    updateDhcpWidgets(dhcpCheck->isChecked());
    QObject::connect(dhcpCheck, &QCheckBox::toggled, updateDhcpWidgets);
    QObject::connect(dnsCheck, &QCheckBox::toggled, [=](bool) {
        updateDhcpWidgets(dhcpCheck->isChecked());
    });

    // --- NAT ---
    auto *natCheck = new QCheckBox(i18n("Enable NAT (Internet access)"));
    natCheck->setChecked(net.enableNat);
    natCheck->setToolTip(
        i18n("When disabled, the virtual network is completely isolated "
             "from the host network (air-gapped)."));
    form->addRow(natCheck);

    auto *hostAccessCheck = new QCheckBox(i18n("Allow access to host services"));
    hostAccessCheck->setChecked(net.allowHostAccess);
    hostAccessCheck->setToolTip(
        i18n("Guests can reach services listening on the host's loopback "
             "interface (127.0.0.1) via the gateway address."));
    form->addRow(hostAccessCheck);

    // --- Port Forwards ---
    auto *portFwdCheck = new QCheckBox(i18n("Enable Port Forwarding"));
    // Checked if there are existing forwards
    portFwdCheck->setChecked(!net.portForwards.isEmpty());
    form->addRow(portFwdCheck);

    auto *portFwdEdit = new QLineEdit(net.portForwards.join(u", "_s));
    portFwdEdit->setPlaceholderText(u"tcp/8080:192.168.100.50:80, tcp/2222:192.168.100.51:22"_s);
    portFwdEdit->setToolTip(
        i18n("Comma-separated list of port forwards from host to guest.\n"
             "Format: protocol/hostPort:guestIp:guestPort"));
    auto *portFwdLabel = new QLabel(i18n("Port forwards:"));
    form->addRow(portFwdLabel, portFwdEdit);

    auto updatePortFwdWidgets = [=]() {
        const bool natEnabled = natCheck->isChecked();
        const bool fwdEnabled = portFwdCheck->isChecked();

        hostAccessCheck->setEnabled(natEnabled);
        portFwdCheck->setEnabled(natEnabled);
        portFwdLabel->setEnabled(natEnabled && fwdEnabled);
        portFwdEdit->setEnabled(natEnabled && fwdEnabled);
    };

    updatePortFwdWidgets();
    QObject::connect(natCheck, &QCheckBox::toggled, updatePortFwdWidgets);
    QObject::connect(portFwdCheck, &QCheckBox::toggled, updatePortFwdWidgets);

    // --- Buttons ---
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    form->addRow(buttons);

    // The name becomes a directory and part of the socket paths QEMU gets,
    // so it is only validated where it can still be changed.
    const auto updateNameHint = [=]() {
        QString reason;
        const bool valid = !isNew || Utils::isValidNetworkName(nameEdit->text().trimmed(), &reason);
        nameHint->setText(valid ? QString() : reason);
        nameHint->setVisible(!valid);
        return valid;
    };

    const auto updateDomainHint = [=]() {
        const QString domain = domainEdit->text().trimmed().toLower();
        const bool dnsOn = dhcpCheck->isChecked() && dnsCheck->isChecked();
        const bool valid = domain.isEmpty() || Utils::isValidDnsDomain(domain);
        buttons->button(QDialogButtonBox::Ok)->setEnabled((valid || !dnsOn) && updateNameHint());

        QString hint;
        if (!dnsOn || domain.isEmpty()) {
            hint.clear();
        } else if (!valid) {
            hint = i18n("Not a valid domain name.");
        } else if (domain == u"local"_s || domain.endsWith(u".local"_s)) {
            hint = i18n("\".local\" is used by multicast DNS; guests will not ask this network's DNS server.");
        } else {
            static const QStringList privateSuffixes = {u"internal"_s, u"lan"_s, u"test"_s, u"example"_s, u"invalid"_s, u"home.arpa"_s};
            const bool isPrivate = std::any_of(privateSuffixes.cbegin(), privateSuffixes.cend(), [&domain](const QString &suffix) {
                return domain == suffix || domain.endsWith(u'.' + suffix);
            });
            if (!isPrivate && natCheck->isChecked())
                hint = i18n("Names below this domain are answered inside the network and never reach the real DNS.");
        }
        domainHint->setText(hint);
        domainHint->setVisible(!hint.isEmpty());
    };
    QObject::connect(nameEdit, &QLineEdit::textChanged, &dlg, updateDomainHint);
    QObject::connect(domainEdit, &QLineEdit::textChanged, &dlg, updateDomainHint);
    QObject::connect(dnsCheck, &QCheckBox::toggled, &dlg, updateDomainHint);
    QObject::connect(dhcpCheck, &QCheckBox::toggled, &dlg, updateDomainHint);
    QObject::connect(natCheck, &QCheckBox::toggled, &dlg, updateDomainHint);
    updateDomainHint();
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    net.name = nameEdit->text().trimmed();
    net.gateway = gatewayEdit->text().trimmed();
    net.prefixLength = prefixSpin->value();
    net.enableDhcp = dhcpCheck->isChecked();
    net.enableDns = dnsCheck->isChecked();
    net.dnsDomain = domainEdit->text().trimmed().toLower();
    net.dhcpRangeStart = dhcpStartEdit->text().trimmed();
    net.dhcpRangeEnd = dhcpEndEdit->text().trimmed();
    net.enableNat = natCheck->isChecked();
    net.allowHostAccess = net.enableNat && hostAccessCheck->isChecked();

    net.portForwards.clear();
    // Only parse and save port forwards if NAT and Port Forwarding are enabled
    if (natCheck->isChecked() && portFwdCheck->isChecked()) {
        const QString rawForwards = portFwdEdit->text().trimmed();
        if (!rawForwards.isEmpty()) {
            const QStringList parts = rawForwards.split(u',', Qt::SkipEmptyParts);
            for (const QString &p : parts)
                net.portForwards << p.trimmed();
        }
    }

    return net.isValid();
}

void SettingsDialog::addNetwork()
{
    if (!m_networkManager)
        return;

    VirtualNetwork net;
    if (!showNetworkForm(this, net, i18n("New Virtual Network"), true))
        return;

    if (!m_networkManager->addNetwork(net)) {
        KMessageBox::error(this, i18n("A network named '%1' already exists.", net.name));
        return;
    }

    refreshNetworkTable();
}

void SettingsDialog::editNetwork()
{
    if (!m_networkManager || !networkTable || networkTable->currentRow() < 0)
        return;

    const QString name = networkTable->item(networkTable->currentRow(), 0)->text();
    if (m_networkManager->isRunning(name)) {
        KMessageBox::error(this, i18n("Network '%1' is currently in use. Stop all connected VMs first.", name));
        return;
    }

    VirtualNetwork net = m_networkManager->network(name);
    if (!showNetworkForm(this, net, i18n("Edit Virtual Network"), false))
        return;

    m_networkManager->updateNetwork(net);
    refreshNetworkTable();
}

void SettingsDialog::removeNetwork()
{
    if (!m_networkManager || !networkTable || networkTable->currentRow() < 0)
        return;

    const QString name = networkTable->item(networkTable->currentRow(), 0)->text();

    if (m_networkManager->isRunning(name)) {
        KMessageBox::error(this, i18n("Network '%1' is currently in use. Stop all connected VMs first.", name));
        return;
    }

    auto result = KMessageBox::questionTwoActions(this,
                                                  i18n("Delete virtual network '%1'?", name),
                                                  i18n("Confirm Deletion"),
                                                  KGuiItem(i18n("Delete"), u"edit-delete"_s),
                                                  KStandardGuiItem::cancel());

    if (result != KMessageBox::PrimaryAction)
        return;

    m_networkManager->removeNetwork(name);
    refreshNetworkTable();
}

void SettingsDialog::onNetworkSelectionChanged()
{
    const bool hasSelection = networkTable && networkTable->currentRow() >= 0;
    if (netEditButton)
        netEditButton->setEnabled(hasSelection);
    if (netRemoveButton)
        netRemoveButton->setEnabled(hasSelection);
}

void SettingsDialog::refreshNetworkTable()
{
    if (!networkTable || !m_networkManager)
        return;

    networkTable->setRowCount(0);
    for (const auto &net : m_networkManager->networks()) {
        const int row = networkTable->rowCount();
        networkTable->insertRow(row);

        networkTable->setItem(row, 0, new QTableWidgetItem(net.name));

        const QString gwText = net.gateway.isEmpty() ? i18n("(not set)") : u"%1/%2"_s.arg(net.gateway).arg(net.prefixLength);
        networkTable->setItem(row, 1, new QTableWidgetItem(gwText));

        QString dhcpText;
        if (!net.enableDhcp)
            dhcpText = i18n("Off");
        else if (net.dhcpRangeStart.isEmpty())
            dhcpText = i18n("Auto");
        else
            dhcpText = u"%1 – %2"_s.arg(net.dhcpRangeStart, net.dhcpRangeEnd);
        networkTable->setItem(row, 2, new QTableWidgetItem(dhcpText));

        networkTable->setItem(row, 3, new QTableWidgetItem(net.enableDhcp && net.enableDns ? i18n("Yes") : i18n("No")));

        networkTable->setItem(row, 4, new QTableWidgetItem(net.enableNat ? i18n("Yes") : i18n("No")));

        QString statusText;
        if (m_networkManager->isRunning(net.name)) {
            const int count = m_networkManager->connectedVmCount(net.name);
            statusText = i18np("Running (%1 VM)", "Running (%1 VMs)", count);
        } else {
            statusText = i18n("Stopped");
        }
        networkTable->setItem(row, 5, new QTableWidgetItem(statusText));
    }
}

void SettingsDialog::updateToolVersion(int index)
{
    if (index < 0 || index >= m_toolWidgets.size())
        return;

    const auto &tw = m_toolWidgets[index];
    if (!tw.pathEdit || !tw.statusLabel)
        return;

    if (ExternalTools::instance().unusedTools().contains(tw.tool)) {
        const KColorScheme mutedScheme(QPalette::Disabled, KColorScheme::View);
        const QString col = mutedScheme.foreground(KColorScheme::NegativeText).color().name();
        tw.statusLabel->setText(i18n("<font color='%1'><i>Deactivated in sandbox mode.</i></font>", col));
        return;
    }

    const QString typed = tw.pathEdit->text().trimmed();
    const QString path = typed.isEmpty() ? ExternalTools::instance().detectedPath(tw.tool) : typed;

    const KColorScheme scheme(QPalette::Active, KColorScheme::View);

    if (path.isEmpty() || !QFileInfo::exists(path)) {
        const QString col = scheme.foreground(KColorScheme::NegativeText).color().name();
        tw.statusLabel->setText(i18n("<font color='%1'><b>Executable not found.</b></font>", col));
        return;
    }

    const QString version = ExternalTools::probeVersionString(tw.tool, path);

    if (!version.isEmpty()) {
        const QString col = scheme.foreground(KColorScheme::PositiveText).color().name();
        tw.statusLabel->setText(i18n("<font color='%1'>%2</font>", col, version));
    } else {
        const QFileInfo fi(path);
        if (fi.isExecutable()) {
            const QString col = scheme.foreground(KColorScheme::PositiveText).color().name();
            tw.statusLabel->setText(i18n("<font color='%1'>Found (no version info)</font>", col));
        } else {
            const QString col = scheme.foreground(KColorScheme::NeutralText).color().name();
            tw.statusLabel->setText(i18n("<font color='%1'><b>File exists but is not executable.</b></font>", col));
        }
    }
}

QStringList SettingsDialog::detectHostSshKeys()
{
    const QDir sshDir(QDir::homePath() + u"/.ssh"_s);
    QStringList keys;

    // Preference order matches what ssh-keygen produces by default today.
    for (const auto &name : {u"id_ed25519.pub"_s, u"id_ecdsa.pub"_s, u"id_rsa.pub"_s}) {
        QFile file(sshDir.absoluteFilePath(name));
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        const QString key = QString::fromUtf8(file.readAll()).trimmed();
        if (!key.isEmpty())
            keys.append(key);
    }

    return keys;
}
