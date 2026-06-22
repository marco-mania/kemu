/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "createvmdialog.h"

#include "const.h"
#include "dialogs/cloudconfigdialog.h"
#include "dialogs/iconchooserdialog.h"
#include "dialogs/settingsdialog.h"
#include "utils/utils.h"
#include "utils/vmconfig.h"
#include <kmessagebox.h>

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
} // namespace

CreateVmDialog::CreateVmDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18n("VM Configuration"));
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

    // Icon button
    iconDisplayButton = new QPushButton();
    iconDisplayButton->setFixedSize(132, 132);
    iconDisplayButton->setIconSize(QSize(112, 112));
    iconDisplayButton->setIcon(QIcon::fromTheme(u"computer"_s));
    iconDisplayButton->setCursor(Qt::PointingHandCursor);
    iconDisplayButton->setToolTip(i18n("Click: Choose system theme icon\nRight-click: More options"));
    iconDisplayButton->setContextMenuPolicy(Qt::CustomContextMenu);

    topLayout->addWidget(iconDisplayButton, 0, Qt::AlignTop);

    // Remaining form fields
    auto *genLayout = new QFormLayout();
    genLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    osTypeCombo = new QComboBox();
    osTypeCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    osTypeCombo->addItem(i18n("GNU/Linux"), static_cast<int>(VMSpecs::OsType::Linux));
    osTypeCombo->addItem(i18n("Microsoft Windows"), static_cast<int>(VMSpecs::OsType::Windows));
    osTypeCombo->addItem(i18n("BSD"), static_cast<int>(VMSpecs::OsType::BSD));
    osTypeCombo->addItem(i18n("DOS"), static_cast<int>(VMSpecs::OsType::DOS));
    osTypeCombo->addItem(i18n("Other"), static_cast<int>(VMSpecs::OsType::Other));
    osTypeCombo->insertItem(0, i18n("-- Please select --"), -1);
    osTypeCombo->setCurrentIndex(0);
    genLayout->addRow(i18n("OS Type:"), osTypeCombo);

    auto *osTypeModel = qobject_cast<QStandardItemModel *>(osTypeCombo->model());
    if (osTypeModel) {
        QStandardItem *item = osTypeModel->item(0);
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

    applyDefaultsButton = new QPushButton(QIcon::fromTheme(u"document-import"_s), {});
    applyDefaultsButton->setToolTip(i18n("Apply recommended RAM, CPU, disk and device settings for this OS"));
    applyDefaultsButton->setEnabled(false);
    applyDefaultsButton->setFixedWidth(applyDefaultsButton->sizeHint().height());

    auto *nameLayout = new QHBoxLayout();
    nameLayout->addWidget(osNameCombo, 1);
    nameLayout->addWidget(applyDefaultsButton);

    genLayout->addRow(i18n("OS Name:"), nameLayout);

    osNameLabel = qobject_cast<QLabel *>(genLayout->labelForField(nameLayout));

    // --- Automated Deploy Container ---
    adContainer = new QWidget();
    {
        const KColorScheme qdcontainerscheme(QPalette::Active, KColorScheme::Header);
        const QColor bg = qdcontainerscheme.background(KColorScheme::NormalBackground).color();
        adContainer->setStyleSheet(
            u"QWidget#adContainer {"
            "  background-color: %1;"
            "  border-radius: 6px;"
            "}"_s.arg(bg.name(QColor::HexArgb)));
    }
    adContainer->setObjectName(u"adContainer"_s);

    auto *adFormLayout = new QFormLayout(adContainer);
    adFormLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    adFormLayout->setContentsMargins(8, 8, 8, 8);

    automatedDeployCheck = new QCheckBox(i18n("Automated Deploy"));
    automatedDeployCheck->setToolTip(i18n("Deploy from a pre-built cloud image with cloud-init configuration."));
    automatedDeployCheck->setEnabled(false);
    adFormLayout->addRow({}, automatedDeployCheck);

    versionCombo = new QComboBox();
    versionCombo->addItem(i18n("(no version data available)"));
    versionCombo->setEnabled(false);
    versionCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    adFormLayout->addRow(i18n("Version:"), versionCombo);
    versionLabel = qobject_cast<QLabel *>(adFormLayout->labelForField(versionCombo));

    cloudImageCombo = new QComboBox();
    cloudImageCombo->addItem(i18n("(select a version first)"));
    cloudImageCombo->setEnabled(false);
    cloudImageCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    adFormLayout->addRow(i18n("Image:"), cloudImageCombo);
    cloudImageLabel = qobject_cast<QLabel *>(adFormLayout->labelForField(cloudImageCombo));

    cloudImageStatusLabel = new QLabel();
    cloudImageStatusLabel->setStyleSheet(infoStyle(KColorScheme::NeutralText));
    cloudImageStatusLabel->setFixedHeight(cloudImageStatusLabel->fontMetrics().height() + 2);
    adFormLayout->addRow({}, cloudImageStatusLabel);

    // Manual ISO path (for distros without cloud images, e.g. Windows)
    auto *adIsoLayout = new QHBoxLayout();
    adIsoPathRequester = new KUrlRequester();
    adIsoPathRequester->setEnabled(false);
    adIsoPathRequester->lineEdit()->setPlaceholderText(i18n("Select the ISO file"));
    adIsoPathRequester->lineEdit()->setClearButtonEnabled(true);
    adIsoPathRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    adIsoPathRequester->setNameFilter(i18n("ISO Images (*.iso)"));
    adIsoLayout->addWidget(adIsoPathRequester);
    adFormLayout->addRow(i18n("ISO Image:"), adIsoLayout);
    adIsoLabel = qobject_cast<QLabel *>(adFormLayout->labelForField(adIsoLayout));
    if (adIsoLabel)
        adIsoLabel->setEnabled(false);

    // Cloud Config Button
    cloudInitButton = new QPushButton(QIcon::fromTheme(u"preferences-system"_s), i18n("Cloud Image Settings..."));
    cloudInitButton->setToolTip(i18n("Configure hostname, user, password, SSH keys and more."));
    cloudInitButton->setEnabled(false);
    cloudInitButton->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);

    auto *cloudInitLayout = new QHBoxLayout();
    cloudInitLayout->addStretch();
    cloudInitLayout->addWidget(cloudInitButton);
    adFormLayout->addRow(cloudInitLayout);

    genLayout->addRow(adContainer);

    diskSpin = new QSpinBox();
    diskSpin->setRange(KEMUConst::DefaultMinDiskSizeGB, 32000);
    diskSpin->setValue(40);
    diskSpin->setSuffix(i18n(" GB"));
    genLayout->addRow(i18n("Disk:"), diskSpin);
    diskLabel = qobject_cast<QLabel *>(genLayout->labelForField(diskSpin));

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
    sysLayout->addRow({}, tpmCheck);

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

    // RAM
    auto *ramContainer = new QVBoxLayout();
    auto *ramSliderLayout = new QHBoxLayout();

    const QList<int> ramSteps = {16,   32,   64,   128,   256,   512,   768,   1024,  1536,  2048,  3072,
                                 4096, 6144, 8192, 12288, 16384, 24576, 32768, 49152, 65536, 98304, 131072};

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
    hwLayout->addRow(i18n("Disk Controller:"), diskInterfaceCombo);

    diskHintLabel = new QLabel();
    diskHintLabel->setStyleSheet(infoStyle(KColorScheme::NeutralText));
    hwLayout->addRow({}, diskHintLabel);

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
    mouseCombo->setToolTip(
        i18n("PS/2 and USB mouse are relative pointers; USB tablet gives absolute "
             "positioning (seamless, no grab). USB options need a USB controller."));
    setCombo(mouseCombo, GlobalDefaults.pointerDevice);
    hwLayout->addRow(i18n("Mouse:"), mouseCombo);

    keyboardCombo = new QComboBox();
    keyboardCombo->addItem(VMConfig::toUIString(VMSpecs::KeyboardDevice::Ps2), static_cast<int>(VMSpecs::KeyboardDevice::Ps2));
    keyboardCombo->addItem(VMConfig::toUIString(VMSpecs::KeyboardDevice::Usb), static_cast<int>(VMSpecs::KeyboardDevice::Usb));
    keyboardCombo->setToolTip(i18n("USB keyboard needs a USB controller. Legacy guests require PS/2."));
    setCombo(keyboardCombo, GlobalDefaults.keyboardDevice);
    hwLayout->addRow(i18n("Keyboard:"), keyboardCombo);

    displayCombo = new QComboBox();
    displayCombo->addItem(VMConfig::toUIString(VMSpecs::DisplayType::Gtk), static_cast<int>(VMSpecs::DisplayType::Gtk));
    displayCombo->addItem(VMConfig::toUIString(VMSpecs::DisplayType::Sdl), static_cast<int>(VMSpecs::DisplayType::Sdl));
    if (ExternalTools::instance().isAvailable(ExternalTools::spiceDisplayTools()))
        displayCombo->addItem(VMConfig::toUIString(VMSpecs::DisplayType::Spice), static_cast<int>(VMSpecs::DisplayType::Spice));
    if (ExternalTools::instance().isAvailable(ExternalTools::dbusDisplayTools()))
        displayCombo->addItem(VMConfig::toUIString(VMSpecs::DisplayType::Dbus), static_cast<int>(VMSpecs::DisplayType::Dbus));
    hwLayout->addRow(i18n("Display:"), displayCombo);

    gpuCombo = new QComboBox();
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Vga), static_cast<int>(VMSpecs::GpuType::Vga));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Virtio), static_cast<int>(VMSpecs::GpuType::Virtio));
    gpuCombo->addItem(VMConfig::toUIString(VMSpecs::GpuType::Qxl) + i18n(" (SPICE optimized)"), static_cast<int>(VMSpecs::GpuType::Qxl));
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
             "Requires VirtIO GPU with GTK or SDL display.\n"
             "Not available with SPICE or Standard VGA/QXL."));
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

    // --- SPICE Options (end of hardware tab) ---
    spiceGroup = new QGroupBox(i18n("SPICE Options"));
    auto *spiceLayout = new QFormLayout(spiceGroup);

    spiceAgentCheck = new QCheckBox(i18n("Clipboard sharing and resolution sync"));
    spiceAgentCheck->setChecked(true);
    spiceAgentCheck->setToolTip(i18n("Requires spice-vdagent in the guest."));
    spiceLayout->addRow(spiceAgentCheck);

    spiceUsbRedirCheck = new QCheckBox(i18n("USB device redirection"));
    spiceUsbRedirCheck->setChecked(false);
    spiceLayout->addRow(spiceUsbRedirCheck);

    spiceUsbRedirCountSpin = new QSpinBox;
    spiceUsbRedirCountSpin->setRange(1, 4);
    spiceUsbRedirCountSpin->setValue(2);
    spiceUsbRedirCountSpin->setEnabled(false);
    spiceLayout->addRow(i18n("Redirect channels:"), spiceUsbRedirCountSpin);
    spiceUsbRedirCountLabel = qobject_cast<QLabel *>(spiceLayout->labelForField(spiceUsbRedirCountSpin));
    spiceUsbRedirCountLabel->setEnabled(false);

    connect(spiceUsbRedirCheck, &QCheckBox::toggled, spiceUsbRedirCountSpin, &QSpinBox::setEnabled);
    connect(spiceUsbRedirCheck, &QCheckBox::toggled, spiceUsbRedirCountLabel, &QLabel::setEnabled);

    spiceGroup->setEnabled(false);
    hwLayout->addRow(spiceGroup);

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
    networkModeCombo->addItem(VMConfig::toUIString(VMSpecs::NetworkMode::Passt), static_cast<int>(VMSpecs::NetworkMode::Passt));
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

    // --- Passt Section (Port Forwarding) ---
    passtGroup = new QGroupBox(i18n("Port Forwarding (Host → Guest)"));
    auto *pfLayout = new QVBoxLayout(passtGroup);

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

    netMainLayout->addWidget(passtGroup);

    // --- Virtual Network Section ---
    vdeGroup = new QGroupBox(i18n("Virtual Network"));
    auto *vdeLayout = new QFormLayout(vdeGroup);
    vdeLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    virtualNetworkCombo = new QComboBox();
    vdeLayout->addRow(i18n("Network:"), virtualNetworkCombo);
    netMainLayout->addWidget(vdeGroup);

    passtGroup->setVisible(true);
    vdeGroup->setVisible(false);

    netMainLayout->addStretch();

    tabs->addTab(netTab, QIcon::fromTheme(u"network-wired"_s), i18n("Network"));

    // --- Advanced Tab ---
    auto *advTab = new QWidget();
    auto *advLayout = new QFormLayout(advTab);
    advLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    extraArgsEdit = new QLineEdit();
    extraArgsEdit->setPlaceholderText(i18n("Additional QEMU parameters"));
    extraArgsEdit->setClearButtonEnabled(true);
    advLayout->addRow(i18n("Extra Arguments:"), extraArgsEdit);

    tabs->addTab(advTab, QIcon::fromTheme(u"configure"_s), i18n("Advanced"));

    mainLayout->addWidget(tabs);

    connect(nameEdit, &QLineEdit::textChanged, this, &CreateVmDialog::onNameChanged);
    connect(iconDisplayButton, &QPushButton::clicked, this, &CreateVmDialog::chooseFromIconSet);
    connect(iconDisplayButton, &QPushButton::customContextMenuRequested, this, &CreateVmDialog::showIconContextMenu);
    connect(osTypeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateVmDialog::onOsTypeChanged);
    connect(applyDefaultsButton, &QPushButton::clicked, this, [this]() {
        if (!m_imageDatabase)
            return;
        const QString tplId = osNameCombo->currentData().toString();
        if (tplId.isEmpty() || tplId == u"other"_s)
            return;
        applyReleaseDefaults(m_imageDatabase->defaultsFor(tplId));
    });
    connect(osNameCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateVmDialog::onOsNameChanged);
    connect(osNameCombo, &QComboBox::editTextChanged, this, [this]() {
        autoSetIconFromOS();
        validateForm();
    });
    connect(automatedDeployCheck, &QCheckBox::toggled, this, &CreateVmDialog::onAutomatedDeployToggled);
    connect(versionCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateVmDialog::onVersionChanged);
    connect(cloudImageCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        updateCloudImageStatus();
        updateAutomatedDeployWidgets();
    });
    connect(adIsoPathRequester, &KUrlRequester::textChanged, this, [this]() {
        validateForm();
    });
    connect(cloudInitButton, &QPushButton::clicked, this, &CreateVmDialog::openCloudConfigDialog);
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

    connect(btnBrowseShare, &QPushButton::clicked, this, &CreateVmDialog::browseSharedDir);
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

    connect(diskInterfaceCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateVmDialog::updateDiskInterfaceHint);

    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateVmDialog::updateDiskInterfaceHint);
    connect(machineCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &CreateVmDialog::onMachineTypeChanged);

    connect(usbCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const bool hasUsb = static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()) != VMSpecs::UsbController::None;
        updateInputDeviceAvailability();

        // SPICE USB redirection requires an USB controller
        spiceUsbRedirCheck->setEnabled(hasUsb);
        if (!hasUsb)
            spiceUsbRedirCheck->setChecked(false);

        const QString tip = hasUsb ? QString() : i18n("Requires a USB controller.");
        spiceUsbRedirCheck->setToolTip(tip);
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
    connect(gpuCombo, &QComboBox::currentIndexChanged, this, &CreateVmDialog::onGpuDisplayChanged);
    connect(displayCombo, &QComboBox::currentIndexChanged, this, &CreateVmDialog::onGpuDisplayChanged);
    connect(accel3dCheck, &QCheckBox::toggled, this, &CreateVmDialog::onGpuDisplayChanged);
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
    connect(addPortFwdButton, &QPushButton::clicked, this, &CreateVmDialog::addPortForwardRow);
    connect(removePortFwdButton, &QPushButton::clicked, this, &CreateVmDialog::removePortForwardRow);
    connect(portForwardTable, &QTableWidget::itemSelectionChanged, this, [this]() {
        removePortFwdButton->setEnabled(portForwardTable->currentRow() >= 0);
    });
    connect(networkModeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        auto mode = static_cast<VMSpecs::NetworkMode>(networkModeCombo->currentData().toInt());
        passtGroup->setVisible(mode == VMSpecs::NetworkMode::Passt);
        vdeGroup->setVisible(mode == VMSpecs::NetworkMode::VirtualNetwork);
    });

    connect(netCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() {
        const bool hasNetwork = static_cast<VMSpecs::NetworkCard>(netCombo->currentData().toInt()) != VMSpecs::NetworkCard::None;
        networkModeCombo->setEnabled(hasNetwork);
        macEdit->setEnabled(hasNetwork);
        passtGroup->setEnabled(hasNetwork);
        vdeGroup->setEnabled(hasNetwork);
        portForwardTable->setEnabled(hasNetwork);
        addPortFwdButton->setEnabled(hasNetwork);
        auto card = static_cast<VMSpecs::NetworkCard>(netCombo->currentData().toInt());
        const bool hasNet = (card != VMSpecs::NetworkCard::None);
        regenerateMacButton->setEnabled(hasNet);
        if (hasNet && macEdit->text().isEmpty())
            macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));
        validateForm();
    });

    connect(regenerateMacButton, &QPushButton::clicked, this, [this]() {
        macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));
    });

    connect(macEdit, &QLineEdit::textEdited, this, [this]() {
        validateForm();
    });

    buttonBox = new QDialogButtonBox(QDialogButtonBox::RestoreDefaults | QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(buttonBox->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked, this, &CreateVmDialog::resetToFactoryDefaults);
    mainLayout->addWidget(buttonBox);

    if (macEdit->text().isEmpty() && netCombo->currentData().toInt() != static_cast<int>(VMSpecs::NetworkCard::None)) {
        macEdit->setText(Utils::generateRandomMac(usedMacAddresses()));
    }

    onGpuDisplayChanged();
    updateIconDisplay();
    updateDiskInterfaceHint();
    updateDeviceAvailability();

    // Sync SPICE USB redir with the initial USB controller state.
    {
        const bool hasUsb = static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()) != VMSpecs::UsbController::None;
        spiceUsbRedirCheck->setEnabled(hasUsb);
        if (!hasUsb)
            spiceUsbRedirCheck->setChecked(false);
    }

    updateRamSlider(GlobalDefaults.ramMb);
}

// --- Icon handling ---

void CreateVmDialog::chooseFromIconSet()
{
    IconChooserDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString path = dlg.selectedIconPath();
    if (path.isEmpty())
        return;

    const QDir baseDir(VMConfig::osIconDir());
    selectedIconSetFilename = baseDir.relativeFilePath(path);

    customIcon = {};
    selectedIconName.clear();
    iconManuallySet = true;
    updateIconDisplay();
}

void CreateVmDialog::chooseThemeIcon()
{
    const QString icon = KIconDialog::getIcon(KIconLoader::Desktop, KIconLoader::Application, false, 0, false, this, i18n("Choose Icon"));
    if (icon.isEmpty())
        return;

    selectedIconName = icon;
    selectedIconSetFilename.clear();
    customIcon = {};
    iconManuallySet = true;
    updateIconDisplay();
}

void CreateVmDialog::browseCustomIcon()
{
    const QString path =
        QFileDialog::getOpenFileName(this, i18n("Select Icon Image"), QDir::homePath(), i18n("Images (*.png *.svg *.svgz *.xpm *.ico *.jpg *.jpeg *.bmp)"));

    if (path.isEmpty())
        return;

    // Load as QIcon directly - handles all formats including SVG
    const QIcon icon(path);
    if (icon.isNull()) {
        KMessageBox::error(this, i18n("Failed to load icon from: %1", path));
        return;
    }

    customIcon = icon;
    selectedIconName.clear();
    selectedIconSetFilename.clear();
    iconManuallySet = true;
    updateIconDisplay();
}

void CreateVmDialog::clearIcon()
{
    selectedIconName.clear();
    selectedIconSetFilename.clear();
    customIcon = {};
    iconManuallySet = false;

    updateIconDisplay();
}

void CreateVmDialog::showIconContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(u"preferences-desktop-icons"_s), i18n("Choose Icon..."), this, &CreateVmDialog::chooseFromIconSet);
    menu.addAction(QIcon::fromTheme(u"preferences-desktop-icons"_s), i18n("Choose System Theme Icon..."), this, &CreateVmDialog::chooseThemeIcon);
    menu.addAction(QIcon::fromTheme(u"document-open"_s), i18n("Load Custom Image..."), this, &CreateVmDialog::browseCustomIcon);
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(u"edit-clear"_s), i18n("Reset Icon"), this, &CreateVmDialog::clearIcon);
    menu.exec(iconDisplayButton->mapToGlobal(pos));
}

void CreateVmDialog::openCloudConfigDialog()
{
    CloudConfigDialog dlg(this);
    dlg.setConfig(m_cloudConfig);

    if (dlg.exec() == QDialog::Accepted) {
        m_cloudConfig = dlg.getResult();

        // Set templateId based on selected distro (not managed by the dialog)
        if (m_imageDatabase) {
            const QString tplId = osNameCombo->currentData().toString();
            if (auto *d = m_imageDatabase->findById(tplId)) {
                m_cloudConfig.templateId = d->cloudInitTemplate;
                m_cloudConfig.networkTemplateId = d->networkTemplate;
            }
        }
    }
}

void CreateVmDialog::browseQdIso()
{
    const QString path = QFileDialog::getOpenFileName(this, i18n("Select ISO Image"), QDir::homePath(), i18n("ISO Files (*.iso);;All Files (*)"));
    if (!path.isEmpty())
        adIsoPathRequester->setText(path);
    validateForm();
}

void CreateVmDialog::updateIconDisplay()
{
    const QSize targetSize = iconDisplayButton->iconSize(); // 112x112

    // Custom Icon
    if (!customIcon.isNull()) {
        iconDisplayButton->setIcon(customIcon);
        return;
    }

    // Icon-Set File
    if (!selectedIconSetFilename.isEmpty()) {
        QString dir = VMConfig::osIconDir();
        if (!dir.isEmpty()) {
            const QString fullPath = dir + u"/"_s + selectedIconSetFilename;
            if (QFile::exists(fullPath)) {
                // Handle SVG icons from icon set
                if (fullPath.endsWith(u".svg", Qt::CaseInsensitive)) {
                    iconDisplayButton->setIcon(QIcon(fullPath));
                    return;
                }
                // Fallback for non-SVG
                QPixmap pix(fullPath);
                if (!pix.isNull()) {
                    pix = pix.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    iconDisplayButton->setIcon(QIcon(pix));
                    return;
                }
            }
        }
        iconDisplayButton->setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
        return;
    }

    // System Theme Icon
    if (!selectedIconName.isEmpty()) {
        // Get themed icon at appropriate size
        const QIcon themedIcon = QIcon::fromTheme(selectedIconName);
        if (!themedIcon.isNull()) {
            const QPixmap scaled = themedIcon.pixmap(targetSize);
            if (!scaled.isNull()) {
                iconDisplayButton->setIcon(QIcon(scaled));
            } else {
                iconDisplayButton->setIcon(themedIcon);
            }
        } else {
            iconDisplayButton->setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
        }
        return;
    }

    // Default
    iconDisplayButton->setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
}

void CreateVmDialog::onMachineTypeChanged(int /*index*/)
{
    updateDeviceAvailability();
}

void CreateVmDialog::onRamSliderChanged(int gb)
{
    const QSignalBlocker blockRAMSpin(ramSpin);
    ramSpin->setValue(gb * 1024);
}

void CreateVmDialog::onRamSpinChanged(int mb)
{
    const QSignalBlocker blockRAMSlider(ramSlider);
    ramSlider->setValue(mb / 1024);
}

void CreateVmDialog::browseSharedDir()
{
    const QString dir = QFileDialog::getExistingDirectory(this, i18n("Select shared folder"), QDir::homePath());
    if (!dir.isEmpty())
        sharedPathEdit->setText(dir);
}

void CreateVmDialog::onOsTypeChanged(int /*index*/)
{
    const int typeVal = osTypeCombo->currentData().toInt();

    if (typeVal == -1) {
        // Placeholder
        osNameCombo->clear();
        osNameCombo->addItem(i18n("-- Please select --"), {});
        osNameCombo->setCurrentIndex(0);
        osNameCombo->setEnabled(false);
        osNameLabel->setEnabled(false);
        applyDefaultsButton->setEnabled(false);

    } else if (static_cast<VMSpecs::OsType>(typeVal) == VMSpecs::OsType::Other) {
        // special case "Other"
        osNameCombo->clear();
        osNameCombo->addItem(i18n("Other"), u"other"_s);
        osNameCombo->setCurrentIndex(0);
        osNameCombo->setEnabled(false);
        osNameLabel->setEnabled(false);
        applyDefaultsButton->setEnabled(false);

    } else {
        osNameCombo->setEnabled(true);
        osNameLabel->setEnabled(true);
        auto osType = static_cast<VMSpecs::OsType>(typeVal);
        filterOsVariants(osType);
    }

    autoSetIconFromOS();
    validateForm();
    updateDiskInterfaceHint();
}

void CreateVmDialog::onOsNameChanged(int /*index*/)
{
    autoSetIconFromOS();

    const QString distroId = osNameCombo->currentData().toString();
    bool hasReleases = false;
    bool quickDeployAllowed = true;

    if (m_imageDatabase && !distroId.isEmpty() && distroId != u"other"_s) {
        const auto *d = m_imageDatabase->findById(distroId);
        hasReleases = d && !d->releases.isEmpty();

        if (d) {
            quickDeployAllowed = false;
            for (const auto &r : d->releases) {
                if (r.quickDeploy) {
                    quickDeployAllowed = true;
                    break;
                }
            }
        }
    }

    automatedDeployCheck->setEnabled(hasReleases && quickDeployAllowed);
    if (!hasReleases || !quickDeployAllowed) {
        automatedDeployCheck->setChecked(false);
        versionCombo->setEnabled(false);
        versionLabel->setEnabled(false);
        cloudImageCombo->setEnabled(false);
        cloudImageLabel->setEnabled(false);
        cloudImageStatusLabel->clear();
        int minDiskGb = KEMUConst::DefaultMinDiskSizeGB;
        if (m_imageDatabase) {
            const int templateMin = m_imageDatabase->minDiskGbFor(distroId, {});
            minDiskGb = qMin(minDiskGb, templateMin);
        }
        diskSpin->setMinimum(minDiskGb);
        updateAutomatedDeployWidgets();
    }

    if (automatedDeployCheck->isChecked())
        populateVersions();

    applyDefaultsButton->setEnabled(m_imageDatabase && !distroId.isEmpty() && distroId != u"other"_s);

    validateForm();
}

void CreateVmDialog::onGpuDisplayChanged()
{
    const auto gpu = static_cast<VMSpecs::GpuType>(gpuCombo->currentData().toInt());
    const auto display = static_cast<VMSpecs::DisplayType>(displayCombo->currentData().toInt());
    const auto osType = static_cast<VMSpecs::OsType>(osTypeCombo->currentData().toInt());

    const bool isQxl = (gpu == VMSpecs::GpuType::Qxl);
    const bool isVirtio = (gpu == VMSpecs::GpuType::Virtio);
    const bool isSpice = (display == VMSpecs::DisplayType::Spice);
    const bool isWindows = (osType == VMSpecs::OsType::Windows);

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

    const bool can3d = isVirtio && !isSpice;
    accel3dCheck->setEnabled(can3d);
    if (!can3d)
        accel3dCheck->setChecked(false);

    // Hints (mutually exclusive, priority order)
    if (isSpice && !isQxl) {
        gpuHintLabel->setText(u"<i>"_s
                              + i18n("Tip: QXL is optimized for SPICE and offers "
                                     "better 2D performance.")
                              + u"</i>"_s);
    } else if (isQxl && !isSpice) {
        gpuHintLabel->setText(u"<i>"_s + i18n("Tip: QXL works best with the SPICE display.") + u"</i>"_s);
    } else if (can3d && isWindows && accel3dCheck->isChecked()) {
        gpuHintLabel->setText(u"<i>"_s
                              + i18n("Note: VirtIO 3D acceleration is not supported "
                                     "on Windows guests.")
                              + u"</i>"_s);
    } else {
        gpuHintLabel->clear();
    }

    if (spiceGroup)
        spiceGroup->setEnabled(isSpice);
}

void CreateVmDialog::onVersionChanged(int /*index*/)
{
    cloudImageCombo->clear();
    cloudImageStatusLabel->clear();

    const QString tplId = osNameCombo->currentData().toString();
    const QString version = versionCombo->currentData().toString();

    if (tplId.isEmpty() || version.isEmpty() || !m_imageDatabase) {
        cloudImageCombo->addItem(i18n("(select a version first)"));
        cloudImageCombo->setEnabled(false);
        updateAutomatedDeployWidgets();
        return;
    }

    const bool qdEnabled = m_imageDatabase->isQuickDeployEnabled(tplId, version);

    if (!qdEnabled) {
        automatedDeployCheck->setChecked(false);
        automatedDeployCheck->setEnabled(false);
        cloudImageCombo->addItem(i18n("(not available for this release)"));
        cloudImageCombo->setEnabled(false);
        updateAutomatedDeployWidgets();
        return;
    }

    auto images = m_imageDatabase->cloudImagesFor(tplId, version, SettingsDialog::showAllArchitectures());
    for (const auto &img : images) {
        const QString label = u"%1 (%2)"_s.arg(img.arch, img.format);
        cloudImageCombo->addItem(label);
    }

    if (m_imageDatabase && automatedDeployCheck->isChecked()) {
        const int minGb = m_imageDatabase->minDiskGbFor(tplId, version);
        diskSpin->setMinimum(minGb);
        if (diskSpin->value() < minGb)
            diskSpin->setValue(minGb);
    }

    if (images.isEmpty()) {
        cloudImageCombo->addItem(i18n("(no cloud image available)"));
        cloudImageCombo->setEnabled(false);
    } else {
        cloudImageCombo->setEnabled(automatedDeployCheck->isChecked());
    }

    updateCloudImageStatus();
    updateAutomatedDeployWidgets();
}

void CreateVmDialog::onAutomatedDeployToggled(bool checked)
{
    if (checked) {
        populateVersions();
    } else {
        diskSpin->setMinimum(KEMUConst::DefaultMinDiskSizeGB);
    }
    updateAutomatedDeployWidgets();
    validateForm();
}

void CreateVmDialog::onNameChanged(const QString &text)
{
    // OS guessing
    bool found;
    auto [type, variantId] = VMConfig::guessOSFromText(text, &found);
    if (found) {
        const int typeIndex = osTypeCombo->findData(static_cast<int>(type));
        if (typeIndex >= 0 && osTypeCombo->currentIndex() != typeIndex)
            osTypeCombo->setCurrentIndex(typeIndex);
        const int variantIndex = osNameCombo->findData(variantId);
        if (variantIndex >= 0 && osNameCombo->currentIndex() != variantIndex)
            osNameCombo->setCurrentIndex(variantIndex);
        autoSetIconFromOS();
    }

    validateForm();
}

void CreateVmDialog::addPortForwardRow()
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

void CreateVmDialog::removePortForwardRow()
{
    const int row = portForwardTable->currentRow();
    if (row >= 0)
        portForwardTable->removeRow(row);
    removePortFwdButton->setEnabled(portForwardTable->rowCount() > 0);
}

void CreateVmDialog::filterOsVariants(VMSpecs::OsType osType)
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
        for (const auto &d : m_imageDatabase->templatesForType(osType))
            osNameCombo->addItem(d.displayName, d.id);
    }

    if (auto *comp = osNameCombo->completer())
        comp->setCompletionMode(QCompleter::PopupCompletion);

    if (osNameCombo->count() > 0)
        osNameCombo->insertSeparator(osNameCombo->count());
    osNameCombo->addItem(i18n("Other"), u"other"_s);

    osNameCombo->setCurrentIndex(-1);
}

void CreateVmDialog::autoSetIconFromOS()
{
    if (iconManuallySet)
        return;

    auto osType = static_cast<VMSpecs::OsType>(osTypeCombo->currentData().toInt());
    QString variantId = osNameCombo->currentData().toString();
    if (variantId.isEmpty())
        variantId = osNameCombo->currentText().trimmed();
    const QString path = VMConfig::defaultIconFor(osType, variantId);

    selectedIconName.clear();
    selectedIconSetFilename.clear();
    customIcon = {};

    if (!path.isEmpty() && QFile::exists(path)) {
        const QDir baseDir(VMConfig::osIconDir());
        selectedIconSetFilename = baseDir.relativeFilePath(path);
    }

    updateIconDisplay();
}

void CreateVmDialog::populateVersions()
{
    versionCombo->clear();

    const QString distroId = osNameCombo->currentData().toString();
    if (distroId.isEmpty() || !m_imageDatabase) {
        versionCombo->addItem(i18n("(no releases available)"));
        versionCombo->setEnabled(false);
        versionLabel->setEnabled(false);
        cloudImageLabel->setEnabled(false);
        return;
    }

    for (const auto &r : m_imageDatabase->releasesFor(distroId)) {
        QString label = r.displayString();
        if (r.lts)
            label += u" (LTS)"_s;
        versionCombo->addItem(label, r.version);
    }

    if (versionCombo->count() == 0) {
        versionCombo->addItem(i18n("(no releases available)"));
        versionCombo->setEnabled(false);
        versionLabel->setEnabled(false);
        cloudImageLabel->setEnabled(false);
        return;
    }

    versionCombo->setCurrentIndex(0);

    const bool active = automatedDeployCheck->isChecked();
    versionCombo->setEnabled(active);
    versionLabel->setEnabled(active);
    cloudImageLabel->setEnabled(active);
}

void CreateVmDialog::setupForEdit(const VMConfig &config)
{
    isEditMode = true;
    setWindowTitle(i18n("Edit VM: %1", config.name()));

    originalName = config.name();
    nameEdit->setText(config.name());

    originalMac = config.macAddress();

    m_originalArchitecture = config.architecture();

    diskSpin->setValue(config.diskSizeGb());
    diskSpin->setDisabled(true);
    diskLabel->setDisabled(true);

    commentEdit->setPlainText(config.comment());

    setCombo(osTypeCombo, config.osType());
    const int variantIdx = osNameCombo->findData(config.distroId());
    if (variantIdx >= 0)
        osNameCombo->setCurrentIndex(variantIdx);
    else if (!config.distroId().isEmpty())
        osNameCombo->setEditText(config.distroId());

    if (config.osType() != VMSpecs::OsType::Other) {
        osNameCombo->setEnabled(true);
        if (osNameLabel)
            osNameLabel->setEnabled(true);
    }

    // Icon - based on source type
    switch (config.iconSource()) {
    case VMConfig::IconSource::Custom:
        customIcon = config.customIcon();
        selectedIconName.clear();
        selectedIconSetFilename.clear();
        break;
    case VMConfig::IconSource::IconSet:
        selectedIconSetFilename = config.iconIdentifier();
        selectedIconName.clear();
        customIcon = {};
        break;
    case VMConfig::IconSource::SystemTheme:
        selectedIconName = config.iconIdentifier();
        selectedIconSetFilename.clear();
        customIcon = {};
        break;
    case VMConfig::IconSource::None:
    default:
        selectedIconName.clear();
        selectedIconSetFilename.clear();
        customIcon = {};
        break;
    }

    // Check if VM has any saved icon data
    iconManuallySet = !customIcon.isNull() || !selectedIconSetFilename.isEmpty() || !selectedIconName.isEmpty();

    updateIconDisplay();

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
    sharedPathEdit->setText(config.sharedPath());
    sharedNameEdit->setText(config.sharedName());

    setCombo(machineCombo, config.machineType());

    updateDeviceAvailability();

    if (!isDisplayTypeAvailable(config.displayType())) {
        const auto configuredType = config.displayType();
        const auto fallbackType = VMSpecs::DisplayType::Gtk;
        KMessageBox::information(this,
                                 i18n("The configured display type '%1' is currently not available.\n"
                                      "KEMU will set to '%2' instead.\n\n",
                                      VMConfig::toUIString(configuredType),
                                      VMConfig::toUIString(fallbackType)),
                                 i18n("Display Type Unavailable"));
    }

    setCombo(displayCombo, config.displayType());
    spiceAgentCheck->setChecked(config.spiceAgent());
    spiceUsbRedirCheck->setChecked(config.spiceUsbRedir());
    spiceUsbRedirCountSpin->setValue(config.spiceUsbRedirCount());
    spiceGroup->setEnabled(config.displayType() == VMSpecs::DisplayType::Spice);
    setCombo(gpuCombo, config.gpuType());
    accel3dCheck->setChecked(config.gpu3dAcceleration());
    setCombo(audioCombo, config.audioDevice());
    m_sb16Config = config.sb16Config();
    setCombo(netCombo, config.networkCard());
    setCombo(networkModeCombo, config.networkMode());
    if (virtualNetworkCombo->count() > 0) {
        const int vnIdx = virtualNetworkCombo->findData(config.virtualNetworkName());
        if (vnIdx >= 0)
            virtualNetworkCombo->setCurrentIndex(vnIdx);
    }
    setCombo(diskInterfaceCombo, config.diskInterface());
    setCombo(usbCombo, config.usbController());
    setCombo(mouseCombo, config.pointerDevice());
    setCombo(keyboardCombo, config.keyboardDevice());

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

    adContainer->setVisible(false);
}

void CreateVmDialog::setExistingVmNames(const QStringList &names)
{
    existingVmNames = names;
    onNameChanged(nameEdit->text());
}

void CreateVmDialog::setMacUsageInfo(const QList<MacUsageInfo> &info)
{
    m_macUsageInfo = info;
}

void CreateVmDialog::setImageDatabase(ImageDatabase *db)
{
    m_imageDatabase = db;

    const int typeVal = osTypeCombo->currentData().toInt();
    if (typeVal >= 0 && static_cast<VMSpecs::OsType>(typeVal) != VMSpecs::OsType::Other)
        filterOsVariants(static_cast<VMSpecs::OsType>(typeVal));
}

void CreateVmDialog::setNetworkManager(VirtualNetworkManager *mgr)
{
    m_networkManager = mgr;
    virtualNetworkCombo->clear();
    if (!mgr)
        return;
    for (const auto &net : mgr->networks())
        virtualNetworkCombo->addItem(net.name, net.name);

    if (mgr->networks().isEmpty()) {
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
}

bool CreateVmDialog::isCloudDeployRequested() const
{
    if (!automatedDeployCheck || !automatedDeployCheck->isChecked())
        return false;
    const QString distroId = osNameCombo->currentData().toString();
    return m_imageDatabase && m_imageDatabase->hasCloudImages(distroId);
}

CloudImageInfo CreateVmDialog::selectedCloudImage() const
{
    const QString distroId = osNameCombo->currentData().toString();
    const QString version = versionCombo->currentData().toString();
    const auto images = m_imageDatabase->cloudImagesFor(distroId, version, SettingsDialog::showAllArchitectures());
    const int idx = cloudImageCombo->currentIndex();
    if (idx >= 0 && idx < images.size())
        return images[idx];
    return {};
}

VMConfig CreateVmDialog::selectedDefaults() const
{
    if (!m_imageDatabase)
        return {};
    return m_imageDatabase->defaultsFor(osNameCombo->currentData().toString());
}

CloudDeployConfig CreateVmDialog::cloudConfig() const
{
    return m_cloudConfig;
}

bool CreateVmDialog::isManualIsoDeployRequested() const
{
    if (!automatedDeployCheck || !automatedDeployCheck->isChecked())
        return false;
    return !isCloudDeployRequested();
}

VMConfig CreateVmDialog::getResult() const
{
    VMConfig c;
    c.setName(nameEdit->text().trimmed());
    c.setComment(commentEdit->toPlainText());
    c.setOsType(static_cast<VMSpecs::OsType>(osTypeCombo->currentData().toInt()));
    QString distroId = osNameCombo->currentData().toString();
    if (distroId.isEmpty())
        distroId = osNameCombo->currentText().trimmed();
    c.setDistroId(distroId);

    if (!customIcon.isNull()) {
        c.setIconSource(VMConfig::IconSource::Custom);
        c.setCustomIcon(customIcon);
    } else if (!selectedIconSetFilename.isEmpty()) {
        c.setIconSource(VMConfig::IconSource::IconSet);
        c.setIconIdentifier(selectedIconSetFilename);
    } else if (!selectedIconName.isEmpty()) {
        c.setIconSource(VMConfig::IconSource::SystemTheme);
        c.setIconIdentifier(selectedIconName);
    } else {
        c.setIconSource(VMConfig::IconSource::None);
        c.setIconIdentifier({});
    }

    c.setCpuModel(static_cast<VMSpecs::CpuModel>(cpuCombo->currentData().toInt()));

    // Architecture: no dedicated selector yet -- currently only x86 vs x86_64.
    //   - Cloud Deploy: architecture of the selected image
    //   - Edit Mode:     keep existing value (never silently overwrite)
    //   - New VM:        host architecture as fallback
    const QString imgArch = isCloudDeployRequested() ? selectedCloudImage().arch : QString();
    if (!imgArch.isEmpty())
        c.setArchitecture(VMConfig::parseArchitecture(imgArch));
    else if (isEditMode)
        c.setArchitecture(m_originalArchitecture);
    else {
        QString hint = VMConfig::x86ArchHint(c.cpuModel());
        if (hint.isEmpty())
            hint = ImageDatabase::hostArch();
        c.setArchitecture(VMConfig::parseArchitecture(hint));
    }

    c.setUseKvm(kvmCheck->isChecked());
    c.setRamMb(ramSpin->value());
    c.setUseBalloon(balloonCheck->isChecked());
    c.setUsePrealloc(preallocCheck->isChecked());
    c.setCpuCoreCount(cpuSpin->value());
    c.setDiskSizeGb(diskSpin->value());
    c.setISOPath(isoPathRequester->text());
    c.setBootFromIso(bootFromIsoCheck->isChecked());
    c.setFloppyPath(floppyPathRequester->text());
    c.setBootFromFloppy(bootFromFloppyCheck->isChecked());
    c.setExtraArgs(extraArgsEdit->text());
    c.setMachineType(static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt()));
    c.setUseUefi(uefiCheck->isChecked());
    c.setUseLocalTime(localTimeCheck->isChecked());
    c.setUseTpm(tpmCheck->isChecked());
    c.setUseSecureBoot(secureBootCheck->isChecked());
    c.setUseAcpi(acpiCheck->isChecked());
    c.setSharedPath(sharedPathEdit->text());
    c.setSharedName(sharedNameEdit->text());
    c.setDisplayType(static_cast<VMSpecs::DisplayType>(displayCombo->currentData().toInt()));
    c.setSpiceAgent(spiceAgentCheck->isChecked());
    c.setSpiceUsbRedir(spiceUsbRedirCheck->isChecked());
    c.setSpiceUsbRedirCount(spiceUsbRedirCountSpin->value());
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
    c.setDiskInterface(static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->currentData().toInt()));
    c.setUsbController(static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()));
    c.setPointerDevice(static_cast<VMSpecs::PointerDevice>(mouseCombo->currentData().toInt()));
    c.setKeyboardDevice(static_cast<VMSpecs::KeyboardDevice>(keyboardCombo->currentData().toInt()));

    return c;
}

void CreateVmDialog::applyReleaseDefaults(const VMConfig &defaults)
{
    ramSpin->setValue(defaults.ramMb());
    cpuSpin->setValue(defaults.cpuCoreCount());
    if (!isEditMode)
        diskSpin->setValue(defaults.diskSizeGb());

    setCombo(cpuCombo, defaults.cpuModel());
    setCombo(machineCombo, defaults.machineType());
    setCombo(gpuCombo, defaults.gpuType());
    setCombo(audioCombo, defaults.audioDevice());
    setCombo(diskInterfaceCombo, defaults.diskInterface());
    setCombo(netCombo, defaults.networkCard());
    setCombo(displayCombo, defaults.displayType());
    setCombo(usbCombo, defaults.usbController());

    kvmCheck->setChecked(defaults.useKvm());
    balloonCheck->setChecked(defaults.useMemoryBallooning());
    preallocCheck->setChecked(defaults.preallocMemory());
    accel3dCheck->setChecked(defaults.gpu3dAcceleration());
    gpuMemSpin->setValue(defaults.gpuMemMb());
    setCombo(mouseCombo, defaults.pointerDevice());
    setCombo(keyboardCombo, defaults.keyboardDevice());
    spiceAgentCheck->setChecked(defaults.spiceAgent());
    spiceUsbRedirCheck->setChecked(defaults.spiceUsbRedir());
    spiceUsbRedirCountSpin->setValue(defaults.spiceUsbRedirCount());
    acpiCheck->setChecked(defaults.useAcpi());

    uefiCheck->setChecked(defaults.useUefi());
    tpmCheck->setChecked(defaults.useTpm());
    secureBootCheck->setChecked(defaults.useSecureBoot());
    localTimeCheck->setChecked(defaults.useLocalTime());

    m_sb16Config = defaults.sb16Config();
    extraArgsEdit->setText(defaults.extraArgs());

    const QSize res = defaults.displayResolution();
    for (int i = 0; i < resolutionCombo->count(); ++i) {
        if (resolutionCombo->itemData(i).toSize() == res) {
            resolutionCombo->setCurrentIndex(i);
            break;
        }
    }
}

void CreateVmDialog::updateAutomatedDeployWidgets()
{
    const QString distroId = osNameCombo->currentData().toString();
    const bool hasCloud = m_imageDatabase && m_imageDatabase->hasCloudImages(distroId);
    const bool qdActive = automatedDeployCheck->isEnabled() && automatedDeployCheck->isChecked();

    versionCombo->setEnabled(qdActive);
    versionLabel->setEnabled(qdActive);
    cloudImageCombo->setEnabled(qdActive && hasCloud);
    cloudImageLabel->setEnabled(qdActive);
    cloudImageStatusLabel->setVisible(qdActive && hasCloud);

    // ISO path: visible only for distros without cloud images (e.g. Windows)
    adIsoPathRequester->setEnabled(qdActive && !hasCloud);
    adIsoPathRequester->setVisible(!hasCloud);
    if (adIsoLabel) {
        adIsoLabel->setEnabled(qdActive && !hasCloud);
        adIsoLabel->setVisible(!hasCloud);
    }

    cloudInitButton->setEnabled(qdActive);

    isoPathRequester->setEnabled(!qdActive);
    if (isoPathLabel)
        isoPathLabel->setEnabled(!qdActive);
    bootFromIsoCheck->setEnabled(!qdActive && !isoPathRequester->text().trimmed().isEmpty());

    floppyPathRequester->setEnabled(!qdActive);
    if (floppyPathLabel)
        floppyPathLabel->setEnabled(!qdActive);
    bootFromFloppyCheck->setEnabled(!qdActive && !floppyPathRequester->text().trimmed().isEmpty());
}

void CreateVmDialog::updateCloudImageStatus()
{
    cloudImageStatusLabel->clear();

    const QString distroId = osNameCombo->currentData().toString();
    const QString version = versionCombo->currentData().toString();
    if (distroId.isEmpty() || version.isEmpty() || !m_imageDatabase)
        return;

    const bool showAll = SettingsDialog::showAllArchitectures();
    const auto images = m_imageDatabase->cloudImagesFor(distroId, version, showAll);

    const int idx = cloudImageCombo->currentIndex();
    if (idx < 0 || idx >= images.size())
        return;

    const auto &img = images[idx];

    if (!ImageDatabase::isArchCompatible(img.arch)) {
        cloudImageStatusLabel->setText(i18n("! Foreign architecture -- emulation not yet supported"));
    } else if (ImageDatabase::isImageCached(img.url)) {
        cloudImageStatusLabel->setText(i18n("Cached"));
    } else {
        cloudImageStatusLabel->setText(i18n("Will be downloaded"));
    }

    validateForm();
}

void CreateVmDialog::updateDiskInterfaceHint()
{
    const auto disk = static_cast<VMSpecs::DiskInterface>(diskInterfaceCombo->currentData().toInt());
    const auto machine = static_cast<VMSpecs::MachineType>(machineCombo->currentData().toInt());
    const auto os = static_cast<VMSpecs::OsType>(osTypeCombo->currentData().toInt());
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
    if (disk == VMSpecs::DiskInterface::Virtio && os == VMSpecs::OsType::Windows) {
        diskHintLabel->setText(u"<i>"_s
                               + i18n("Note: Windows requires the VirtIO driver ISO "
                                      "during installation.")
                               + u"</i>"_s);
        return;
    }
    diskHintLabel->clear();
};

void CreateVmDialog::updateDeviceAvailability()
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

void CreateVmDialog::updateInputDeviceAvailability()
{
    const bool hasUsb = static_cast<VMSpecs::UsbController>(usbCombo->currentData().toInt()) != VMSpecs::UsbController::None;
    const QString tip = hasUsb ? QString() : i18n("Requires a USB controller.");

    // Enable/disable USB-dependent items via the combo's item model.
    const auto setUsbItemEnabled = [&](QComboBox *combo, int enumValue) {
        if (auto *model = qobject_cast<QStandardItemModel *>(combo->model())) {
            const int idx = combo->findData(enumValue);
            if (idx >= 0 && model->item(idx)) {
                model->item(idx)->setEnabled(hasUsb);
                combo->setItemData(idx, tip, Qt::ToolTipRole);
            }
        }
    };

    setUsbItemEnabled(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::UsbMouse));
    setUsbItemEnabled(mouseCombo, static_cast<int>(VMSpecs::PointerDevice::UsbTablet));
    setUsbItemEnabled(keyboardCombo, static_cast<int>(VMSpecs::KeyboardDevice::Usb));

    // Fall back to PS/2 if a USB device is selected but USB is unavailable.
    if (!hasUsb) {
        if (static_cast<VMSpecs::PointerDevice>(mouseCombo->currentData().toInt()) != VMSpecs::PointerDevice::Ps2)
            setCombo(mouseCombo, VMSpecs::PointerDevice::Ps2);
        if (static_cast<VMSpecs::KeyboardDevice>(keyboardCombo->currentData().toInt()) != VMSpecs::KeyboardDevice::Ps2)
            setCombo(keyboardCombo, VMSpecs::KeyboardDevice::Ps2);
    }
}

bool CreateVmDialog::isDisplayTypeAvailable(const VMSpecs::DisplayType type)
{
    switch (type) {
    case VMSpecs::DisplayType::Spice:
        return ExternalTools::instance().isAvailable(ExternalTools::spiceDisplayTools());
    case VMSpecs::DisplayType::Dbus:
        return ExternalTools::instance().isAvailable(ExternalTools::DbusDaemon);
    case VMSpecs::DisplayType::Gtk:
    case VMSpecs::DisplayType::Sdl:
        return true;
    default:
        return false;
    }
}

void CreateVmDialog::validateForm()
{
    QString warning;
    bool valid = true;

    const QString trimmed = nameEdit->text().trimmed();
    if (trimmed.isEmpty()) {
        warning = i18n("A name is required.");
        valid = false;
    } else if (trimmed != originalName && existingVmNames.contains(trimmed, Qt::CaseSensitive)) {
        warning = i18n("A virtual machine named '%1' already exists.", trimmed);
        valid = false;
    }

    if (isCloudDeployRequested()) {
        const auto img = selectedCloudImage();
        if (!ImageDatabase::isArchCompatible(img.arch)) {
            cloudImageStatusLabel->setText(i18n("Foreign architecture. Emulation support is not yet available."));
            valid = false;
        }
    }

    // OS Type Placeholder?
    if (osTypeCombo->currentData().toInt() == -1)
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

    // Cloud Deploy Validation
    if (valid && isCloudDeployRequested()) {
        const auto img = selectedCloudImage();
        if (!img.arch.isEmpty() && !ImageDatabase::isArchCompatible(img.arch))
            valid = false;
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

        for (const auto &info : m_macUsageInfo) {
            if (isEditMode && info.vmName == originalName)
                continue;

            if (info.mac.compare(mac, Qt::CaseInsensitive) != 0)
                continue;

            if (mode == VMSpecs::NetworkMode::VirtualNetwork && info.networkMode == VMSpecs::NetworkMode::VirtualNetwork
                && info.virtualNetworkName == netName) {
                macWarningText = i18n("MAC conflict with '%1' on network '%2'.", info.vmName, netName);
                valid = false;
            }
            break;
        }
    }

    // Update visibility and text of the label
    if (macWarningLabel && !macWarningText.isEmpty()) {
        macWarningLabel->setText(macWarningText);
    } else {
        macWarningLabel->clear();
    }

    if (buttonBox)
        buttonBox->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

void CreateVmDialog::resetToFactoryDefaults()
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
    if (!isEditMode)
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

    spiceAgentCheck->setChecked(GlobalDefaults.spiceAgent);
    spiceUsbRedirCheck->setChecked(GlobalDefaults.spiceUsbRedir);
    spiceUsbRedirCountSpin->setValue(GlobalDefaults.spiceUsbRedirCount);
    spiceGroup->setEnabled(GlobalDefaults.displayType == VMSpecs::DisplayType::Spice);

    uefiCheck->setChecked(GlobalDefaults.useUefi);
    tpmCheck->setChecked(GlobalDefaults.useTpm);
    secureBootCheck->setChecked(GlobalDefaults.useSecureBoot);
    acpiCheck->setChecked(GlobalDefaults.useAcpi);
    localTimeCheck->setChecked(GlobalDefaults.useLocalTime);

    m_sb16Config = Sb16Config();

    extraArgsEdit->clear();
}

QStringList CreateVmDialog::usedMacAddresses() const
{
    QStringList macs;
    for (const auto &info : m_macUsageInfo)
        macs << info.mac;
    return macs;
}
