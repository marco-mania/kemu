/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clonevmdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <KLocalizedString>

CloneVmDialog::CloneVmDialog(const QString &sourceName, const QString &suggestedName, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "Clone VM"));

    m_nameEdit = new QLineEdit(suggestedName, this);
    m_nameEdit->selectAll();

    m_linkedCheck = new QCheckBox(i18nc("@option:check", "Create linked clone"), this);

    m_linkedHint = new QLabel(i18n("Linked clones share disks with '%1' and store only changes. "
                                   "While linked clones exist, '%1' cannot be started or modified.",
                                   sourceName),
                              this);
    m_linkedHint->setWordWrap(true);
    m_linkedHint->setEnabled(false);
    QFont hintFont = m_linkedHint->font();
    hintFont.setPointSizeF(hintFont.pointSizeF() * 0.9);
    m_linkedHint->setFont(hintFont);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttons->button(QDialogButtonBox::Ok)->setText(i18nc("@action:button", "Clone"));
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(m_nameEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!text.trimmed().isEmpty());
    });
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(!suggestedName.trimmed().isEmpty());

    auto *form = new QFormLayout;
    form->addRow(i18n("Name for the cloned VM:"), m_nameEdit);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(m_linkedCheck);
    layout->addWidget(m_linkedHint);
    layout->addStretch();
    layout->addWidget(m_buttons);

    setMinimumWidth(460);
}

void CloneVmDialog::setLinkedCloneBlocker(const QString &reason)
{
    if (reason.isEmpty())
        return;
    m_linkedCheck->setChecked(false);
    m_linkedCheck->setEnabled(false);
    m_linkedHint->setText(reason);
}

QString CloneVmDialog::cloneName() const
{
    return m_nameEdit->text().trimmed();
}

bool CloneVmDialog::isLinkedClone() const
{
    return m_linkedCheck->isEnabled() && m_linkedCheck->isChecked();
}
