/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "licenseagreementdialog.h"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QTabWidget>
#include <QVBoxLayout>

#include <KLocalizedString>

LicenseAgreementDialog::LicenseAgreementDialog(const QString &applianceName, const QStringList &licenses, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "License Agreement"));

    auto *textLabel =
        new QLabel(i18n("<p>The appliance '%1' comes with license terms. Read and accept them to import it.</p>", applianceName.toHtmlEscaped()), this);
    textLabel->setTextFormat(Qt::RichText);
    textLabel->setWordWrap(true);

    const auto makeView = [this](const QString &text) {
        auto *view = new QPlainTextEdit(this);
        view->setReadOnly(true);
        view->setPlainText(text);
        return view;
    };

    QWidget *content = nullptr;
    if (licenses.size() == 1) {
        content = makeView(licenses.first());
    } else {
        auto *tabs = new QTabWidget(this);
        for (qsizetype i = 0; i < licenses.size(); ++i)
            tabs->addTab(makeView(licenses.at(i)), i18n("License %1", i + 1));
        content = tabs;
    }

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *acceptButton = buttons->addButton(i18n("Accept"), QDialogButtonBox::AcceptRole);
    QPushButton *declineButton = buttons->addButton(i18n("Decline"), QDialogButtonBox::RejectRole);
    acceptButton->setAutoDefault(false);
    declineButton->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(textLabel);
    layout->addWidget(content, 1);
    layout->addWidget(buttons);

    resize(640, 480);
}

bool LicenseAgreementDialog::ask(const QString &applianceName, const QStringList &licenses, QWidget *parent)
{
    const QPointer<LicenseAgreementDialog> dialog = new LicenseAgreementDialog(applianceName, licenses, parent);
    const bool accepted = dialog->exec() == QDialog::Accepted;
    delete dialog;
    return accepted;
}
