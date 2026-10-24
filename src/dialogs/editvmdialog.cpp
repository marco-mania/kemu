/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "editvmdialog.h"

#include "const.h"
#include "dialogs/diskdialog.h"
#include "dialogs/settingsdialog.h"
#include "utils/diskkeystore.h"
#include "utils/externaltools.h"
#include "utils/qemuimgprocess.h"
#include "utils/sandboxcapabilities.h"
#include "utils/snapshotmanager.h"
#include "utils/tpmstate.h"
#include "utils/utils.h"
#include "utils/vmconfig.h"

#include <KIconUtils>
#include <KMessageBox>

#include <QScopeGuard>
#include <QStyle>

namespace
{
// Select the combo entry whose userData matches the given enum value.
template<typename E>
void setCombo(QComboBox *combo, E enumValue)
{
    const int idx = combo->findData(static_cast<int>(enumValue));
    if (idx >= 0)
        combo->setCurrentIndex(idx);
}
// Text-like widgets stay enabled so their content remains legible and
// selectable; everything else is disabled.
void makeReadOnly(QWidget *widget)
{
    if (auto *combo = qobject_cast<QComboBox *>(widget)) {
        combo->setEnabled(false);
        return; // do not descend into the internal line edit
    }
    if (auto *spin = qobject_cast<QAbstractSpinBox *>(widget)) {
        spin->setReadOnly(true);
        spin->setButtonSymbols(QAbstractSpinBox::NoButtons);
        spin->setFocusPolicy(Qt::NoFocus);
        return;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(widget)) {
        edit->setReadOnly(true);
        return;
    }
    if (auto *edit = qobject_cast<QPlainTextEdit *>(widget)) {
        edit->setReadOnly(true);
        return;
    }
    if (auto *view = qobject_cast<QAbstractItemView *>(widget)) {
        view->setEditTriggers(QAbstractItemView::NoEditTriggers);
        return;
    }
    if (qobject_cast<QAbstractButton *>(widget) || qobject_cast<QAbstractSlider *>(widget)) {
        widget->setEnabled(false);
        return;
    }
    for (QWidget *child : widget->findChildren<QWidget *>(Qt::FindDirectChildrenOnly))
        makeReadOnly(child);
}
} // namespace

EditVmDialog::EditVmDialog(Mode mode, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
{
    setWindowTitle(mode == Mode::Edit ? i18n("Edit Virtual Machine") : i18n("Customise Virtual Machine"));
    resize(640, 560);

    KColorScheme scheme(QPalette::Active, KColorScheme::View);

    auto *mainLayout = new QVBoxLayout(this);
    auto *tabs = new QTabWidget();

    // --- General Tab ---
    auto *generalTab = new QWidget();
    auto *genMainLayout = new QVBoxLayout(generalTab);

    auto *contentWidget = new QWidget();
    contentWidget->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);

    // Top section: Name + Comment on left, Icon on right
    auto *topLayout = new QHBoxLayout();

    auto *topFormLayout = new QFormLayout();
    topFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(i18n("e.g. Win11_Dev"));
    nameEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^[a-zA-Z0-9_\\-. ]*$"_s), this));

    topFormLayout->addRow(i18n("Name:"), nameEdit);

    auto infoStyle = [&](KColorScheme::ForegroundRole role) {
        return u"QLabel { color: %1; font-size: 11px; }"_s.arg(scheme.foreground(role).color().name());
    };

    nameWarningLabel = new QLabel();
    nameWarningLabel->setStyleSheet(infoStyle(KColorScheme::NegativeText));
    nameWarningLabel->setWordWrap(false);
    nameWarningLabel->setFixedHeight(nameWarningLabel->fontMetrics().height() + 2);
    topFormLayout->addRow({}, nameWarningLabel);

    commentEdit = new QPlainTextEdit();
    commentEdit->setPlaceholderText(i18n("Optional notes about this VM"));
    commentEdit->setFixedHeight(90);
    topFormLayout->addRow(i18n("Comment:"), commentEdit);

    topLayout->addLayout(topFormLayout, 1);

    topLayout->addSpacing(12);

    iconSelector = new VmIconSelector();
    topLayout->addWidget(iconSelector, 0, Qt::AlignTop);

    // Remaining form fields
    auto *genLayout = new QFormLayout();
    genLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    osFamilyCombo = new QComboBox();
    osFamilyCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    osFamilyCombo->addItem(i18n("GNU/Linux"), static_cast<int>(VMSpecs::OsFamily::Linux));
    osFamilyCombo->addItem(i18n("Microsoft Windows"), static_cast<int>(VMSpecs::OsFamily::Windows));
    osFamilyCombo->addItem(i18n("BSD"), static_cast<int>(VMSpecs::OsFamily::BSD));
    osFamilyCombo->addItem(i18n("DOS"), static_cast<int>(VMSpecs::OsFamily::DOS));
    osFamilyCombo->addItem(i18n("Other"), static_cast<int>(VMSpecs::OsFamily::Other));
    osFamilyCombo->insertItem(0, i18n("-- Please select --"), -1);
    osFamilyCombo->setCurrentIndex(0);
    genLayout->addRow(i18n("OS Family:"), osFamilyCombo);

    auto *osFamilyModel = qobject_cast<QStandardItemModel *>(osFamilyCombo->model());
    if (osFamilyModel) {
        QStandardItem *item = osFamilyModel->item(0);
        item->setEnabled(false); // Disabled and non-reselectable once selected
    }

    osNameCombo = new QComboBox();
    osNameCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    osNameCombo->setEnabled(false);
    osNameCombo->setEditable(true);
    osNameCombo->completer()->setFilterMode(Qt::MatchContains);
    osNameCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    osNameCombo->completer()->setCompletionMode(QCompleter::PopupCompletion);
    osNameCombo->completer()->setModel(osNameCombo->model());
    osNameCombo->setInsertPolicy(QComboBox::NoInsert);

    auto *nameLayout = new QHBoxLayout();
    nameLayout->addWidget(osNameCombo);

    genLayout->addRow(i18n("OS Name:"), nameLayout);

    osNameLabel = qobject_cast<QLabel *>(genLayout->labelForField(nameLayout));

    diskSpin = new QSpinBox();
    diskSpin->setRange(KEMUConst::DefaultMinDiskSizeGB, 32000);
    diskSpin->setValue(40);
    diskSpin->setSuffix(i18n(" GB"));
    diskSpin->setToolTip(i18n("This is the size of the system disk. You can add additional disks under Hardware."));
    auto *diskLayout = new QHBoxLayout();
    diskLayout->addWidget(diskSpin);
    if (m_mode == Mode::Edit) {
        // Encryption, as small buttons; which ones are shown tells the state.
        // They change the disk at once, so a new VM chooses it elsewhere.
        const auto makeButton = [](const QIcon &icon, const QString &text) {
            auto *button = new QToolButton();
            button->setIcon(icon);
            button->setToolTip(text);
            button->setAccessibleName(text);
            return button;
        };
        encryptButton = makeButton(QIcon::fromTheme(u"lock"_s), i18n("Encrypt the system disk..."));
        decryptButton = makeButton(QIcon::fromTheme(u"unlock"_s, QIcon::fromTheme(u"object-unlocked"_s)), i18n("Decrypt the system disk..."));
        changePasswordButton = makeButton(QIcon::fromTheme(u"dialog-password"_s, QIcon::fromTheme(u"lock"_s)), i18n("Change the password..."));
        walletButton = new QToolButton();

        // Resizing and compacting act at once as well. A larger value in the
        // spin box only takes effect through the expand button.
        expandDiskButton = makeButton(QIcon::fromTheme(u"zoom-in"_s, QIcon::fromTheme(u"list-add"_s)), i18n("Expand the system disk..."));
        compactDiskButton = makeButton(QIcon::fromTheme(u"edit-clear-history"_s), i18n("Compact the system disk..."));
        expandDiskButton->setEnabled(false);
        diskLayout->addWidget(expandDiskButton);
        diskLayout->addWidget(compactDiskButton);
        connect(diskSpin, &QSpinBox::valueChanged, this, &EditVmDialog::updateDiskSizeButtons);
        connect(expandDiskButton, &QToolButton::clicked, this, [this] {
            Q_EMIT expandDiskRequested(systemDiskPath(), diskSpin->value(), this);
        });
        connect(compactDiskButton, &QToolButton::clicked, this, [this] {
            Q_EMIT compactDiskRequested(systemDiskPath(), this);
        });

        diskLayout->addSpacing(diskLayout->spacing());
        diskLayout->addWidget(encryptButton);
        diskLayout->addWidget(decryptButton);
        diskLayout->addWidget(changePasswordButton);
        diskLayout->addWidget(walletButton);

        connect(encryptButton, &QToolButton::clicked, this, &EditVmDialog::encryptSystemDiskRequested);
        connect(decryptButton, &QToolButton::clicked, this, &EditVmDialog::decryptSystemDiskRequested);
        connect(changePasswordButton, &QToolButton::clicked, this, &EditVmDialog::changeDiskPasswordRequested);
        connect(walletButton, &QToolButton::clicked, this, [this] {
            if (DiskKeyStore::hasKey(m_original.diskKeyId()))
                Q_EMIT forgetDiskPasswordRequested();
            else
                Q_EMIT storeDiskPasswordRequested();
        });
    }
    diskLayout->addStretch();
    genLayout->addRow(i18n("System Disk:"), diskLayout);
    diskLabel = qobject_cast<QLabel *>(genLayout->labelForField(diskLayout));

    isoPathRequester = new KUrlRequester();
    isoPathRequester->lineEdit()->setPlaceholderText(i18n("Leave empty for HDD boot"));
    isoPathRequester->lineEdit()->setClearButtonEnabled(true);
    isoPathRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    isoPathRequester->setNameFilter(i18n("ISO Images (*.iso)"));
    genLayout->addRow(i18n("ISO Image:"), isoPathRequester);
    isoPathLabel = qobject_cast<QLabel *>(genLayout->labelForField(isoPathRequester));

    bootFromIsoCheck = new QCheckBox(i18n("Boot from ISO"));
    bootFromIsoCheck->setToolTip(i18n("If checked, the VM boots from the ISO image instead of the hard disk."));
    bootFromIsoCheck->setEnabled(!isoPathRequester->text().isEmpty());
    genLayout->addRow({}, bootFromIsoCheck);

    floppyPathRequester = new KUrlRequester();
    floppyPathRequester->lineEdit()->setPlaceholderText(i18n("Leave empty if not needed"));
    floppyPathRequester->lineEdit()->setClearButtonEnabled(true);
    floppyPathRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    floppyPathRequester->setNameFilter(i18n("Floppy Images (*.img *.ima *.flp *.vfd);;All Files (*)"));
    genLayout->addRow(i18n("Floppy Image:"), floppyPathRequester);
    floppyPathLabel = qobject_cast<QLabel *>(genLayout->labelForField(floppyPathRequester));

    bootFromFloppyCheck = new QCheckBox(i18n("Boot from Floppy"));
    bootFromFloppyCheck->setToolTip(i18n("If checked, the VM boots from the floppy image (drive A:)."));
    bootFromFloppyCheck->setEnabled(false);
    genLayout->addRow({}, bootFromFloppyCheck);

    auto *shareLayout = new QHBoxLayout();
    sharedPathEdit = new QLineEdit();
    sharedPathEdit->setPlaceholderText(i18n("Host directory"));
    auto *btnBrowseShare = new QPushButton(QIcon::fromTheme(u"document-open"_s), {});
    sharedPathEdit->setClearButtonEnabled(true);
    shareLayout->addWidget(sharedPathEdit);
    shareLayout->addWidget(btnBrowseShare);
    shareToolWarning = new QLabel();
    shareToolWarning->hide();
    shareLayout->addWidget(shareToolWarning);
    genLayout->addRow(i18n("Shared Folder (VirtFS):"), shareLayout);

    sharedNameEdit = new QLineEdit();
    sharedNameEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^[a-zA-Z0-9]*$"_s), this));
    sharedNameEdit->setClearButtonEnabled(true);
    genLayout->addRow(i18n("Shared Name (for guest):"), sharedNameEdit);

    sharedNameEdit->setEnabled(!sharedPathEdit->text().isEmpty());

    contentLayout->addLayout(topLayout);
    contentLayout->addLayout(genLayout);
    genMainLayout->addWidget(contentWidget);
    genMainLayout->addStretch();

    tabs->addTab(generalTab, QIcon::fromTheme(u"computer"_s), i18n("General"));

    // --- System Tab ---
    auto *sysTab = new QWidget();
    auto *sysLayout = new QFormLayout(sysTab);
    sysLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    cpuCombo = new QComboBox();
    {
        const QMetaEnum metaEnum = QMetaEnum::fromType<VMSpecs::CpuModel>();
        for (int i = 0; i < metaEnum.keyCount(); ++i) {
            auto model = static_cast<VMSpecs::CpuModel>(metaEnum.value(i));
            cpuCombo->addItem(VMConfig::toUIString(model), static_cast<int>(model));
        }
    }
    sysLayout->addRow(i18n("CPU Model:"), cpuCombo);

    cpuSpin = new QSpinBox();
    cpuSpin->setRange(1, 64);
    cpuSpin->setValue(2);
    cpuSpin->setSuffix(i18n(" Cores"));
    sysLayout->addRow(i18n("CPU:"), cpuSpin);

    kvmCheck = new QCheckBox(i18n("Enable KVM hardware acceleration"));
    kvmCheck->setChecked(true);
    kvmCheck->setToolTip(
        i18n("Uses KVM for near-native performance. Disable for pure "
             "software emulation (TCG), which is much slower but does "
             "not require KVM and supports all CPU models."));
    sysLayout->addRow({}, kvmCheck);

    machineCombo = new QComboBox();
    machineCombo->addItem(VMConfig::toUIString(VMSpecs::MachineType::Q35), static_cast<int>(VMSpecs::MachineType::Q35));
    machineCombo->addItem(VMConfig::toUIString(VMSpecs::MachineType::Pc), static_cast<int>(VMSpecs::MachineType::Pc));
    machineCombo->addItem(VMConfig::toUIString(VMSpecs::MachineType::PcISA), static_cast<int>(VMSpecs::MachineType::PcISA));
    sysLayout->addRow(i18n("Chipset:"), machineCombo);

    uefiCheck = new QCheckBox(i18n("Use UEFI firmware (OVMF)"));
    uefiCheck->setToolTip(i18n("Required for Windows 11. Creates a separate VARS file."));
    sysLayout->addRow({}, uefiCheck);

    localTimeCheck = new QCheckBox(i18n("Use local time"));
    localTimeCheck->setToolTip(
        i18n("Sets the hardware clock to local time instead of UTC.\nThis should "
             "be set for Windows/DOS guests."));
    sysLayout->addRow({}, localTimeCheck);

    tpmCheck = new QCheckBox(i18n("Emulate TPM 2.0 (swtpm)"));
    tpmCheck->setToolTip(i18n("Required for Windows 11. Uses swtpm as software TPM emulator."));
    tpmToolWarning = new QLabel();
    tpmToolWarning->hide();
    auto *tpmLayout = new QHBoxLayout();
    tpmLayout->addWidget(tpmCheck);
    tpmLayout->addWidget(tpmToolWarning);
    tpmLayout->addStretch();
    sysLayout->addRow({}, tpmLayout);

    // Secure Boot
    secureBootCheck = new QCheckBox(i18n("Enable Secure Boot"));
    secureBootCheck->setToolTip(
        i18n("Requires UEFI and Q35 chipset. Uses OVMF "
             "firmware with enrolled Microsoft keys."));
    secureBootCheck->setEnabled(false); // Initially disabled until UEFI+Q35
    sysLayout->addRow({}, secureBootCheck);

    // ACPI
    acpiCheck = new QCheckBox(i18n("Enable ACPI"));
    acpiCheck->setChecked(true);
    acpiCheck->setToolTip(
        i18n("Advanced Configuration and Power Interface. Disable for "
             "legacy guest OSes (e.g. Windows NT4, DOS) that do not "
             "support ACPI and may fail to boot with it enabled."));
    sysLayout->addRow({}, acpiCheck);

    exitActionCombo = new QComboBox(this);
    for (const auto action : {VMSpecs::ExitAction::SaveState, VMSpecs::ExitAction::Shutdown, VMSpecs::ExitAction::PowerOff}) {
        exitActionCombo->addItem(VMConfig::toUIString(action), QVariant::fromValue(action));
    }
    exitActionCombo->setToolTip(
        i18n("What to do with this machine if it is still running when KEMU quits. "
             "A confirmation dialog lets you override this."));
    sysLayout->addRow(i18n("When KEMU exits:"), exitActionCombo);

    guestShutdownActionCombo = new QComboBox(this);
    for (const auto action : {VMSpecs::GuestShutdownAction::None, VMSpecs::GuestShutdownAction::ShutdownHost}) {
        guestShutdownActionCombo->addItem(VMConfig::toUIString(action), QVariant::fromValue(action));
    }
    const auto &caps = SandboxCapabilities::instance();
    if (caps.has(SandboxCapabilities::HostPowerOff)) {
        guestShutdownActionCombo->setToolTip(
            i18n("Shut down the host once the guest operating system has powered itself off. "
                 "Shutting the machine down from KEMU never does this, and a countdown lets you "
                 "cancel. This can also be changed for a single run in the VM menu."));
    } else {
        // Keep the stored value: the VM may move to a host where this works.
        guestShutdownActionCombo->setEnabled(false);
        guestShutdownActionCombo->setToolTip(caps.unavailableReason(SandboxCapabilities::HostPowerOff));
    }
    sysLayout->addRow(i18n("When the guest shuts down:"), guestShutdownActionCombo);

    // RAM
    auto *ramContainer = new QVBoxLayout();
    auto *ramSliderLayout = new QHBoxLayout();

    const QList<int> ramSteps = RamSpinBox::steps();

    ramSlider = new QSlider(Qt::Horizontal);
    ramSlider->setRange(0, ramSteps.size() - 1);
    ramSlider->setTickPosition(QSlider::TicksBelow);
    ramSlider->setTickInterval(1);

    ramSpin = new RamSpinBox();
    ramSpin->setValue(GlobalDefaults.ramMb);
    ramSliderLayout->addWidget(ramSlider);
    ramSliderLayout->addWidget(ramSpin);

    balloonCheck = new QCheckBox(i18n("VirtIO Memory Ballooning"));
    balloonCheck->setChecked(true);
    balloonCheck->setToolTip(
        i18n("Allows the host system to dynamically adjust the VM's memory based "
             "on its actual usage.\nEnable this to efficiently share memory "
             "(RAM) among multiple virtual machines."));
    preallocCheck = new QCheckBox(i18n("Pre-allocate Memory"));
    preallocCheck->setToolTip(
        i18n("Allocates the full amount of configured memory (RAM) immediately "
             "when the VM starts.\nThis improves guest performance but strictly "
             "reserves the memory on the host."));

    ramContainer->addLayout(ramSliderLayout);
    ramContainer->addWidget(balloonCheck);
    ramContainer->addWidget(preallocCheck);
    sysLayout->addRow(i18n("Memory:"), ramContainer);

    tabs->addTab(sysTab, QIcon::fromTheme(u"preferences-system"_s), i18n("System"));

    // --- Hardware Tab ---
    auto *hwTab = new QWidget();
    auto *hwLayout = new QFormLayout(hwTab);
    hwLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    diskInterfaceCombo = new QComboBox();
    diskInterfaceCombo->addItem(VMConfig::toUIString(VMSpecs::DiskInterface::Virtio), static_cast<int>(VMSpecs::DiskInterface::Virtio));
    diskInterfaceCombo->addItem(VMConfig::toUIString(VMSpecs::DiskInterface::Nvme), static_cast<int>(VMSpecs::DiskInterface::Nvme));
    diskInterfaceCombo->addItem(VMConfig::toUIString(VMSpecs::DiskInterface::SataIde), static_cast<int>(VMSpecs::DiskInterface::SataIde));
    diskInterfaceCombo->setToolTip(i18n("This is the disc controller of the system disk."));
    hwLayout->addRow(i18n("Disk Controller:"), diskInterfaceCombo);

    diskHintLabel = new QLabel();
    diskHintLabel->setStyleSheet(infoStyle(KColorScheme::NeutralText));
    hwLayout->addRow({}, diskHintLabel);

    diskHintLabel->setVisible(false);

    auto *extraDisksWidget = new QWidget();
    auto *extraDisksLayout = new QVBoxLayout(extraDisksWidget);
    extraDisksLayout->setContentsMargins(0, 0, 0, 0);

    diskTree = new QTreeWidget();
    diskTree->setHeaderLabels({i18n("Name"), i18n("Size"), i18n("Controller")});
    diskTree->setRootIsDecorated(false);
    diskTree->setUniformRowHeights(true);
    diskTree->setSelectionMode(QAbstractItemView::SingleSelection);
    diskTree->setMaximumHeight(diskTree->fontMetrics().height() * 8);
    diskTree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    diskTree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    diskTree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    extraDisksLayout->addWidget(diskTree);

    auto *diskButtonsLayout = new QHBoxLayout();
    addDiskButton = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18n("Add Disk…"));
    editDiskButton = new QPushButton(QIcon::fromTheme(u"document-properties"_s), i18n("Properties…"));
    removeDiskButton = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18n("Remove"));
    diskButtonsLayout->addWidget(addDiskButton);
    diskButtonsLayout->addWidget(editDiskButton);
    diskButtonsLayout->addWidget(removeDiskButton);
    diskButtonsLayout->addStretch();
    extraDisksLayout->addLayout(diskButtonsLayout);

    storageWarningLabel = new QLabel();
    storageWarningLabel->setStyleSheet(infoStyle(KColorScheme::NegativeText));
    storageWarningLabel->setWordWrap(true);
    storageWarningLabel->setVisible(false);
    extraDisksLayout->addWidget(storageWarningLabel);

    hwLayout->addRow(i18n("Additional Disks:"), extraDisksWidget);
    // A VM that does not exist yet has no folder to put further disks into.
    hwLayout->setRowVisible(extraDisksWidget, m_mode == Mode::Edit);

    connect(addDiskButton, &QPushButton::clicked, this, &EditVmDialog::addDisk);
    connect(editDiskButton, &QPushButton::clicked, this, &EditVmDialog::editSelectedDisk);
    connect(removeDiskButton, &QPushButton::clicked, this, &EditVmDialog::removeSelectedDisk);
    connect(diskTree, &QTreeWidget::itemSelectionChanged, this, &EditVmDialog::updateDiskButtons);
    connect(diskTree, &QTreeWidget::itemDoubleClicked, this, &EditVmDialog::editSelectedDisk);

    usbCombo = new QComboBox();
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::Xhci), static_cast<int>(VMSpecs::UsbController::Xhci));
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::Ehci), static_cast<int>(VMSpecs::UsbController::Ehci));
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::Default), static_cast<int>(VMSpecs::UsbController::Default));
    usbCombo->addItem(VMConfig::toUIString(VMSpecs::UsbController::None), static_cast<int>(VMSpecs::UsbController::None));
    hwLayout->addRow(i18n("USB Controller:"), usbCombo);
    usbControllerLabel = qobject_cast<QLabel *>(hwLayout->labelForField(usbCombo));

    mouseCombo = new QComboBox();
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::Ps2), static_cast<int>(VMSpecs::PointerDevice::Ps2));
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::UsbMouse), static_cast<int>(VMSpecs::PointerDevice::UsbMouse));
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::UsbTablet), static_cast<int>(VMSpecs::PointerDevice::UsbTablet));
    mouseCombo->addItem(VMConfig::toUIString(VMSpecs::PointerDevice::VirtioTablet), static_cast<int>(VMSpecs::PointerDevice::VirtioTablet));
    mouseCombo->setToolTip(
        i18n("PS/2 and USB mouse are relative pointers; tablets give absolute "
             "positioning (seamless, no grab). USB options need a USB controller, "
             "VirtIO needs PCI and a virtio input driver in the guest."));
    setCombo(mouseCombo, GlobalDefaults.pointerDevice);
    hwLayout->addRow(i18n("Mouse:"), mouseCombo);

    keyboardCombo = new QComboBox();
    keyboardCombo->addItem(VMConfig::toUIString(VMSpecs::KeyboardDevice::Ps2), static_cast<int>(VMSpecs::KeyboardDevice::Ps2));
    keyboardCombo->addItem(VMConfig::toUIString(VMSpecs::KeyboardDevice::Usb), static_cast<int>(VMSpecs::KeyboardDevice::Usb));
    keyboardCombo->setToolTip(i18n("USB keyboard needs a USB controller. Legacy guests require PS/2."));
    setCombo(keyboardCombo, GlobalDefaults.keyboardDevice);
    hwLayout->addRow(i18n("Keyboard:"), keyboardCombo);

    displayCombo = new QComboBox();
    // Integrated Viewer first: it is the recommended default, SDL is the
    // fallback for a QEMU build without dbus-display support, or for running
    // the exported qemu.sh script standalone, without KEMU as its peer.
    for (const auto type : {VMSpecs::DisplayType::Dbus, VMSpecs::DisplayType::Sdl}) {
        if (VMConfig::isDisplayTypeAvailable(type))
            displayCombo->addItem(VMConfig::toUIString(type), static_cast<int>(type));
    }
    displayCombo->setToolTip(
        i18n("The integrated viewer is recommended for everyday use. SDL opens "
             "a native QEMU window outside KEMU - useful for the standalone qemu.sh "
             "script, or if your QEMU build lacks dbus-display support."));
    hwLayout->addRow(i18n("Display:"), displayCombo);

    gpuCombo = new QComboBox();
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Vga), static_cast<int>(VMSpecs::GpuType::Vga));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Virtio), static_cast<int>(VMSpecs::GpuType::Virtio));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Qxl), static_cast<int>(VMSpecs::GpuType::Qxl));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::CirrusGD5446), static_cast<int>(VMSpecs::GpuType::CirrusGD5446));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::CirrusGD5430), static_cast<int>(VMSpecs::GpuType::CirrusGD5430));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::VgaISA), static_cast<int>(VMSpecs::GpuType::VgaISA));
    hwLayout->addRow(i18n("Graphics:"), gpuCombo);

    gpuMemSpin = new QSpinBox();
    gpuMemSpin->setValue(64);
    gpuMemSpin->setSuffix(i18n(" MiB"));
    vgaMemLabel = new QLabel(i18n("GPU Memory:"));
    hwLayout->addRow(vgaMemLabel, gpuMemSpin);

    accel3dCheck = new QCheckBox(i18n("3D acceleration (GL)"));
    accel3dCheck->setToolTip(
        i18n("Enables GPU-accelerated 3D rendering via virtio-vga-gl.\n"
             "Requires VirtIO GPU with the SDL display or the integrated viewer.\n"
             "Not available with Standard VGA/QXL."));
    accel3dCheck->setChecked(false);
    hwLayout->addRow(u""_s, accel3dCheck);

    gpuHintLabel = new QLabel();
    gpuHintLabel->setStyleSheet(infoStyle(KColorScheme::NeutralText));
    hwLayout->addRow(gpuHintLabel);

    resolutionCombo = new QComboBox();
    resolutionCombo->addItem(u"1280 × 800"_s, QSize(1280, 800));
    resolutionCombo->addItem(u"1280 × 720 (HD)"_s, QSize(1280, 720));
    resolutionCombo->addItem(u"1366 × 768"_s, QSize(1366, 768));
    resolutionCombo->addItem(u"1440 × 900"_s, QSize(1440, 900));
    resolutionCombo->addItem(u"1600 × 900"_s, QSize(1600, 900));
    resolutionCombo->addItem(u"1920 × 1080 (Full HD)"_s, QSize(1920, 1080));
    resolutionCombo->addItem(u"2560 × 1440 (QHD)"_s, QSize(2560, 1440));
    resolutionCombo->addItem(u"3840 × 2160 (4K)"_s, QSize(3840, 2160));
    const QSize defaultRes = SettingsDialog::getDefaultResolution();
    for (int i = 0; i < resolutionCombo->count(); ++i) {
        if (resolutionCombo->itemData(i).toSize() == defaultRes) {
            resolutionCombo->setCurrentIndex(i);
            break;
        }
    }
    hwLayout->addRow(i18n("Initial Resolution:"), resolutionCombo);
    resolutionLabel = qobject_cast<QLabel *>(hwLayout->labelForField(resolutionCombo));

    audioCombo = new QComboBox();
    {
        const QMetaEnum metaEnum = QMetaEnum::fromType<VMSpecs::AudioDevice>();
        for (int i = 0; i < metaEnum.keyCount(); ++i) {
            auto model = static_cast<VMSpecs::AudioDevice>(metaEnum.value(i));
            audioCombo->addItem(VMConfig::toUIString(model), static_cast<int>(model));
        }
    }
    audioCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    audioCombo->setCurrentIndex(1);

    sb16ConfigButton = new QPushButton(QIcon::fromTheme(u"configure"_s), {});
    sb16ConfigButton->setToolTip(i18n("Configure SB16 ISA resources"));
    sb16ConfigButton->setFixedWidth(sb16ConfigButton->sizeHint().height());
    sb16ConfigButton->setEnabled(false);

    auto *audioRow = new QHBoxLayout();
    audioRow->addWidget(audioCombo);
    audioRow->addWidget(sb16ConfigButton);
    audioRow->addStretch();

    hwLayout->addRow(i18n("Audio:"), audioRow);

    tabs->addTab(hwTab, QIcon::fromTheme(u"preferences-desktop-peripherals"_s), i18n("Hardware"));

    // --- Network Tab ---
    auto *netTab = new QWidget();
    auto *netMainLayout = new QVBoxLayout(netTab);

    auto *netFormLayout = new QFormLayout();
    netFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    netCombo = new QComboBox();
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::None), static_cast<int>(VMSpecs::NetworkCard::None));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Virtio), static_cast<int>(VMSpecs::NetworkCard::Virtio));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::E1000), static_cast<int>(VMSpecs::NetworkCard::E1000));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Rtl8139), static_cast<int>(VMSpecs::NetworkCard::Rtl8139));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Pcnet), static_cast<int>(VMSpecs::NetworkCard::Pcnet));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Rtl8029), static_cast<int>(VMSpecs::NetworkCard::Rtl8029));
    netCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkCard::Ne2000), static_cast<int>(VMSpecs::NetworkCard::Ne2000));
    netFormLayout->addRow(i18n("Network Adapter:"), netCombo);

    networkModeCombo = new QComboBox();
    networkModeCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkMode::UserMode), static_cast<int>(VMSpecs::NetworkMode::UserMode));
    networkModeCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkMode::VirtualNetwork), static_cast<int>(VMSpecs::NetworkMode::VirtualNetwork));
    netFormLayout->addRow(i18n("Network Mode:"), networkModeCombo);

    auto *macLayout = new QHBoxLayout();
    macEdit = new QLineEdit();
    macEdit->setPlaceholderText(u"52:54:00:xx:xx:xx"_s);
    macEdit->setClearButtonEnabled(false);

    regenerateMacButton = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), {});
    regenerateMacButton->setToolTip(i18n("Generate a new random MAC address"));
    regenerateMacButton->setFixedWidth(regenerateMacButton->sizeHint().height());

    macLayout->addWidget(macEdit);
    macLayout->addWidget(regenerateMacButton);

    netFormLayout->addRow(i18n("MAC Address:"), macLayout);

    macWarningLabel = new QLabel();
    macWarningLabel->setStyleSheet(u"QLabel { color: %1; font-size: 11px; }"_s.arg(scheme.foreground(KColorScheme::NegativeText).color().name()));
    macWarningLabel->setWordWrap(false);
    macWarningLabel->setFixedHeight(macWarningLabel->fontMetrics().height() + 2);
    netFormLayout->addRow({}, macWarningLabel);

    netMainLayout->addLayout(netFormLayout);

    // --- User Mode Section (Port Forwarding) ---
    userModeGroup = new QGroupBox(i18n("Port Forwarding (Host → Guest)"));
    auto *pfLayout = new QVBoxLayout(userModeGroup);

    portForwardTable = new QTableWidget(0, 3);
    portForwardTable->setHorizontalHeaderLabels({i18n("Protocol"), i18n("Host Port"), i18n("Guest Port")});
    portForwardTable->horizontalHeader()->setStretchLastSection(true);
    portForwardTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    portForwardTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    portForwardTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    portForwardTable->verticalHeader()->setVisible(false);
    portForwardTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    portForwardTable->setSelectionMode(QAbstractItemView::SingleSelection);
    pfLayout->addWidget(portForwardTable);

    auto *pfButtonLayout = new QHBoxLayout();
    addPortFwdButton = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18n("Add Rule"));
    removePortFwdButton = new QPushButton(QIcon::fromTheme(u"list-remove"_s), i18n("Remove"));
    removePortFwdButton->setEnabled(false);
    pfButtonLayout->addWidget(addPortFwdButton);
    pfButtonLayout->addWidget(removePortFwdButton);
    pfButtonLayout->addStretch();
    pfLayout->addLayout(pfButtonLayout);

    netMainLayout->addWidget(userModeGroup);

    // --- Virtual Network Section ---
    vdeGroup = new QGroupBox(i18n("Virtual Network"));
    auto *vdeLayout = new QFormLayout(vdeGroup);
    vdeLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    virtualNetworkCombo = new QComboBox();
    vdeLayout->addRow(i18n("Network:"), virtualNetworkCombo);

    hostnameEdit = new QLineEdit();
    hostnameEdit->setClearButtonEnabled(false);
    hostnameEdit->setToolTip(i18n("DNS hostname of this VM inside the virtual network (\"<hostname>.<DNS domain of the network>\")"));
    vdeLayout->addRow(i18n("Hostname:"), hostnameEdit);

    hostnameWarningLabel = new QLabel();
    hostnameWarningLabel->setStyleSheet(u"QLabel { color: %1; font-size: 11px; }"_s.arg(scheme.foreground(KColorScheme::NegativeText).color().name()));
    hostnameWarningLabel->setWordWrap(false);
    hostnameWarningLabel->setFixedHeight(hostnameWarningLabel->fontMetrics().height() + 2);
    vdeLayout->addRow({}, hostnameWarningLabel);

    netMainLayout->addWidget(vdeGroup);

    userModeGroup->setVisible(true);
    vdeGroup->setVisible(false);

    netMainLayout->addStretch();

    tabs->addTab(netTab, QIcon::fromTheme(u"network-wired"_s), i18n("Network"));

    // --- Advanced Tab ---
    auto *advTab = new QWidget();
    auto *advMainLayout = new QVBoxLayout(advTab);
    auto *advLayout = new QFormLayout();
    advLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    extraArgsEdit = new QLineEdit();
    extraArgsEdit->setPlaceholderText(i18n("Additional QEMU parameters"));
    extraArgsEdit->setClearButtonEnabled(true);
    advLayout->addRow(i18n("Extra Arguments:"), extraArgsEdit);

    guestAgentCheck = new QCheckBox(i18n("Connect the QEMU guest agent"));
    guestAgentCheck->setToolTip(
        i18n("Adds a channel for the QEMU guest agent, so KEMU can see whether it runs in the guest. Linux guests need the package "
             "qemu-guest-agent, Windows guests get it with the virtio-win guest tools."));
    advLayout->addRow({}, guestAgentCheck);
    advMainLayout->addLayout(advLayout);

    auto *identityGroup = new QGroupBox(i18n("System Identity (SMBIOS)"));
    auto *identityLayout = new QFormLayout(identityGroup);
    identityLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    auto *uuidLayout = new QHBoxLayout();
    uuidEdit = new QLineEdit();
    uuidEdit->setPlaceholderText(m_mode == Mode::Edit ? i18n("Not set (all zeros)") : i18n("Assigned when the VM is created"));
    uuidEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^\\{?[0-9A-Fa-f-]{0,36}\\}?$"_s), this));

    regenerateUuidButton = new QPushButton(QIcon::fromTheme(u"view-refresh"_s), {});
    regenerateUuidButton->setToolTip(i18n("Generate a new random UUID"));
    regenerateUuidButton->setFixedWidth(regenerateUuidButton->sizeHint().height());

    uuidLayout->addWidget(uuidEdit);
    uuidLayout->addWidget(regenerateUuidButton);
    identityLayout->addRow(i18n("UUID:"), uuidLayout);

    uuidWarningLabel = new QLabel();
    uuidWarningLabel->setStyleSheet(infoStyle(KColorScheme::NegativeText));
    uuidWarningLabel->setVisible(false);
    identityLayout->addRow({}, uuidWarningLabel);

    const QList<std::pair<QString SmbiosSystemInfo::*, QString>> smbiosLabels = {
        {&SmbiosSystemInfo::manufacturer, i18n("Manufacturer:")},
        {&SmbiosSystemInfo::product, i18n("Product:")},
        {&SmbiosSystemInfo::version, i18n("Version:")},
        {&SmbiosSystemInfo::serial, i18n("Serial Number:")},
        {&SmbiosSystemInfo::sku, i18n("SKU:")},
        {&SmbiosSystemInfo::family, i18n("Family:")},
    };
    const QRegularExpression smbiosRx(u"^[\\x20-\\x7e]*$"_s);
    for (const auto &[member, label] : smbiosLabels) {
        auto *edit = new QLineEdit();
        edit->setPlaceholderText(i18nc("@info:placeholder SMBIOS field", "QEMU default"));
        edit->setMaxLength(KEMUConst::MaxSmbiosStringLength);
        edit->setValidator(new QRegularExpressionValidator(smbiosRx, edit));
        identityLayout->addRow(label, edit);
        m_smbiosEdits.append({member, edit});
    }

    auto *identityHint =
        new QLabel(i18n("The guest sees these values as its hardware identity. Changing them on an installed system can affect software licensing and "
                        "Windows activation."));
    identityHint->setWordWrap(true);
    identityHint->setStyleSheet(infoStyle(KColorScheme::InactiveText));
    identityLayout->addRow(identityHint);

    advMainLayout->addWidget(identityGroup);
    advMainLayout->addStretch();

    tabs->addTab(advTab, QIcon::fromTheme(u"configure"_s), i18n("Advanced"));

    mainLayout->addWidget(tabs);

    connect(nameEdit, &QLineEdit::textChanged, this, &EditVmDialog::onNameChanged);
    connect(osFamilyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::onOsFamilyChanged);
    connect(osFamilyCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::updateToolWarnings);
    connect(tpmCheck, &QCheckBox::toggled, this, &EditVmDialog::updateToolWarnings);
    connect(sharedPathEdit, &QLineEdit::textChanged, this, &EditVmDialog::updateToolWarnings);
    connect(osNameCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::onOsNameChanged);
    connect(osNameCombo, &QComboBox::editTextChanged, this, [this]() {
        updateIconForOs();
        validateForm();
    });

    connect(isoPathRequester, &KUrlRequester::textChanged, this, [this](const QString &text) {
        const bool hasIso = !text.trimmed().isEmpty();
        bootFromIsoCheck->setEnabled(hasIso);
        bootFromIsoCheck->setChecked(hasIso);
    });

    connect(floppyPathRequester, &KUrlRequester::textChanged, this, [this]() {
        const bool hasFloppy = !floppyPathRequester->text().trimmed().isEmpty();
        bootFromFloppyCheck->setEnabled(hasFloppy);
        if (!hasFloppy)
            bootFromFloppyCheck->setChecked(false);
        if (hasFloppy)
            bootFromFloppyCheck->setChecked(true);
        validateForm();
    });

    connect(bootFromFloppyCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked)
            bootFromIsoCheck->setChecked(false);
    });
    connect(bootFromIsoCheck, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked)
            bootFromFloppyCheck->setChecked(false);
    });

    connect(btnBrowseShare, &QPushButton::clicked, this, &EditVmDialog::browseSharedDir);
    connect(sharedPathEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        sharedNameEdit->setEnabled(!text.isEmpty());
        if (!text.isEmpty() && sharedNameEdit->text().isEmpty()) {
            QString folderName = QFileInfo(text).fileName();
            if (folderName.isEmpty()) {
                folderName = u"exchange"_s;
            }
            sharedNameEdit->setText(folderName);
        }
        validateForm();
    });
    connect(sharedNameEdit, &QLineEdit::textChanged, this, [this] {
        validateForm();
    });

    connect(kvmCheck, &QCheckBox::toggled, this, [this](bool enabled) {
        auto *model = qobject_cast<QStandardItemModel *>(cpuCombo->model());
        if (!model)
            return;

        const int hostIdx = cpuCombo->findData(static_cast<int>(VMSpecs::CpuModel::Host));
        if (hostIdx >= 0) {
            QStandardItem *item = model->item(hostIdx);
            item->setEnabled(enabled);
            if (!enabled && cpuCombo->currentIndex() == hostIdx) {
                const int fallback = cpuCombo->findData(static_cast<int>(VMSpecs::CpuModel::Qemu64));
                if (fallback >= 0)
                    cpuCombo->setCurrentIndex(fallback);
            }
        }
    });

    connect(cpuCombo, &QComboBox::currentIndexChanged, this, [this]() {
        const auto selectedCpu = static_cast<VMSpecs::CpuModel>(cpuCombo->currentData().toInt());
        cpuSpin->setMaximum(VMConfig::maxCoresForModel(selectedCpu));

        const auto currentMachine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());

        // Only switch the chipset automatically if the new CPU is incompatible with
        // the currently selected chipset.
        if (!VMConfig::isCpuCompatible(selectedCpu, currentMachine)) {
            const auto recommendedMachine = VMConfig::recommendedMachineType(selectedCpu);
            const int idx = machineCombo->findData(static_cast<int>(recommendedMachine));
            if (idx >= 0) {
                machineCombo->setCurrentIndex(idx);
            }
        }
    });

    connect(ramSlider, &QSlider::valueChanged, this, [this, ramSteps](int index) {
        if (index >= 0 && index < ramSteps.size()) {
            const QSignalBlocker blocker(ramSpin);
            ramSpin->setValue(ramSteps[index]);
        }
    });
    auto updateRamSlider = [this, ramSteps](int mb) {
        int bestIndex = 0;
        int minDiff = std::numeric_limits<int>::max();

        for (int i = 0; i < ramSteps.size(); ++i) {
            const int diff = std::abs(ramSteps[i] - mb);
            if (diff < minDiff) {
                minDiff = diff;
                bestIndex = i;
            }
        }
        const QSignalBlocker blocker(ramSlider);
        ramSlider->setValue(bestIndex);
    };
    connect(ramSpin, &RamSpinBox::valueChanged, this, updateRamSlider);

    connect(diskInterfaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::updateDiskInterfaceHint);

    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::updateDiskInterfaceHint);
    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::onMachineTypeChanged);
    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::validateForm);
    connect(diskInterfaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::validateForm);

    connect(usbCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateInputDeviceAvailability();
    });

    auto updateSecureBootState = [this]() {
        const bool uefiOn = uefiCheck->isChecked();
        const bool q35 = (machineCombo->currentData().toInt() == static_cast<int>(VMSpecs::MachineType::Q35));
        secureBootCheck->setEnabled(uefiOn && q35);
        if (!uefiOn || !q35) {
            secureBootCheck->setChecked(false);
        }
    };
    connect(uefiCheck, &QCheckBox::toggled, this, updateSecureBootState);
    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, updateSecureBootState);
    connect(gpuCombo, &QComboBox::currentIndexChanged, this, &EditVmDialog::onGpuDisplayChanged);
    connect(displayCombo, &QComboBox::currentIndexChanged, this, &EditVmDialog::onGpuDisplayChanged);
    connect(accel3dCheck, &QCheckBox::toggled, this, &EditVmDialog::onGpuDisplayChanged);
    connect(audioCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        sb16ConfigButton->setEnabled(static_cast<VMSpecs::AudioDevice>(audioCombo->currentData().toInt()) == VMSpecs::AudioDevice::Sb16);
    });
    connect(sb16ConfigButton, &QPushButton::clicked, this, [this]() {
        QDialog dlg(this);
        dlg.setWindowTitle(i18n("Sound Blaster 16 Configuration"));

        auto *form = new QFormLayout(&dlg);

        auto *irqCombo = new QComboBox;
        for (const int v : {2, 3, 5, 7, 9, 10, 11})
            irqCombo->addItem(QString::number(v), v);
        irqCombo->setCurrentIndex(irqCombo->findData(m_sb16Config.irq));
        form->addRow(i18n("IRQ:"), irqCombo);

        auto *dmaCombo = new QComboBox;
        for (const int v : {0, 1, 3})
            dmaCombo->addItem(QString::number(v), v);
        dmaCombo->setCurrentIndex(dmaCombo->findData(m_sb16Config.dma));
        form->addRow(i18n("DMA (8-bit):"), dmaCombo);

        auto *dma16Combo = new QComboBox;
        for (const int v : {5, 6, 7})
            dma16Combo->addItem(QString::number(v), v);
        dma16Combo->setCurrentIndex(dma16Combo->findData(m_sb16Config.dma16));
        form->addRow(i18n("DMA (16-bit):"), dma16Combo);

        auto *iobaseCombo = new QComboBox;
        for (const int v : {0x220, 0x240, 0x260, 0x280})
            iobaseCombo->addItem(u"0x%1"_s.arg(v, 0, 16), v);
        iobaseCombo->setCurrentIndex(iobaseCombo->findData(m_sb16Config.iobase));
        form->addRow(i18n("I/O Base:"), iobaseCombo);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        form->addRow(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

        if (dlg.exec() == QDialog::Accepted) {
            m_sb16Config.irq = irqCombo->currentData().toInt();
            m_sb16Config.dma = dmaCombo->currentData().toInt();
            m_sb16Config.dma16 = dma16Combo->currentData().toInt();
            m_sb16Config.iobase = iobaseCombo->currentData().toInt();
        }
    });
    connect(addPortFwdButton, &QPushButton::clicked, this, &EditVmDialog::addPortForwardRow);
    connect(removePortFwdButton, &QPushButton::clicked, this, &EditVmDialog::removePortForwardRow);
    connect(portForwardTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        removePortFwdButton->setEnabled(portForwardTable->currentRow() >= 0);
    });
    connect(networkModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        auto mode = static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt());
        userModeGroup->setVisible(mode == VMSpecs::NetworkMode::UserMode);
        vdeGroup->setVisible(mode == VMSpecs::NetworkMode::VirtualNetwork);
        validateForm();
    });

    connect(virtualNetworkCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        validateForm();
    });

    connect(hostnameEdit, &QLineEdit::textEdited, this, [this]() {
        validateForm();
    });

    connect(netCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &EditVmDialog::updateNetworkCardDependencies);

    connect(regenerateMacButton, &QPushButton::clicked, this, [this]() {
        macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));
    });

    connect(macEdit, &QLineEdit::textEdited, this, [this]() {
        validateForm();
    });

    connect(regenerateUuidButton, &QPushButton::clicked, this, [this]() {
        VMConfig scratch;
        scratch.regenerateUuid(m_vmUuids.values());
        uuidEdit->setText(scratch.uuid().toString(QUuid::WithoutBraces));
        validateForm();
    });
    connect(uuidEdit, &QLineEdit::textEdited, this, &EditVmDialog::validateForm);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, &EditVmDialog::resetToFactoryDefaults);
    mainLayout->addWidget(buttonBox);

    updateNetworkCardDependencies();

    onGpuDisplayChanged();
    updateDiskInterfaceHint();
    updateDeviceAvailability();

    updateRamSlider(GlobalDefaults.ramMb);
}

void EditVmDialog::setConfig(const VMConfig &config)
{
    m_original = config;

    m_disks.clear();
    m_removedDisks.clear();
    for (const DiskConfig &disk : config.extraDisks())
        m_disks.append({disk, true, 0, {}});

    if (m_mode == Mode::Edit)
        setWindowTitle(i18n("Edit VM: %1", config.name()));

    nameEdit->setText(config.name());

    diskSpin->setValue(config.diskSizeGb());

    commentEdit->setPlainText(config.comment());

    setCombo(osFamilyCombo, config.osFamily());
    const int variantIdx = osNameCombo->findData(config.distroId());
    if (variantIdx >= 0)
        osNameCombo->setCurrentIndex(variantIdx);
    else if (!config.distroId().isEmpty())
        osNameCombo->setEditText(config.distroId());

    if (config.osFamily() != VMSpecs::OsFamily::Other) {
        osNameCombo->setEnabled(true);
        if (osNameLabel)
            osNameLabel->setEnabled(true);
    }

    // In PreCreate the icon was derived by NewVmDialog, so it should keep
    // following the operating system until the user picks one.
    iconSelector->loadFrom(config, m_mode == Mode::Edit);

    setCombo(cpuCombo, config.cpuModel());
    kvmCheck->setChecked(config.useKvm());
    cpuSpin->setValue(config.cpuCoreCount());
    ramSpin->setValue(config.ramMb());
    balloonCheck->setChecked(config.useMemoryBallooning());
    preallocCheck->setChecked(config.preallocMemory());
    isoPathRequester->setText(config.isoPath());
    bootFromIsoCheck->setChecked(config.bootFromIso());
    floppyPathRequester->setText(config.floppyPath());
    bootFromFloppyCheck->setChecked(config.bootFromFloppy());
    macEdit->setText(config.macAddress());

    uuidEdit->setText(config.uuid().isNull() ? QString() : config.uuid().toString(QUuid::WithoutBraces));
    const SmbiosSystemInfo smbios = config.smbiosSystemInfo();
    const bool smbiosSupported = VMConfig::supportsSmbios(config.architecture());
    for (const auto &[member, edit] : std::as_const(m_smbiosEdits)) {
        edit->setText(smbios.*member);
        edit->setEnabled(smbiosSupported);
        edit->setToolTip(smbiosSupported ? QString() : i18n("Not supported for this architecture."));
    }

    for (const auto &rule : config.portForwards()) {
        addPortForwardRow();
        const int row = portForwardTable->rowCount() - 1;

        auto *protocolCombo = qobject_cast<QComboBox *>(portForwardTable->cellWidget(row, 0));
        auto *hostPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 1));
        auto *guestPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 2));

        if (protocolCombo) {
            const int idx = protocolCombo->findData(static_cast<int>(rule.protocol));
            if (idx >= 0)
                protocolCombo->setCurrentIndex(idx);
        }
        if (hostPortSpin)
            hostPortSpin->setValue(rule.hostPort);
        if (guestPortSpin)
            guestPortSpin->setValue(rule.guestPort);
    }
    extraArgsEdit->setText(config.extraArgs());
    guestAgentCheck->setChecked(config.useGuestAgent());
    sharedPathEdit->setText(config.sharedPath());
    sharedNameEdit->setText(config.sharedName());

    setCombo(machineCombo, config.machineType());

    updateDeviceAvailability();

    if (!VMConfig::isDisplayTypeAvailable(config.displayType())) {
        const auto configuredType = config.displayType();
        // Prefer the integrated viewer; SDL only if that is missing too.
        const auto fallbackType = VMConfig::isDisplayTypeAvailable(VMSpecs::DisplayType::Dbus) ? VMSpecs::DisplayType::Dbus : VMSpecs::DisplayType::Sdl;
        KMessageBox::information(this,
                                 i18n("The configured display type '%1' is not available in this build.\n"
                                      "KEMU will use '%2' instead.",
                                      VMConfig::toUIString(configuredType),
                                      VMConfig::toUIString(fallbackType)),
                                 i18n("Display Type Unavailable"));
        setCombo(displayCombo, fallbackType);
    } else {
        setCombo(displayCombo, config.displayType());
    }

    setCombo(gpuCombo, config.gpuType());
    accel3dCheck->setChecked(config.gpu3dAcceleration());
    setCombo(audioCombo, config.audioDevice());
    m_sb16Config = config.sb16Config();
    setCombo(netCombo, config.networkCard());
    updateNetworkCardDependencies();
    setCombo(networkModeCombo, config.networkMode());
    if (virtualNetworkCombo->count() > 0) {
        const int vnIdx = virtualNetworkCombo->findData(config.virtualNetworkName());
        if (vnIdx >= 0)
            virtualNetworkCombo->setCurrentIndex(vnIdx);
    }
    m_lastAutoHostname = Utils::vmNameToHostname(config.name());
    hostnameEdit->setText(config.dnsHostname().isEmpty() ? m_lastAutoHostname : config.dnsHostname());
    setCombo(diskInterfaceCombo, config.diskInterface());
    setCombo(usbCombo, config.usbController());
    setCombo(mouseCombo, config.pointerDevice());
    setCombo(keyboardCombo, config.keyboardDevice());

    setCombo(exitActionCombo, config.exitAction());
    applyEncryptionConstraints();
    refreshEncryptionState();
    updateToolWarnings();
    setCombo(guestShutdownActionCombo, config.guestShutdownAction());

    uefiCheck->setChecked(config.useUefi());
    localTimeCheck->setChecked(config.useLocalTime());
    tpmCheck->setChecked(config.useTpm());
    secureBootCheck->setChecked(config.useSecureBoot());
    acpiCheck->setChecked(config.useAcpi());
    gpuMemSpin->setValue(config.gpuMemMb() > 0 ? config.gpuMemMb() : 64);
    const QSize res = config.displayResolution();
    for (int i = 0; i < resolutionCombo->count(); ++i) {
        if (resolutionCombo->itemData(i).toSize() == res) {
            resolutionCombo->setCurrentIndex(i);
            break;
        }
    }
    // Fall back to the first entry when the stored size is not offered
    if (resolutionCombo->currentIndex() < 0)
        resolutionCombo->setCurrentIndex(0);

    applyModeConstraints();
    refreshDiskList();
}

void EditVmDialog::setReadOnly(bool readOnly, bool frozen)
{
    m_readOnly = readOnly;
    if (!readOnly)
        return;

    makeReadOnly(this);

    // Composite widget without a QAbstractButton root.
    iconSelector->setEnabled(false);

    // setStandardButtons() replaces the buttons the walk above disabled.
    buttonBox->setStandardButtons(QDialogButtonBox::Close);
    buttonBox->setEnabled(true);

    if (!m_original.name().isEmpty()) {
        setWindowTitle(frozen ? i18n("Specification of VM: %1 (frozen)", m_original.name()) : i18n("Specification of VM: %1 (running)", m_original.name()));
    }
}

void EditVmDialog::onMachineTypeChanged(int /*index*/)
{
    updateDeviceAvailability();
}

void EditVmDialog::onRamSpinChanged(int mb)
{
    const QSignalBlocker blockRAMSlider(ramSlider);
    ramSlider->setValue(mb / 1024);
}

void EditVmDialog::browseSharedDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, i18n("Select shared folder"), QDir::homePath());
    if (!dir.isEmpty())
        sharedPathEdit->setText(dir);
}

void EditVmDialog::onOsFamilyChanged(int /*index*/)
{
    const int familyVal = osFamilyCombo->currentData().toInt();

    if (familyVal == -1) {
        // Placeholder
        osNameCombo->clear();
        osNameCombo->addItem(i18n("-- Please select --"), {});
        osNameCombo->setCurrentIndex(0);
        osNameCombo->setEnabled(false);
        osNameLabel->setEnabled(false);

    } else if (static_cast<VMSpecs::OsFamily>(familyVal) == VMSpecs::OsFamily::Other) {
        // special case "Other"
        osNameCombo->clear();
        osNameCombo->addItem(i18n("Other"), u"other"_s);
        osNameCombo->setCurrentIndex(0);
        osNameCombo->setEnabled(false);
        osNameLabel->setEnabled(false);

    } else {
        osNameCombo->setEnabled(true);
        osNameLabel->setEnabled(true);
        auto osFamily = static_cast<VMSpecs::OsFamily>(familyVal);
        filterOsVariants(osFamily);
    }

    updateIconForOs();
    validateForm();
    updateDiskInterfaceHint();
}

void EditVmDialog::onOsNameChanged(int /*index*/)
{
    updateIconForOs();

    const QString distroId = osNameCombo->currentData().toString();

    validateForm();
}

void EditVmDialog::onGpuDisplayChanged()
{
    const auto gpu = static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt());
    const auto osFamily = static_cast<VMSpecs::OsFamily>(osFamilyCombo->currentData().toInt());

    const bool isVirtio = (gpu == VMSpecs::GpuType::Virtio);
    const bool isWindows = (osFamily == VMSpecs::OsFamily::Windows);

    gpuMemSpin->setRange(VMConfig::vgaMemMbLowerLimit(gpu), VMConfig::vgaMemMbUpperLimit(gpu));
    if (gpu == VMSpecs::GpuType::CirrusGD5430 || gpu == VMSpecs::GpuType::CirrusGD5446) {
        gpuMemSpin->setSingleStep(4);
        // Snap to nearest valid value
        const int val = gpuMemSpin->value();
        if (val <= 4)
            gpuMemSpin->setValue(4);
        else if (val <= 8)
            gpuMemSpin->setValue(8);
        else
            gpuMemSpin->setValue(16);
    } else {
        gpuMemSpin->setSingleStep(1);
    }
    gpuMemSpin->setEnabled(true);
    vgaMemLabel->setEnabled(true);

    resolutionCombo->setEnabled(isVirtio);
    resolutionLabel->setEnabled(isVirtio);

    const bool can3d = isVirtio;
    accel3dCheck->setEnabled(can3d);
    if (!can3d)
        accel3dCheck->setChecked(false);

    if (can3d && isWindows && accel3dCheck->isChecked()) {
        gpuHintLabel->setText(u"<i>"_s + i18n("Note: VirtIO 3D acceleration is not supported on Windows guests.") + u"</i>"_s);
    } else {
        gpuHintLabel->clear();
    }
}

void EditVmDialog::onNameChanged(const QString &text)
{
    // OS guessing
    bool found;
    auto [family, variantId] = VMConfig::guessOSFromText(text, &found);
    if (found) {
        const int familyIndex = osFamilyCombo->findData(static_cast<int>(family));
        if (familyIndex >= 0 && osFamilyCombo->currentIndex() != familyIndex)
            osFamilyCombo->setCurrentIndex(familyIndex);
        const int variantIndex = osNameCombo->findData(variantId);
        if (variantIndex >= 0 && osNameCombo->currentIndex() != variantIndex)
            osNameCombo->setCurrentIndex(variantIndex);
        updateIconForOs();
    }

    // Keep the DNS hostname in sync with the VM name as long as the user
    // has not changed it away from the auto-generated value.
    if (hostnameEdit && (hostnameEdit->text().isEmpty() || hostnameEdit->text() == m_lastAutoHostname)) {
        m_lastAutoHostname = Utils::vmNameToHostname(text);
        hostnameEdit->setText(m_lastAutoHostname);
    }

    validateForm();
}

void EditVmDialog::addPortForwardRow()
{
    const int row = portForwardTable->rowCount();
    portForwardTable->insertRow(row);

    auto *protocolCombo = new QComboBox();
    protocolCombo->addItem(u"TCP"_s, static_cast<int>(PortForwardRule::Protocol::Tcp));
    protocolCombo->addItem(u"UDP"_s, static_cast<int>(PortForwardRule::Protocol::Udp));
    portForwardTable->setCellWidget(row, 0, protocolCombo);

    auto *hostPortSpin = new QSpinBox();
    hostPortSpin->setRange(1, 65535);
    hostPortSpin->setValue(8080);
    hostPortSpin->setFrame(false);
    portForwardTable->setCellWidget(row, 1, hostPortSpin);

    auto *guestPortSpin = new QSpinBox();
    guestPortSpin->setRange(1, 65535);
    guestPortSpin->setValue(8080);
    guestPortSpin->setFrame(false);
    portForwardTable->setCellWidget(row, 2, guestPortSpin);

    portForwardTable->setCurrentCell(row, 0);
    removePortFwdButton->setEnabled(true);
}

void EditVmDialog::removePortForwardRow()
{
    const int row = portForwardTable->currentRow();
    if (row >= 0)
        portForwardTable->removeRow(row);
    removePortFwdButton->setEnabled(portForwardTable->rowCount() > 0);
}

void EditVmDialog::filterOsVariants(VMSpecs::OsFamily osFamily)
{
    osNameCombo->clear();
    osNameCombo->setCurrentIndex(-1);

    if (osNameCombo->lineEdit() && osNameCombo->lineEdit()->actions().isEmpty())
        osNameCombo->lineEdit()->addAction(QIcon::fromTheme(u"edit-find"_s), QLineEdit::LeadingPosition);

    if (osNameCombo->lineEdit()) {
        osNameCombo->lineEdit()->setPlaceholderText(i18n("Type or select"));
        osNameCombo->lineEdit()->clear();
    }

    if (m_imageDatabase) {
        for (const auto &d : m_imageDatabase->templatesForFamily(osFamily))
            osNameCombo->addItem(d.displayName, d.id);
    }

    if (auto *comp = osNameCombo->completer())
        comp->setCompletionMode(QCompleter::PopupCompletion);

    if (osNameCombo->count() > 0)
        osNameCombo->insertSeparator(osNameCombo->count());
    osNameCombo->addItem(i18n("Other"), u"other"_s);

    osNameCombo->setCurrentIndex(-1);
}

void EditVmDialog::updateIconForOs()
{
    QString distroId = osNameCombo->currentData().toString();
    if (distroId.isEmpty())
        distroId = osNameCombo->currentText().trimmed();

    iconSelector->followOs(static_cast<VMSpecs::OsFamily>(osFamilyCombo->currentData().toInt()), distroId);
}

void EditVmDialog::setExistingVmNames(const QStringList &names)
{
    existingVmNames = names;
    onNameChanged(nameEdit->text());
}

void EditVmDialog::setMacUsageInfo(const QList<MacUsageInfo> &info)
{
    m_macUsageInfo = info;
}

void EditVmDialog::setVmUuids(const QMap<QString, QUuid> &vmUuids)
{
    m_vmUuids = vmUuids;
}

void EditVmDialog::setImageDatabase(ImageDatabase *db)
{
    m_imageDatabase = db;

    const int familyVal = osFamilyCombo->currentData().toInt();
    if (familyVal >= 0 && static_cast<VMSpecs::OsFamily>(familyVal) != VMSpecs::OsFamily::Other)
        filterOsVariants(static_cast<VMSpecs::OsFamily>(familyVal));
}

void EditVmDialog::setNetworkManager(VirtualNetworkManager *mgr)
{
    m_networkManager = mgr;
    virtualNetworkCombo->clear();
    if (!mgr)
        return;
    for (const auto &net : mgr->networks())
        virtualNetworkCombo->addItem(net.name, net.name);

    // Virtual networks run in-process; they are unavailable only while no
    // network has been defined yet.
    const bool disableVirtualNetwork = mgr->networks().isEmpty();

    if (disableVirtualNetwork) {
        auto *model = qobject_cast<QStandardItemModel *>(networkModeCombo->model());
        if (model) {
            const int idx = networkModeCombo->findData(static_cast<int>(VMSpecs::NetworkMode::VirtualNetwork));
            if (idx >= 0) {
                QStandardItem *item = model->item(idx);
                item->setEnabled(false);
                item->setToolTip(i18n("Create a virtual network first in Settings"));
            }
        }
    }

    if (disableVirtualNetwork && static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()) == VMSpecs::NetworkMode::VirtualNetwork) {
        const int userModeIdx = networkModeCombo->findData(static_cast<int>(VMSpecs::NetworkMode::UserMode));
        if (userModeIdx >= 0)
            networkModeCombo->setCurrentIndex(userModeIdx);
    }
}

bool EditVmDialog::iconManuallySet() const
{
    return iconSelector->isManuallySet();
}

VMConfig EditVmDialog::getResult() const
{
    VMConfig c;
    c.setName(nameEdit->text().trimmed());
    c.setComment(commentEdit->toPlainText());
    c.setOsFamily(static_cast<VMSpecs::OsFamily>(osFamilyCombo->currentData().toInt()));
    QString distroId = osNameCombo->currentData().toString();
    if (distroId.isEmpty())
        distroId = osNameCombo->currentText().trimmed();
    c.setDistroId(distroId);

    iconSelector->applyTo(c);

    c.setCpuModel(static_cast<VMSpecs::CpuModel>(cpuCombo->currentData().toInt()));

    // Architecture: no dedicated selector yet -- currently only x86 vs x86_64.
    // The CPU model decides when it implies one (i486 and friends); otherwise
    // keep what the caller supplied, so an existing value is never silently
    // overwritten.
    const QString archHint = VMConfig::x86ArchHint(c.cpuModel());
    c.setArchitecture(archHint.isEmpty() ? m_original.architecture() : VMConfig::parseArchitecture(archHint));

    c.setUseKvm(kvmCheck->isChecked());
    c.setRamMb(ramSpin->value());
    c.setUseBalloon(balloonCheck->isChecked());
    c.setUsePrealloc(preallocCheck->isChecked());
    c.setCpuCoreCount(cpuSpin->value());
    // A larger, not yet applied value is not the size of the disk.
    c.setDiskSizeGb(m_mode == Mode::Edit && m_systemDiskGb > 0 ? m_systemDiskGb : diskSpin->value());
    c.setISOPath(isoPathRequester->text());
    c.setBootFromIso(bootFromIsoCheck->isChecked());
    c.setFloppyPath(floppyPathRequester->text());
    c.setBootFromFloppy(bootFromFloppyCheck->isChecked());
    c.setExtraArgs(extraArgsEdit->text());
    c.setUseGuestAgent(guestAgentCheck->isChecked());
    c.setMachineType(static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()));
    c.setUseUefi(uefiCheck->isChecked());
    c.setUseLocalTime(localTimeCheck->isChecked());
    c.setExitAction(static_cast<VMSpecs::ExitAction>(exitActionCombo->currentData().toInt()));
    c.setGuestShutdownAction(static_cast<VMSpecs::GuestShutdownAction>(guestShutdownActionCombo->currentData().toInt()));
    c.setUseTpm(tpmCheck->isChecked());
    c.setUseSecureBoot(secureBootCheck->isChecked());
    c.setUseAcpi(acpiCheck->isChecked());
    c.setSharedPath(sharedPathEdit->text());
    c.setSharedName(sharedNameEdit->text());
    c.setDisplayType(static_cast<VMSpecs::DisplayType>(displayCombo->currentData().toInt()));
    c.setGpuType(static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt()));
    c.setGpu3dAcceleration(accel3dCheck->isChecked());
    c.setGpuMemMb(gpuMemSpin->value());
    c.setDisplayResolution(resolutionCombo->currentData().toSize());
    c.setAudioDevice(static_cast<VMSpecs::AudioDevice>(audioCombo->currentData().toInt()));
    c.setSb16Config(m_sb16Config);
    c.setNetworkCard(static_cast<VMSpecs::NetworkCard>(netCombo->currentData().toInt()));
    c.setMacAddress(macEdit->text());
    c.setNetworkMode(static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()));
    c.setVirtualNetworkName(virtualNetworkCombo->currentData().toString());
    if (static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()) == VMSpecs::NetworkMode::VirtualNetwork) {
        // Store only explicit overrides; empty means "derive from the VM name".
        const QString hostname = hostnameEdit->text().trimmed().toLower();
        c.setDnsHostname(hostname == Utils::vmNameToHostname(nameEdit->text().trimmed()) ? QString() : hostname);
    }
    QList<PortForwardRule> rules;
    for (int row = 0; row < portForwardTable->rowCount(); ++row) {
        auto *protocolCombo = qobject_cast<QComboBox *>(portForwardTable->cellWidget(row, 0));
        auto *hostPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 1));
        auto *guestPortSpin = qobject_cast<QSpinBox *>(portForwardTable->cellWidget(row, 2));
        if (!protocolCombo || !hostPortSpin || !guestPortSpin)
            continue;
        PortForwardRule rule;
        rule.protocol = static_cast<PortForwardRule::Protocol>(protocolCombo->currentData().toInt());
        rule.hostPort = static_cast<quint16>(hostPortSpin->value());
        rule.guestPort = static_cast<quint16>(guestPortSpin->value());
        rules.append(rule);
    }
    c.setPortForwards(rules);

    c.setUuid(QUuid::fromString(uuidEdit->text().trimmed()));
    // Not editable here, but the dialog must not drop it: a linked clone
    // would lose track of its source and unfreeze it.
    c.setLinkedSource(m_original.linkedSourceName(), m_original.linkedSourceUuid());
    c.setDiskKeyId(m_original.diskKeyId());
    SmbiosSystemInfo smbios;
    for (const auto &[member, edit] : m_smbiosEdits)
        smbios.*member = edit->text().trimmed();
    c.setSmbiosSystemInfo(smbios);

    c.setDiskInterface(static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->currentData().toInt()));

    // Imported images join the configuration once their conversion is done.
    QList<DiskConfig> extraDisks;
    for (const DiskEntry &entry : m_disks) {
        if (entry.sourcePath.isEmpty())
            extraDisks << entry.disk;
    }
    c.setExtraDisks(extraDisks);

    c.setUsbController(static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()));
    c.setPointerDevice(static_cast<VMSpecs::PointerDevice>(mouseCombo->currentData().toInt()));
    c.setKeyboardDevice(static_cast<VMSpecs::KeyboardDevice>(keyboardCombo->currentData().toInt()));

    return c;
}

void EditVmDialog::updateDiskInterfaceHint()
{
    const auto updateVisibility = qScopeGuard([this] {
        diskHintLabel->setVisible(!diskHintLabel->text().isEmpty());
    });

    const auto disk = static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->currentData().toInt());
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    const auto os = static_cast<VMSpecs::OsFamily>(osFamilyCombo->currentData().toInt());
    if (disk == VMSpecs::DiskInterface::SataIde) {
        if (machine == VMSpecs::MachineType::Q35) {
            diskHintLabel->setText(u"<i>"_s + i18n("Q35 chipset: disk attached via AHCI (SATA).") + u"</i>"_s);
        } else if (machine == VMSpecs::MachineType::PcISA) {
            // Explicitly handle the ISA-only machine type
            diskHintLabel->setText(u"<i>"_s + i18n("ISA-only PC: disk attached via legacy ISA IDE.") + u"</i>"_s);
        } else {
            diskHintLabel->setText(u"<i>"_s + i18n("PC (i440FX) chipset: disk attached via PIIX3 (IDE).") + u"</i>"_s);
        }
        return;
    }
    if (disk == VMSpecs::DiskInterface::Virtio && os == VMSpecs::OsFamily::Windows) {
        diskHintLabel->setText(u"<i>"_s
                               + i18n("Note: Windows requires the VirtIO driver ISO "
                                      "during installation.")
                               + u"</i>"_s);
        return;
    }
    diskHintLabel->clear();
};

void EditVmDialog::updateDeviceAvailability()
{
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    const bool supportsIsa = VMConfig::supportsIsaBus(machine);
    const bool supportsPci = VMConfig::supportsPciBus(machine);

    // Requirement-based tooltip messages
    const QString isaReqMsg = i18n("Requires an ISA bus (e.g., Legacy PC or ISA-only).");
    const QString pciReqMsg = i18n("Requires a PCI/PCIe bus (e.g., Q35 or Legacy PC).");

    // Helper lambda to process ISA/PCI dependent combo boxes (Enable/Disable,
    // Tooltips, Fallback)
    auto updateCombo = [&](QComboBox *combo, auto isIsaChecker, int noneValue = -1) {
        auto *model = qobject_cast<QStandardItemModel *>(combo->model());
        if (!model)
            return;

        bool currentValid = false;
        int firstValidIdx = -1;

        for (int i = 0; i < combo->count(); ++i) {
            const int dataVal = combo->itemData(i).toInt();

            // Handle "None" or implicitly always-available items
            if (dataVal == noneValue) {
                model->item(i)->setEnabled(true);
                combo->setItemData(i, QString(), Qt::ToolTipRole);
                if (firstValidIdx == -1)
                    firstValidIdx = i;
                if (i == combo->currentIndex())
                    currentValid = true;
                continue;
            }

            const bool isIsa = isIsaChecker(dataVal);
            const bool shouldShow = (supportsIsa && isIsa) || (supportsPci && !isIsa);

            model->item(i)->setEnabled(shouldShow);

            if (shouldShow) {
                combo->setItemData(i, QString(), Qt::ToolTipRole);
                if (firstValidIdx == -1)
                    firstValidIdx = i;
                if (i == combo->currentIndex())
                    currentValid = true;
            } else {
                combo->setItemData(i, isIsa ? isaReqMsg : pciReqMsg, Qt::ToolTipRole);
            }
        }

        // Fallback to the first available item if current is disabled
        if (!currentValid && firstValidIdx != -1) {
            combo->setCurrentIndex(firstValidIdx);
        }
    };

    // --- Update CPU Availability ---
    if (auto *cpuModelItem = qobject_cast<QStandardItemModel *>(cpuCombo->model())) {
        bool currentCpuValid = false;
        int firstValidCpuIdx = -1;

        const QString cpuReqMsg = i18n("CPU is not compatible with the selected chipset.");

        for (int i = 0; i < cpuCombo->count(); ++i) {
            const auto cpuVal = static_cast<VMSpecs::CpuModel>(cpuCombo->itemData(i).toInt());
            bool shouldShow = VMConfig::isCpuCompatible(cpuVal, machine);

            // Respect KVM state for "Host" CPU
            if (cpuVal == VMSpecs::CpuModel::Host && !kvmCheck->isChecked()) {
                shouldShow = false;
            }

            cpuModelItem->item(i)->setEnabled(shouldShow);
            cpuCombo->setItemData(i, shouldShow ? QString() : cpuReqMsg, Qt::ToolTipRole);

            if (shouldShow) {
                if (firstValidCpuIdx == -1) {
                    firstValidCpuIdx = i;
                }
                if (i == cpuCombo->currentIndex()) {
                    currentCpuValid = true;
                }
            }
        }

        // Fallback if current CPU becomes invalid
        if (!currentCpuValid && firstValidCpuIdx != -1) {
            cpuCombo->setCurrentIndex(firstValidCpuIdx);
        }
    }

    // --- Update Standard Devices ---

    updateCombo(gpuCombo, [](int val) {
        return VMConfig::isIsaDevice(static_cast<VMSpecs::GpuType>(val));
    });

    updateCombo(audioCombo, [](int val) {
        return VMConfig::isIsaDevice(static_cast<VMSpecs::AudioDevice>(val));
    });

    updateCombo(
        netCombo,
        [](int val) {
            return VMConfig::isIsaDevice(static_cast<VMSpecs::NetworkCard>(val));
        },
        static_cast<int>(VMSpecs::NetworkCard::None));

    // --- Audio Extras ---
    const auto currentAudio = static_cast<VMSpecs::AudioDevice>(audioCombo->currentData().toInt());
    sb16ConfigButton->setEnabled(currentAudio == VMSpecs::AudioDevice::Sb16 && supportsIsa);

    // --- USB (PCI only) ---
    if (auto *usbModel = qobject_cast<QStandardItemModel *>(usbCombo->model())) {
        for (int i = 0; i < usbCombo->count(); ++i) {
            auto usbCtrl = static_cast<VMSpecs::UsbController>(usbCombo->itemData(i).toInt());
            if (usbCtrl != VMSpecs::UsbController::None) {
                usbModel->item(i)->setEnabled(supportsPci);
                usbCombo->setItemData(i, supportsPci ? QString() : pciReqMsg, Qt::ToolTipRole);
            }
        }
        if (!supportsPci) {
            const int noneIdx = usbCombo->findData(static_cast<int>(VMSpecs::UsbController::None));
            if (noneIdx != -1)
                usbCombo->setCurrentIndex(noneIdx);
            usbCombo->setEnabled(false);
            usbControllerLabel->setEnabled(false);
        } else {
            usbCombo->setEnabled(true);
            usbControllerLabel->setEnabled(true);
        }
    }

    updateInputDeviceAvailability(); // keep mouse/keyboard combos in sync

    // --- Disk Interface ---
    if (auto *diskModel = qobject_cast<QStandardItemModel *>(diskInterfaceCombo->model())) {
        bool currentDiskValid = false;
        int firstValidDiskIdx = -1;

        for (int i = 0; i < diskInterfaceCombo->count(); ++i) {
            auto diskIf = static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->itemData(i).toInt());
            // Assume SataIde is always available, others require PCI
            const bool shouldShow = (diskIf == VMSpecs::DiskInterface::SataIde) || supportsPci;

            diskModel->item(i)->setEnabled(shouldShow);
            diskInterfaceCombo->setItemData(i, shouldShow ? QString() : pciReqMsg, Qt::ToolTipRole);

            if (shouldShow) {
                if (firstValidDiskIdx == -1)
                    firstValidDiskIdx = i;
                if (i == diskInterfaceCombo->currentIndex())
                    currentDiskValid = true;
            }
        }

        if (!currentDiskValid && firstValidDiskIdx != -1) {
            diskInterfaceCombo->setCurrentIndex(firstValidDiskIdx);
        }
    }

    // --- UEFI / TPM / Misc ---
    const bool canUseUefi = (machine == VMSpecs::MachineType::Q35);
    uefiCheck->setEnabled(canUseUefi);
    tpmCheck->setEnabled(canUseUefi);

    if (!canUseUefi) {
        uefiCheck->setChecked(false);
        tpmCheck->setChecked(false);
    }

    secureBootCheck->setEnabled(uefiCheck->isChecked() && canUseUefi);
    if (!secureBootCheck->isEnabled()) {
        secureBootCheck->setChecked(false);
    }

    if (!supportsPci) {
        balloonCheck->setChecked(false);
    }
    balloonCheck->setEnabled(supportsPci);
    // Kept as chosen; without PCI the channel is just left out.
    guestAgentCheck->setEnabled(supportsPci);

    const bool hasFloppy = (machine != VMSpecs::MachineType::Q35);
    floppyPathRequester->setEnabled(hasFloppy);
    floppyPathLabel->setEnabled(hasFloppy);
    bootFromFloppyCheck->setEnabled(hasFloppy && !floppyPathRequester->text().trimmed().isEmpty());
    if (!hasFloppy) {
        floppyPathRequester->clear();
        bootFromFloppyCheck->setChecked(false);
    }

    updateDiskInterfaceHint();
    onGpuDisplayChanged();
}

void EditVmDialog::updateInputDeviceAvailability()
{
    const bool hasUsb = static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()) != VMSpecs::UsbController::None;
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    const bool hasPci = VMConfig::supportsPciBus(machine);

    const QString usbTip = i18n("Requires a USB controller.");
    const QString pciTip = i18n("Requires a PCI/PCIe bus (e.g., Q35 or Legacy PC).");

    const auto setItemState = [](QComboBox *combo, int enumValue, bool enabled, const QString &tip) {
        if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
            const int idx = combo->findData(enumValue);
            if (idx >= 0 && model->item(idx)) {
                model->item(idx)->setEnabled(enabled);
                combo->setItemData(idx, enabled ? QString() : tip, Qt::ToolTipRole);
            }
        }
    };

    setItemState(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::UsbMouse), hasUsb, usbTip);
    setItemState(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::UsbTablet), hasUsb, usbTip);
    setItemState(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::VirtioTablet), hasPci, pciTip);
    setItemState(keyboardCombo, static_cast<int>(VMSpecs::KeyboardDevice::Usb), hasUsb, usbTip);

    // Fall back to PS/2 when the selected device is unavailable.
    const auto pointer = static_cast<VMSpecs::PointerDevice>(mouseCombo->currentData().toInt());
    const bool pointerOk = (pointer == VMSpecs::PointerDevice::Ps2) || (pointer == VMSpecs::PointerDevice::VirtioTablet ? hasPci : hasUsb);
    if (!pointerOk)
        setCombo(mouseCombo, VMSpecs::PointerDevice::Ps2);

    if (!hasUsb && static_cast<VMSpecs::KeyboardDevice>(keyboardCombo->currentData().toInt()) != VMSpecs::KeyboardDevice::Ps2)
        setCombo(keyboardCombo, VMSpecs::KeyboardDevice::Ps2);
}

void EditVmDialog::validateForm()
{
    QString warning;
    bool valid = true;

    QString nameError;
    const QString trimmed = nameEdit->text().trimmed();
    if (!VMConfig::isValidVmName(trimmed, &nameError)) {
        warning = nameError;
        valid = false;
    } else if (trimmed != m_original.name() && existingVmNames.contains(trimmed, Qt::CaseSensitive)) {
        warning = i18n("A virtual machine named '%1' already exists.", trimmed);
        valid = false;
    }

    // OS family placeholder?
    if (osFamilyCombo->currentData().toInt() == -1)
        valid = false;

    // OS variant: accept combo selection OR free text (min. 2 chars)
    const QString osDistroId = osNameCombo->currentData().toString();
    const QString osText = osNameCombo->currentText().trimmed();
    if (osDistroId.isEmpty() && osText.length() < 2)
        valid = false;

    if (warning.isEmpty()) {
        nameWarningLabel->clear();
    } else {
        nameWarningLabel->setText(warning);
    }

    // Shared folder requires a mount tag
    if (valid && !sharedPathEdit->text().trimmed().isEmpty() && sharedNameEdit->text().trimmed().isEmpty()) {
        valid = false;
    }

    // Check for duplicate mac addresses
    QString macWarningText;
    const QString mac = macEdit->text();
    if (!mac.isEmpty() && !Utils::isValidMacAddress(mac)) {
        macWarningText = i18n("Invalid MAC address format.");
        valid = false;
    } else if (!mac.isEmpty()) {
        const auto mode = static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt());
        const QString netName = virtualNetworkCombo->currentData().toString();

        // Every VM is checked: the first one holding this MAC may sit on
        // another network while a later one shares ours.
        for (const auto &info : m_macUsageInfo) {
            if (m_mode == Mode::Edit && info.vmName == m_original.name())
                continue;

            if (info.mac.compare(mac, Qt::CaseInsensitive) != 0)
                continue;

            if (mode == VMSpecs::NetworkMode::VirtualNetwork && info.networkMode == VMSpecs::NetworkMode::VirtualNetwork
                && info.virtualNetworkName == netName) {
                macWarningText = i18n("MAC conflict with '%1' on network '%2'.", info.vmName, netName);
                valid = false;
                break;
            }

            // Duplicate elsewhere: harmless today, but it breaks as soon as
            // both VMs end up on the same network.
            if (macWarningText.isEmpty())
                macWarningText = i18n("MAC address is already used by '%1'.", info.vmName);
        }
    }

    // Update visibility and text of the label
    if (macWarningLabel && !macWarningText.isEmpty()) {
        macWarningLabel->setText(macWarningText);
    } else {
        macWarningLabel->clear();
    }

    // DNS hostname: only relevant (and visible) in VirtualNetwork mode.
    QString hostnameWarningText;
    if (static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt()) == VMSpecs::NetworkMode::VirtualNetwork) {
        const QString hostname = hostnameEdit->text().trimmed().toLower();
        if (hostname.isEmpty() || !Utils::isValidDnsHostname(hostname)) {
            hostnameWarningText = i18n("Invalid DNS hostname.");
            valid = false;
        } else {
            // Must be unique per virtual network.
            const QString netName = virtualNetworkCombo->currentData().toString();
            for (const auto &info : m_macUsageInfo) {
                if (m_mode == Mode::Edit && info.vmName == m_original.name())
                    continue;
                if (info.networkMode != VMSpecs::NetworkMode::VirtualNetwork || info.virtualNetworkName != netName)
                    continue;
                if (info.dnsHostname.compare(hostname, Qt::CaseInsensitive) == 0) {
                    hostnameWarningText = i18n("Hostname conflict with '%1' on network '%2'.", info.vmName, netName);
                    valid = false;
                    break;
                }
            }
        }
    }

    if (hostnameWarningLabel && !hostnameWarningText.isEmpty()) {
        hostnameWarningLabel->setText(hostnameWarningText);
    } else {
        hostnameWarningLabel->clear();
    }

    // A duplicate UUID only confuses guest software, so it does not block.
    QString uuidWarningText;
    const QString uuidText = uuidEdit->text().trimmed();
    if (!uuidText.isEmpty()) {
        const QUuid uuid = QUuid::fromString(uuidText);
        if (uuid.isNull()) {
            uuidWarningText = i18n("Enter a valid, non-zero UUID or leave the field empty.");
            valid = false;
        } else {
            for (auto it = m_vmUuids.cbegin(); it != m_vmUuids.cend(); ++it) {
                if (it.value() == uuid && !(m_mode == Mode::Edit && it.key() == m_original.name())) {
                    uuidWarningText = i18n("This UUID is already used by '%1'.", it.key());
                    break;
                }
            }
        }
    }
    uuidWarningLabel->setText(uuidWarningText);
    uuidWarningLabel->setVisible(!uuidWarningText.isEmpty());

    if (storageWarningLabel) {
        const QString storageError = storageLayoutError();
        storageWarningLabel->setText(storageError);
        storageWarningLabel->setVisible(!storageError.isEmpty());
        if (!storageError.isEmpty())
            valid = false;
    }

    if (auto *ok = buttonBox ? buttonBox->button(QDialogButtonBox::Ok) : nullptr)
        ok->setEnabled(valid);
}

void EditVmDialog::setVmDirectory(const QString &path, bool seedIsoPresent)
{
    m_vmDir = path;
    m_seedIsoPresent = seedIsoPresent;
    refreshDiskList();
}

QList<EditVmDialog::NewDisk> EditVmDialog::newDisks() const
{
    QList<NewDisk> result;
    for (const DiskEntry &entry : m_disks) {
        if (!entry.exists)
            result.append({entry.disk, entry.sizeGb, entry.sourcePath});
    }
    return result;
}

QList<EditVmDialog::RemovedDisk> EditVmDialog::removedDisks() const
{
    return m_removedDisks;
}

QString EditVmDialog::storageLayoutError() const
{
    VMConfig layout;
    layout.setMachineType(static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()));
    layout.setDiskInterface(static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->currentData().toInt()));

    QList<DiskConfig> disks;
    for (const DiskEntry &entry : m_disks)
        disks << entry.disk;
    layout.setExtraDisks(disks);

    return layout.storageLayoutError(m_seedIsoPresent);
}

QString EditVmDialog::nextFreeDiskFileName() const
{
    // Names of removed disks stay taken until the dialog is gone: their images
    // are only deleted or moved away after OK.
    QStringList taken;
    for (const DiskEntry &entry : m_disks)
        taken << entry.disk.fileName;
    for (const RemovedDisk &removed : m_removedDisks)
        taken << removed.fileName;
    if (!m_vmDir.isEmpty())
        taken << QDir(m_vmDir).entryList({u"disk-*"_s}, QDir::Files | QDir::Hidden);

    return VMConfig::nextExtraDiskFileName(taken);
}

void EditVmDialog::refreshDiskList()
{
    if (!diskTree)
        return;

    diskTree->clear();

    for (const DiskEntry &entry : std::as_const(m_disks)) {
        QString size;
        if (entry.exists && !m_vmDir.isEmpty()) {
            const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(QDir(m_vmDir).filePath(entry.disk.fileName));
            size = info.isValid ? i18n("%1 GB", QString::number(info.virtualSizeGb(), 'f', 1)) : i18n("Missing");
        } else if (!entry.sourcePath.isEmpty()) {
            const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(entry.sourcePath);
            size = info.isValid ? i18n("%1 GB, to be imported", QString::number(info.virtualSizeGb(), 'f', 1)) : i18n("To be imported");
        } else if (!entry.exists) {
            size = i18n("%1 GB, new", entry.sizeGb);
        }

        auto *item = new QTreeWidgetItem(diskTree);
        item->setIcon(0, QIcon::fromTheme(u"drive-harddisk"_s));
        item->setText(0, entry.disk.label.isEmpty() ? entry.disk.fileName : entry.disk.label);
        item->setToolTip(0, entry.disk.fileName);
        item->setText(1, size);
        item->setText(2, VMConfig::toUIString(entry.disk.diskInterface));
    }

    diskTree->setVisible(!m_disks.isEmpty());
    updateDiskButtons();
    validateForm();
}

void EditVmDialog::updateDiskButtons()
{
    if (!diskTree)
        return;

    const bool hasDisks = !m_disks.isEmpty();
    const bool hasSelection = diskTree->currentItem() && diskTree->currentItem()->isSelected();

    editDiskButton->setVisible(hasDisks);
    removeDiskButton->setVisible(hasDisks);

    addDiskButton->setEnabled(!m_readOnly && m_disks.size() < KEMUConst::MaxExtraDisks);
    editDiskButton->setEnabled(!m_readOnly && hasSelection);
    removeDiskButton->setEnabled(!m_readOnly && hasSelection);
}

void EditVmDialog::addDisk()
{
    if (m_readOnly || m_disks.size() >= KEMUConst::MaxExtraDisks)
        return;

    DiskDialog dlg(DiskDialog::Mode::Add, static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()), this);
    dlg.setDiskInterface(static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->currentData().toInt()));
    if (dlg.exec() != QDialog::Accepted)
        return;

    DiskEntry entry;
    entry.exists = false;
    entry.sizeGb = dlg.sizeGb();
    entry.sourcePath = dlg.sourcePath();
    entry.disk.fileName = nextFreeDiskFileName();
    entry.disk.label = dlg.label();
    entry.disk.diskInterface = dlg.diskInterface();
    entry.disk.serial = VMConfig::defaultDiskSerial(entry.disk.fileName);

    if (entry.disk.fileName.isEmpty())
        return;

    m_disks.append(entry);
    refreshDiskList();
}

void EditVmDialog::editSelectedDisk()
{
    const int row = diskTree->indexOfTopLevelItem(diskTree->currentItem());
    if (m_readOnly || row < 0 || row >= m_disks.size())
        return;

    DiskEntry &entry = m_disks[row];

    DiskDialog dlg(DiskDialog::Mode::Edit, static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()), this);
    dlg.setLabel(entry.disk.label);
    dlg.setDiskInterface(entry.disk.diskInterface);
    if (m_mode == Mode::Edit && entry.exists && !m_vmDir.isEmpty()) {
        const QString diskPath = QDir(m_vmDir).filePath(entry.disk.fileName);
        const QemuImgProcess::ImageInfo info = QemuImgProcess::inspect(diskPath);
        dlg.setCurrentSize(info.isValid ? int((info.virtualSizeBytes + (1LL << 30) - 1) >> 30) : 0);
        dlg.setCompactAvailable(true);
        connect(&dlg, &DiskDialog::compactRequested, this, [this, diskPath, &dlg] {
            Q_EMIT compactDiskRequested(diskPath, &dlg);
        });
        connect(&dlg, &DiskDialog::expandRequested, this, [this, diskPath, &dlg](int sizeGb) {
            Q_EMIT expandDiskRequested(diskPath, sizeGb, &dlg);
        });
        m_openDiskDialog = &dlg;
        m_openDiskPath = diskPath;
    } else if (!entry.exists && entry.sourcePath.isEmpty()) {
        // Created on OK, so its size can still change freely.
        dlg.setNewDiskSize(entry.sizeGb);
    }
    const auto closed = qScopeGuard([this] {
        m_openDiskDialog = nullptr;
        m_openDiskPath.clear();
    });
    if (dlg.exec() != QDialog::Accepted)
        return;

    if (!entry.exists && entry.sourcePath.isEmpty())
        entry.sizeGb = dlg.sizeGb();

    entry.disk.label = dlg.label();
    entry.disk.diskInterface = dlg.diskInterface();
    refreshDiskList();
}

void EditVmDialog::removeSelectedDisk()
{
    const int row = diskTree->indexOfTopLevelItem(diskTree->currentItem());
    if (m_readOnly || row < 0 || row >= m_disks.size())
        return;

    const DiskEntry entry = m_disks.at(row);

    if (entry.exists) {
        const QString name = entry.disk.label.isEmpty() ? entry.disk.fileName : entry.disk.label;

        QString snapshotNote;
        if (!m_original.diskPath().isEmpty()) {
            const QList<SnapshotInfo> snapshots = SnapshotManager().listSnapshots(m_original.diskPath());
            if (std::any_of(snapshots.cbegin(), snapshots.cend(), [](const SnapshotInfo &snap) {
                    return snap.vmStateSizeBytes > 0;
                }))
                snapshotNote = u"<br/><br/>"_s + i18n("Snapshots with a saved running state can afterwards only be restored while the VM is shut down.");
        }

        // An overlay is useless without the source next to it.
        if (m_original.isLinkedClone()) {
            const auto answer = KMessageBox::warningTwoActions(this,
                                                               i18n("Remove the disk <b>%1</b> from this virtual machine?<br/><br/>"
                                                                    "This linked clone only stores its own changes in the image, so it cannot be kept "
                                                                    "as a separate file and is deleted. The change takes effect when you confirm the settings.",
                                                                    name.toHtmlEscaped())
                                                                   + snapshotNote,
                                                               i18n("Remove Disk"),
                                                               KGuiItem(i18n("Delete Image"), u"edit-delete"_s),
                                                               KStandardGuiItem::cancel());
            if (answer != KMessageBox::PrimaryAction)
                return;
            m_removedDisks.append({entry.disk.fileName, {}});
            m_disks.removeAt(row);
            refreshDiskList();
            return;
        }

        const auto answer = KMessageBox::questionTwoActionsCancel(this,
                                                                  i18n("Remove the disk <b>%1</b> from this virtual machine?<br/><br/>"
                                                                       "Its image can be deleted or kept as a separate file. "
                                                                       "The change takes effect when you confirm the settings.",
                                                                       name.toHtmlEscaped())
                                                                      + snapshotNote,
                                                                  i18n("Remove Disk"),
                                                                  KGuiItem(i18n("Delete Image"), u"edit-delete"_s),
                                                                  KGuiItem(i18n("Keep Image File…"), u"document-save-as"_s));
        if (answer == KMessageBox::Cancel)
            return;

        RemovedDisk removed{entry.disk.fileName, {}};
        if (answer == KMessageBox::SecondaryAction) {
            const QString suggestion = QDir::home().filePath(u"%1-%2"_s.arg(m_original.name(), entry.disk.fileName));
            removed.keepPath = QFileDialog::getSaveFileName(this, i18n("Keep Disk Image As"), suggestion, i18n("QEMU Disk Images (*.qcow2)"));
            if (removed.keepPath.isEmpty())
                return;
            if (!m_vmDir.isEmpty() && QFileInfo(removed.keepPath).absoluteDir() == QDir(m_vmDir)) {
                KMessageBox::error(this, i18n("Please choose a location outside the folder of this virtual machine."), i18n("Remove Disk"));
                return;
            }
        }
        m_removedDisks.append(removed);
    }

    m_disks.removeAt(row);
    refreshDiskList();
}

void EditVmDialog::updateNetworkCardDependencies()
{
    const auto card = static_cast<VMSpecs::NetworkCard>(netCombo->currentData().toInt());
    const bool hasNetwork = (card != VMSpecs::NetworkCard::None);

    networkModeCombo->setEnabled(hasNetwork);
    macEdit->setEnabled(hasNetwork);
    regenerateMacButton->setEnabled(hasNetwork);
    userModeGroup->setEnabled(hasNetwork);
    vdeGroup->setEnabled(hasNetwork);
    portForwardTable->setEnabled(hasNetwork);
    addPortFwdButton->setEnabled(hasNetwork);

    if (hasNetwork && macEdit->text().isEmpty())
        macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));

    validateForm();
}

void EditVmDialog::setDiskKeyId(const QString &keyId)
{
    m_original.setDiskKeyId(keyId);
    applyEncryptionConstraints();
    refreshEncryptionState();
}

void EditVmDialog::applyEncryptionConstraints()
{
    // The saved memory would be written unencrypted.
    auto *model = qobject_cast<QStandardItemModel *>(exitActionCombo->model());
    if (!model)
        return;
    const bool encrypted = m_original.isEncrypted();
    const int saveStateIndex = exitActionCombo->findData(QVariant::fromValue(VMSpecs::ExitAction::SaveState));
    if (QStandardItem *item = model->item(saveStateIndex)) {
        item->setEnabled(!encrypted);
        item->setToolTip(encrypted ? i18n("Not available for an encrypted system disk") : QString());
    }
    if (encrypted && exitActionCombo->currentIndex() == saveStateIndex)
        setCombo(exitActionCombo, VMSpecs::ExitAction::Shutdown);
}

void EditVmDialog::refreshEncryptionState()
{
    if (!encryptButton)
        return;

    // Which buttons are shown tells whether the disk is encrypted.
    const bool encrypted = m_original.isEncrypted();
    const bool walletAvailable = DiskKeyStore::isAvailable();
    const bool stored = encrypted && DiskKeyStore::hasKey(m_original.diskKeyId());

    encryptButton->setVisible(!encrypted);
    decryptButton->setVisible(encrypted);
    changePasswordButton->setVisible(encrypted);
    walletButton->setVisible(encrypted);

    // These act at once and move an existing TPM state to the new password.
    const bool tpmToolsMissing = !m_vmDir.isEmpty() && TpmState::encryptionOf(QDir(m_vmDir)) != TpmState::Encryption::NoState
        && !ExternalTools::instance().isAvailable({ExternalTools::Swtpm, ExternalTools::SwtpmIoctl});
    const QString toolsMissing = i18n(
        "Not available: The TPM state is moved to the new password with swtpm and swtpm_ioctl, which were not found. Install or set their paths under Settings "
        "> Executables.");
    encryptButton->setEnabled(!m_readOnly && !tpmToolsMissing);
    decryptButton->setEnabled(!m_readOnly && !tpmToolsMissing);
    changePasswordButton->setEnabled(!m_readOnly && !tpmToolsMissing);
    encryptButton->setToolTip(tpmToolsMissing ? toolsMissing : i18n("Encrypt the system disk..."));
    decryptButton->setToolTip(tpmToolsMissing ? toolsMissing : i18n("Decrypt the system disk..."));
    changePasswordButton->setToolTip(tpmToolsMissing ? toolsMissing : i18n("Change the password..."));

    // KWallet's icon with a plus or minus.
    const QString walletText = !walletAvailable ? i18n("KWallet is not available")
        : stored                                ? i18n("Remove the password from KWallet")
                                                : i18n("Store the password in KWallet...");
    walletButton->setIcon(KIconUtils::addOverlay(QIcon::fromTheme(u"kwalletmanager"_s, QIcon::fromTheme(u"wallet-open"_s)),
                                                 QIcon::fromTheme(stored ? u"list-remove"_s : u"list-add"_s),
                                                 Qt::BottomRightCorner));
    walletButton->setToolTip(walletText);
    walletButton->setAccessibleName(walletText);
    walletButton->setEnabled(!m_readOnly && walletAvailable);
}

void EditVmDialog::updateToolWarnings()
{
    // Settings stay usable, since the tool may be installed later; the VM
    // just does not start without it.
    const auto show = [this](QLabel *label, bool missing, const QString &text) {
        if (!label)
            return;
        if (missing) {
            const int size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
            label->setPixmap(QIcon::fromTheme(u"dialog-warning"_s).pixmap(size));
            label->setToolTip(text);
            label->setAccessibleName(text);
        }
        label->setVisible(missing);
    };

    const ExternalTools &tools = ExternalTools::instance();
    show(tpmToolWarning,
         tpmCheck->isChecked() && !tools.isAvailable(ExternalTools::Swtpm),
         i18n("swtpm was not found. The virtual machine does not start until it is available."));

    const bool windows = static_cast<VMSpecs::OsFamily>(osFamilyCombo->currentData().toInt()) == VMSpecs::OsFamily::Windows;
    show(shareToolWarning,
         windows && !sharedPathEdit->text().trimmed().isEmpty() && !tools.isAvailable(ExternalTools::Virtiofsd),
         i18n("virtiofsd was not found. Windows guests need it for shared folders. The virtual machine does not start until it is available."));
}

QString EditVmDialog::systemDiskPath() const
{
    if (m_vmDir.isEmpty())
        return {};
    return m_original.diskPaths(QDir(m_vmDir)).value(0);
}

void EditVmDialog::updateDiskSizeButtons()
{
    if (!expandDiskButton)
        return;
    expandDiskButton->setEnabled(!m_readOnly && m_systemDiskGb > 0 && diskSpin->value() > m_systemDiskGb);
    compactDiskButton->setEnabled(!m_readOnly && m_systemDiskGb > 0);
}

void EditVmDialog::diskExpanded(const QString &diskPath, int sizeGb)
{
    if (diskPath == systemDiskPath()) {
        m_systemDiskGb = sizeGb;
        diskSpin->setMinimum(sizeGb);
        diskSpin->setValue(sizeGb);
        updateDiskSizeButtons();
        return;
    }
    if (m_openDiskDialog && m_openDiskPath == diskPath)
        m_openDiskDialog->setCurrentSize(sizeGb);
    refreshDiskList();
}

void EditVmDialog::applyModeConstraints()
{
    if (m_mode != Mode::Edit)
        return;

    // The spin box offers sizes from the current one up; the image grows
    // through the expand button only.
    const QemuImgProcess::ImageInfo info = systemDiskPath().isEmpty() ? QemuImgProcess::ImageInfo{} : QemuImgProcess::inspect(systemDiskPath());
    m_systemDiskGb = info.isValid ? int((info.virtualSizeBytes + (1LL << 30) - 1) >> 30) : 0;
    if (m_systemDiskGb > 0) {
        diskSpin->setMinimum(m_systemDiskGb);
        diskSpin->setValue(m_systemDiskGb);
        diskSpin->setToolTip(i18n("The size of the system disk. Enter a larger size and expand the disk with the button next to it."));
    } else {
        diskSpin->setEnabled(false);
        compactDiskButton->setEnabled(false);
    }
    updateDiskSizeButtons();

    // Changing these on an installed guest usually makes it unbootable.
    const QString warning = i18n("Changing this on an installed guest can make it unbootable.");
    machineCombo->setToolTip(warning);
    uefiCheck->setToolTip(warning);
    secureBootCheck->setToolTip(warning);
}

void EditVmDialog::resetToFactoryDefaults()
{
    const auto result = KMessageBox::questionTwoActions(this,
                                                        i18n("Are you sure you want to reset all settings to their default "
                                                             "values?"),
                                                        i18n("Reset to Defaults"),
                                                        KGuiItem(i18n("Reset"), QStringLiteral("edit-undo")),
                                                        KStandardGuiItem::cancel());

    if (result != KMessageBox::PrimaryAction) {
        return;
    }

    // VMConfig defaults from vmconfig.h
    ramSpin->setValue(GlobalDefaults.ramMb);
    cpuSpin->setValue(GlobalDefaults.cpuCoreCount);
    if (m_mode != Mode::Edit)
        diskSpin->setValue(GlobalDefaults.diskSizeGb);

    setCombo(cpuCombo, GlobalDefaults.cpuModel);
    kvmCheck->setChecked(GlobalDefaults.useKvm);
    setCombo(machineCombo, GlobalDefaults.machineType);
    setCombo(gpuCombo, GlobalDefaults.gpuType);
    gpuMemSpin->setValue(GlobalDefaults.gpuMemMb);
    setCombo(audioCombo, GlobalDefaults.audioDevice);
    setCombo(diskInterfaceCombo, GlobalDefaults.diskInterface);
    setCombo(netCombo, GlobalDefaults.networkCard);
    setCombo(displayCombo, GlobalDefaults.displayType);
    setCombo(usbCombo, GlobalDefaults.usbController);
    setCombo(mouseCombo, GlobalDefaults.pointerDevice);
    setCombo(keyboardCombo, GlobalDefaults.keyboardDevice);

    const QSize defaultRes = SettingsDialog::getDefaultResolution();
    for (int i = 0; i < resolutionCombo->count(); ++i) {
        if (resolutionCombo->itemData(i).toSize() == defaultRes) {
            resolutionCombo->setCurrentIndex(i);
            break;
        }
    }

    accel3dCheck->setChecked(GlobalDefaults.gpu3dAcceleration);

    uefiCheck->setChecked(GlobalDefaults.useUefi);
    tpmCheck->setChecked(GlobalDefaults.useTpm);
    secureBootCheck->setChecked(GlobalDefaults.useSecureBoot);
    acpiCheck->setChecked(GlobalDefaults.useAcpi);
    localTimeCheck->setChecked(GlobalDefaults.useLocalTime);
    setCombo(exitActionCombo, GlobalDefaults.exitAction);
    setCombo(guestShutdownActionCombo, GlobalDefaults.guestShutdownAction);

    m_sb16Config = Sb16Config();

    extraArgsEdit->clear();
    guestAgentCheck->setChecked(GlobalDefaults.useGuestAgent);
}

QStringList EditVmDialog::usedMacAddresses() const
{
    QStringList macs;
    for (const auto &info : m_macUsageInfo)
        macs << info.mac;
    return macs;
}
