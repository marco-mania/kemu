/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPushButton>
#include <QRadioButton>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>

#include <KLineEdit>
#include <KUrlRequester>

class QCheckBox;
class QToolButton;

#include "utils/cloudimagedeployer.h"
#include "utils/imagedatabase.h"
#include "utils/virtualnetworkmanager.h"
#include "utils/vmconfig.h"
#include "widgets/ramspinbox.h"
#include "widgets/steppedspinbox.h"
#include "widgets/vmiconselector.h"

using namespace Qt::StringLiterals;

/**
 * @brief Compact dialog for creating a virtual machine.
 *
 * Only asks for what has no sensible default -- operating system,
 * installation source and name -- plus the three resource values users
 * expect to adjust. Everything else comes from the OS template in
 * data/templates.d/.
 *
 * "Customise..." opens EditVmDialog as a modal child; this dialog stays open
 * behind it and keeps the result, so getResult() then builds on the customised
 * configuration instead of the template defaults.
 */
class NewVmDialog : public QDialog
{
    Q_OBJECT

public:
    explicit NewVmDialog(QWidget *parent = nullptr);

    void setExistingVmNames(const QStringList &names);
    void setImageDatabase(ImageDatabase *db);
    void setNetworkManager(VirtualNetworkManager *mgr);
    void setMacUsageInfo(const QList<MacUsageInfo> &info);
    void setVmUuids(const QMap<QString, QUuid> &vmUuids);

    enum class Source {
        CloudImage,
        InstallerIso,
        BlankDisk
    };

    /// Template defaults with name, memory, CPUs and disk size applied.
    [[nodiscard]] VMConfig getResult() const;

    [[nodiscard]] Source source() const;

    /// Only meaningful for Source::CloudImage.
    [[nodiscard]] CloudImageInfo selectedCloudImage() const;

    /// Deployment defaults from the settings, with hostname and template ids filled in.
    [[nodiscard]] CloudDeployConfig cloudConfig() const;

    /// Only meaningful for Source::InstallerIso.
    [[nodiscard]] QString installerIsoPath() const;

    /// Password for encrypted disks; empty if they stay unencrypted.
    [[nodiscard]] QString diskPassword() const;
    [[nodiscard]] bool storeDiskPasswordInWallet() const;

private Q_SLOTS:
    void openCustomiseDialog();
    void onOsChanged();
    void onSourceChanged();
    void onNameEdited();
    void openCloudConfigDialog();
    void openDiskPasswordDialog();
    void validateForm();

private:
    void buildUi();
    void populateOsCombo();
    void detectOsFromName();
    void applyTemplateDefaults();
    void updateCloudImages();
    void updateIcon();
    void updateDeployInfo();
    void clearCustomisation();

    [[nodiscard]] QString selectedTemplateId() const;
    [[nodiscard]] QString selectedVersion() const;
    [[nodiscard]] const TemplateInfo *selectedTemplate() const;
    [[nodiscard]] bool hasCloudDeploy() const;
    [[nodiscard]] QStringList usedMacAddresses() const;

    void syncSlider(QSlider *slider, const QList<int> &steps, int value);

    VmIconSelector *iconSelector = nullptr;

    QLineEdit *nameEdit = nullptr;
    QLabel *warningLabel = nullptr;
    QComboBox *osCombo = nullptr;

    QRadioButton *cloudRadio = nullptr;
    QRadioButton *isoRadio = nullptr;
    QRadioButton *blankRadio = nullptr;
    KUrlRequester *isoRequester = nullptr;

    QWidget *cloudOptions = nullptr;
    QPushButton *cloudInitButton = nullptr;
    QComboBox *cloudImageCombo = nullptr;
    QLabel *cloudImageLabel = nullptr;

    QSlider *ramSlider = nullptr;
    RamSpinBox *ramSpin = nullptr;
    QSlider *cpuSlider = nullptr;
    QSpinBox *cpuSpin = nullptr;
    QSlider *diskSlider = nullptr;
    SteppedSpinBox *diskSpin = nullptr;

    QCheckBox *encryptCheck = nullptr;
    QToolButton *passwordButton = nullptr;
    QLabel *passwordStatusLabel = nullptr;
    QString m_diskPassword;
    bool m_storeDiskPassword = true;

    QLabel *deployInfoLabel = nullptr;
    QDialogButtonBox *buttonBox = nullptr;
    QPushButton *customiseButton = nullptr;

    ImageDatabase *m_imageDatabase = nullptr;
    VirtualNetworkManager *m_networkManager = nullptr;
    QStringList m_existingVmNames;
    QList<MacUsageInfo> m_macUsageInfo;
    QMap<QString, QUuid> m_vmUuids;

    // Set once the user has been through EditVmDialog. getResult() then builds
    // on this instead of the template defaults.
    VMConfig m_customConfig;
    bool m_hasCustomConfig = false;

    // Read once at construction; edited through the cloud-init dialog.
    CloudDeployConfig m_cloudConfig;

    // Stays false once the user has typed a host name of their own.
    bool m_hostnameIsDerived = true;

    // Set while detection changes the OS, so onOsChanged() can tell an
    // implicit switch from a deliberate one.
    bool m_detectingOs = false;

    QList<CloudImageInfo> m_cloudImages;

    QList<int> m_ramSteps;
    QList<int> m_diskSteps;
};
