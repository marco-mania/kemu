/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hostshutdowndialog.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

#include <KGuiItem>
#include <KLocalizedString>
#include <KStandardGuiItem>

using namespace Qt::StringLiterals;

HostShutdownDialog::HostShutdownDialog(const QString &vmName, int seconds, QWidget *parent)
    : QDialog(parent)
    , m_remaining(seconds)
{
    setWindowTitle(i18nc("@title:window", "Shut Down Host"));
    setWindowIcon(QIcon::fromTheme(u"system-shutdown"_s));

    auto *iconLabel = new QLabel(this);
    const int iconSize = style()->pixelMetric(QStyle::PM_MessageBoxIconSize, nullptr, this);
    iconLabel->setPixmap(QIcon::fromTheme(u"system-shutdown"_s).pixmap(iconSize));
    iconLabel->setAlignment(Qt::AlignTop);

    auto *titleLabel = new QLabel(i18n("<b>'%1' has shut down.</b>", vmName.toHtmlEscaped()), this);
    titleLabel->setTextFormat(Qt::RichText);
    titleLabel->setWordWrap(true);

    m_countdownLabel = new QLabel(this);
    m_countdownLabel->setWordWrap(true);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, seconds);
    m_progress->setTextVisible(false);

    auto *buttons = new QDialogButtonBox(this);
    QPushButton *nowButton = buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
    KGuiItem::assign(nowButton, KGuiItem(i18n("Shut Down Now"), u"system-shutdown"_s));
    QPushButton *cancelButton = buttons->addButton(QDialogButtonBox::Cancel);
    KGuiItem::assign(cancelButton, KStandardGuiItem::cancel());
    cancelButton->setDefault(true);
    cancelButton->setFocus();
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *textLayout = new QVBoxLayout;
    textLayout->addWidget(titleLabel);
    textLayout->addWidget(m_countdownLabel);
    textLayout->addWidget(m_progress);

    auto *contentLayout = new QHBoxLayout;
    contentLayout->addWidget(iconLabel);
    contentLayout->addLayout(textLayout, 1);

    auto *layout = new QVBoxLayout(this);
    layout->addLayout(contentLayout);
    layout->addWidget(buttons);

    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (--m_remaining <= 0) {
            m_timer.stop();
            accept();
            return;
        }
        updateCountdown();
    });
    connect(this, &QDialog::finished, &m_timer, &QTimer::stop);

    updateCountdown();
    m_timer.start();
}

void HostShutdownDialog::updateCountdown()
{
    m_countdownLabel->setText(i18np("The host will shut down in 1 second.", "The host will shut down in %1 seconds.", m_remaining));
    m_progress->setValue(m_remaining);
}
