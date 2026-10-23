/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskkeystore.h"

#include <utility>

#include <QWidget>

#include <KWallet>

#include "kemu_debug.h"

using namespace Qt::StringLiterals;

namespace
{
const QString kFolder = u"KEMU"_s;
}

DiskKeyStore::DiskKeyStore(QWidget *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
{
}

DiskKeyStore::~DiskKeyStore() = default;

bool DiskKeyStore::isAvailable()
{
    return KWallet::Wallet::isEnabled();
}

bool DiskKeyStore::hasKey(const QString &keyId)
{
    return isAvailable() && !keyId.isEmpty() && !KWallet::Wallet::keyDoesNotExist(KWallet::Wallet::NetworkWallet(), kFolder, keyId);
}

void DiskKeyStore::readKey(const QString &keyId, std::function<void(bool found, const QString &password)> done)
{
    if (!hasKey(keyId)) {
        done(false, {});
        return;
    }

    withWallet([keyId, done = std::move(done)](KWallet::Wallet *wallet) {
        QString password;
        const bool found = wallet && wallet->readPassword(keyId, password) == 0 && !password.isEmpty();
        done(found, password);
    });
}

void DiskKeyStore::storeKey(const QString &keyId, const QString &password, std::function<void(bool ok)> done)
{
    withWallet([keyId, password, done = std::move(done)](KWallet::Wallet *wallet) {
        const bool ok = wallet && wallet->writePassword(keyId, password) == 0;
        if (!ok)
            qCWarning(KEMU) << "Could not store the disk password in KWallet";
        if (done)
            done(ok);
    });
}

void DiskKeyStore::removeKey(const QString &keyId, std::function<void(bool ok)> done)
{
    if (!hasKey(keyId)) {
        if (done)
            done(true);
        return;
    }

    withWallet([keyId, done = std::move(done)](KWallet::Wallet *wallet) {
        const bool ok = wallet && wallet->removeEntry(keyId) == 0;
        if (done)
            done(ok);
    });
}

void DiskKeyStore::withWallet(std::function<void(KWallet::Wallet *)> action)
{
    if (!isAvailable()) {
        action(nullptr);
        return;
    }

    if (m_wallet && m_wallet->isOpen()) {
        action(m_wallet.get());
        return;
    }

    m_pending.append(std::move(action));
    if (m_opening)
        return;

    m_opening = true;
    const WId windowId = m_window ? m_window->window()->winId() : 0;
    m_wallet.reset(KWallet::Wallet::openWallet(KWallet::Wallet::NetworkWallet(), windowId, KWallet::Wallet::Asynchronous));
    if (!m_wallet) {
        onWalletOpened(false);
        return;
    }

    connect(m_wallet.get(), &KWallet::Wallet::walletOpened, this, &DiskKeyStore::onWalletOpened);
    connect(m_wallet.get(), &KWallet::Wallet::walletClosed, this, [this] {
        // Locked again, e.g. by the user or a timeout: reopen on demand.
        if (!m_opening && m_wallet)
            m_wallet.release()->deleteLater();
    });
}

void DiskKeyStore::onWalletOpened(bool ok)
{
    m_opening = false;

    if (ok && m_wallet) {
        if (!m_wallet->hasFolder(kFolder))
            m_wallet->createFolder(kFolder);
        ok = m_wallet->setFolder(kFolder);
    }
    if (!ok) {
        qCWarning(KEMU) << "KWallet could not be opened";
        if (m_wallet)
            m_wallet.release()->deleteLater();
    }

    const auto pending = std::exchange(m_pending, {});
    for (const auto &action : pending)
        action(ok ? m_wallet.get() : nullptr);
}
