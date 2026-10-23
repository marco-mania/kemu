/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <memory>

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

class QWidget;

namespace KWallet
{
class Wallet;
}

/**
 * @brief Keeps the passwords of encrypted VMs in KWallet.
 *
 * Entries live in the folder "KEMU" of the network wallet, keyed by the
 * VM's disk key id, so renaming a VM does not lose them. Everything is
 * asynchronous: the wallet may ask the user to unlock it first. Without
 * KWallet every request completes as "not found".
 */
class DiskKeyStore : public QObject
{
    Q_OBJECT

public:
    explicit DiskKeyStore(QWidget *window, QObject *parent = nullptr);
    ~DiskKeyStore() override;

    [[nodiscard]] static bool isAvailable();

    // Checks without opening the wallet, so it never prompts.
    [[nodiscard]] static bool hasKey(const QString &keyId);

    void readKey(const QString &keyId, std::function<void(bool found, const QString &password)> done);
    void storeKey(const QString &keyId, const QString &password, std::function<void(bool ok)> done = {});
    void removeKey(const QString &keyId, std::function<void(bool ok)> done = {});

private:
    // Runs @p action with the open wallet, or with nullptr if there is none.
    void withWallet(std::function<void(KWallet::Wallet *)> action);
    void onWalletOpened(bool ok);

    QPointer<QWidget> m_window;
    std::unique_ptr<KWallet::Wallet> m_wallet;
    bool m_opening = false;
    QList<std::function<void(KWallet::Wallet *)>> m_pending;
};
