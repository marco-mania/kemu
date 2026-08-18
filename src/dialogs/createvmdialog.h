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

class CreateVmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CreateVmDialog(QWidget *parent = nullptr);

    VMConfig getResult() const;

    void setupForEdit(const VMConfig &config);

    void setExistingVmNames(const QStringList &names);
    void setMacUsageInfo(const QList<MacUsageInfo> &info);

    void setImageDatabase(ImageDatabase *db);
    void setNetworkManager(VirtualNetworkManager *mgr);

    [[nodiscard]] bool isCloudDeployRequested() const;
    [[nodiscard]] CloudImageInfo selectedCloudImage() const;
    [[nodiscard]] VMConfig selectedDefaults() const;
    [[nodiscard]] CloudDeployConfig cloudConfig() const;
    [[nodiscard]] bool isManualIsoDeployRequested() const;

    void applyGlobalDefaults();

private Q_SLOTS:
    void browseSharedDir();
    void chooseFromIconSet();
    void chooseThemeIcon();
    void browseCustomIcon();
    void clearIcon();
    void showIconContextMenu(const QPoint &pos);

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

    void onNameChanged(const QString &text);

    void addPortForwardRow();
    void removePortForwardRow();

private:
    void updateIconDisplay();
    void filterOsVariants(VMSpecs::OsType osType); // e.g. Distributions for Linux
    void autoSetIconFromOS();

    void populateVersions();
    void populateEditions();
    void applyReleaseDefaults(const VMConfig &defaults);

    void updateAutomatedDeployWidgets();
    void updateCloudImageStatus();
    void updateDiskInterfaceHint();
    void updateDeviceAvailability();
    void updateInputDeviceAvailability();

    void validateForm();

    QStringList existingVmNames;
    QString originalName;

    QString originalMac;

    VMSpecs::Architecture m_originalArchitecture = VMSpecs::Architecture::X86_64;

    QLineEdit *nameEdit = nullptr;
    QPlainTextEdit *commentEdit = nullptr;
    QComboBox *osTypeCombo = nullptr;
    QComboBox *osNameCombo = nullptr;
    QLabel *osNameLabel = nullptr;
    QPushButton *applyDefaultsButton = nullptr;

    QComboBox *versionCombo = nullptr;
    QLabel *versionLabel = nullptr;
    QCheckBox *automatedDeployCheck = nullptr;
    QComboBox *cloudImageCombo = nullptr;
    QLabel *cloudImageLabel = nullptr;
    QLabel *cloudImageStatusLabel = nullptr;

    // Automated Deploy Section
    QWidget *adContainer = nullptr;
    KUrlRequester *adIsoPathRequester = nullptr;
    QLabel *adIsoLabel = nullptr;
    QPushButton *cloudInitButton = nullptr;
    QLabel *cloudInitLabel = nullptr;

    CloudDeployConfig m_cloudConfig;

    QPushButton *iconDisplayButton = nullptr;
    QString selectedIconName;
    QString selectedIconSetFilename;
    QIcon customIcon;

    QComboBox *cpuCombo = nullptr;
    QCheckBox *kvmCheck = nullptr;
    QSlider *ramSlider = nullptr;
    RamSpinBox *ramSpin = nullptr;
    QCheckBox *balloonCheck = nullptr;
    QCheckBox *preallocCheck = nullptr;
    QSpinBox *cpuSpin = nullptr;
    QSpinBox *diskSpin = nullptr;
    QLabel *diskLabel = nullptr;
    KUrlRequester *isoPathRequester = nullptr;
    QLabel *isoPathLabel = nullptr;
    QCheckBox *bootFromIsoCheck = nullptr;
    KUrlRequester *floppyPathRequester = nullptr;
    QLabel *floppyPathLabel = nullptr;
    QCheckBox *bootFromFloppyCheck = nullptr;
    QLineEdit *extraArgsEdit = nullptr;
    QComboBox *displayCombo = nullptr;
    QCheckBox *spiceAgentCheck = nullptr;
    QCheckBox *spiceUsbRedirCheck = nullptr;
    QSpinBox *spiceUsbRedirCountSpin = nullptr;
    QLabel *spiceUsbRedirCountLabel = nullptr;
    QGroupBox *spiceGroup = nullptr;
    QComboBox *gpuCombo = nullptr;
    QSpinBox *gpuMemSpin = nullptr;
    QCheckBox *accel3dCheck = nullptr;
    QComboBox *resolutionCombo = nullptr;
    QLabel *resolutionLabel = nullptr;
    QLabel *vgaMemLabel = nullptr;
    QLabel *gpuHintLabel = nullptr;
    QComboBox *audioCombo = nullptr;
    QPushButton *sb16ConfigButton = nullptr;
    QComboBox *diskInterfaceCombo = nullptr;
    QLabel *diskHintLabel = nullptr;
    QLabel *usbControllerLabel = nullptr;
    QComboBox *usbCombo = nullptr;
    QComboBox *mouseCombo = nullptr;
    QComboBox *keyboardCombo = nullptr;
    QComboBox *machineCombo = nullptr;
    QCheckBox *uefiCheck = nullptr;
    QCheckBox *localTimeCheck = nullptr;
    QCheckBox *tpmCheck = nullptr;
    QCheckBox *secureBootCheck = nullptr;
    QCheckBox *acpiCheck = nullptr;
    QLineEdit *sharedPathEdit = nullptr;
    QLineEdit *sharedNameEdit = nullptr;
    QComboBox *netCombo = nullptr;
    QLineEdit *macEdit = nullptr;
    QPushButton *regenerateMacButton = nullptr;
    QTableWidget *portForwardTable = nullptr;
    QPushButton *addPortFwdButton = nullptr;
    QPushButton *removePortFwdButton = nullptr;
    QComboBox *networkModeCombo = nullptr;
    QComboBox *virtualNetworkCombo = nullptr;
    QWidget *passtGroup = nullptr;
    QWidget *vdeGroup = nullptr;

    QLabel *nameWarningLabel = nullptr;
    QLabel *macWarningLabel = nullptr;

    QDialogButtonBox *buttonBox = nullptr;

    ImageDatabase *m_imageDatabase = nullptr;
    VirtualNetworkManager *m_networkManager = nullptr;

    QList<MacUsageInfo> m_macUsageInfo;
    QStringList usedMacAddresses() const;

    bool isEditMode = false;

    bool iconManuallySet = false;

    void resetToFactoryDefaults();

    Sb16Config m_sb16Config;
};
