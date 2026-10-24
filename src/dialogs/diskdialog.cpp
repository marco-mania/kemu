/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskdialog.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KLocalizedString>
#include <KUrlRequester>

DiskDialog::DiskDialog(Mode mode, VMSpecs::MachineType machineType, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
{
    setWindowTitle(mode == Mode::Add ? i18n("Add Disk") : i18n("Disk Properties"));
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    if (mode == Mode::Add) {
        m_newRadio = new QRadioButton(i18n("Create a new, empty disk"), this);
        m_existingRadio = new QRadioButton(i18n("Use an existing disk image"), this);
        m_newRadio->setChecked(true);

        auto *group = new QButtonGroup(this);
        group->addButton(m_newRadio);
        group->addButton(m_existingRadio);

        layout->addWidget(m_newRadio);
        layout->addWidget(m_existingRadio);
        layout->addSpacing(6);
    }

    m_labelEdit = new QLineEdit(this);
    m_labelEdit->setPlaceholderText(i18n("e.g. Data"));
    m_labelEdit->setMaxLength(64);
    form->addRow(i18n("Name:"), m_labelEdit);

    if (mode == Mode::Add) {
        m_sizeSpin = new QSpinBox(this);
        m_sizeSpin->setRange(1, 32000);
        m_sizeSpin->setValue(20);
        m_sizeSpin->setSuffix(i18n(" GB"));
        form->addRow(i18n("Size:"), m_sizeSpin);
        m_sizeLabel = qobject_cast<QLabel *>(form->labelForField(m_sizeSpin));

        m_sourceRequester = new KUrlRequester(this);
        m_sourceRequester->setMode(KFile::File | KFile::ExistingOnly | KFile::LocalOnly);
        m_sourceRequester->setNameFilter(i18n("Disk Images (*.qcow2 *.img *.raw *.vmdk *.vdi *.vhd *.vhdx);;All Files (*)"));
        form->addRow(i18n("Image:"), m_sourceRequester);
        m_sourceLabel = qobject_cast<QLabel *>(form->labelForField(m_sourceRequester));

        const KColorScheme scheme(QPalette::Active, KColorScheme::View);
        m_sourceHint = new QLabel(i18n("The image is copied into the VM folder and converted to qcow2. The original file is not changed."), this);
        m_sourceHint->setWordWrap(true);
        m_sourceHint->setStyleSheet(u"QLabel { color: %1; font-size: 11px; }"_s.arg(scheme.foreground(KColorScheme::InactiveText).color().name()));
        form->addRow({}, m_sourceHint);

        connect(m_newRadio, &QRadioButton::toggled, this, &DiskDialog::updateState);
        connect(m_sourceRequester, &KUrlRequester::textChanged, this, &DiskDialog::updateState);
    }

    if (mode == Mode::Edit) {
        m_sizeSpin = new QSpinBox(this);
        m_sizeSpin->setRange(1, 32000);
        m_sizeSpin->setSuffix(i18n(" GB"));
        m_sizeSpin->setEnabled(false);
        form->addRow(i18n("Size:"), m_sizeSpin);
        m_sizeLabel = qobject_cast<QLabel *>(form->labelForField(m_sizeSpin));
    }

    m_interfaceCombo = new QComboBox(this);
    for (const auto iface : {VMSpecs::DiskInterface::Virtio, VMSpecs::DiskInterface::Nvme, VMSpecs::DiskInterface::SataIde})
        m_interfaceCombo->addItem(VMConfig::toUIString(iface), static_cast<int>(iface));

    if (!VMConfig::supportsPciBus(machineType)) {
        if (auto *model = qobject_cast<QStandardItemModel *>(m_interfaceCombo->model())) {
            for (int i = 0; i < m_interfaceCombo->count(); ++i) {
                if (static_cast<VMSpecs::DiskInterface>(m_interfaceCombo->itemData(i).toInt()) != VMSpecs::DiskInterface::SataIde)
                    model->item(i)->setEnabled(false);
            }
        }
        setDiskInterface(VMSpecs::DiskInterface::SataIde);
    }
    form->addRow(i18n("Controller:"), m_interfaceCombo);

    layout->addLayout(form);
    layout->addStretch();

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (mode == Mode::Add)
        m_buttonBox->button(QDialogButtonBox::Ok)->setText(i18n("Add"));
    if (mode == Mode::Edit) {
        m_expandButton = m_buttonBox->addButton(i18n("Expand..."), QDialogButtonBox::ActionRole);
        m_expandButton->setIcon(QIcon::fromTheme(u"zoom-in"_s, QIcon::fromTheme(u"list-add"_s)));
        m_expandButton->setToolTip(i18n("Grow the disk image to the size above. The guest enlarges its partition and file system itself."));
        m_expandButton->hide();
        connect(m_expandButton, &QPushButton::clicked, this, [this] {
            Q_EMIT expandRequested(m_sizeSpin->value());
        });
        connect(m_sizeSpin, &QSpinBox::valueChanged, this, [this](int value) {
            m_expandButton->setEnabled(m_currentSizeGb > 0 && value > m_currentSizeGb);
        });

        m_compactButton = m_buttonBox->addButton(i18n("Compact..."), QDialogButtonBox::ActionRole);
        m_compactButton->setIcon(QIcon::fromTheme(u"edit-clear-history"_s));
        m_compactButton->setToolTip(i18n("Compact the disk image by removing unused blocks. Please free blocks on guest OS level first."));
        m_compactButton->hide();
        connect(m_compactButton, &QPushButton::clicked, this, &DiskDialog::compactRequested);
    }
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttonBox);

    updateState();
}

void DiskDialog::setCompactAvailable(bool available)
{
    if (m_compactButton)
        m_compactButton->setVisible(available);
}

void DiskDialog::setCurrentSize(int sizeGb)
{
    if (!m_expandButton)
        return;
    m_currentSizeGb = sizeGb;
    if (sizeGb > 0) {
        m_sizeSpin->setMinimum(sizeGb);
        m_sizeSpin->setValue(sizeGb);
    }
    m_sizeSpin->setEnabled(sizeGb > 0);
    m_expandButton->setVisible(true);
    m_expandButton->setEnabled(false);
}

void DiskDialog::setNewDiskSize(int sizeGb)
{
    if (!m_sizeSpin || m_mode != Mode::Edit)
        return;
    m_sizeSpin->setValue(sizeGb);
    m_sizeSpin->setEnabled(true);
}

void DiskDialog::setLabel(const QString &label)
{
    m_labelEdit->setText(label);
}

void DiskDialog::setDiskInterface(VMSpecs::DiskInterface diskInterface)
{
    const int index = m_interfaceCombo->findData(static_cast<int>(diskInterface));
    if (index >= 0)
        m_interfaceCombo->setCurrentIndex(index);
}

QString DiskDialog::label() const
{
    return m_labelEdit->text().simplified();
}

VMSpecs::DiskInterface DiskDialog::diskInterface() const
{
    return static_cast<VMSpecs::DiskInterface>(m_interfaceCombo->currentData().toInt());
}

int DiskDialog::sizeGb() const
{
    return m_sizeSpin ? m_sizeSpin->value() : 0;
}

QString DiskDialog::sourcePath() const
{
    if (!m_existingRadio || !m_existingRadio->isChecked())
        return {};
    return m_sourceRequester->url().toLocalFile();
}

void DiskDialog::updateState()
{
    if (m_mode != Mode::Add)
        return;

    const bool useExisting = m_existingRadio->isChecked();
    m_sizeSpin->setEnabled(!useExisting);
    m_sizeLabel->setEnabled(!useExisting);
    m_sourceRequester->setEnabled(useExisting);
    m_sourceLabel->setEnabled(useExisting);
    m_sourceHint->setEnabled(useExisting);

    const bool valid = !useExisting || QFileInfo(sourcePath()).isFile();
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(valid);
}
