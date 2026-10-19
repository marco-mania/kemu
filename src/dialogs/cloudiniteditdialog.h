/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QStringList>

#include "utils/cloudimagedeployer.h"

class KMessageWidget;
class QComboBox;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;

/**
 * @brief Edits one cloud-init template, with a preview rendered from sample values.
 */
class CloudInitEditDialog : public QDialog
{
    Q_OBJECT

public:
    enum class Mode {
        New,
        Edit, // the user's own template; the id may change to an unused one
        Customize // a user copy of a shipped or downloaded template; the id is fixed
    };

    /// @p takenIds: ids the result must not use; ignored when customizing.
    CloudInitEditDialog(Mode mode,
                        const QString &id,
                        const QString &text,
                        const QStringList &takenIds,
                        const CloudDeployConfig &sampleConfig,
                        QWidget *parent = nullptr);

    [[nodiscard]] QString id() const;
    [[nodiscard]] QString text() const;

private:
    void validate();
    void insertPlaceholder();
    void setType(bool network);
    [[nodiscard]] QString starterText(bool network) const;

    Mode m_mode;
    QString m_originalId;
    QString m_originalText;
    QStringList m_takenIds;
    CloudDeployConfig m_sampleConfig;

    QLineEdit *m_idEdit = nullptr;
    QComboBox *m_typeCombo = nullptr;
    QTabWidget *m_tabs = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QPlainTextEdit *m_preview = nullptr;
    QListWidget *m_placeholders = nullptr;
    KMessageWidget *m_error = nullptr;
    KMessageWidget *m_warning = nullptr;
    QPushButton *m_saveButton = nullptr;
};
