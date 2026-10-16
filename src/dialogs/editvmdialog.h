/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

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
#include <QMetaEnum>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KLineEdit>
#include <KLocalizedString>
#include <KUrlRequester>

#include "utils/imagedatabase.h"
#include "utils/virtualnetworkmanager.h"
#include "utils/vmconfig.h"
#include "widgets/ramspinbox.h"
#include "widgets/vmiconselector.h"

using namespace Qt::StringLiterals;

class EditVmDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode {
        PreCreate, // VM does not exist yet: everything is still negotiable
        Edit // VM exists on disk: some choices are already baked in
    };

    explicit EditVmDialog(Mode mode, QWidget *parent = nullptr);

    VMConfig getResult() const;

    void setConfig(const VMConfig &config);

    // Show the configuration without allowing changes. Call after setConfig().
    void setReadOnly(bool readOnly);
    [[nodiscard]] bool isReadOnly() const
    {
        return m_readOnly;
    }

    void setExistingVmNames(const QStringList &names);
    void setMacUsageInfo(const QList<MacUsageInfo> &info);

    void setImageDatabase(ImageDatabase *db);
    void setNetworkManager(VirtualNetworkManager *mgr);

    [[nodiscard]] bool iconManuallySet() const;

private Q_SLOTS:
    void browseSharedDir();
    void onMachineTypeChanged(int index);
    void onRamSpinChanged(int mb);
    void onOsTypeChanged(int index);
    void onOsNameChanged(int index);
    void onGpuDisplayChanged();

    void onNameChanged(const QString &text);

    void addPortForwardRow();
    void removePortForwardRow();

private:
    void filterOsVariants(VMSpecs::OsType osType); // e.g. Distributions for Linux
    void updateIconForOs();
    void updateDiskInterfaceHint();
    void updateDeviceAvailability();
    void updateInputDeviceAvailability();
    void updateNetworkCardDependencies();

    void validateForm();

    QStringList existingVmNames;

    VmIconSelector *iconSelector = nullptr;

    QLineEdit *nameEdit = nullptr;
    QPlainTextEdit *commentEdit = nullptr;
    QComboBox *osTypeCombo = nullptr;
    QComboBox *osNameCombo = nullptr;
    QLabel *osNameLabel = nullptr;

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
    QComboBox *exitActionCombo = nullptr;
    QComboBox *guestShutdownActionCombo = nullptr;
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
    QLineEdit *hostnameEdit = nullptr;
    QWidget *userModeGroup = nullptr;
    QWidget *vdeGroup = nullptr;

    QLabel *nameWarningLabel = nullptr;
    QLabel *macWarningLabel = nullptr;
    QLabel *hostnameWarningLabel = nullptr;

    QDialogButtonBox *buttonBox = nullptr;

    ImageDatabase *m_imageDatabase = nullptr;
    VirtualNetworkManager *m_networkManager = nullptr;

    QList<MacUsageInfo> m_macUsageInfo;
    QStringList usedMacAddresses() const;

    Mode m_mode = Mode::Edit;
    bool m_readOnly = false;
    VMConfig m_original;
    QString m_lastAutoHostname;

    void applyModeConstraints();

    void resetToFactoryDefaults();

    Sb16Config m_sb16Config;
};
