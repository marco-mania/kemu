/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cloudiniteditdialog.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QFormLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QTabWidget>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KMessageWidget>

#include "utils/templatestore.h"

using namespace Qt::StringLiterals;

namespace
{

constexpr int SnippetRole = Qt::UserRole;

struct Placeholder {
    QString name;
    QString snippet;
    QString description;
};

// The variables CloudImageDeployer provides for user-data templates.
QList<Placeholder> placeholders()
{
    return {
        {u"HOSTNAME"_s, u"{{ HOSTNAME }}"_s, i18n("Host name of the virtual machine")},
        {u"USERNAME"_s, u"{{ USERNAME }}"_s, i18n("Name of the user account")},
        {u"PASSWORD_HASH"_s, u"{% if PASSWORD_HASH %}{{ PASSWORD_HASH }}{% endif %}"_s, i18n("SHA-512 hash of the password; empty without a password")},
        {u"SSH_PWAUTH"_s, u"{{ SSH_PWAUTH }}"_s, i18n("Whether SSH accepts passwords")},
        {u"SSH_KEYS"_s, u"{% for key in SSH_KEYS %}\n  - {{ key }}\n{% endfor %}"_s, i18n("List of SSH public keys")},
        {u"TIMEZONE"_s, u"{{ TIMEZONE }}"_s, i18n("Time zone, e.g. Europe/Berlin")},
        {u"LOCALE"_s, u"{{ LOCALE }}"_s, i18n("Locale, e.g. de_DE.UTF-8")},
        {u"LANG"_s, u"{{ LANG }}"_s, i18n("Locale without the encoding, e.g. de_DE")},
        {u"LANG_SHORT"_s, u"{{ LANG_SHORT }}"_s, i18n("Language code, e.g. de")},
        {u"KB_LAYOUT"_s, u"{{ KB_LAYOUT }}"_s, i18n("Keyboard layout, e.g. de")},
        {u"KB_VARIANT"_s, u"{{ KB_VARIANT }}"_s, i18n("Keyboard variant, may be empty")},
        {u"PACKAGES"_s, u"{% for pkg in PACKAGES %}\n  - {{ pkg }}\n{% endfor %}"_s, i18n("List of extra packages")},
        {u"DESKTOP_ENVIRONMENT"_s, u"{% if DESKTOP_ENVIRONMENT %}\n{% endif %}"_s, i18n("Whether a desktop environment is to be installed")},
    };
}

} // namespace

CloudInitEditDialog::CloudInitEditDialog(Mode mode,
                                         const QString &id,
                                         const QString &text,
                                         const QStringList &takenIds,
                                         const CloudDeployConfig &sampleConfig,
                                         QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
    , m_originalId(id)
    , m_originalText(text)
    , m_takenIds(takenIds)
    , m_sampleConfig(sampleConfig)
{
    switch (mode) {
    case Mode::New:
        setWindowTitle(i18nc("@title:window", "New Cloud-Init Template"));
        break;
    case Mode::Edit:
        setWindowTitle(i18nc("@title:window", "Edit Cloud-Init Template"));
        break;
    case Mode::Customize:
        setWindowTitle(i18nc("@title:window", "Customize Cloud-Init Template"));
        break;
    }

    m_idEdit = new QLineEdit(id, this);
    m_idEdit->setReadOnly(mode == Mode::Customize);
    m_idEdit->setToolTip(i18n("Templates whose id starts with \"network-\" provide the network configuration, all others the user data."));
    auto *form = new QFormLayout;
    form->addRow(i18n("ID:"), m_idEdit);

    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_editor = new QPlainTextEdit(this);
    m_editor->setFont(fixed);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabChangesFocus(false);
    m_editor->setPlainText(text);

    m_preview = new QPlainTextEdit(this);
    m_preview->setFont(fixed);
    m_preview->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_preview->setReadOnly(true);

    m_tabs = new QTabWidget(this);
    m_tabs->addTab(m_editor, i18n("Template"));
    m_tabs->addTab(m_preview, i18n("Preview"));
    m_tabs->setTabToolTip(1, i18n("Rendered with the deployment defaults from the settings"));

    m_placeholders = new QListWidget(this);
    for (const Placeholder &p : placeholders()) {
        auto *item = new QListWidgetItem(p.name, m_placeholders);
        item->setToolTip(p.description);
        item->setData(SnippetRole, p.snippet);
    }
    auto *insertButton = new QPushButton(QIcon::fromTheme(u"list-add"_s), i18n("Insert"), this);
    auto *placeholderBox = new QGroupBox(i18n("Placeholders"), this);
    auto *placeholderLayout = new QVBoxLayout(placeholderBox);
    placeholderLayout->addWidget(m_placeholders);
    placeholderLayout->addWidget(insertButton);

    auto *splitter = new QSplitter(this);
    splitter->addWidget(m_tabs);
    splitter->addWidget(placeholderBox);
    splitter->setStretchFactor(0, 1);

    m_error = new KMessageWidget(this);
    m_error->setMessageType(KMessageWidget::Error);
    m_error->setCloseButtonVisible(false);
    m_error->setWordWrap(true);
    m_error->hide();
    m_warning = new KMessageWidget(this);
    m_warning->setMessageType(KMessageWidget::Warning);
    m_warning->setCloseButtonVisible(false);
    m_warning->setWordWrap(true);
    m_warning->hide();

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, this);
    m_saveButton = buttons->button(QDialogButtonBox::Save);
    // For editing in an external editor: copy out, paste back.
    QPushButton *copyButton = buttons->addButton(i18n("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    copyButton->setIcon(QIcon::fromTheme(u"edit-copy"_s));
    copyButton->setToolTip(i18n("Copy the whole template, for editing it in another editor"));
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_editor->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(form);
    layout->addWidget(splitter, 1);
    layout->addWidget(m_error);
    layout->addWidget(m_warning);
    layout->addWidget(buttons);

    connect(m_editor, &QPlainTextEdit::textChanged, this, &CloudInitEditDialog::validate);
    connect(m_idEdit, &QLineEdit::textChanged, this, &CloudInitEditDialog::validate);
    connect(insertButton, &QPushButton::clicked, this, &CloudInitEditDialog::insertPlaceholder);
    connect(m_placeholders, &QListWidget::itemDoubleClicked, this, &CloudInitEditDialog::insertPlaceholder);

    validate();
    resize(860, 600);
}

QString CloudInitEditDialog::id() const
{
    return m_idEdit->text().trimmed();
}

QString CloudInitEditDialog::text() const
{
    return m_editor->toPlainText();
}

void CloudInitEditDialog::insertPlaceholder()
{
    const QListWidgetItem *item = m_placeholders->currentItem();
    if (!item)
        return;
    m_editor->insertPlainText(item->data(SnippetRole).toString());
    m_tabs->setCurrentWidget(m_editor);
    m_editor->setFocus();
}

void CloudInitEditDialog::validate()
{
    const QString newId = id();
    const bool network = TemplateStore::isNetworkTemplate(newId);
    m_placeholders->parentWidget()->setVisible(!network);

    QString error;
    if (!TemplateStore::isValidId(newId))
        error = i18n("The id must be 1 to 64 letters, digits, '.', '-' or '_', starting with a letter or digit.");
    else if (m_mode == Mode::Customize && newId != m_originalId)
        error = i18n("The id identifies the template you customize and cannot be changed.");
    else if (m_mode != Mode::Customize && m_takenIds.contains(newId))
        error = i18n("A cloud-init template with the id '%1' exists already. Choose another id, or customize that template instead.", newId);

    QString renderError;
    const QString rendered = CloudImageDeployer::renderTemplate(text(), m_sampleConfig, renderError);
    m_preview->setPlainText(renderError.isEmpty() ? rendered : QString());
    if (error.isEmpty() && !renderError.isEmpty())
        error = i18n("The template cannot be rendered: %1", renderError);

    // cloud-init ignores user data without this header.
    const bool header = text().startsWith(u"#cloud-config"_s);
    m_warning->setText(i18n("User data must start with \"#cloud-config\", otherwise cloud-init ignores it."));
    m_warning->setVisible(!network && !header && error.isEmpty());

    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    const bool changed = newId != m_originalId || text() != m_originalText;
    m_saveButton->setEnabled(error.isEmpty() && changed);
}
