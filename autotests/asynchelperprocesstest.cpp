/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QLocalServer>
#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include "utils/asynchelperprocess.h"

using namespace Qt::StringLiterals;

class AsyncHelperProcessTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void helperReady_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("swtpm") << u"swtpm.sock"_s;
        QTest::newRow("virtiofsd") << u"virtiofsd.sock"_s;
    }

    void helperReady()
    {
        QFETCH(QString, name);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString socketPath = dir.filePath(name);
        QObject owner;
        QLocalServer server;

        AsyncHelperProcess::Options opts;
        opts.program = u"/bin/sh"_s;
        opts.arguments = {u"-c"_s, u"exec sleep 10"_s};
        opts.socketPath = socketPath;
        opts.socketTimeoutMs = 2000;
        opts.socketPollIntervalMs = 10;
        opts.setParentDeathSignal = false;

        int calls = 0;
        bool succeeded = false;
        QPointer<KProcess> process;
        AsyncHelperProcess::run(&owner, opts, [&](KProcess *p, bool ok, const QString &error) {
            ++calls;
            succeeded = ok;
            process = p;
            if (!ok)
                qWarning() << error;
        });
        QTimer::singleShot(50, &owner, [&] { QVERIFY(server.listen(socketPath)); });

        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 3000);
        QVERIFY(succeeded);
        QVERIFY(process);
        QCOMPARE(process->parent(), &owner);
        QCOMPARE(process->state(), KProcess::Running);
        process->kill();
        QVERIFY(process->waitForFinished(3000) || process->state() == KProcess::NotRunning);
        QTest::qWait(30);
        QCOMPARE(calls, 1);
    }

    void helperExitsAfterReady_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("swtpm") << u"swtpm.sock"_s;
        QTest::newRow("virtiofsd") << u"virtiofsd.sock"_s;
    }

    void helperExitsAfterReady()
    {
        QFETCH(QString, name);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString socketPath = dir.filePath(name);
        QLocalServer server;
        QVERIFY(server.listen(socketPath));
        QObject owner;
        AsyncHelperProcess::Options opts;
        opts.program = u"/bin/sh"_s;
        opts.arguments = {u"-c"_s, u"sleep 2; exit 31"_s};
        opts.socketPath = socketPath;
        opts.setParentDeathSignal = false;

        QPointer<KProcess> process;
        int calls = 0;
        AsyncHelperProcess::run(&owner, opts, [&](KProcess *p, bool ok, const QString &) {
            ++calls;
            if (ok)
                process = p;
        });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 2000);
        QVERIFY(process);
        QSignalSpy exited(process, QOverload<int, KProcess::ExitStatus>::of(&KProcess::finished));
        QTRY_COMPARE_WITH_TIMEOUT(exited.size(), 1, 5500);
        QCOMPARE(process->exitCode(), 31);
        QCOMPARE(process->exitStatus(), KProcess::NormalExit);
        QCOMPARE(calls, 1);
    }

    void helperExitsBeforeSocket_data()
    {
        QTest::addColumn<QString>("name");
        QTest::newRow("swtpm") << u"swtpm.sock"_s;
        QTest::newRow("virtiofsd") << u"virtiofsd.sock"_s;
    }

    void helperExitsBeforeSocket()
    {
        QFETCH(QString, name);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QObject owner;
        AsyncHelperProcess::Options opts;
        opts.program = u"/bin/sh"_s;
        opts.arguments = {u"-c"_s, u"exit 17"_s};
        opts.socketPath = dir.filePath(name);
        opts.socketTimeoutMs = 1000;
        opts.setParentDeathSignal = false;

        int calls = 0;
        bool succeeded = true;
        QString reason;
        AsyncHelperProcess::run(&owner, opts, [&](KProcess *p, bool ok, const QString &error) {
            ++calls;
            succeeded = ok;
            reason = error;
            QVERIFY(!p);
        });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 2000);
        QVERIFY(!succeeded);
        QVERIFY(!reason.isEmpty());
    }

    void missingExecutable()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QObject owner;
        AsyncHelperProcess::Options opts;
        opts.program = dir.filePath(u"nonexistent-helper"_s);
        opts.setParentDeathSignal = false;
        int calls = 0;
        AsyncHelperProcess::run(&owner, opts, [&](KProcess *p, bool ok, const QString &reason) {
            ++calls;
            QVERIFY(!ok);
            QVERIFY(!p);
            QVERIFY(!reason.isEmpty());
        });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 2000);
    }

    void socketTimeout()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QObject owner;
        AsyncHelperProcess::Options opts;
        opts.program = u"/bin/sh"_s;
        opts.arguments = {u"-c"_s, u"exec sleep 10"_s};
        opts.socketPath = dir.filePath(u"absent.sock"_s);
        opts.socketTimeoutMs = 150;
        opts.socketPollIntervalMs = 10;
        opts.setParentDeathSignal = false;
        int calls = 0;
        AsyncHelperProcess::run(&owner, opts, [&](KProcess *p, bool ok, const QString &reason) {
            ++calls;
            QVERIFY(!ok);
            QVERIFY(!p);
            QVERIFY(!reason.isEmpty());
        });
        QTRY_COMPARE_WITH_TIMEOUT(calls, 1, 3000);
        QTest::qWait(30);
        QCOMPARE(calls, 1);
    }
};

QTEST_GUILESS_MAIN(AsyncHelperProcessTest)

#include "asynchelperprocesstest.moc"
