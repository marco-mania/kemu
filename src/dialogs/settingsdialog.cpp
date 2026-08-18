/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "settingsdialog.h"

#include "const.h"
#include "utils/sandboxenvironment.h"
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

    auto *netLine = new QFrame(this);
    netLine->setFrameShape(QFrame::HLine);
    netLine->setFrameShadow(QFrame::Sunken);
    layout->addWidget(netLine);

    slirpNetworkingCheck = new QCheckBox(i18n("Use QEMU's built-in usermode networking (SLIRP) instead of passt"));
    slirpNetworkingCheck->setToolTip(
        i18n("passt is used by default and is significantly faster. Enable "
             "this only if passt is unavailable or causes problems on your "
             "system."));
    slirpNetworkingCheck->setChecked(useSlirpNetworking());
    slirpNetworkingCheck->setEnabled(ExternalTools::instance().isAvailable(ExternalTools::Passt) && !SandboxEnvironment::isFlatpak());
    if (!ExternalTools::instance().isAvailable(ExternalTools::Passt) || SandboxEnvironment::isFlatpak())
        slirpNetworkingCheck->setToolTip(i18n("passt is not available or in sandbox environment - SLIRP is used for all VMs."));
    layout->addWidget(slirpNetworkingCheck);

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
    auto *page = new QWidget;
    auto *scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto *layout = new QVBoxLayout(page);
    layout->setSpacing(4);

    auto *refreshLayout = new QHBoxLayout;
    auto *refreshBtn = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), i18n("Re-detect All"));
    refreshBtn->setEnabled(!SandboxEnvironment::isFlatpak());
    connect(refreshBtn, &QPushButton::clicked, this, [this]() {
        ExternalTools::instance().refresh();
        for (int i = 0; i < m_toolWidgets.size(); ++i) {
            m_toolWidgets[i].pathEdit->setText(ExternalTools::instance().path(m_toolWidgets[i].tool));
        }
    });
    refreshLayout->addWidget(refreshBtn);
    refreshLayout->addStretch();
    layout->addLayout(refreshLayout);

    const auto &tools = ExternalTools::instance();

    for (const ExternalTools::Tool tool : ExternalTools::allTools()) {
        const QString binary = tools.defaultBinary(tool);
        const QString description = ExternalTools::displayName(tool);
        const QString currentPath = tools.path(tool);
        const bool sandboxIrrelevant = ExternalTools::sandboxIrrelevantTools().contains(tool);

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
        pathEdit->setText(currentPath);
        pathEdit->setPlaceholderText(i18n("Auto-detected if empty"));
        pathRow->addWidget(pathEdit);
        auto *browseBtn = new QPushButton(QIcon::fromTheme(u"document-open"_s), {});
        pathRow->addWidget(browseBtn);
        layout->addLayout(pathRow);

        auto *statusLabel = new QLabel;
        layout->addWidget(statusLabel);

        if (sandboxIrrelevant) {
            pathEdit->setReadOnly(SandboxEnvironment::isFlatpak());
            browseBtn->setEnabled(!SandboxEnvironment::isFlatpak());
            descLabel->setText(u"<font color='%1'> (%2 - %3)</font>"_s.arg(midColor, description, i18n("not used in the sandboxed version")));
        }

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
    auto *layout = new QFormLayout(page);

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

    layout->addRow(i18n("Default resolution for new VMs:"), resolutionCombo);

    if (VMConfig::isDisplayTypeAvailable(VMSpecs::DisplayType::Spice)) {
        spiceCloseActionCombo = new QComboBox;
        spiceCloseActionCombo->addItem(i18n("Switch to headless mode"), SwitchToHeadless);
        spiceCloseActionCombo->addItem(i18n("Shut down the VM"), ShutdownVm);
        spiceCloseActionCombo->setCurrentIndex(static_cast<int>(spiceViewerCloseAction()));
        layout->addRow(i18n("When SPICE viewer is closed:"), spiceCloseActionCombo);
    }

    auto *item = addPage(page, i18n("Display"));
    item->setIcon(QIcon::fromTheme(u"preferences-desktop-display"_s));
    m_pages[Display] = item;
}

void SettingsDialog::setupAutomatedDeployPage()
{
    auto *page = new QWidget;
    auto *layout = new QFormLayout(page);

    auto *archInfoLabel = new QLabel(i18n("Detected host architecture: <b>%1</b>", QSysInfo::currentCpuArchitecture()));
    layout->addRow(archInfoLabel);

    allArchCheck = new QCheckBox(i18n("Show all CPU architectures in edition list"));
    allArchCheck->setToolTip(
        i18n("By default only editions matching your host architecture are shown. "
             "Enable this to see all architectures (note: non-native architectures "
             "use slow TCG emulation instead of KVM)."));
    allArchCheck->setChecked(showAllArchitectures());
    layout->addRow(allArchCheck);

    deployHeadlessCheck = new QCheckBox(i18n("Run deployment in background (headless)"));
    deployHeadlessCheck->setToolTip(
        i18n("When disabled, the QEMU window is visible during deployment, "
             "which can be useful for diagnosing installation issues."));
    deployHeadlessCheck->setChecked(deployHeadless());
    layout->addRow(deployHeadlessCheck);

    deploySerialLogCheck = new QCheckBox(i18n("Write serial console log during deployment"));
    deploySerialLogCheck->setToolTip(
        i18n("Writes kernel and installer output to '%1' "
             "in the VM directory. Useful for diagnosing failed installations.\n"
             "Note: some installers (e.g. Debian) redirect their UI to the "
             "serial console when enabled, making the QEMU window appear blank.",
             KEMUConst::DeploySerialLogFileName));
    deploySerialLogCheck->setChecked(deploySerialLog());
    layout->addRow(deploySerialLogCheck);

    auto *item = addPage(page, i18n("Automated Deploy"));
    item->setIcon(QIcon::fromTheme(u"system-run"_s));
    m_pages[AutomatedDeploy] = item;
}

void SettingsDialog::setupVirtualNetworksPage()
{
    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);

    networkTable = new QTableWidget(0, 5);
    networkTable->setHorizontalHeaderLabels({i18n("Name"), i18n("Gateway"), i18n("DHCP"), i18n("NAT"), i18n("Status")});
    networkTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    networkTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
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

    // Virtual networking relies on user namespaces, which are blocked in the
    // Flatpak sandbox. The page stays visible but disabled so an existing
    // configuration remains discoverable.
    if (SandboxEnvironment::isFlatpak())
        item->setEnabled(false);
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
    KConfig config(u"kemurc"_s);
    KConfigGroup general = config.group(u"General"_s);
    general.writeEntry("VmBasePath", pathEdit->text());
    general.writeEntry("OvmfPath", ovmfEdit->text());
    // Never persist sandbox paths as user overrides.
    if (!SandboxEnvironment::isFlatpak()) {
        for (const auto &tw : m_toolWidgets) {
            const QString key = ExternalTools::instance().configKey(tw.tool);
            const QString text = tw.pathEdit->text().trimmed();
            // Storing the auto-detected path would freeze detection, so only
            // a real deviation counts as an override.
            const bool isOverride = !text.isEmpty() && text != QStandardPaths::findExecutable(ExternalTools::instance().defaultBinary(tw.tool));
            general.writeEntry(key, isOverride ? text : QString());
        }
    }
    general.writeEntry("PerformanceMonitorEnabled", perfMonitorCheck->isChecked());
    const QSize res = resolutionCombo->currentData().toSize();
    general.writeEntry("DisplayWidth", res.width());
    general.writeEntry("DisplayHeight", res.height());
    general.writeEntry("ShowAllArchitectures", allArchCheck->isChecked());
    general.writeEntry("CloneRegenerateMac", cloneRegenerateMacCheck->isChecked());
    general.writeEntry("ImportRegenerateMac", importRegenerateMacCheck->isChecked());
    general.writeEntry("DeployHeadless", deployHeadlessCheck->isChecked());
    general.writeEntry("DeploySerialLog", deploySerialLogCheck->isChecked());
    general.writeEntry("SpiceViewerCloseAction", spiceCloseActionCombo->currentData().toInt());
    // Only persist the user's actual choice - not the forced value, so the
    // real preference survives once passt becomes available again.
    if (slirpNetworkingCheck->isEnabled())
        general.writeEntry("PreferSlirpNetworking", slirpNetworkingCheck->isChecked());
    config.sync();
    ExternalTools::instance().refresh();
    accept();
}

QString SettingsDialog::getVmBasePath()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    const QString defaultPath = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + u"/kemu/vms"_s;
    return general.readEntry("VmBasePath", defaultPath);
}

QString SettingsDialog::getOvmfPath()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("OvmfPath", {});
}

bool SettingsDialog::isPerformanceMonitorEnabled()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("PerformanceMonitorEnabled", true);
}

QSize SettingsDialog::getDefaultResolution()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    const int w = general.readEntry("DisplayWidth", KEMUConst::DefaultDisplaySize.width());
    const int h = general.readEntry("DisplayHeight", KEMUConst::DefaultDisplaySize.height());
    return QSize(w, h);
}

bool SettingsDialog::showAllArchitectures()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("ShowAllArchitectures", false);
}

bool SettingsDialog::cloneRegenerateMac()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("CloneRegenerateMac", true);
}

bool SettingsDialog::importRegenerateMac()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("ImportRegenerateMac", false);
}

bool SettingsDialog::deployHeadless()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("DeployHeadless", true);
}

bool SettingsDialog::deploySerialLog()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("DeploySerialLog", false);
}

SettingsDialog::SpiceViewerCloseAction SettingsDialog::spiceViewerCloseAction()
{
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    const int val = general.readEntry("SpiceViewerCloseAction", static_cast<int>(SwitchToHeadless));
    if (val == ShutdownVm)
        return ShutdownVm;
    return SwitchToHeadless;
}

bool SettingsDialog::useSlirpNetworking()
{
    if (!ExternalTools::instance().isAvailable(ExternalTools::Passt) || SandboxEnvironment::isFlatpak())
        return true;
    KConfig config(u"kemurc"_s);
    const KConfigGroup general = config.group(u"General"_s);
    return general.readEntry("PreferSlirpNetworking", false);
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

    // --- Gateway / Subnet ---
    auto *gatewayEdit = new QLineEdit(net.gateway);
    gatewayEdit->setPlaceholderText(u"192.168.100.1"_s);
    gatewayEdit->setToolTip(i18n("Bridge gateway address inside the network namespace."));
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
    auto *dhcpCheck = new QCheckBox(i18n("Enable DHCP (via dnsmasq)"));
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

    auto updateDhcpWidgets = [=](bool checked) {
        dhcpStartEdit->setEnabled(checked);
        dhcpStartLabel->setEnabled(checked);
        dhcpEndEdit->setEnabled(checked);
        dhcpEndLabel->setEnabled(checked);
    };
    updateDhcpWidgets(dhcpCheck->isChecked());
    QObject::connect(dhcpCheck, &QCheckBox::toggled, updateDhcpWidgets);

    // --- NAT ---
    auto *natCheck = new QCheckBox(i18n("Enable NAT (Internet access via pasta)"));
    natCheck->setChecked(net.enableNat);
    natCheck->setToolTip(
        i18n("When disabled, the virtual network is completely isolated "
             "from the host network (air-gapped)."));
    form->addRow(natCheck);

    // --- Port Forwards ---
    auto *portFwdCheck = new QCheckBox(i18n("Enable Port Forwarding"));
    // Checked if there are existing forwards
    portFwdCheck->setChecked(!net.portForwards.isEmpty());
    form->addRow(portFwdCheck);

    auto *portFwdEdit = new QLineEdit(net.portForwards.join(u", "_s));
    portFwdEdit->setPlaceholderText(u"tcp/8080:80, tcp/2222:22"_s);
    portFwdEdit->setToolTip(
        i18n("Comma-separated list of port forwards from host to namespace.\n"
             "Format: protocol/hostPort:guestPort"));
    auto *portFwdLabel = new QLabel(i18n("Port forwards:"));
    form->addRow(portFwdLabel, portFwdEdit);

    auto updatePortFwdWidgets = [=]() {
        const bool natEnabled = natCheck->isChecked();
        const bool fwdEnabled = portFwdCheck->isChecked();

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
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted)
        return false;

    net.name = nameEdit->text().trimmed();
    net.gateway = gatewayEdit->text().trimmed();
    net.prefixLength = prefixSpin->value();
    net.enableDhcp = dhcpCheck->isChecked();
    net.dhcpRangeStart = dhcpStartEdit->text().trimmed();
    net.dhcpRangeEnd = dhcpEndEdit->text().trimmed();
    net.enableNat = natCheck->isChecked();

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

        networkTable->setItem(row, 3, new QTableWidgetItem(net.enableNat ? i18n("Yes") : i18n("No")));

        QString statusText;
        if (m_networkManager->isRunning(net.name)) {
            const int count = m_networkManager->connectedVmCount(net.name);
            statusText = i18np("Running (%1 VM)", "Running (%1 VMs)", count);
        } else {
            statusText = i18n("Stopped");
        }
        networkTable->setItem(row, 4, new QTableWidgetItem(statusText));
    }
}

void SettingsDialog::updateToolVersion(int index)
{
    if (index < 0 || index >= m_toolWidgets.size())
        return;

    const auto &tw = m_toolWidgets[index];
    if (ExternalTools::sandboxIrrelevantTools().contains(tw.tool)) {
        tw.statusLabel->setText(i18n("Not used in the sandboxed version."));
        return;
    }
    if (!tw.pathEdit || !tw.statusLabel)
        return;

    const QString path = tw.pathEdit->text();
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
