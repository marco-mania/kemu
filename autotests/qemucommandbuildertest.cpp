/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QTest>

#include "const.h"
#include "utils/externaltools.h"
#include "utils/tpmstate.h"
#include "utils/qemucommandbuilder.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

namespace
{

QByteArray encodedArgs(const QStringList &args)
{
    QByteArray result;
    for (const QString &arg : args) {
        result += arg.toUtf8();
        result += '\0';
    }
    return result;
}

bool writeExecutable(const QString &path, const QByteArray &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    if (file.write(content) != content.size())
        return false;
    file.close();
    return file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

VMConfig testConfig(const QDir &dir)
{
    VMConfig config;
    config.setName(u"test; echo PWNED"_s);
    config.setUseKvm(false);
    config.setUseUefi(false);
    config.setUseGuestAgent(false);
    config.setDisplayType(VMSpecs::DisplayType::Sdl);
    config.setAudioDevice(VMSpecs::AudioDevice::None);
    config.setNetworkCard(VMSpecs::NetworkCard::None);
    config.setDiskPath(dir.filePath(u"disk;$(touch pwned) ' &.qcow2"_s));
    config.setExtraArgs(u"-fw_cfg \"name=opt,string=hello;$(touch pwned)&|\" -S"_s);

    SmbiosSystemInfo smbios;
    smbios.product = u"a 'quoted' ; product"_s;
    config.setSmbiosSystemInfo(smbios);
    return config;
}

} // namespace

class QemuCommandBuilderTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void quoteShellArg_data()
    {
        QTest::addColumn<QString>("value");
        QTest::newRow("empty") << QString();
        QTest::newRow("plain") << u"simple-name_123"_s;
        QTest::newRow("spaces") << u"folder with spaces"_s;
        QTest::newRow("quotes") << u"a'bc\"def"_s;
        QTest::newRow("operators") << u"a;b&c|d<e>f(g)"_s;
        QTest::newRow("expansions") << u"$HOME `id` $(id) ${PATH} ~/*"_s;
        QTest::newRow("escapes") << u"back\\slash\ttab\nnewline\rreturn"_s;
        QTest::newRow("unicode") << u"Grüße 世界"_s;
        QTest::newRow("leading-hyphen") << u"-dangerous-flag"_s;
    }

    void quoteShellArg()
    {
        QFETCH(QString, value);

        QProcess shell;
        shell.start(u"/bin/sh"_s, {u"-c"_s, u"printf '%s\\0' %1"_s.arg(Utils::quoteShellArg(value))});
        QVERIFY(shell.waitForFinished(5000));
        QCOMPARE(shell.exitStatus(), QProcess::NormalExit);
        QCOMPARE(shell.exitCode(), 0);
        QCOMPARE(shell.readAllStandardOutput(), encodedArgs({value}));
    }

    void shellOperatorsCannotExecute()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString marker = dir.filePath(u"injected"_s);
        const QString value = u"value; touch %1; #"_s.arg(marker);

        QProcess shell;
        shell.start(u"/bin/sh"_s, {u"-c"_s, u"printf '%s\\0' %1"_s.arg(Utils::quoteShellArg(value))});
        QVERIFY(shell.waitForFinished(5000));
        QCOMPARE(shell.exitCode(), 0);
        QCOMPARE(shell.readAllStandardOutput(), encodedArgs({value}));
        QVERIFY(!QFile::exists(marker));
    }

    void readableArgsRoundTrip()
    {
        const QStringList args = {u"-name"_s, u"guest=hello world"_s, u"-device"_s, u"foo;bar&baz|qux"_s,
                                  u"-option"_s, u""_s, u"-value-starting-with-hyphen"_s, u"a'b\n$(id)"_s};

        QProcess shell;
        shell.start(u"/bin/sh"_s, {u"-c"_s, u"printf '%s\\0' "_s + QemuCommandBuilder::argsToReadableString(args)});
        QVERIFY(shell.waitForFinished(5000));
        QCOMPARE(shell.exitCode(), 0);
        QCOMPARE(shell.readAllStandardOutput(), encodedArgs(args));
    }

    void graphicsInHeadlessMode_data()
    {
        QTest::addColumn<bool>("dbus");
        QTest::addColumn<bool>("headless");
        QTest::addColumn<bool>("acceleration");
        QTest::addColumn<bool>("virtio");
        QTest::addColumn<QString>("expectedDevice");
        QTest::addColumn<QString>("expectedDisplay");

        QTest::newRow("dbus-windowed-3d") << true << false << true << true << u"virtio-vga-gl"_s << u"dbus,p2p=on,gl=on"_s;
        QTest::newRow("dbus-windowed-2d") << true << false << false << true << u"virtio-vga"_s << u"dbus,p2p=on"_s;
        QTest::newRow("dbus-headless-3d") << true << true << true << true << u"virtio-vga-gl"_s << u"dbus,p2p=on,gl=on"_s;
        QTest::newRow("dbus-headless-2d") << true << true << false << true << u"virtio-vga"_s << u"dbus,p2p=on"_s;
        QTest::newRow("dbus-headless-qxl") << true << true << true << false << u"qxl-vga"_s << u"dbus,p2p=on"_s;
        QTest::newRow("sdl-windowed-3d") << false << false << true << true << u"virtio-vga-gl"_s << u"sdl,gl=on"_s;
        QTest::newRow("sdl-windowed-2d") << false << false << false << true << u"virtio-vga"_s << u"sdl"_s;
        QTest::newRow("sdl-headless-3d") << false << true << true << true << u"virtio-vga"_s << u"none"_s;
        QTest::newRow("sdl-headless-2d") << false << true << false << true << u"virtio-vga"_s << u"none"_s;
        QTest::newRow("sdl-headless-qxl") << false << true << true << false << u"qxl-vga"_s << u"none"_s;
    }

    void graphicsInHeadlessMode()
    {
        QFETCH(bool, dbus);
        QFETCH(bool, headless);
        QFETCH(bool, acceleration);
        QFETCH(bool, virtio);
        QFETCH(QString, expectedDevice);
        QFETCH(QString, expectedDisplay);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setDisplayType(dbus ? VMSpecs::DisplayType::Dbus : VMSpecs::DisplayType::Sdl);
        config.setGpuType(virtio ? VMSpecs::GpuType::Virtio : VMSpecs::GpuType::Qxl);
        config.setGpu3dAcceleration(acceleration);

        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s), {}, false, headless);
        const int displayIndex = args.indexOf(u"-display"_s);
        QVERIFY(displayIndex >= 0);
        QVERIFY(displayIndex + 1 < args.size());
        QCOMPARE(args.at(displayIndex + 1), expectedDisplay);

        QStringList graphicsDevices;
        for (const QString &arg : args) {
            if (arg.startsWith(u"virtio-vga"_s) || arg.startsWith(u"qxl-vga"_s))
                graphicsDevices << arg;
        }
        QCOMPARE(graphicsDevices.size(), 1);
        QCOMPARE(graphicsDevices.constFirst().section(u',', 0, 0), expectedDevice);
    }

    void runtimeChannelsAndNetwork_data()
    {
        QTest::addColumn<bool>("virtualNetwork");
        QTest::addColumn<bool>("windowsSharedFolder");
        QTest::newRow("isolated-network-linux") << true << false;
        QTest::newRow("isolated-network-windows") << true << true;
        QTest::newRow("user-mode-linux") << false << false;
        QTest::newRow("user-mode-windows") << false << true;
    }

    void runtimeChannelsAndNetwork()
    {
        QFETCH(bool, virtualNetwork);
        QFETCH(bool, windowsSharedFolder);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setNetworkCard(VMSpecs::NetworkCard::Virtio);
        config.setNetworkMode(virtualNetwork ? VMSpecs::NetworkMode::VirtualNetwork : VMSpecs::NetworkMode::UserMode);
        config.setUseGuestAgent(true);
        config.setUseTpm(true);
        config.setSharedPath(dir.filePath(u"shared,folder"_s));
        config.setOsFamily(windowsSharedFolder ? VMSpecs::OsFamily::Windows : VMSpecs::OsFamily::Linux);

        QemuNetworkContext context;
        context.switchFd = 51;
        context.userNetFd = 52;
        context.monitorFd = 53;
        context.guestAgentFd = 54;
        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s), context);

        const QString networkArg = virtualNetwork ? u"stream,id=%1,server=off,addr.type=fd,addr.str=51"_s.arg(KEMUConst::NetdevId)
                                                   : u"stream,id=%1,server=off,addr.type=fd,addr.str=52"_s.arg(KEMUConst::NetdevId);
        QVERIFY(args.contains(networkArg));
        QVERIFY(args.contains(u"socket,id=%1,fd=53"_s.arg(KEMUConst::QmpChardevId)));
        QVERIFY(args.contains(u"socket,id=%1,fd=54"_s.arg(KEMUConst::GuestAgentChardevId)));
        QVERIFY(!args.contains(u"-qmp"_s));
        QVERIFY(args.contains(u"socket,id=chrtpm,path=%1"_s.arg(Utils::escapeQemuOptionValue(vmDir.filePath(KEMUConst::SwtpmSocketFileName)))));
        QVERIFY(args.contains(u"emulator,id=tpm0,chardev=chrtpm"_s));

        if (windowsSharedFolder) {
            QVERIFY(args.contains(u"socket,id=char_virtiofs,path=%1"_s.arg(Utils::escapeQemuOptionValue(vmDir.filePath(KEMUConst::VirtiofsdSocketFileName)))));
            QVERIFY(args.contains(u"memory-backend-file,id=mem,size=%1M,mem-path=/dev/shm,share=on"_s.arg(config.ramMb())));
            QVERIFY(args.contains(u"node,memdev=mem"_s));
            QVERIFY(!args.contains(u"-virtfs"_s));
        } else {
            QVERIFY(args.contains(u"-virtfs"_s));
            QVERIFY(!args.contains(u"socket,id=char_virtiofs,path=%1"_s.arg(vmDir.filePath(KEMUConst::VirtiofsdSocketFileName))));
        }
    }

    void scriptNetworksStayIndependent()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setNetworkCard(VMSpecs::NetworkCard::Virtio);
        config.setNetworkMode(VMSpecs::NetworkMode::VirtualNetwork);
        VirtualNetwork first;
        first.name = u"isolated-a"_s;
        VirtualNetwork second;
        second.name = u"isolated-b"_s;

        const QString a = QemuCommandBuilder::buildShellScript(config, vmDir, dir.filePath(u"monitor.sock"_s), {}, first, true);
        const QString b = QemuCommandBuilder::buildShellScript(config, vmDir, dir.filePath(u"monitor.sock"_s), {}, second, true);
        QVERIFY(a.contains(u"localaddr=127.0.0.1"_s));
        QVERIFY(b.contains(u"localaddr=127.0.0.1"_s));
        QVERIFY(a != b);
    }

    void multiheadArguments_data()
    {
        QTest::addColumn<bool>("dbus");
        QTest::addColumn<bool>("virtio");
        QTest::addColumn<bool>("acceleration");
        QTest::addColumn<bool>("headless");
        QTest::addColumn<int>("outputs");
        QTest::addColumn<bool>("expectedMultihead");

        QTest::newRow("default") << true << true << true << false << 1 << false;
        QTest::newRow("dbus-3d") << true << true << true << false << 2 << true;
        QTest::newRow("dbus-3d-headless") << true << true << true << true << 2 << true;
        QTest::newRow("dbus-2d") << true << true << false << false << 2 << true;
        QTest::newRow("sdl") << false << true << true << false << 2 << false;
        QTest::newRow("qxl") << true << false << false << false << 2 << false;
        QTest::newRow("max-clamped") << true << true << false << false << 20 << true;
    }

    void multiheadArguments()
    {
        QFETCH(bool, dbus);
        QFETCH(bool, virtio);
        QFETCH(bool, acceleration);
        QFETCH(bool, headless);
        QFETCH(int, outputs);
        QFETCH(bool, expectedMultihead);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setDisplayType(dbus ? VMSpecs::DisplayType::Dbus : VMSpecs::DisplayType::Sdl);
        config.setGpuType(virtio ? VMSpecs::GpuType::Virtio : VMSpecs::GpuType::Qxl);
        config.setGpu3dAcceleration(acceleration);
        config.setMonitorCount(outputs);

        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s), {}, false, headless);
        QString graphicsDevice;
        for (const QString &arg : args) {
            if (arg.startsWith(u"virtio-vga"_s) || arg.startsWith(u"qxl-vga"_s)) {
                graphicsDevice = arg;
                break;
            }
        }
        // Only the guest agent knows how the guest arranged the monitors.
        QCOMPARE(args.contains(u"qemu-vdagent,id=vdagent,clipboard=on,mouse=on"_s), expectedMultihead);
        QVERIFY(!graphicsDevice.isEmpty());
        QCOMPARE(graphicsDevice.contains(u"max_outputs="_s), expectedMultihead);
        if (expectedMultihead)
            QVERIFY(graphicsDevice.contains(u"max_outputs=%1"_s.arg(qMin(outputs, KEMUConst::SupportedMonitorCount))));
    }

    void exportedScript_data()
    {
        QTest::addColumn<bool>("relativePaths");
        QTest::addColumn<bool>("encrypted");
        QTest::addColumn<bool>("sandboxEnabled");
        QTest::addColumn<bool>("sandboxSupported");
        QTest::newRow("relative") << true << false << false << false;
        QTest::newRow("relative-sandbox-supported") << true << false << true << true;
        QTest::newRow("relative-sandbox-unsupported") << true << false << true << false;
        QTest::newRow("relative-encrypted") << true << true << false << false;
        QTest::newRow("absolute") << false << false << false << false;
        QTest::newRow("absolute-sandbox") << false << false << true << true;
        QTest::newRow("absolute-encrypted") << false << true << false << false;
    }

    void exportedScript()
    {
        QFETCH(bool, relativePaths);
        QFETCH(bool, encrypted);
        QFETCH(bool, sandboxEnabled);
        QFETCH(bool, sandboxSupported);

        QTemporaryDir dir(u"/tmp/kemu shell;'&-XXXXXX"_s);
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        const QString qemuArgsPath = dir.filePath(u"qemu-args"_s);
        const QString tpmArgsPath = dir.filePath(u"tpm-args"_s);
        const QString fsArgsPath = dir.filePath(u"fs-args"_s);
        const QString scriptPath = dir.filePath(u"qemu.sh"_s);
        const QString qemuSecretPath = dir.filePath(u"qemu-secret"_s);
        const QString tpmSecretPath = dir.filePath(u"tpm-secret"_s);
        const QString password = u"secret; ' $(touch pwned) &\nline"_s;

        const QByteArray qemuStub =
            "#!/bin/sh\n"
            "if [ \"$1\" = --version ]; then echo test-tool; exit 0; fi\n"
            "if [ \"$1\" = -sandbox ] && [ \"$3\" = -version ]; then\n"
            "  [ \"$KEMU_TEST_SANDBOX\" = 1 ]\n"
            "  exit $?\n"
            "fi\n"
            "printf '%s\\0' \"$@\" > \"$KEMU_TEST_QEMU_ARGS\"\n"
            "if [ \"$KEMU_TEST_ENCRYPTED\" = 1 ]; then cat <&3 > \"$KEMU_TEST_QEMU_SECRET\"; fi\n";
        const QByteArray tpmStub = "#!/bin/sh\nif [ \"$1\" = --version ]; then echo test-tool; exit 0; fi\nprintf '%s\\0' \"$@\" > \"$KEMU_TEST_TPM_ARGS\"\nif [ \"$KEMU_TEST_ENCRYPTED\" = 1 ]; then cat > \"$KEMU_TEST_TPM_SECRET\"; fi\n";
        const QByteArray fsStub = "#!/bin/sh\nif [ \"$1\" = --version ]; then echo test-tool; exit 0; fi\nprintf '%s\\0' \"$@\" > \"$KEMU_TEST_FS_ARGS\"\n";
        QVERIFY(writeExecutable(dir.filePath(u"qemu-system-x86_64"_s), qemuStub));
        QVERIFY(writeExecutable(dir.filePath(u"swtpm"_s), tpmStub));
        QVERIFY(writeExecutable(dir.filePath(u"virtiofsd"_s), fsStub));

        if (!relativePaths) {
            const QByteArray originalPath = qgetenv("PATH");
            QByteArray path = dir.path().toUtf8();
            path += ':';
            path += originalPath;
            qputenv("PATH", path);
            ExternalTools::instance().refresh();
            qputenv("PATH", originalPath);
            QCOMPARE(ExternalTools::instance().path(ExternalTools::QemuSystem), dir.filePath(u"qemu-system-x86_64"_s));
            QCOMPARE(ExternalTools::instance().path(ExternalTools::Swtpm), dir.filePath(u"swtpm"_s));
            QCOMPARE(ExternalTools::instance().path(ExternalTools::Virtiofsd), dir.filePath(u"virtiofsd"_s));
        }

        VMConfig config = testConfig(vmDir);
        config.setOsFamily(VMSpecs::OsFamily::Windows);
        config.setUseTpm(true);
        config.setSharedPath(dir.filePath(u"shared folder;\n$(touch pwned) ' &"_s));
        if (encrypted)
            config.setDiskKeyId(u"test-encrypted-disk"_s);
        const QString monitorPath = dir.filePath(u"monitor.sock"_s);
        const QString script = QemuCommandBuilder::buildShellScript(config, vmDir, monitorPath, {}, {}, relativePaths, sandboxEnabled);
        QVERIFY(writeExecutable(scriptPath, script.toUtf8()));

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(u"PATH"_s, dir.path() + u":" + env.value(u"PATH"_s));
        env.insert(u"KEMU_TEST_QEMU_ARGS"_s, qemuArgsPath);
        env.insert(u"KEMU_TEST_TPM_ARGS"_s, tpmArgsPath);
        env.insert(u"KEMU_TEST_FS_ARGS"_s, fsArgsPath);
        env.insert(u"KEMU_TEST_SANDBOX"_s, sandboxSupported ? u"1"_s : u"0"_s);
        env.insert(u"KEMU_TEST_ENCRYPTED"_s, encrypted ? u"1"_s : u"0"_s);
        env.insert(u"KEMU_TEST_QEMU_SECRET"_s, qemuSecretPath);
        env.insert(u"KEMU_TEST_TPM_SECRET"_s, tpmSecretPath);
        if (encrypted)
            env.insert(u"KEMU_DISK_PASSWORD"_s, password);

        QProcess shell;
        shell.setProcessEnvironment(env);
        if (!relativePaths)
            shell.setWorkingDirectory(dir.path());
        shell.start(u"/bin/sh"_s, {scriptPath});
        QVERIFY2(shell.waitForFinished(15000), qPrintable(shell.errorString()));
        QCOMPARE(shell.exitStatus(), QProcess::NormalExit);
        QCOMPARE(shell.exitCode(), 0);

        QFile qemuFile(qemuArgsPath);
        QFile tpmFile(tpmArgsPath);
        QFile fsFile(fsArgsPath);
        QVERIFY(qemuFile.open(QIODevice::ReadOnly));
        QVERIFY(tpmFile.open(QIODevice::ReadOnly));
        QVERIFY(fsFile.open(QIODevice::ReadOnly));

        QStringList expected = QemuCommandBuilder::build(config, vmDir, monitorPath, {}, relativePaths);
        if (sandboxEnabled && sandboxSupported) {
            expected.prepend(QString(KEMUConst::QemuSandboxOptions));
            expected.prepend(u"-sandbox"_s);
        }
        QCOMPARE(qemuFile.readAll(), encodedArgs(expected));
        const QString tpmSocket = relativePaths ? QString(KEMUConst::SwtpmSocketFileName) : vmDir.filePath(KEMUConst::SwtpmSocketFileName);
        const QString tpmState = relativePaths ? u"."_s : vmDir.absolutePath();
        const QString fsSocket = relativePaths ? QString(KEMUConst::VirtiofsdSocketFileName) : vmDir.filePath(KEMUConst::VirtiofsdSocketFileName);
        QStringList expectedTpm = {u"socket"_s, u"--ctrl"_s, u"type=unixio,path="_s + tpmSocket,
                                   u"--terminate"_s, u"--tpmstate"_s, u"dir="_s + tpmState};
        if (encrypted)
            expectedTpm << u"--key"_s << TpmState::keyOption(0);
        expectedTpm << u"--tpm2"_s;
        QCOMPARE(tpmFile.readAll(), encodedArgs(expectedTpm));
        QCOMPARE(fsFile.readAll(), encodedArgs({u"--socket-path="_s + fsSocket,
                                               u"--shared-dir="_s + config.sharedPath(), u"--cache=auto"_s, u"--sandbox=none"_s}));
        QVERIFY(!QFile::exists(dir.filePath(u"pwned"_s)));
        if (encrypted) {
            QFile qemuSecret(qemuSecretPath);
            QFile tpmSecret(tpmSecretPath);
            QVERIFY(qemuSecret.open(QIODevice::ReadOnly));
            QVERIFY(tpmSecret.open(QIODevice::ReadOnly));
            QCOMPARE(qemuSecret.readAll(), password.toUtf8());
            QCOMPARE(tpmSecret.readAll(), password.toUtf8());
        }
    }

    void monitorCountConfiguration()
    {
        VMConfig config;
        QCOMPARE(config.monitorCount(), 1);

        QJsonObject legacy = config.toJson();
        legacy.remove(u"monitor_count"_s);
        QCOMPARE(VMConfig::fromJson(legacy).monitorCount(), 1);

        config.setMonitorCount(2);
        QCOMPARE(config.toJson().value(u"monitor_count"_s).toInt(), 2);
        QCOMPARE(VMConfig::fromJson(config.toJson()).monitorCount(), 2);

        config.setMonitorCount(0);
        QCOMPARE(config.monitorCount(), 1);
        config.setMonitorCount(42);
        QCOMPARE(config.monitorCount(), KEMUConst::MaxMonitorCount);
        QJsonObject invalid = config.toJson();
        invalid[u"monitor_count"_s] = -1;
        QCOMPARE(VMConfig::fromJson(invalid).monitorCount(), 1);
        invalid[u"monitor_count"_s] = 99;
        QCOMPARE(VMConfig::fromJson(invalid).monitorCount(), KEMUConst::MaxMonitorCount);
    }
};

QTEST_GUILESS_MAIN(QemuCommandBuilderTest)

#include "qemucommandbuildertest.moc"
