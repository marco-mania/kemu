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

class TakeSnapshotDialog : public QDialog {
    Q_OBJECT

public:
    explicit TakeSnapshotDialog(const QString& vmName, QWidget* parent = nullptr);

    QString snapshotName() const;
    QString snapshotDescription() const;

private Q_SLOTS:
    void onNameChanged(const QString& text);

private:
    QLineEdit* m_nameEdit = nullptr;
    QTextEdit* m_descriptionEdit = nullptr;
    QLabel* m_nameHintLabel = nullptr;
    QDialogButtonBox* m_buttonBox = nullptr;
};
