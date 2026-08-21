/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpressionValidator>
#include <QTimeZone>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KLocalizedString>

#include "utils/cloudimagedeployer.h"

using namespace Qt::StringLiterals;

class CloudConfigDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CloudConfigDialog(QWidget *parent = nullptr);

    void setConfig(const CloudDeployConfig &cfg);
    [[nodiscard]] CloudDeployConfig getResult() const;

private Q_SLOTS:
    void onAddSshKey();
    void validate();

private:
    QLineEdit *hostnameEdit = nullptr;
    QLineEdit *usernameEdit = nullptr;
    QLineEdit *passwordEdit = nullptr;
    QLineEdit *passwordConfirmEdit = nullptr;
    QLabel *passwordMismatchLabel = nullptr;
    QCheckBox *passwordLoginCheck = nullptr;
    QComboBox *timezoneCombo = nullptr;
    QComboBox *localeCombo = nullptr;
    QComboBox *kbLayoutCombo = nullptr;
    QPlainTextEdit *sshKeysEdit = nullptr;
    QCheckBox *installDesktopCheck = nullptr;
    QLineEdit *extraPackagesEdit = nullptr;
    QDialogButtonBox *buttonBox = nullptr;
};
