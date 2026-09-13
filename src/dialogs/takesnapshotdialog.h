/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextEdit>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KLocalizedString>

using namespace Qt::StringLiterals;

class TakeSnapshotDialog : public QDialog
{
    Q_OBJECT

public:
    explicit TakeSnapshotDialog(const QString &vmName, const QStringList &existingNames, QWidget *parent = nullptr);

    QString snapshotName() const;
    QString snapshotDescription() const;

private Q_SLOTS:
    void onNameChanged();

private:
    void setHint(const QString &text, bool isError);

    QStringList m_existingNames;
    QString m_hintColor;
    QString m_errorColor;

    QLineEdit *m_nameEdit = nullptr;
    QTextEdit *m_descriptionEdit = nullptr;
    QLabel *m_nameHintLabel = nullptr;
    QDialogButtonBox *m_buttonBox = nullptr;
};
