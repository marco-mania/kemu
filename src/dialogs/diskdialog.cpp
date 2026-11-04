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
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QToolButton>
#include <QVBoxLayout>

#include <KIconUtils>
#include <KLocalizedString>
#include <KMessageWidget>
#include <KUrlRequester>

namespace
{
KMessageWidget *inlineMessage(KMessageWidget::MessageType type, QWidget *parent)
{
    // As in the VM settings: no close button, it goes when its reason does.
    auto *message = new KMessageWidget(parent);
    message->setMessageType(type);
    message->setCloseButtonVisible(false);
    message->setWordWrap(true);
    message->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    message->setMinimumWidth(0);
    message->hide();
    return message;
}

void showMessage(KMessageWidget *message, bool visible)
{
    if (visible && (message->isHidden() || message->isHideAnimationRunning()))
        message->animatedShow();
    else if (!visible && !message->isHidden() && !message->isHideAnimationRunning())
        message->animatedHide();
}
}

DiskDialog::DiskDialog(Mode mode, VMSpecs::MachineType machineType, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
    , m_machineType(machineType)
{
    setWindowTitle(mode == Mode::Add ? i18n("Add Disk") : i18n("Disk Properties"));
    setMinimumWidth(460);

    auto *layout = new QVBoxLayout(this);

    // On top of the form entries, as the KDE HIG places inline messages.
    m_interfaceHint = inlineMessage(KMessageWidget::Information, this);
    layout->addWidget(m_interfaceHint);
    if (mode == Mode::Add) {
        // Only while an image is chosen.
        m_sourceHint = inlineMessage(KMessageWidget::Information, this);
        m_sourceHint->setText(i18n("The image is copied into the VM folder and converted to qcow2. The original file is not changed."));
        layout->addWidget(m_sourceHint);
    }

    auto *form = new QFormLayout;
    m_form = form;
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

        connect(m_newRadio, &QRadioButton::toggled, this, &DiskDialog::updateState);
        connect(m_sourceRequester, &KUrlRequester::textChanged, this, &DiskDialog::updateState);
    }

    if (mode == Mode::Edit) {
        m_sizeSpin = new QSpinBox(this);
        m_sizeSpin->setRange(1, 32000);
        m_sizeSpin->setSuffix(i18n(" GB"));
        m_sizeSpin->setEnabled(false);

        // Next to the size they act on. Both act at once, independent of OK/Cancel.
        m_expandButton = new QPushButton(QIcon::fromTheme(u"zoom-in"_s, QIcon::fromTheme(u"list-add"_s)), i18nc("@action:button", "Expand…"), this);
        m_expandButton->setToolTip(i18n("Grows the disk to the size entered. This happens at once."));
        m_expandButton->hide();
        m_compactButton = new QPushButton(QIcon::fromTheme(u"edit-clear-history"_s), i18nc("@action:button", "Compact…"), this);
        m_compactButton->setToolTip(i18n("Frees the unused space in the image file. This happens at once."));
        m_compactButton->hide();
        connect(m_expandButton, &QPushButton::clicked, this, [this] {
            Q_EMIT expandRequested(m_sizeSpin->value());
        });
        connect(m_sizeSpin, &QSpinBox::valueChanged, this, [this](int value) {
            m_expandButton->setEnabled(m_currentSizeGb > 0 && value > m_currentSizeGb);
        });
        connect(m_compactButton, &QPushButton::clicked, this, &DiskDialog::compactRequested);

        auto *sizeRow = new QHBoxLayout;
        sizeRow->addWidget(m_sizeSpin);
        sizeRow->addWidget(m_expandButton);
        sizeRow->addWidget(m_compactButton);
        sizeRow->addStretch();
        form->addRow(i18n("Size:"), sizeRow);
        m_sizeLabel = qobject_cast<QLabel *>(form->labelForField(sizeRow));
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
    connect(m_interfaceCombo, &QComboBox::currentIndexChanged, this, &DiskDialog::updateInterfaceHint);

    layout->addLayout(form);
    layout->addStretch();

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    if (mode == Mode::Add)
        m_buttonBox->button(QDialogButtonBox::Ok)->setText(i18n("Add"));
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(m_buttonBox);

    updateState();
    updateInterfaceHint();
}

void DiskDialog::setSystemDisk(VMSpecs::OsFamily osFamily)
{
    m_systemOsFamily = osFamily;
    setWindowTitle(i18n("System Disk Properties"));
    m_labelEdit->setText(i18nc("@item name of the system disk", "System"));
    m_labelEdit->setEnabled(false);
    m_labelEdit->setToolTip(i18n("The disk the guest boots from keeps this name."));
    updateInterfaceHint();
}

void DiskDialog::setEncryption(const Encryption &encryption)
{
    // Small buttons right after the state; they act at once as well.
    if (!m_encryptionLabel) {
        auto *row = new QHBoxLayout;
        m_encryptionLabel = new QLabel(this);
        row->addWidget(m_encryptionLabel);
        const auto makeButton = [this, row](const QIcon &icon, void (DiskDialog::*requested)()) {
            auto *button = new QToolButton(this);
            button->setIcon(icon);
            connect(button, &QToolButton::clicked, this, requested);
            row->addWidget(button);
            return button;
        };
        m_encryptButton = makeButton(QIcon::fromTheme(u"lock"_s), &DiskDialog::encryptRequested);
        m_decryptButton = makeButton(QIcon::fromTheme(u"unlock"_s, QIcon::fromTheme(u"object-unlocked"_s)), &DiskDialog::decryptRequested);
        m_changePasswordButton = makeButton(QIcon::fromTheme(u"dialog-password"_s, QIcon::fromTheme(u"lock"_s)), &DiskDialog::changePasswordRequested);
        m_walletButton = new QToolButton(this);
        connect(m_walletButton, &QToolButton::clicked, this, [this] {
            if (m_passwordStored)
                Q_EMIT forgetPasswordRequested();
            else
                Q_EMIT storePasswordRequested();
        });
        row->addWidget(m_walletButton);
        row->addStretch();
        m_form->addRow(i18n("Encryption:"), row);
    }

    m_passwordStored = encryption.encrypted && encryption.passwordStored;
    m_encryptionLabel->setText(!encryption.encrypted ? i18n("Not encrypted")
                                   : m_passwordStored ? i18n("Encrypted, password in KWallet")
                                                      : i18n("Encrypted, password asked at start"));

    // Which buttons are shown tells the state.
    m_encryptButton->setVisible(!encryption.encrypted);
    m_decryptButton->setVisible(encryption.encrypted);
    m_changePasswordButton->setVisible(encryption.encrypted);
    m_walletButton->setVisible(encryption.encrypted);

    const bool blocked = !encryption.blockedReason.isEmpty();
    const auto offer = [&encryption, blocked](QToolButton *button, const QString &text) {
        button->setEnabled(!blocked);
        button->setToolTip(blocked ? encryption.blockedReason : text);
        button->setAccessibleName(text);
    };
    offer(m_encryptButton, i18n("Encrypt the system disk…"));
    offer(m_decryptButton, i18n("Decrypt the system disk…"));
    offer(m_changePasswordButton, i18n("Change the password…"));

    // KWallet's icon with a plus or minus.
    const QString walletText = !encryption.walletAvailable ? i18n("KWallet is not available")
        : m_passwordStored                                 ? i18n("Remove the password from KWallet")
                                                           : i18n("Store the password in KWallet…");
    m_walletButton->setIcon(KIconUtils::addOverlay(QIcon::fromTheme(u"kwalletmanager"_s, QIcon::fromTheme(u"wallet-open"_s)),
                                                   QIcon::fromTheme(m_passwordStored ? u"list-remove"_s : u"list-add"_s),
                                                   Qt::BottomRightCorner));
    m_walletButton->setToolTip(walletText);
    m_walletButton->setAccessibleName(walletText);
    m_walletButton->setEnabled(encryption.walletAvailable);
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

void DiskDialog::setNewDiskSize(int sizeGb, int minimumGb)
{
    if (!m_sizeSpin || m_mode != Mode::Edit)
        return;
    m_sizeSpin->setMinimum(minimumGb);
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
    showMessage(m_sourceHint, useExisting);

    const bool valid = !useExisting || QFileInfo(sourcePath()).isFile();
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(valid);
}

void DiskDialog::updateInterfaceHint()
{
    QString hint;
    auto type = KMessageWidget::Information;
    if (diskInterface() == VMSpecs::DiskInterface::SataIde) {
        if (m_machineType == VMSpecs::MachineType::Q35)
            hint = i18n("Q35 chipset: disk attached via AHCI (SATA).");
        else if (m_machineType == VMSpecs::MachineType::PcISA)
            hint = i18n("ISA-only PC: disk attached via legacy ISA IDE.");
        else
            hint = i18n("PC (i440FX) chipset: disk attached via PIIX3 (IDE).");
    } else if (diskInterface() == VMSpecs::DiskInterface::Virtio && m_systemOsFamily == VMSpecs::OsFamily::Windows) {
        hint = i18n("Windows needs the VirtIO driver ISO during installation, or its installer finds no disk.");
        type = KMessageWidget::Warning;
    }
    m_interfaceHint->setMessageType(type);
    if (!hint.isEmpty())
        m_interfaceHint->setText(hint);
    showMessage(m_interfaceHint, !hint.isEmpty());
}
