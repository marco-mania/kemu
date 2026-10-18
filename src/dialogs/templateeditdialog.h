/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QJsonObject>
#include <QStringList>

class KMessageWidget;
class QPlainTextEdit;
class QPushButton;

/**
 * @brief Edits one OS template as JSON.
 */
class TemplateEditDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode {
        New, // a new id, not used by any template yet
        Edit, // the user's own template; the id may change to an unused one
        Customize // a user copy of a shipped or downloaded template; the id is fixed
    };

    /// @p takenIds: ids the result must not use; ignored when customizing.
    TemplateEditDialog(Mode mode, const QJsonObject &tpl, const QStringList &takenIds, QWidget *parent = nullptr);

    [[nodiscard]] QJsonObject result() const;

private:
    void validate();

    Mode m_mode;
    QString m_originalId;
    QStringList m_takenIds;
    QJsonObject m_result;

    QPlainTextEdit *m_editor = nullptr;
    KMessageWidget *m_message = nullptr;
    QPushButton *m_saveButton = nullptr;
};
