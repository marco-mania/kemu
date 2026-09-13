/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "takesnapshotdialog.h"

TakeSnapshotDialog::TakeSnapshotDialog(const QString &vmName, const QStringList &existingNames, QWidget *parent)
    : QDialog(parent)
    , m_existingNames(existingNames)
{
    setWindowTitle(i18n("Take Snapshot – %1", vmName));
    setMinimumWidth(440);

    auto *layout = new QVBoxLayout(this);
    layout->setSpacing(12);

    auto *infoLabel = new QLabel(i18n("Save the current state of <b>%1</b> as a snapshot.", vmName));
    infoLabel->setWordWrap(true);
    layout->addWidget(infoLabel);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    m_nameEdit = new QLineEdit(this);
    m_nameEdit->setPlaceholderText(i18n("e.g. Before_Update"));
    m_nameEdit->setMaxLength(64);
    form->addRow(i18n("Name:"), m_nameEdit);

    m_nameHintLabel = new QLabel(this);
    {
        const KColorScheme scheme(QPalette::Active, KColorScheme::View);
        m_hintColor = scheme.foreground(KColorScheme::InactiveText).color().name();
        m_errorColor = scheme.foreground(KColorScheme::NegativeText).color().name();
    }
    setHint(i18n("Spaces will be replaced by underscores."), false);
    form->addRow({}, m_nameHintLabel);

    m_descriptionEdit = new QTextEdit(this);
    m_descriptionEdit->setPlaceholderText(i18n("Optional: describe what this snapshot captures..."));
    m_descriptionEdit->setMaximumHeight(80);
    form->addRow(i18n("Description:"), m_descriptionEdit);

    layout->addLayout(form);

    m_buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    m_buttonBox->button(QDialogButtonBox::Ok)->setText(i18n("Take Snapshot"));
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    layout->addWidget(m_buttonBox);

    connect(m_nameEdit, &QLineEdit::textChanged, this, &TakeSnapshotDialog::onNameChanged);
    connect(m_buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

QString TakeSnapshotDialog::snapshotName() const
{
    static const QRegularExpression invalidChars(u"[^A-Za-z0-9_-]"_s);
    QString n = m_nameEdit->text().trimmed();
    n.replace(u' ', u'_');
    n.remove(invalidChars);
    return n;
}

QString TakeSnapshotDialog::snapshotDescription() const
{
    return m_descriptionEdit->toPlainText().trimmed();
}

void TakeSnapshotDialog::onNameChanged()
{
    // Validate what will actually be written, not what was typed.
    const QString raw = m_nameEdit->text();
    const QString cleaned = snapshotName();

    if (cleaned.isEmpty()) {
        setHint(raw.trimmed().isEmpty() ? i18n("Spaces will be replaced by underscores.") : i18n("This name contains no usable characters."),
                !raw.trimmed().isEmpty());
        m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    // qcow2 does not enforce unique tags: qemu-img would happily create a
    // second snapshot of the same name, while the QMP path replaces the
    // existing one.
    if (m_existingNames.contains(cleaned)) {
        setHint(i18n("A snapshot named <b>%1</b> already exists.", cleaned), true);
        m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
        return;
    }

    setHint(cleaned != raw ? i18n("Will be saved as: <b>%1</b>", cleaned) : i18n("Spaces will be replaced by underscores."), false);
    m_buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void TakeSnapshotDialog::setHint(const QString &text, bool isError)
{
    m_nameHintLabel->setStyleSheet(u"color: %1; font-size: 11px;"_s.arg(isError ? m_errorColor : m_hintColor));
    m_nameHintLabel->setText(text);
}
