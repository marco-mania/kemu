/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dialogs/newvmdialog.h"

#include <QCompleter>
#include <QDir>
#include <QFileInfo>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KLocalizedString>

#include "const.h"
#include "dialogs/cloudconfigdialog.h"
#include "dialogs/editvmdialog.h"
#include "dialogs/settingsdialog.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

namespace
{

// Index of the step closest to `value`, clamped to the slider's own range.
int nearestStepIndex(const QList<int> &steps, int value, int minIndex = 0)
{
    int best = minIndex;
    int bestDiff = std::numeric_limits<int>::max();

    for (int i = minIndex; i < steps.size(); ++i) {
        const int diff = std::abs(steps.at(i) - value);
        if (diff < bestDiff) {
            bestDiff = diff;
            best = i;
        }
    }

    return best;
}

// First index whose step is >= minValue, so the slider cannot offer a value
// the spin box would clamp away.
int firstStepIndexAtLeast(const QList<int> &steps, int minValue)
{
    for (int i = 0; i < steps.size(); ++i) {
        if (steps.at(i) >= minValue)
            return i;
    }
    return steps.size() - 1;
}

QLabel *sectionLabel(const QString &text)
{
    auto *label = new QLabel(text);
    QFont font = label->font();
    font.setBold(true);
    label->setFont(font);
    return label;
}

} // namespace

NewVmDialog::NewVmDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18n("New Virtual Machine"));

    m_ramSteps = RamSpinBox::steps();
    m_diskSteps = {1, 2, 4, 5, 8, 10, 12, 15, 16, 20, 25, 30, 40, 50, 60, 80, 100, 128, 160, 200, 256, 320, 400, 500, 640, 800, 1000, 1500, 2000};

    m_cloudConfig = SettingsDialog::deploymentDefaults();

    buildUi();

    resize(520, sizeHint().height());
}

// --- Construction ---

void NewVmDialog::buildUi()
{
    auto *mainLayout = new QVBoxLayout(this);

    // --- Name and icon ---

    auto *topLayout = new QHBoxLayout();

    auto *nameLayout = new QFormLayout();
    nameLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    nameEdit = new QLineEdit();
    nameEdit->setPlaceholderText(i18n("e.g. ubuntu-dev"));
    nameEdit->setClearButtonEnabled(true);
    nameLayout->addRow(i18n("Name:"), nameEdit);

    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    warningLabel = new QLabel();
    warningLabel->setStyleSheet(u"color: %1; font-size: 11px;"_s.arg(scheme.foreground(KColorScheme::NegativeText).color().name()));
    warningLabel->setWordWrap(true);
    warningLabel->setFixedHeight(warningLabel->fontMetrics().height() + 2);
    nameLayout->addRow({}, warningLabel);

    topLayout->addLayout(nameLayout, 1);
    topLayout->addSpacing(12);

    iconSelector = new VmIconSelector();
    iconSelector->setSelectorSize(64);
    topLayout->addWidget(iconSelector, 0, Qt::AlignTop);

    mainLayout->addLayout(topLayout);

    // --- Operating system ---

    mainLayout->addSpacing(8);
    mainLayout->addWidget(sectionLabel(i18n("Operating System")));

    osCombo = new QComboBox();
    osCombo->setEditable(true);
    osCombo->setInsertPolicy(QComboBox::NoInsert);
    osCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    osCombo->completer()->setFilterMode(Qt::MatchContains);
    osCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);
    osCombo->completer()->setCompletionMode(QCompleter::PopupCompletion);
    mainLayout->addWidget(osCombo);

    auto *sourceLayout = new QVBoxLayout();
    sourceLayout->setContentsMargins(0, 4, 0, 0);

    cloudRadio = new QRadioButton(i18n("Automated install (cloud image)"));
    cloudRadio->setToolTip(i18n("Downloads the official cloud image and configures it through cloud-init."));
    sourceLayout->addWidget(cloudRadio);

    // Everything below the cloud image option, indented to belong to it.
    cloudOptions = new QWidget();
    auto *cloudOptionsLayout = new QVBoxLayout(cloudOptions);
    cloudOptionsLayout->setContentsMargins(24, 2, 0, 4);
    cloudOptionsLayout->setSpacing(4);

    auto *cloudImageLayout = new QHBoxLayout();
    cloudImageLabel = new QLabel(i18n("Architecture:"));
    cloudImageCombo = new QComboBox();
    cloudImageLayout->addWidget(cloudImageLabel);
    cloudImageLayout->addWidget(cloudImageCombo, 1);
    cloudOptionsLayout->addLayout(cloudImageLayout);

    auto *deployLayout = new QHBoxLayout();
    deployInfoLabel = new QLabel();
    deployInfoLabel->setWordWrap(true);
    cloudInitButton = new QPushButton(QIcon::fromTheme(u"preferences-system"_s), i18n("Configure..."));
    cloudInitButton->setToolTip(i18n("User, password, SSH keys, locale and packages applied on first boot."));
    deployLayout->addWidget(deployInfoLabel, 1);
    deployLayout->addWidget(cloudInitButton, 0, Qt::AlignTop);
    cloudOptionsLayout->addLayout(deployLayout);

    sourceLayout->addWidget(cloudOptions);

    isoRadio = new QRadioButton(i18n("Install from ISO"));
    sourceLayout->addWidget(isoRadio);

    auto *isoLayout = new QHBoxLayout();
    isoLayout->addSpacing(24);
    isoRequester = new KUrlRequester();
    isoRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
    isoRequester->setNameFilter(i18n("ISO Images (*.iso)"));
    isoRequester->lineEdit()->setClearButtonEnabled(true);
    isoLayout->addWidget(isoRequester, 1);
    sourceLayout->addLayout(isoLayout);

    blankRadio = new QRadioButton(i18n("Empty disk (no installation media)"));
    blankRadio->setToolTip(i18n("Creates the disk image only. Attach media later from the main window."));
    sourceLayout->addWidget(blankRadio);

    mainLayout->addLayout(sourceLayout);

    // --- Resources ---

    mainLayout->addSpacing(8);
    mainLayout->addWidget(sectionLabel(i18n("Resources")));

    auto *resourceLayout = new QFormLayout();
    resourceLayout->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    ramSlider = new QSlider(Qt::Horizontal);
    ramSlider->setRange(0, m_ramSteps.size() - 1);
    ramSpin = new RamSpinBox();
    auto *ramLayout = new QHBoxLayout();
    ramLayout->addWidget(ramSlider, 1);
    ramLayout->addWidget(ramSpin);
    resourceLayout->addRow(i18n("Memory:"), ramLayout);

    cpuSlider = new QSlider(Qt::Horizontal);
    cpuSlider->setRange(1, 64);
    cpuSpin = new QSpinBox();
    cpuSpin->setRange(1, 64);
    cpuSpin->setSuffix(i18n(" Cores"));
    auto *cpuLayout = new QHBoxLayout();
    cpuLayout->addWidget(cpuSlider, 1);
    cpuLayout->addWidget(cpuSpin);
    resourceLayout->addRow(i18n("Processors:"), cpuLayout);

    diskSlider = new QSlider(Qt::Horizontal);
    diskSlider->setRange(0, m_diskSteps.size() - 1);
    diskSpin = new SteppedSpinBox();
    diskSpin->setSteps(m_diskSteps);
    diskSpin->setRange(m_diskSteps.first(), m_diskSteps.last());
    diskSpin->setSuffix(i18n(" GB"));
    auto *diskLayout = new QHBoxLayout();
    diskLayout->addWidget(diskSlider, 1);
    diskLayout->addWidget(diskSpin);
    resourceLayout->addRow(i18n("Disk:"), diskLayout);

    mainLayout->addLayout(resourceLayout);

    // --- Buttons ---

    buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    buttonBox->button(QDialogButtonBox::Ok)->setText(i18n("Create"));
    buttonBox->button(QDialogButtonBox::Ok)->setIcon(QIcon::fromTheme(u"list-add"_s));

    customiseButton = buttonBox->addButton(i18n("Customise..."), QDialogButtonBox::ActionRole);
    customiseButton->setIcon(QIcon::fromTheme(u"configure"_s));
    customiseButton->setToolTip(i18n("Review and change the full hardware configuration before the machine is created."));

    mainLayout->addWidget(buttonBox);

    // --- Connections ---

    connect(nameEdit, &QLineEdit::textChanged, this, &NewVmDialog::onNameEdited);
    connect(osCombo, &QComboBox::currentIndexChanged, this, &NewVmDialog::onOsChanged);

    connect(cloudRadio, &QRadioButton::toggled, this, &NewVmDialog::onSourceChanged);
    connect(isoRadio, &QRadioButton::toggled, this, &NewVmDialog::onSourceChanged);
    connect(blankRadio, &QRadioButton::toggled, this, &NewVmDialog::onSourceChanged);
    connect(isoRequester, &KUrlRequester::textChanged, this, &NewVmDialog::validateForm);

    connect(ramSlider, &QSlider::valueChanged, this, [this](int index) {
        if (index < 0 || index >= m_ramSteps.size())
            return;
        const QSignalBlocker blocker(ramSpin);
        ramSpin->setValue(m_ramSteps.at(index));
    });
    connect(ramSpin, &RamSpinBox::valueChanged, this, [this](int mb) {
        syncSlider(ramSlider, m_ramSteps, mb);
    });
    connect(ramSpin, &QAbstractSpinBox::editingFinished, ramSpin, &SteppedSpinBox::snapToStep);

    connect(cpuSlider, &QSlider::valueChanged, this, [this](int cores) {
        const QSignalBlocker blocker(cpuSpin);
        cpuSpin->setValue(cores);
    });
    connect(cpuSpin, &QSpinBox::valueChanged, this, [this](int cores) {
        const QSignalBlocker blocker(cpuSlider);
        cpuSlider->setValue(cores);
    });

    connect(diskSlider, &QSlider::valueChanged, this, [this](int index) {
        if (index < 0 || index >= m_diskSteps.size())
            return;
        const QSignalBlocker blocker(diskSpin);
        diskSpin->setValue(m_diskSteps.at(index));
    });
    connect(diskSpin, &QSpinBox::valueChanged, this, [this](int gb) {
        syncSlider(diskSlider, m_diskSteps, gb);
    });
    connect(diskSpin, &QAbstractSpinBox::editingFinished, diskSpin, &SteppedSpinBox::snapToStep);

    connect(cloudInitButton, &QPushButton::clicked, this, &NewVmDialog::openCloudConfigDialog);

    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(customiseButton, &QPushButton::clicked, this, &NewVmDialog::openCustomiseDialog);

    cloudRadio->setChecked(true);
    updateDeployInfo();
    validateForm();
}

// --- Setters ---

void NewVmDialog::setExistingVmNames(const QStringList &names)
{
    m_existingVmNames = names;
    validateForm();
}

void NewVmDialog::setImageDatabase(ImageDatabase *db)
{
    m_imageDatabase = db;
    populateOsCombo();
}

void NewVmDialog::setNetworkManager(VirtualNetworkManager *mgr)
{
    m_networkManager = mgr;
}

void NewVmDialog::setMacUsageInfo(const QList<MacUsageInfo> &info)
{
    m_macUsageInfo = info;
}

// --- Operating system list ---

void NewVmDialog::populateOsCombo()
{
    const QSignalBlocker blocker(osCombo);
    osCombo->clear();
    m_cloudImages.clear();

    if (!m_imageDatabase)
        return;

    auto *model = qobject_cast<QStandardItemModel *>(osCombo->model());

    for (const auto type : {VMSpecs::OsType::Linux, VMSpecs::OsType::Windows, VMSpecs::OsType::BSD, VMSpecs::OsType::DOS, VMSpecs::OsType::Other}) {
        const QList<TemplateInfo> templates = m_imageDatabase->templatesForType(type);
        if (templates.isEmpty())
            continue;

        osCombo->addItem(VMConfig::toUIString(type));
        if (model) {
            // Group header: visible, but not selectable.
            model->item(osCombo->count() - 1)->setEnabled(false);
        }

        for (const auto &tpl : templates) {
            for (const auto &release : tpl.releases) {
                const QString version = release.displayString();
                const QString label = version.isEmpty() ? tpl.displayName : u"%1 %2"_s.arg(tpl.displayName, version);
                osCombo->addItem(u"    "_s + label, QStringList{tpl.id, release.version});
            }
        }
    }

    // Select the first real entry, skipping the leading group header.
    for (int i = 0; i < osCombo->count(); ++i) {
        if (!osCombo->itemData(i).isNull()) {
            osCombo->setCurrentIndex(i);
            break;
        }
    }

    onOsChanged();
}

QString NewVmDialog::selectedTemplateId() const
{
    const QStringList data = osCombo->currentData().toStringList();
    return data.value(0);
}

QString NewVmDialog::selectedVersion() const
{
    const QStringList data = osCombo->currentData().toStringList();
    return data.value(1);
}

const TemplateInfo *NewVmDialog::selectedTemplate() const
{
    if (!m_imageDatabase)
        return nullptr;
    const QString id = selectedTemplateId();
    return id.isEmpty() ? nullptr : m_imageDatabase->findById(id);
}

bool NewVmDialog::hasCloudDeploy() const
{
    if (!m_imageDatabase)
        return false;

    const QString tplId = selectedTemplateId();
    const QString version = selectedVersion();
    if (tplId.isEmpty() || version.isEmpty())
        return false;

    // Both conditions matter: kdeneon ships installer ISOs only, and a release
    // can be marked as not quick-deployable even when images exist.
    return m_imageDatabase->isQuickDeployEnabled(tplId, version)
        && !m_imageDatabase->cloudImagesFor(tplId, version, SettingsDialog::showAllArchitectures()).isEmpty();
}

// --- Slots ---

void NewVmDialog::openCustomiseDialog()
{
    // Modal child: this dialog stays open behind it, exactly like the settings
    // dialog behind the "Change in Settings" link.
    EditVmDialog editor(EditVmDialog::Mode::PreCreate, this);
    editor.setExistingVmNames(m_existingVmNames);
    editor.setImageDatabase(m_imageDatabase);
    editor.setNetworkManager(m_networkManager);
    editor.setMacUsageInfo(m_macUsageInfo);

    editor.setConfig(getResult());

    if (editor.exec() != QDialog::Accepted)
        return;

    m_customConfig = editor.getResult();
    m_hasCustomConfig = true;

    // Reflect what the editor changed in the fields this dialog owns --
    // otherwise getResult() would overwrite them from the widgets again.
    {
        const QSignalBlocker blockName(nameEdit);
        const QSignalBlocker blockRam(ramSpin);
        const QSignalBlocker blockCpu(cpuSpin);
        const QSignalBlocker blockDisk(diskSpin);

        cpuSpin->setMaximum(VMConfig::maxCoresForModel(m_customConfig.cpuModel()));
        cpuSlider->setMaximum(cpuSpin->maximum());

        nameEdit->setText(m_customConfig.name());
        ramSpin->setValue(m_customConfig.ramMb());
        cpuSpin->setValue(m_customConfig.cpuCoreCount());
        diskSpin->setValue(m_customConfig.diskSizeGb());

        if (isoRadio->isChecked() && !m_customConfig.isoPath().isEmpty())
            isoRequester->setText(m_customConfig.isoPath());
    }

    syncSlider(ramSlider, m_ramSteps, ramSpin->value());
    syncSlider(diskSlider, m_diskSteps, diskSpin->value());
    {
        const QSignalBlocker blocker(cpuSlider);
        cpuSlider->setValue(cpuSpin->value());
    }

    customiseButton->setText(i18n("Customised"));
    customiseButton->setToolTip(i18n("Hardware settings have been customised. Changing the operating system discards them."));

    iconSelector->loadFrom(m_customConfig, editor.iconManuallySet());

    updateIcon();
    updateDeployInfo();
    validateForm();
}

void NewVmDialog::onOsChanged()
{
    // A customisation belongs to the template it was made for.
    clearCustomisation();

    updateCloudImages();

    const bool cloudAvailable = hasCloudDeploy();
    cloudRadio->setEnabled(cloudAvailable);
    if (!cloudAvailable && cloudRadio->isChecked())
        isoRadio->setChecked(true);

    applyTemplateDefaults();
    updateIcon();
    onSourceChanged();
}

void NewVmDialog::onSourceChanged()
{
    const Source src = source();
    const bool isCloud = src == Source::CloudImage;

    cloudOptions->setVisible(isCloud);

    const bool showArchitectures = isCloud && cloudImageCombo->count() > 1;
    cloudImageLabel->setVisible(showArchitectures);
    cloudImageCombo->setVisible(showArchitectures);

    isoRequester->setEnabled(src == Source::InstallerIso);

    updateDeployInfo();
    validateForm();
}

void NewVmDialog::onNameEdited()
{
    updateDeployInfo();
    validateForm();
}

void NewVmDialog::openCloudConfigDialog()
{
    const CloudDeployConfig before = cloudConfig();

    CloudConfigDialog dialog(this);
    dialog.setConfig(before);

    if (dialog.exec() != QDialog::Accepted)
        return;

    m_cloudConfig = dialog.getResult();

    // Once the user has set a host name of their own, stop deriving it from
    // the VM name. Never the other way round.
    if (m_cloudConfig.hostname != before.hostname)
        m_hostnameIsDerived = false;

    updateDeployInfo();
}

void NewVmDialog::validateForm()
{
    const QString name = nameEdit->text().trimmed();

    QString error;
    bool valid = VMConfig::isValidVmName(name, &error);

    if (valid && m_existingVmNames.contains(name, Qt::CaseSensitive)) {
        error = i18n("A virtual machine named '%1' already exists.", name);
        valid = false;
    }

    if (valid && selectedTemplateId().isEmpty()) {
        error = i18n("Please select an operating system.");
        valid = false;
    }

    if (valid && source() == Source::InstallerIso) {
        const QString iso = isoRequester->text().trimmed();
        if (iso.isEmpty()) {
            error = i18n("Please select an installation image.");
            valid = false;
        } else if (!QFileInfo::exists(iso)) {
            error = i18n("The selected installation image does not exist.");
            valid = false;
        }
    }

    if (valid && source() == Source::CloudImage) {
        const CloudImageInfo img = selectedCloudImage();
        if (!img.arch.isEmpty() && !ImageDatabase::isArchCompatible(img.arch)) {
            error = i18n("Foreign architecture. Emulation support is not yet available.");
            valid = false;
        }
    }

    warningLabel->setText(error);
    buttonBox->button(QDialogButtonBox::Ok)->setEnabled(valid);
    customiseButton->setEnabled(valid);
}

// --- Helpers ---

void NewVmDialog::updateCloudImages()
{
    m_cloudImages.clear();
    cloudImageCombo->clear();

    if (!m_imageDatabase)
        return;

    m_cloudImages = m_imageDatabase->cloudImagesFor(selectedTemplateId(), selectedVersion(), SettingsDialog::showAllArchitectures());

    for (int i = 0; i < m_cloudImages.size(); ++i)
        cloudImageCombo->addItem(m_cloudImages.at(i).arch, i);
}

void NewVmDialog::applyTemplateDefaults()
{
    if (!m_imageDatabase)
        return;

    const QString tplId = selectedTemplateId();
    if (tplId.isEmpty())
        return;

    const VMConfig defaults = m_imageDatabase->defaultsFor(tplId);

    cpuSpin->setMaximum(VMConfig::maxCoresForModel(defaults.cpuModel()));
    cpuSlider->setMaximum(cpuSpin->maximum());
    cpuSpin->setValue(defaults.cpuCoreCount());

    ramSpin->setValue(defaults.ramMb());

    // minDiskGbFor() already falls back to the project default, so no qMax():
    // it would override a template that deliberately asks for less.
    const int minDisk = m_imageDatabase->minDiskGbFor(tplId, selectedVersion());

    diskSpin->setMinimum(minDisk);
    diskSlider->setMinimum(firstStepIndexAtLeast(m_diskSteps, minDisk));
    diskSpin->setValue(qMax(defaults.diskSizeGb(), minDisk));
}

void NewVmDialog::updateIcon()
{
    const TemplateInfo *tpl = selectedTemplate();
    if (!tpl)
        return;

    iconSelector->followOs(tpl->osType, tpl->id, tpl->iconHint);
}

void NewVmDialog::updateDeployInfo()
{
    const CloudDeployConfig config = cloudConfig();

    QString credentials;
    if (!config.sshKeys.isEmpty())
        credentials = i18n("SSH key");
    else if (!config.password.isEmpty())
        credentials = i18n("password");
    else
        credentials = i18n("no way to log in");

    deployInfoLabel->setText(u"<small>%1</small>"_s.arg(i18n("User \"%1\", host name \"%2\", %3.", config.username, config.hostname, credentials)));
}

void NewVmDialog::clearCustomisation()
{
    if (!m_hasCustomConfig)
        return;

    m_hasCustomConfig = false;
    m_customConfig = {};

    customiseButton->setText(i18n("Customise..."));
    customiseButton->setToolTip(i18n("Review and change the full hardware configuration before the machine is created."));
}

QStringList NewVmDialog::usedMacAddresses() const
{
    QStringList macs;
    for (const auto &info : m_macUsageInfo)
        macs << info.mac;
    return macs;
}

void NewVmDialog::syncSlider(QSlider *slider, const QList<int> &steps, int value)
{
    const QSignalBlocker blocker(slider);
    slider->setValue(nearestStepIndex(steps, value, slider->minimum()));
}

// --- Results ---

NewVmDialog::Source NewVmDialog::source() const
{
    if (cloudRadio->isChecked())
        return Source::CloudImage;
    if (isoRadio->isChecked())
        return Source::InstallerIso;
    return Source::BlankDisk;
}

QString NewVmDialog::installerIsoPath() const
{
    return source() == Source::InstallerIso ? isoRequester->text().trimmed() : QString();
}

CloudImageInfo NewVmDialog::selectedCloudImage() const
{
    if (source() != Source::CloudImage || m_cloudImages.isEmpty())
        return {};

    const int index = cloudImageCombo->currentIndex() >= 0 ? cloudImageCombo->currentData().toInt() : 0;
    return m_cloudImages.value(index);
}

CloudDeployConfig NewVmDialog::cloudConfig() const
{
    CloudDeployConfig config = m_cloudConfig;

    if (m_hostnameIsDerived)
        config.hostname = Utils::vmNameToHostname(nameEdit->text().trimmed());

    // Template ids always follow the current selection.
    if (const TemplateInfo *tpl = selectedTemplate()) {
        config.templateId = tpl->cloudInitTemplate;
        config.networkTemplateId = tpl->networkTemplate;
    }

    return config;
}

VMConfig NewVmDialog::getResult() const
{
    const TemplateInfo *tpl = selectedTemplate();

    VMConfig config = m_hasCustomConfig ? m_customConfig : (m_imageDatabase ? m_imageDatabase->defaultsFor(selectedTemplateId()) : VMConfig());

    config.setName(nameEdit->text().trimmed());
    config.setDistroId(selectedTemplateId());
    if (tpl)
        config.setOsType(tpl->osType);

    config.setRamMb(ramSpin->value());
    config.setCpuCoreCount(cpuSpin->value());
    config.setDiskSizeGb(diskSpin->value());

    if (!m_hasCustomConfig) {
        // Architecture follows the image for cloud deployments; otherwise the
        // CPU model decides, with the host architecture as the fallback.
        const QString imageArch = source() == Source::CloudImage ? selectedCloudImage().arch : QString();
        if (!imageArch.isEmpty()) {
            config.setArchitecture(VMConfig::parseArchitecture(imageArch));
        } else {
            QString hint = VMConfig::x86ArchHint(config.cpuModel());
            if (hint.isEmpty())
                hint = ImageDatabase::hostArch();
            config.setArchitecture(VMConfig::parseArchitecture(hint));
        }

        // templates.json asks for the D-Bus display, which needs dbus-daemon.
        if (!VMConfig::isDisplayTypeAvailable(config.displayType()))
            config.setDisplayType(VMSpecs::DisplayType::Gtk);
    }

    // The installer medium lives in this dialog's own field, so it must be
    // applied even when a customised configuration is in play.
    if (source() == Source::InstallerIso) {
        config.setISOPath(installerIsoPath());
        config.setBootFromIso(true);
    }

    iconSelector->applyTo(config);

    config.ensureMacAddress(usedMacAddresses());

    return config;
}
