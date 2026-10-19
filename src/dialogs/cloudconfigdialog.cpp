/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dialogs/cloudconfigdialog.h"

#include "utils/guestlocales.h"
#include "utils/templatestore.h"
#include "utils/utils.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
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

using namespace Qt::StringLiterals;

CloudConfigDialog::CloudConfigDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18n("Cloud Image Configuration"));
    resize(480, 0);

    auto *layout = new QVBoxLayout(this);

    auto *form = new QFormLayout();
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

    hostnameEdit = new QLineEdit();
    hostnameEdit->setPlaceholderText(i18n("e.g. my-workstation"));
    hostnameEdit->setValidator(new QRegularExpressionValidator(QRegularExpression(u"^[a-zA-Z0-9][a-zA-Z0-9\\-]*$"_s), this));
    form->addRow(i18n("Hostname:"), hostnameEdit);

    usernameEdit = new QLineEdit();
    usernameEdit->setPlaceholderText(i18n("e.g. user"));
    form->addRow(i18n("Username:"), usernameEdit);

    passwordEdit = new QLineEdit();
    passwordEdit->setEchoMode(QLineEdit::Password);
    form->addRow(i18n("Password:"), passwordEdit);

    passwordConfirmEdit = new QLineEdit();
    passwordConfirmEdit->setEchoMode(QLineEdit::Password);
    form->addRow(i18n("Confirm Password:"), passwordConfirmEdit);

    passwordLoginCheck = new QCheckBox(i18n("Allow password login over SSH"));
    form->addRow({}, passwordLoginCheck);

    const KColorScheme scheme(QPalette::Active, KColorScheme::View);
    passwordMismatchLabel = new QLabel();
    passwordMismatchLabel->setStyleSheet(u"color: %1; font-size: 11px;"_s.arg(scheme.foreground(KColorScheme::NegativeText).color().name()));
    passwordMismatchLabel->setFixedHeight(passwordMismatchLabel->fontMetrics().height() + 2);
    form->addRow({}, passwordMismatchLabel);

    // Timezone
    timezoneCombo = new QComboBox();
    timezoneCombo->setEditable(true);
    for (const QByteArray &tzId : QTimeZone::availableTimeZoneIds())
        timezoneCombo->addItem(QString::fromUtf8(tzId));
    // Pre-select system timezone
    timezoneCombo->setCurrentText(QString::fromUtf8(QTimeZone::systemTimeZoneId()));
    form->addRow(i18n("Timezone:"), timezoneCombo);

    // Language / Locale
    localeCombo = new QComboBox();
    localeCombo->setEditable(true);
    localeCombo->completer()->setFilterMode(Qt::MatchContains);
    localeCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);

    const auto locales = availableLocales();
    for (const auto &loc : locales)
        localeCombo->addItem(u"%1"_s.arg(loc.displayName), loc.code);

    const QString sysLocaleCode = QLocale::system().name() + u".UTF-8"_s;
    const int locIdx = localeCombo->findData(sysLocaleCode);
    if (locIdx >= 0)
        localeCombo->setCurrentIndex(locIdx);
    else
        localeCombo->setCurrentText(sysLocaleCode);

    form->addRow(i18n("Language (Locale):"), localeCombo);

    // Keyboard Layout
    kbLayoutCombo = new QComboBox();
    kbLayoutCombo->setEditable(true);
    kbLayoutCombo->completer()->setFilterMode(Qt::MatchContains);
    kbLayoutCombo->completer()->setCaseSensitivity(Qt::CaseInsensitive);

    const auto keyboards = availableKeyboards();
    for (int i = 0; i < keyboards.size(); ++i) {
        const auto &kb = keyboards[i];
        QString label = kb.displayName;
        if (!kb.variant.isEmpty())
            label += u" -- %1"_s.arg(kb.variant);
        // Store index to retrieve layout+variant pair later
        kbLayoutCombo->addItem(label, i);
    }

    // Pre-select host keyboard layout
    const QString hostLayout = Utils::detectHostKeyboardLayout();
    for (int i = 0; i < keyboards.size(); ++i) {
        if (keyboards[i].layout == hostLayout) {
            kbLayoutCombo->setCurrentIndex(kbLayoutCombo->findData(i));
            break;
        }
    }

    form->addRow(i18n("Keyboard Layout:"), kbLayoutCombo);

    // SSH Keys
    sshKeysEdit = new QPlainTextEdit();
    sshKeysEdit->setPlaceholderText(i18n("One key per line (ssh-ed25519 AAAA...)"));
    sshKeysEdit->setMaximumHeight(80);
    form->addRow(i18n("SSH Keys:"), sshKeysEdit);

    auto *sshButtonLayout = new QHBoxLayout();
    auto *addKeyButton = new QPushButton(i18n("Import from File..."));
    sshButtonLayout->addWidget(addKeyButton);
    sshButtonLayout->addStretch();
    form->addRow({}, sshButtonLayout);
    connect(addKeyButton, &QPushButton::clicked, this, &CloudConfigDialog::onAddSshKey);

    // Desktop environment
    installDesktopCheck = new QCheckBox(i18n("Install the default desktop environment"));
    form->addRow({}, installDesktopCheck);

    // Extra packages (generic, not tied to desktop checkbox)
    extraPackagesEdit = new QLineEdit();
    extraPackagesEdit->setPlaceholderText(i18n("e.g. htop, vim, git (comma-separated)"));
    form->addRow(i18n("Extra Packages:"), extraPackagesEdit);

    // The first entry, the template of the OS, is labelled in setConfig().
    cloudInitCombo = new QComboBox();
    cloudInitCombo->addItem(i18n("Default of the operating system"), QString());
    for (const CloudInitEntry &entry : TemplateStore().cloudInitTemplates()) {
        if (!TemplateStore::isNetworkTemplate(entry.id))
            cloudInitCombo->addItem(entry.id, entry.id);
    }
    cloudInitCombo->setToolTip(i18n("The cloud-init template that sets up the guest. Templates are managed in the settings under Templates."));
    form->addRow(i18n("Cloud-Init Template:"), cloudInitCombo);

    layout->addLayout(form);

    buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttonBox);

    connect(hostnameEdit, &QLineEdit::textChanged, this, &CloudConfigDialog::validate);

    connect(passwordEdit, &QLineEdit::textChanged, this, &CloudConfigDialog::validate);
    connect(passwordConfirmEdit, &QLineEdit::textChanged, this, &CloudConfigDialog::validate);
}

void CloudConfigDialog::setConfig(const CloudDeployConfig &cfg)
{
    hostnameEdit->setText(cfg.hostname);
    usernameEdit->setText(cfg.username);
    passwordEdit->setText(cfg.password);
    passwordConfirmEdit->setText(cfg.password);

    passwordLoginCheck->setChecked(cfg.passwordLogin);

    if (!cfg.timezone.isEmpty())
        timezoneCombo->setCurrentText(cfg.timezone);

    // Locale
    if (!cfg.locale.isEmpty()) {
        const int idx = localeCombo->findData(cfg.locale);
        if (idx >= 0)
            localeCombo->setCurrentIndex(idx);
        else
            localeCombo->setCurrentText(cfg.locale);
    }

    // Keyboard
    if (!cfg.kbLayout.isEmpty()) {
        const auto keyboards = availableKeyboards();
        for (int i = 0; i < keyboards.size(); ++i) {
            if (keyboards[i].layout == cfg.kbLayout && keyboards[i].variant == cfg.kbVariant) {
                const int idx = kbLayoutCombo->findData(i);
                if (idx >= 0)
                    kbLayoutCombo->setCurrentIndex(idx);
                break;
            }
        }
    }

    if (!cfg.sshKeys.isEmpty())
        sshKeysEdit->setPlainText(cfg.sshKeys.join(u"\n"_s));

    installDesktopCheck->setChecked(cfg.installDesktop);

    if (!cfg.extraPackages.isEmpty())
        extraPackagesEdit->setText(cfg.extraPackages.join(u", "_s));

    if (!cfg.templateId.isEmpty())
        cloudInitCombo->setItemText(0, i18n("Default of the operating system (%1)", cfg.templateId));
    const int templateIdx = cloudInitCombo->findData(cfg.customTemplateId);
    cloudInitCombo->setCurrentIndex(templateIdx >= 0 ? templateIdx : 0);

    validate();
}

CloudDeployConfig CloudConfigDialog::getResult() const
{
    CloudDeployConfig cfg;
    cfg.hostname = hostnameEdit->text().trimmed();
    cfg.username = usernameEdit->text().trimmed();
    cfg.password = passwordEdit->text();
    cfg.timezone = timezoneCombo->currentText();

    // Locale
    const QVariant localeData = localeCombo->currentData();
    cfg.locale = localeData.isValid() ? localeData.toString() : localeCombo->currentText().trimmed();

    // Keyboard (independent from locale)
    const int kbIdx = kbLayoutCombo->currentData().toInt();
    const auto keyboards = availableKeyboards();
    if (kbIdx >= 0 && kbIdx < keyboards.size()) {
        cfg.kbLayout = keyboards[kbIdx].layout;
        cfg.kbVariant = keyboards[kbIdx].variant;
    } else {
        cfg.kbLayout = kbLayoutCombo->currentText().trimmed();
    }

    cfg.passwordLogin = passwordLoginCheck->isChecked();
    cfg.installDesktop = installDesktopCheck->isChecked();

    const QString pkgText = extraPackagesEdit->text().trimmed();
    if (!pkgText.isEmpty()) {
        const auto parts = pkgText.split(u","_s, Qt::SkipEmptyParts);
        for (const auto &p : parts)
            cfg.extraPackages.append(p.trimmed());
    }

    const QString keysText = sshKeysEdit->toPlainText().trimmed();
    if (!keysText.isEmpty())
        cfg.sshKeys = keysText.split(u"\n"_s, Qt::SkipEmptyParts);

    cfg.customTemplateId = cloudInitCombo->currentData().toString();

    return cfg;
}

void CloudConfigDialog::onAddSshKey()
{
    const QString path =
        QFileDialog::getOpenFileName(this, i18n("Select SSH Public Key"), QDir::homePath() + u"/.ssh"_s, i18n("Public Keys (*.pub);;All Files (*)"));
    if (path.isEmpty())
        return;

    QFile f(path);
    if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QString key = QString::fromUtf8(f.readAll()).trimmed();
        if (!key.isEmpty()) {
            if (!sshKeysEdit->toPlainText().isEmpty())
                sshKeysEdit->appendPlainText(u""_s);
            sshKeysEdit->appendPlainText(key);
        }
    }
}

void CloudConfigDialog::validate()
{
    const bool pwMatch = passwordEdit->text() == passwordConfirmEdit->text();
    if (!pwMatch && !passwordConfirmEdit->text().isEmpty())
        passwordMismatchLabel->setText(i18n("Passwords do not match."));
    else
        passwordMismatchLabel->clear();

    const bool hasHostname = !hostnameEdit->text().trimmed().isEmpty();
    buttonBox->button(QDialogButtonBox::Ok)->setEnabled(pwMatch && hasHostname);
}
