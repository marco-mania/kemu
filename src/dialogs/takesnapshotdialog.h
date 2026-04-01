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
#include <QPointer>
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
    QPointer<QLineEdit> m_nameEdit;
    QPointer<QTextEdit> m_descriptionEdit;
    QPointer<QLabel> m_nameHintLabel;
    QPointer<QDialogButtonBox> m_buttonBox;
};
