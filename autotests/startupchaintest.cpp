/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QPointer>
#include <QTest>
#include <QTimer>

#include <memory>

#include "utils/startupchain.h"

using namespace Qt::StringLiterals;

class StartupChainTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void emptyChain()
    {
        QObject owner;
        int completed = 0;
        auto *chain = new StartupChain(&owner);
        QPointer<StartupChain> guard(chain);
        chain->run([&](bool ok) {
            QVERIFY(ok);
            ++completed;
        });
        QCOMPARE(completed, 1);
        QTRY_VERIFY(guard.isNull());
    }

    void asynchronousSteps()
    {
        QObject owner;
        QStringList order;
        int completed = 0;
        bool succeeded = false;
        auto *chain = new StartupChain(&owner);
        chain->then([&](auto done) {
            order << u"swtpm"_s;
            QTimer::singleShot(0, &owner, [done] { done(true); });
        });
        chain->then([&](auto done) {
            order << u"virtiofsd"_s;
            QTimer::singleShot(0, &owner, [done] { done(true); });
        });
        chain->then([&](auto done) {
            order << u"qemu"_s;
            done(true);
        });
        chain->run([&](bool ok) {
            succeeded = ok;
            ++completed;
        });
        QTRY_COMPARE(completed, 1);
        QCOMPARE(order, (QStringList{u"swtpm"_s, u"virtiofsd"_s, u"qemu"_s}));
        QVERIFY(succeeded);
    }

    void failedHelperSkipsQemu()
    {
        QObject owner;
        QStringList order;
        int completed = 0;
        bool succeeded = true;
        auto *chain = new StartupChain(&owner);
        chain->then([&](auto done) {
            order << u"swtpm"_s;
            done(true);
        });
        chain->then([&](auto done) {
            order << u"virtiofsd"_s;
            QTimer::singleShot(0, &owner, [done] { done(false); });
        });
        chain->then([&](auto done) {
            order << u"qemu"_s;
            done(true);
        });
        chain->run([&](bool ok) {
            succeeded = ok;
            ++completed;
        });
        QTRY_COMPARE(completed, 1);
        QVERIFY(!succeeded);
        QCOMPARE(order, (QStringList{u"swtpm"_s, u"virtiofsd"_s}));
    }

    void destroyedOwnerCancelsPendingChain()
    {
        auto owner = std::make_unique<QObject>();
        std::function<void(bool)> continuation;
        int completed = 0;
        auto *chain = new StartupChain(owner.get());
        chain->then([&](auto done) { continuation = std::move(done); });
        chain->run([&](bool) { ++completed; });
        QVERIFY(continuation);
        owner.reset();
        QCOMPARE(completed, 0);
    }
};

QTEST_GUILESS_MAIN(StartupChainTest)

#include "startupchaintest.moc"
