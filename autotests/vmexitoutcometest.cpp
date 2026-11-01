/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QTest>

#include "runtime/vmexitoutcome.h"
#include "models/virtualenvironmentmodel.h"

using namespace Qt::StringLiterals;

class VmExitOutcomeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void exitClassification_data()
    {
        QTest::addColumn<int>("exitCode");
        QTest::addColumn<bool>("crash");
        QTest::addColumn<bool>("deployment");
        QTest::addColumn<bool>("savestate");
        QTest::addColumn<bool>("abnormal");
        QTest::addColumn<bool>("markSaved");
        QTest::newRow("guest-shutdown") << 0 << false << false << false << false << false;
        QTest::newRow("nonzero-exit") << 3 << false << false << false << true << false;
        QTest::newRow("signal-crash") << 9 << true << false << false << true << false;
        QTest::newRow("saved-state") << 0 << false << false << true << false << true;
        QTest::newRow("failed-save") << 1 << false << false << true << true << false;
        QTest::newRow("crash-with-save") << 9 << true << false << true << true << false;
        QTest::newRow("deployment-finished") << 0 << false << true << true << false << false;
    }

    void exitClassification()
    {
        QFETCH(int, exitCode);
        QFETCH(bool, crash);
        QFETCH(bool, deployment);
        QFETCH(bool, savestate);
        QFETCH(bool, abnormal);
        QFETCH(bool, markSaved);
        const auto status = crash ? KProcess::CrashExit : KProcess::NormalExit;
        const VmExitOutcome outcome = VmExitOutcome::fromProcess(exitCode, status, deployment, savestate);
        QCOMPARE(outcome.abnormal, abnormal);
        QCOMPARE(outcome.markSaved, markSaved);
    }

    void crashedQemuProcess()
    {
        KProcess process;
        process.setProgram(u"/bin/sh"_s);
        process.setArguments({u"-c"_s, u"sleep 1; kill -KILL $$"_s});
        process.start();
        QVERIFY(process.waitForStarted(3000));
        QVERIFY(process.waitForFinished(3000) || process.state() == KProcess::NotRunning);
        QCOMPARE(process.exitStatus(), KProcess::CrashExit);
        const VmExitOutcome outcome = VmExitOutcome::fromProcess(process.exitCode(), process.exitStatus(), false, false);
        QVERIFY(outcome.abnormal);
        QVERIFY(!outcome.markSaved);
    }

    void stoppedRuntimeStateClearsResources()
    {
        VmEntry entry(u"test"_s, QDir(u"/tmp/test"_s));
        entry.state = VmEntry::State::Running;
        entry.isHeadless = true;
        entry.startedAt = QDateTime::currentDateTime();
        entry.guestAddress = u"192.0.2.10"_s;
        entry.cpuHistory.append(33.0);
        entry.attachedUsb.insert(u"1:2"_s, u"device-0"_s);
        entry.resetRuntimeState();
        QCOMPARE(entry.state, VmEntry::State::Stopped);
        QVERIFY(!entry.isHeadless);
        QVERIFY(!entry.startedAt.isValid());
        QVERIFY(entry.guestAddress.isEmpty());
        QVERIFY(entry.cpuHistory.isEmpty());
        QVERIFY(entry.attachedUsb.isEmpty());
        QVERIFY(!entry.qemuProcess);
        QVERIFY(!entry.swtpmProcess);
        QVERIFY(!entry.virtiofsdProcess);
    }

    void adoptKeepsRunningRuntimeState()
    {
        VmEntry original(u"test"_s, QDir(u"/tmp/test"_s));
        original.state = VmEntry::State::ShuttingDown;
        original.isHeadless = true;
        original.startedAt = QDateTime::currentDateTime();
        VmEntry refreshed(u"test"_s, QDir(u"/tmp/test"_s));
        refreshed.adoptRuntimeStateFrom(original);
        QCOMPARE(refreshed.state, VmEntry::State::ShuttingDown);
        QCOMPARE(refreshed.isHeadless, original.isHeadless);
        QCOMPARE(refreshed.startedAt, original.startedAt);
    }
};

QTEST_GUILESS_MAIN(VmExitOutcomeTest)

#include "vmexitoutcometest.moc"
