/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "templateeditdialog.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include <KLocalizedString>
#include <KMessageWidget>

#include "utils/templatestore.h"

using namespace Qt::StringLiterals;

TemplateEditDialog::TemplateEditDialog(Mode mode, const QJsonObject &tpl, const QStringList &takenIds, QWidget *parent)
    : QDialog(parent)
    , m_mode(mode)
    , m_originalId(tpl.value(u"id"_s).toString())
    , m_original(tpl)
    , m_takenIds(takenIds)
{
    switch (mode) {
    case Mode::New:
        setWindowTitle(i18nc("@title:window", "New OS Template"));
        break;
    case Mode::Edit:
        setWindowTitle(i18nc("@title:window", "Edit OS Template"));
        break;
    case Mode::Customize:
        setWindowTitle(i18nc("@title:window", "Customize OS Template"));
        break;
    }

    auto *hint = new QLabel(mode == Mode::Customize ? i18n("Your copy replaces the original template until you reset it. The id stays the same.")
                                                    : i18n("The template in JSON format. The README describes the available keys."),
                            this);
    hint->setWordWrap(true);

    m_editor = new QPlainTextEdit(this);
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setTabStopDistance(4 * m_editor->fontMetrics().horizontalAdvance(u' '));
    m_editor->setPlainText(QString::fromUtf8(QJsonDocument(tpl).toJson(QJsonDocument::Indented)));

    m_message = new KMessageWidget(this);
    m_message->setMessageType(KMessageWidget::Error);
    m_message->setCloseButtonVisible(false);
    m_message->setWordWrap(true);
    m_message->hide();

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
    layout->addWidget(hint);
    layout->addWidget(m_editor, 1);
    layout->addWidget(m_message);
    layout->addWidget(buttons);

    connect(m_editor, &QPlainTextEdit::textChanged, this, &TemplateEditDialog::validate);
    validate();
    resize(700, 600);
}

QJsonObject TemplateEditDialog::result() const
{
    return m_result;
}

void TemplateEditDialog::validate()
{
    QString error;
    QJsonParseError parseError;
    const QByteArray text = m_editor->toPlainText().toUtf8();
    const QJsonDocument doc = QJsonDocument::fromJson(text, &parseError);
    // A new template does not exist yet; otherwise compared as JSON, so
    // reformatting alone is no change.
    const bool changed = m_mode == Mode::New || !doc.isObject() || doc.object() != m_original;

    if (parseError.error != QJsonParseError::NoError) {
        const QByteArray before = text.left(parseError.offset);
        const int line = int(before.count('\n')) + 1;
        const int column = int(parseError.offset - before.lastIndexOf('\n'));
        error = i18n("Line %1, column %2: %3", line, column, parseError.errorString());
    } else if (!doc.isObject()) {
        error = i18n("The template must be a JSON object.");
    } else {
        const QJsonObject tpl = doc.object();
        const QString id = tpl.value(u"id"_s).toString();
        error = TemplateStore::validationError(tpl);
        if (error.isEmpty() && m_mode == Mode::Customize && id != m_originalId)
            error = i18n("The id identifies the template you customize and cannot be changed. Use Duplicate for a new template.");
        else if (error.isEmpty() && m_mode != Mode::Customize && m_takenIds.contains(id))
            error = i18n("A template with the id '%1' exists already. Choose another id, or customize that template instead.", id);
        if (error.isEmpty())
            m_result = tpl;
    }

    m_message->setText(error);
    m_message->setVisible(!error.isEmpty());
    m_saveButton->setEnabled(error.isEmpty() && changed);
}
