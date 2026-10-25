/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "trayicon.h"

#include <algorithm>

#include <KLocalizedString>
#include <KStatusNotifierItem>

#include <QAction>
#include <QCollator>
#include <QGuiApplication>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QWidget>
#include <QWindow>

#include "models/virtualenvironmentmodel.h"

using namespace Qt::StringLiterals;

namespace
{

bool isLive(VmEntry::State state)
{
    return state == VmEntry::State::Running || state == VmEntry::State::Deploying || state == VmEntry::State::ShuttingDown
        || state == VmEntry::State::Saving;
}

QString menuText(const QString &vmName)
{
    return QString(vmName).replace(u'&', u"&&"_s);
}

}

TrayIcon::TrayIcon(VirtualEnvironmentModel *model, QWidget *window, QObject *parent)
    : QObject(parent)
    , m_model(model)
    , m_item(new KStatusNotifierItem(u"kemu"_s, this))
    , m_anchor(new QAction(this))
{
    m_item->setCategory(KStatusNotifierItem::ApplicationStatus);
    m_item->setTitle(QGuiApplication::applicationDisplayName());
    m_item->setIconByName(u"org.kde.kemu"_s);
    m_item->setToolTipIconByName(u"org.kde.kemu"_s);
    m_item->setToolTipTitle(QGuiApplication::applicationDisplayName());

    // Also when KEMU starts hidden: the window needs its native handle.
    window->winId();
    m_item->setAssociatedWindow(window->windowHandle());

    // The item quits the application itself unless told otherwise; the main
    // window has to ask about running machines first.
    connect(m_item, &KStatusNotifierItem::quitRequested, this, [this] {
        m_item->abortQuit();
        Q_EMIT quitRequested();
    });

    m_anchor->setVisible(false);
    QMenu *menu = m_item->contextMenu();
    menu->addAction(m_anchor);
    connect(menu, &QMenu::aboutToShow, this, &TrayIcon::rebuildMenu);

    refresh();
}

TrayIcon::~TrayIcon() = default;

bool TrayIcon::isAvailable()
{
    return QSystemTrayIcon::isSystemTrayAvailable();
}

void TrayIcon::refresh()
{
    QStringList running;
    for (const QString &name : m_model->vmNames()) {
        const VmEntry *e = m_model->entryByName(name);
        if (e && (e->state == VmEntry::State::Running || e->state == VmEntry::State::Deploying))
            running << name;
    }
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(running.begin(), running.end(), collator);

    m_item->setStatus(running.isEmpty() ? KStatusNotifierItem::Passive : KStatusNotifierItem::Active);
    m_item->setToolTipSubTitle(running.isEmpty()
                                   ? i18n("No virtual machine is running")
                                   : i18np("Running: %2", "%1 running: %2", running.size(), running.join(i18nc("@info list separator", ", "))));
}

void TrayIcon::rebuildMenu()
{
    for (const QPointer<QObject> &entry : std::as_const(m_entries))
        delete entry.data();
    m_entries.clear();

    QMenu *menu = m_item->contextMenu();

    QStringList names = m_model->vmNames();
    QCollator collator;
    collator.setNumericMode(true);
    std::sort(names.begin(), names.end(), collator);

    // One submenu per live machine.
    QStringList stopped;
    for (const QString &name : std::as_const(names)) {
        const VmEntry *e = m_model->entryByName(name);
        if (!e)
            continue;
        if (!isLive(e->state)) {
            if (e->state == VmEntry::State::Stopped || e->state == VmEntry::State::Saved)
                stopped << name;
            continue;
        }

        auto *sub = new QMenu(menuText(name), menu);
        sub->setIcon(e->styledIcon);
        const bool controllable = e->state == VmEntry::State::Running || e->state == VmEntry::State::Deploying;

        if (e->config.displayType() == VMSpecs::DisplayType::Dbus) {
            QAction *show = sub->addAction(QIcon::fromTheme(u"video-display"_s), i18nc("@action:inmenu", "Show Display"));
            show->setEnabled(controllable);
            connect(show, &QAction::triggered, this, [this, name] {
                Q_EMIT showDisplayRequested(name);
            });
            sub->addSeparator();
        }

        QAction *shutdown = sub->addAction(QIcon::fromTheme(u"system-shutdown"_s), i18nc("@action:inmenu", "Shut Down"));
        shutdown->setEnabled(controllable);
        connect(shutdown, &QAction::triggered, this, [this, name] {
            Q_EMIT shutdownRequested(name);
        });

        QAction *powerOff = sub->addAction(QIcon::fromTheme(u"process-stop"_s), i18nc("@action:inmenu", "Power Off"));
        powerOff->setEnabled(controllable);
        connect(powerOff, &QAction::triggered, this, [this, name] {
            Q_EMIT powerOffRequested(name);
        });

        menu->insertMenu(m_anchor, sub);
        m_entries << sub;
    }

    auto *start = new QMenu(i18nc("@action:inmenu", "Start"), menu);
    start->setIcon(QIcon::fromTheme(u"media-playback-start"_s));
    for (const QString &name : std::as_const(stopped)) {
        const VmEntry *e = m_model->entryByName(name);
        QAction *action = start->addAction(e->styledIcon, menuText(name));
        connect(action, &QAction::triggered, this, [this, name] {
            Q_EMIT startRequested(name);
        });
    }
    start->setEnabled(!stopped.isEmpty());
    menu->insertMenu(m_anchor, start);
    m_entries << start;
}
