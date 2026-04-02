/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMetaEnum>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KIconDialog>
#include <KLineEdit>
#include <KLocalizedString>
#include <KUrlRequester>

#include "utils/cloudimagedeployer.h"
#include "utils/imagedatabase.h"
#include "utils/virtualnetworkmanager.h"
#include "utils/vmconfig.h"
#include "widgets/ramspinbox.h"

using namespace Qt::StringLiterals;

class CreateVmDialog : public QDialog {

    Q_OBJECT

public:
    explicit CreateVmDialog(QWidget* parent = nullptr);

    VMConfig getResult() const;

    void setupForEdit(const VMConfig& config);

    void setExistingVmNames(const QStringList& names);
    void setMacUsageInfo(const QList<MacUsageInfo>& info);

    void setImageDatabase(ImageDatabase* db);
    void setNetworkManager(VirtualNetworkManager* mgr);

    [[nodiscard]] bool isCloudDeployRequested() const;
    [[nodiscard]] CloudImageInfo selectedCloudImage() const;
    [[nodiscard]] VMConfig selectedDefaults() const;
    [[nodiscard]] CloudDeployConfig cloudConfig() const;
    [[nodiscard]] bool isManualIsoDeployRequested() const;

private Q_SLOTS:
    void browseSharedDir();
    void chooseFromIconSet();
    void chooseThemeIcon();
    void browseCustomIcon();
    void clearIcon();
    void showIconContextMenu(const QPoint& pos);

    void openCloudConfigDialog();
    void browseQdIso();

    void onMachineTypeChanged(int index);
    void onRamSliderChanged(int gb);
    void onRamSpinChanged(int mb);
    void onOsTypeChanged(int index);
    void onOsNameChanged(int index);
    void onGpuDisplayChanged();

    void onVersionChanged(int index);
    void onAutomatedDeployToggled(bool checked);

    void onNameChanged(const QString& text);

    void addPortForwardRow();
    void removePortForwardRow();

private:
    void updateIconDisplay();
    void filterOsVariants(VMSpecs::OsType osType); // e.g. Distributions for Linux
    void autoSetIconFromOS();

    void populateVersions();
    void populateEditions();
    void applyReleaseDefaults(const VMConfig& defaults);

    void updateAutomatedDeployWidgets();
    void updateCloudImageStatus();
    void updateDiskInterfaceHint();
    void updateDeviceAvailability();

    void validateForm();

    QStringList existingVmNames;
    QString originalName;

    QString originalMac;

    QPointer<QLineEdit> nameEdit;
    QPointer<QPlainTextEdit> commentEdit;
    QPointer<QComboBox> osTypeCombo;
    QPointer<QComboBox> osNameCombo;
    QPointer<QLabel> osNameLabel;
    QPointer<QPushButton> applyDefaultsButton;

    QPointer<QComboBox> versionCombo;
    QPointer<QLabel> versionLabel;
    QPointer<QCheckBox> automatedDeployCheck;
    QPointer<QComboBox> cloudImageCombo;
    QPointer<QLabel> cloudImageLabel;
    QPointer<QLabel> cloudImageStatusLabel;

    // Automated Deploy Section
    QPointer<QWidget> adContainer;
    QPointer<KUrlRequester> adIsoPathRequester;
    QPointer<QLabel> adIsoLabel;
    QPointer<QPushButton> cloudInitButton;
    QPointer<QLabel> cloudInitLabel;

    CloudDeployConfig m_cloudConfig;

    QPointer<QPushButton> iconDisplayButton;
    QString selectedIconName;
    QString selectedIconSetFilename;
    QIcon customIcon;

    QPointer<QComboBox> cpuCombo;
    QPointer<QCheckBox> kvmCheck;
    QPointer<QSlider> ramSlider;
    QPointer<RamSpinBox> ramSpin;
    QPointer<QCheckBox> balloonCheck;
    QPointer<QCheckBox> preallocCheck;
    QPointer<QSpinBox> cpuSpin;
    QPointer<QSpinBox> diskSpin;
    QPointer<QLabel> diskLabel;
    QPointer<KUrlRequester> isoPathRequester;
    QPointer<QLabel> isoPathLabel;
    QPointer<QCheckBox> bootFromIsoCheck;
    QPointer<QLineEdit> extraArgsEdit;
    QPointer<QComboBox> displayCombo;
    QPointer<QCheckBox> spiceAgentCheck;
    QPointer<QCheckBox> spiceUsbRedirCheck;
    QPointer<QSpinBox> spiceUsbRedirCountSpin;
    QPointer<QLabel> spiceUsbRedirCountLabel;
    QPointer<QGroupBox> spiceGroup;
    QPointer<QComboBox> gpuCombo;
    QPointer<QSpinBox> gpuMemSpin;
    QPointer<QCheckBox> accel3dCheck;
    QPointer<QComboBox> resolutionCombo;
    QPointer<QLabel> resolutionLabel;
    QPointer<QLabel> vgaMemLabel;
    QPointer<QLabel> gpuHintLabel;
    QPointer<QComboBox> audioCombo;
    QPointer<QPushButton> sb16ConfigButton;
    QPointer<QComboBox> diskInterfaceCombo;
    QPointer<QLabel> diskHintLabel;
    QPointer<QLabel> usbControllerLabel;
    QPointer<QComboBox> usbCombo;
    QPointer<QCheckBox> usbTabletCheck;
    QPointer<QCheckBox> usbKeyboardCheck;
    QPointer<QComboBox> machineCombo;
    QPointer<QCheckBox> uefiCheck;
    QPointer<QCheckBox> localTimeCheck;
    QPointer<QCheckBox> tpmCheck;
    QPointer<QCheckBox> secureBootCheck;
    QPointer<QCheckBox> acpiCheck;
    QPointer<QLineEdit> sharedPathEdit;
    QPointer<QLineEdit> sharedNameEdit;
    QPointer<QComboBox> netCombo;
    QPointer<QLineEdit> macEdit;
    QPointer<QPushButton> regenerateMacButton;
    QPointer<QTableWidget> portForwardTable;
    QPointer<QPushButton> addPortFwdButton;
    QPointer<QPushButton> removePortFwdButton;
    QPointer<QComboBox> networkModeCombo;
    QPointer<QComboBox> virtualNetworkCombo;
    QPointer<QWidget> passtGroup;
    QPointer<QWidget> vdeGroup;

    QPointer<QLabel> nameWarningLabel;
    QPointer<QLabel> macWarningLabel;

    QPointer<QDialogButtonBox> buttonBox;

    ImageDatabase* m_imageDatabase = nullptr;
    VirtualNetworkManager* m_networkManager = nullptr;

    QList<MacUsageInfo> m_macUsageInfo;
    QStringList usedMacAddresses() const;

    bool isEditMode = false;

    bool iconManuallySet = false;

    void resetToFactoryDefaults();

    Sb16Config m_sb16Config;
};
