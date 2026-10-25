/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "deploymentabortdialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

DeploymentAbortDialog::DeploymentAbortDialog(const QString &vmName, bool canShutdown, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(i18nc("@title:window", "Abort Deployment"));

    auto *iconLabel = new QLabel(this);
    const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, this);
    iconLabel->setPixmap(QIcon::fromTheme(u"dialog-warning"_s).pixmap(iconSize));
    iconLabel->setAlignment(Qt::AlignTop);

    auto *textLabel = new QLabel(i18n("<p>VM '%1' is currently being deployed. Aborting stops the installation.</p>"
                                      "<p><b>Shut Down</b> asks the guest to power off and lets it close its file "
                                      "systems cleanly.<br/><b>Power Off</b> is equivalent to pulling the plug and "
                                      "may leave the disk in an inconsistent state.</p>",
                                      vmName.toHtmlEscaped()),
                                 this);
    textLabel->setTextFormat(Qt::RichText);
    textLabel->setWordWrap(true);

    auto *buttons = new QDialogButtonBox(this);
    const auto addChoice = [this, buttons](const QString &text, const QString &iconName, const QString &toolTip, Choice choice) {
        QPushButton *button = buttons->addButton(text, QDialogButtonBox::AcceptRole);
        button->setIcon(QIcon::fromTheme(iconName));
        button->setToolTip(toolTip);
        connect(button, &QPushButton::clicked, this, [this, choice] {
            m_choice = choice;
            accept();
        });
        return button;
    };

    QPushButton *shutdownButton = addChoice(i18n("Shut Down && Keep"),
                                            u"system-shutdown"_s,
                                            i18n("Send an ACPI power-off signal to the guest and keep the VM"),
                                            Choice::ShutdownAndKeep);
    addChoice(i18n("Power Off && Keep"), u"media-playback-stop"_s, i18n("Stop the VM immediately and keep it"), Choice::PowerOffAndKeep);
    addChoice(i18n("Power Off && Delete"), u"edit-delete"_s, i18n("Stop the VM immediately and delete it"), Choice::PowerOffAndDelete);
    QPushButton *cancelButton = buttons->addButton(QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    if (canShutdown) {
        shutdownButton->setDefault(true);
    } else {
        shutdownButton->setEnabled(false);
        shutdownButton->setToolTip(i18n("Not available: the QEMU monitor is not connected"));
        cancelButton->setDefault(true);
    }

    auto *contentLayout = new QHBoxLayout;
    contentLayout->addWidget(iconLabel);
    contentLayout->addWidget(textLabel, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(contentLayout);
    layout->addWidget(buttons);
}

DeploymentAbortDialog::Choice DeploymentAbortDialog::choice() const
{
    return m_choice;
}

DeploymentAbortDialog::Choice DeploymentAbortDialog::ask(const QString &vmName, bool canShutdown, QWidget *parent)
{
    const QPointer<DeploymentAbortDialog> dialog = new DeploymentAbortDialog(vmName, canShutdown, parent);
    dialog->exec();
    if (!dialog)
        return Choice::Cancel;

    const Choice result = dialog->choice();
    delete dialog;
    return result;
}
