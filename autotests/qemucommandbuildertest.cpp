/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaEnum>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

#include "const.h"
#include "utils/externaltools.h"
#include "utils/qemucapabilities.h"
#include "utils/qemucommandbuilder.h"
#include "utils/tpmstate.h"
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
    void initTestCase()
    {
        // The builder reads kemurc (firmware directory, initial resolution);
        // keep the developer's own settings out.
        QStandardPaths::setTestModeEnabled(true);
    }

    void initialResolution()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setGpuType(VMSpecs::GpuType::Virtio);
        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s));
        const QSize size = KEMUConst::DefaultDisplaySize;
        QVERIFY(!args.filter(u",xres=%1,yres=%2,"_s.arg(size.width()).arg(size.height())).isEmpty());
    }

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
        const QStringList args =
            {u"-name"_s, u"guest=hello world"_s, u"-device"_s, u"foo;bar&baz|qux"_s, u"-option"_s, u""_s, u"-value-starting-with-hyphen"_s, u"a'b\n$(id)"_s};

        QProcess shell;
        shell.start(u"/bin/sh"_s, {u"-c"_s, u"printf '%s\\0' "_s + QemuCommandBuilder::argsToReadableString(args)});
        QVERIFY(shell.waitForFinished(5000));
        QCOMPARE(shell.exitCode(), 0);
        QCOMPARE(shell.readAllStandardOutput(), encodedArgs(args));
    }

    void vramSize_data()
    {
        QTest::addColumn<VMSpecs::GpuType>("gpu");
        QTest::addColumn<int>("requested");
        QTest::addColumn<QString>("expected");

        QTest::newRow("vga-free") << VMSpecs::GpuType::Vga << 24 << u"VGA,vgamem_mb=24"_s;
        QTest::newRow("vga-low") << VMSpecs::GpuType::Vga << 1 << u"VGA,vgamem_mb=4"_s;
        QTest::newRow("isa-vga-dos") << VMSpecs::GpuType::VgaISA << 1 << u"isa-vga,vgamem_mb=1"_s;
        QTest::newRow("isa-vga-qemu-default") << VMSpecs::GpuType::VgaISA << 8 << u"isa-vga,vgamem_mb=8"_s;
        QTest::newRow("isa-vga-high") << VMSpecs::GpuType::VgaISA << 64 << u"isa-vga,vgamem_mb=16"_s;
        QTest::newRow("qxl-low") << VMSpecs::GpuType::Qxl << 4 << u"qxl-vga,vgamem_mb=8"_s;
        QTest::newRow("qxl-high") << VMSpecs::GpuType::Qxl << 1024 << u"qxl-vga,vgamem_mb=256"_s;
        QTest::newRow("cirrus-snap-up") << VMSpecs::GpuType::CirrusGD5446 << 5 << u"cirrus-vga,vgamem_mb=8"_s;
        QTest::newRow("cirrus-isa-snap") << VMSpecs::GpuType::CirrusGD5430 << 12 << u"isa-cirrus-vga,vgamem_mb=16"_s;
        QTest::newRow("cirrus-low") << VMSpecs::GpuType::CirrusGD5446 << 1 << u"cirrus-vga,vgamem_mb=4"_s;
        QTest::newRow("virtio-low") << VMSpecs::GpuType::Virtio << 1 << u",max_hostmem=16M"_s;
        QTest::newRow("virtio-free") << VMSpecs::GpuType::Virtio << 100 << u",max_hostmem=100M"_s;
    }

    void vramSize()
    {
        QFETCH(VMSpecs::GpuType, gpu);
        QFETCH(int, requested);
        QFETCH(QString, expected);

        // What QEMU accepts, also for sizes edited into config.json by hand.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setMachineType(VMSpecs::MachineType::Pc);
        config.setGpuType(gpu);
        config.setGpu3dAcceleration(false);
        config.setGpuMemMb(requested);
        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s));
        QVERIFY2(std::any_of(args.cbegin(),
                             args.cend(),
                             [&expected](const QString &arg) {
                                 return arg.startsWith(expected) || arg.endsWith(expected);
                             }),
                 qPrintable(args.join(u' ')));
    }

    void virglBlobProbe_data()
    {
        QTest::addColumn<QString>("listing");
        QTest::addColumn<bool>("expected");

        // Excerpt of QEMU 8.2: blob and hostmem are listed, yet virgl refuses
        // blob resources and the VM does not start.
        const QString qemu82 =
            QStringList{
                u"virtio-vga-gl options:"_s,
                u"  big-endian-framebuffer=<bool>"_s,
                u"  blob=<bool>            - on/off (default: false)"_s,
                u"  hostmem=<size>         -  (default: 0)"_s,
                u"  max_hostmem=<size>     -  (default: 268435456)"_s,
                u"  stats=<bool>           - on/off (default: false)"_s,
                u"  virtio-backend=<child<virtio-gpu-gl-device>>"_s,
            }
                .join(u'\n');
        QTest::newRow("qemu-8.2") << qemu82 << false;
        QTest::newRow("qemu-9.2") << qemu82 + u"\n  venus=<bool>           - on/off (default: false)\n"_s << true;
        QTest::newRow("module-unloadable") << u"qemu-system-x86_64: -device virtio-vga-gl,help: failed to open module: "
                                              "/usr/lib/x86_64-linux-gnu/qemu/hw-display-virtio-gpu-gl.so: undefined symbol: qemu_egl_display\n"_s
                                           << false;
        QTest::newRow("device-missing") << u"qemu-system-x86_64: -device virtio-vga-gl,help: Device 'virtio-vga-gl' not found\n"_s << false;
        QTest::newRow("empty") << QString() << false;
    }

    void virglBlobProbe()
    {
        QFETCH(QString, listing);
        QFETCH(bool, expected);

        const QSet<QString> properties = QemuCapabilities::parsePropertyHelp(listing);
        QCOMPARE(QemuCapabilities::allowsVirglBlob(properties), expected);
        if (listing.startsWith(u"virtio-vga-gl options:"_s)) {
            QVERIFY(properties.contains(u"big-endian-framebuffer"_s));
            QVERIFY(properties.contains(u"virtio-backend"_s));
            QVERIFY(!properties.contains(u"virtio-vga-gl options:"_s));
        } else {
            QVERIFY(properties.isEmpty());
        }
    }

    void nativeContextProbe_data()
    {
        QTest::addColumn<QStringList>("properties");
        QTest::addColumn<bool>("expected");

        const QStringList blob{u"blob"_s, u"hostmem"_s, u"venus"_s};
        QTest::newRow("qemu-10.2") << blob << false;
        QTest::newRow("qemu-11.0") << blob + QStringList{u"drm_native_context"_s} << true;
        // Without virgl blob support, the property alone does not count.
        QTest::newRow("no-blob") << QStringList{u"hostmem"_s, u"drm_native_context"_s} << false;
        QTest::newRow("empty") << QStringList() << false;
    }

    void nativeContextProbe()
    {
        QFETCH(QStringList, properties);
        QFETCH(bool, expected);

        const QSet<QString> set(properties.cbegin(), properties.cend());
        QCOMPARE(QemuCapabilities::allowsNativeContext(set), expected);
    }

    void virglRenderServer()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const auto install = [&root](const QString &relative, bool program) {
            const QString path = root.filePath(relative);
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly))
                return QString();
            file.setPermissions(program ? QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner : QFile::ReadOwner | QFile::WriteOwner);
            return path;
        };
        const char *variable = QemuCapabilities::RenderServerEnvironment;
        qunsetenv(variable);
        const QString qemu = install(u"opt/qemu/bin/qemu-system-x86_64"_s, true);
        const QStringList system{root.filePath(u"usr"_s)};

        QCOMPARE(QemuCapabilities::findVirglRenderServer(qemu, system), QString());

        // Only programs count.
        install(u"opt/qemu/libexec/virgl_render_server"_s, false);
        QCOMPARE(QemuCapabilities::findVirglRenderServer(qemu, system), QString());

        // Arch layout below a system prefix.
        const QString arch = install(u"usr/lib/virgl_render_server"_s, true);
        QCOMPARE(QemuCapabilities::findVirglRenderServer(qemu, system), arch);

        // Next to QEMU comes first.
        const QString own = install(u"opt/qemu/libexec/virgl_render_server"_s, true);
        QCOMPARE(QemuCapabilities::findVirglRenderServer(qemu, system), own);

        // A QEMU reached through a symlink: the prefix of its target counts too.
        QDir().mkpath(root.filePath(u"sw/bin"_s));
        const QString link = root.filePath(u"sw/bin/qemu-system-x86_64"_s);
        QVERIFY(QFile::link(qemu, link));
        QCOMPARE(QemuCapabilities::findVirglRenderServer(link, {}), own);
        const QString linked = install(u"sw/libexec/virgl_render_server"_s, true);
        QCOMPARE(QemuCapabilities::findVirglRenderServer(link, {}), linked);

        // The environment replaces the search, as in virglrenderer.
        qputenv(variable, arch.toLocal8Bit());
        QCOMPARE(QemuCapabilities::findVirglRenderServer(qemu, system), arch);
        qputenv(variable, root.filePath(u"missing"_s).toLocal8Bit());
        QCOMPARE(QemuCapabilities::findVirglRenderServer(qemu, system), QString());
        qunsetenv(variable);
    }

    void virtioGpuDevice_data()
    {
        QTest::addColumn<bool>("gl");
        QTest::addColumn<bool>("blob");
        QTest::addColumn<bool>("venus");
        QTest::addColumn<bool>("nativeContext");
        QTest::addColumn<bool>("viewer");
        QTest::addColumn<int>("monitors");
        QTest::addColumn<QString>("expected");

        const QSize size = KEMUConst::DefaultDisplaySize;
        const QString base = u"xres=%1,yres=%2,max_hostmem=64M"_s.arg(size.width()).arg(size.height());
        const QString blob = u",blob=on,hostmem=4096M"_s;
        QTest::newRow("2d") << false << false << false << false << true << 1 << u"virtio-vga,"_s + base;
        QTest::newRow("2d-blob-ignored") << false << true << false << false << true << 1 << u"virtio-vga,"_s + base;
        QTest::newRow("3d") << true << false << false << false << true << 1 << u"virtio-vga-gl,"_s + base;
        QTest::newRow("3d-blob") << true << true << false << false << true << 1 << u"virtio-vga-gl,"_s + base + blob;
        QTest::newRow("3d-blob-venus") << true << true << true << false << true << 1 << u"virtio-vga-gl,"_s + base + blob + u",venus=on"_s;
        QTest::newRow("venus-needs-blob") << true << false << true << false << true << 1 << u"virtio-vga-gl,"_s + base;
        QTest::newRow("3d-blob-native") << true << true << false << true << true << 1 << u"virtio-vga-gl,"_s + base + blob + u",drm_native_context=on"_s;
        QTest::newRow("3d-blob-venus-native") << true << true << true << true << true << 1
                                              << u"virtio-vga-gl,"_s + base + blob + u",venus=on,drm_native_context=on"_s;
        QTest::newRow("native-needs-blob") << true << false << false << true << true << 1 << u"virtio-vga-gl,"_s + base;
        QTest::newRow("3d-blob-multihead") << true << true << false << false << true << 2 << u"virtio-vga-gl,"_s + base + blob + u",max_outputs=2"_s;
        QTest::newRow("script-single-head") << true << false << false << false << false << 2 << u"virtio-vga-gl,"_s + base;
    }

    void virtioGpuDevice()
    {
        QFETCH(bool, gl);
        QFETCH(bool, blob);
        QFETCH(bool, venus);
        QFETCH(bool, nativeContext);
        QFETCH(bool, viewer);
        QFETCH(int, monitors);
        QFETCH(QString, expected);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        VMConfig config = testConfig(QDir(dir.path()));
        config.setGpuType(VMSpecs::GpuType::Virtio);
        config.setGpuMemMb(64);
        config.setMonitorCount(monitors);
        config.setGpuHostmemMb(4096);

        QCOMPARE(QemuCommandBuilder::virtioGpuDevice(config, gl, blob, viewer, venus, nativeContext), expected);
    }

    void gpuHostmemIsPowerOfTwo_data()
    {
        QTest::addColumn<int>("requested");
        QTest::addColumn<int>("expected");
        QTest::newRow("default") << GlobalDefaults.gpuHostmemMb << 1024;
        QTest::newRow("exact") << 4096 << 4096;
        QTest::newRow("rounded-up") << 3000 << 4096;
        QTest::newRow("below-minimum") << 64 << KEMUConst::VirglHostmemMinMb;
        QTest::newRow("above-maximum") << 100000 << KEMUConst::VirglHostmemMaxMb;
        QTest::newRow("garbage") << -1 << KEMUConst::VirglHostmemMinMb;
    }

    void gpuHostmemIsPowerOfTwo()
    {
        QFETCH(int, requested);
        QFETCH(int, expected);

        VMConfig config;
        config.setGpuHostmemMb(requested);
        QCOMPARE(config.gpuHostmemMb(), expected);

        // Stored values pass the same check, e.g. ones edited by hand.
        QJsonObject json = config.toJson();
        json[u"gpu_hostmem"_s] = requested;
        json[u"gpu_host_blobs"_s] = true;
        json[u"gpu_vulkan"_s] = true;
        json[u"gpu_native_context"_s] = true;
        const VMConfig loaded = VMConfig::fromJson(json);
        QCOMPARE(loaded.gpuHostmemMb(), expected);
        QVERIFY(loaded.gpuHostBlobs());
        QVERIFY(loaded.gpuVulkan());
        QVERIFY(loaded.gpuNativeContext());
        QVERIFY(!VMConfig::fromJson(VMConfig().toJson()).gpuVulkan());
        QVERIFY(!VMConfig::fromJson(VMConfig().toJson()).gpuNativeContext());
    }

    void hostBlobsNeedProbedCapabilities()
    {
        // The capabilities are never probed here: a VM asking for host blobs
        // runs with plain 3D, and its guest RAM keeps QEMU's own backend.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setGpuType(VMSpecs::GpuType::Virtio);
        config.setGpu3dAcceleration(true);
        config.setGpuHostBlobs(true);
        config.setGpuNativeContext(true);
        config.setPreallocMemory(true);
        config.setUseKvm(true);

        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s));
        const QString joined = args.join(u' ');
        QVERIFY(joined.contains(u"virtio-vga-gl,"_s));
        QVERIFY(!joined.contains(u"blob="_s));
        QVERIFY(!joined.contains(u"hostmem=1024M"_s));
        QVERIFY(!joined.contains(u"drm_native_context"_s));
        QVERIFY(!joined.contains(u"memory-backend"_s));
        QVERIFY(args.contains(u"-mem-prealloc"_s));
        // Nor the memory types for them, which would move the accelerator.
        QVERIFY(!args.contains(u"-accel"_s));
        QVERIFY(joined.contains(u",accel=kvm"_s));
    }

    void enumsSurviveConfigJson()
    {
        // Every value a config.json or template can hold reads back as itself.
        const auto roundTrips = []<typename Enum>(Enum, auto parse) {
            const QMetaEnum meta = QMetaEnum::fromType<Enum>();
            for (int i = 0; i < meta.keyCount(); ++i) {
                const auto value = static_cast<Enum>(meta.value(i));
                const QString text = VMConfig::toString(value);
                if (parse(text) != value)
                    return u"%1::%2 is written as \"%3\" but not read back"_s.arg(QString::fromLatin1(meta.name()), QString::fromLatin1(meta.key(i)), text);
            }
            return QString();
        };
        QCOMPARE(roundTrips(VMSpecs::CpuModel{}, &VMConfig::parseCpuModel), QString());
        QCOMPARE(roundTrips(VMSpecs::MachineType{}, &VMConfig::parseMachineType), QString());
        QCOMPARE(roundTrips(VMSpecs::GpuType{}, &VMConfig::parseGpuType), QString());
        QCOMPARE(roundTrips(VMSpecs::AudioDevice{}, &VMConfig::parseAudioDevice), QString());
        QCOMPARE(roundTrips(VMSpecs::NetworkCard{}, &VMConfig::parseNetworkCard), QString());
        QCOMPARE(roundTrips(VMSpecs::DiskInterface{}, &VMConfig::parseDiskInterface), QString());
        QCOMPARE(roundTrips(VMSpecs::UsbController{}, &VMConfig::parseUSBController), QString());
        QCOMPARE(roundTrips(VMSpecs::PointerDevice{}, &VMConfig::parsePointerDevice), QString());
        QCOMPARE(roundTrips(VMSpecs::KeyboardDevice{}, &VMConfig::parseKeyboardDevice), QString());
        QCOMPARE(roundTrips(VMSpecs::OsFamily{}, &VMConfig::parseOsFamily), QString());
        QCOMPARE(roundTrips(VMSpecs::Architecture{}, &VMConfig::parseArchitecture), QString());

        // Templates spell the names out, in no language in particular.
        QCOMPARE(VMConfig::toString(VMSpecs::CpuModel::Haswell), u"corei4"_s);
        QCOMPARE(VMConfig::parseCpuModel(u" Pentium3 "_s), VMSpecs::CpuModel::Pentium3);
    }

    void qomPropertyProbe()
    {
        // QEMU 11.0 over "-qmp stdio", shortened.
        const QStringList qemu11{
            uR"({"QMP": {"version": {"qemu": {"micro": 1, "minor": 0, "major": 11}}, "capabilities": ["oob"]}})"_s,
            uR"({"return": {}})"_s,
            uR"({"return": [{"name": "kernel-irqchip", "type": "OnOffSplit"}, {"name": "honor-guest-pat", "type": "OnOffAuto"}]})"_s,
            uR"({"return": {}})"_s,
            uR"({"timestamp": {"seconds": 1, "microseconds": 2}, "event": "SHUTDOWN", "data": {"guest": false, "reason": "host-qmp-quit"}})"_s,
        };
        QCOMPARE(QemuCapabilities::parseQomPropertyList(qemu11.join(u'\n')), (QSet<QString>{u"kernel-irqchip"_s, u"honor-guest-pat"_s}));

        // A QEMU without KVM has no such class.
        const QStringList noKvm{
            uR"({"QMP": {"version": {"qemu": {"micro": 0, "minor": 2, "major": 8}}, "capabilities": ["oob"]}})"_s,
            uR"({"return": {}})"_s,
            uR"({"error": {"class": "DeviceNotFound", "desc": "Class 'kvm-accel' not found"}})"_s,
        };
        QVERIFY(QemuCapabilities::parseQomPropertyList(noKvm.join(u'\n')).isEmpty());
        QVERIFY(QemuCapabilities::parseQomPropertyList(QString()).isEmpty());
    }

    void venusNeedsProbedCapabilities()
    {
        // Unprobed, nothing is known about Venus: no venus=on, and the VM
        // keeps the sandbox, which Venus would have to give up.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setGpuType(VMSpecs::GpuType::Virtio);
        config.setGpu3dAcceleration(true);
        config.setGpuHostBlobs(true);
        config.setGpuVulkan(true);

        QVERIFY(!QemuCommandBuilder::venusAvailable());
        QVERIFY(!QemuCommandBuilder::usesVenus(config));
        QVERIFY(!QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s)).join(u' ').contains(u"venus="_s));
        QCOMPARE(QemuCommandBuilder::sandboxArgs(config, true), (QStringList{u"-sandbox"_s, KEMUConst::QemuSandboxOptions}));
    }

    void graphicsDevice_data()
    {
        QTest::addColumn<bool>("acceleration");
        QTest::addColumn<bool>("virtio");
        QTest::addColumn<QString>("expectedDevice");
        QTest::addColumn<QString>("expectedDisplay");

        // Headless or not: the viewer may attach later, so the command line
        // is the same.
        QTest::newRow("3d") << true << true << u"virtio-vga-gl"_s << u"dbus,p2p=on,gl=on"_s;
        QTest::newRow("2d") << false << true << u"virtio-vga"_s << u"dbus,p2p=on"_s;
        QTest::newRow("qxl") << true << false << u"qxl-vga"_s << u"dbus,p2p=on"_s;
    }

    void graphicsDevice()
    {
        QFETCH(bool, acceleration);
        QFETCH(bool, virtio);
        QFETCH(QString, expectedDevice);
        QFETCH(QString, expectedDisplay);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setGpuType(virtio ? VMSpecs::GpuType::Virtio : VMSpecs::GpuType::Qxl);
        config.setGpu3dAcceleration(acceleration);

        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s));
        const int displayIndex = args.indexOf(u"-display"_s);
        QVERIFY(displayIndex >= 0);
        QVERIFY(displayIndex + 1 < args.size());
        QCOMPARE(args.at(displayIndex + 1), expectedDisplay);
        QCOMPARE(args.count(u"-display"_s), 1);

        QStringList graphicsDevices;
        for (const QString &arg : args) {
            if (arg.startsWith(u"virtio-vga"_s) || arg.startsWith(u"qxl-vga"_s))
                graphicsDevices << arg;
        }
        QCOMPARE(graphicsDevices.size(), 1);
        QCOMPARE(graphicsDevices.constFirst().section(u',', 0, 0), expectedDevice);
    }

    void scriptOpensSdlWindow_data()
    {
        QTest::addColumn<bool>("acceleration");
        QTest::addColumn<QString>("expectedDisplay");
        QTest::newRow("3d") << true << u"sdl,gl=on"_s;
        QTest::newRow("2d") << false << u"sdl"_s;
    }

    void scriptOpensSdlWindow()
    {
        QFETCH(bool, acceleration);
        QFETCH(QString, expectedDisplay);

        // Without KEMU as the viewer's peer, the script shows QEMU's own
        // window: no D-Bus display, no kemu-agent port, a single head.
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setGpuType(VMSpecs::GpuType::Virtio);
        config.setGpu3dAcceleration(acceleration);
        config.setMonitorCount(2);
        config.setAudioDevice(VMSpecs::AudioDevice::Hda);

        QemuNetworkContext context;
        context.agentFd = 61;
        const QStringList args = QemuCommandBuilder::buildForScript(config, vmDir, dir.filePath(u"monitor.sock"_s), context, true);
        const int displayIndex = args.indexOf(u"-display"_s);
        QVERIFY(displayIndex >= 0 && displayIndex + 1 < args.size());
        QCOMPARE(args.at(displayIndex + 1), expectedDisplay);
        const QString joined = args.join(u' ');
        QVERIFY(!joined.contains(u"dbus"_s));
        QVERIFY(!joined.contains(KEMUConst::AgentPortName));
        QVERIFY(!joined.contains(u"max_outputs="_s));

        const QString script = QemuCommandBuilder::buildShellScript(config, vmDir, dir.filePath(u"monitor.sock"_s), context, {}, true);
        QVERIFY(script.contains(Utils::quoteShellArg(expectedDisplay)));
        QVERIFY(!script.contains(u"dbus,p2p"_s));
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
            QVERIFY(args.contains(u"memory-backend-memfd,id=mem,size=%1M,share=on"_s.arg(config.ramMb())));
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
        QTest::addColumn<bool>("virtio");
        QTest::addColumn<bool>("acceleration");
        QTest::addColumn<int>("outputs");
        QTest::addColumn<bool>("expectedMultihead");

        QTest::newRow("default") << true << true << 1 << false;
        QTest::newRow("3d") << true << true << 2 << true;
        QTest::newRow("2d") << true << false << 2 << true;
        QTest::newRow("qxl") << false << false << 2 << false;
        QTest::newRow("max-clamped") << true << false << 20 << true;
    }

    void multiheadArguments()
    {
        QFETCH(bool, virtio);
        QFETCH(bool, acceleration);
        QFETCH(int, outputs);
        QFETCH(bool, expectedMultihead);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setGpuType(virtio ? VMSpecs::GpuType::Virtio : VMSpecs::GpuType::Qxl);
        config.setGpu3dAcceleration(acceleration);
        config.setMonitorCount(outputs);

        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s));
        QString graphicsDevice;
        for (const QString &arg : args) {
            if (arg.startsWith(u"virtio-vga"_s) || arg.startsWith(u"qxl-vga"_s)) {
                graphicsDevice = arg;
                break;
            }
        }
        // kemu-agent reports the monitor arrangement on its own port; no
        // QEMU side agent is involved any more.
        QVERIFY(!args.join(u' ').contains(u"vdagent"_s));
        QVERIFY(!graphicsDevice.isEmpty());
        QCOMPARE(graphicsDevice.contains(u"max_outputs="_s), expectedMultihead);
        if (expectedMultihead)
            QVERIFY(graphicsDevice.contains(u"max_outputs=%1"_s.arg(qMin(outputs, KEMUConst::SupportedMonitorCount))));
    }

    void agentPort_data()
    {
        QTest::addColumn<VMSpecs::MachineType>("machine");
        QTest::addColumn<bool>("guestAgent");
        QTest::addColumn<int>("agentFd");
        QTest::addColumn<bool>("expectedPort");

        QTest::newRow("fd") << VMSpecs::MachineType::Q35 << false << 61 << true;
        QTest::newRow("no-fd") << VMSpecs::MachineType::Q35 << true << -1 << false;
        QTest::newRow("with-qga") << VMSpecs::MachineType::Pc << true << 61 << true;
        QTest::newRow("isapc") << VMSpecs::MachineType::PcISA << false << 61 << false;
    }

    void agentPort()
    {
        QFETCH(VMSpecs::MachineType, machine);
        QFETCH(bool, guestAgent);
        QFETCH(int, agentFd);
        QFETCH(bool, expectedPort);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir vmDir(dir.path());
        VMConfig config = testConfig(vmDir);
        config.setMachineType(machine);
        const bool pci = VMConfig::supportsPciBus(machine);
        config.setUseGuestAgent(guestAgent);

        QemuNetworkContext context;
        context.guestAgentFd = 54;
        context.agentFd = agentFd;
        const QStringList args = QemuCommandBuilder::build(config, vmDir, dir.filePath(u"monitor.sock"_s), context);

        const QString port = u"virtserialport,chardev=%1,id=%2,name=%3"_s.arg(KEMUConst::AgentChardevId, KEMUConst::AgentPortId, KEMUConst::AgentPortName);
        QCOMPARE(args.contains(port), expectedPort);
        QCOMPARE(args.contains(u"socket,id=%1,fd=%2"_s.arg(KEMUConst::AgentChardevId).arg(agentFd)), expectedPort);
        // One bus for both agents.
        QCOMPARE(args.count(u"virtio-serial-pci"_s), (expectedPort || (guestAgent && pci)) ? 1 : 0);
        QCOMPARE(
            args.contains(
                u"virtserialport,chardev=%1,id=%2,name=%3"_s.arg(KEMUConst::GuestAgentChardevId, KEMUConst::GuestAgentPortId, KEMUConst::GuestAgentPortName)),
            guestAgent && pci);
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
        const QByteArray tpmStub =
            "#!/bin/sh\nif [ \"$1\" = --version ]; then echo test-tool; exit 0; fi\nprintf '%s\\0' \"$@\" > \"$KEMU_TEST_TPM_ARGS\"\nif [ "
            "\"$KEMU_TEST_ENCRYPTED\" = 1 ]; then cat > \"$KEMU_TEST_TPM_SECRET\"; fi\n";
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

        QStringList expected = QemuCommandBuilder::buildForScript(config, vmDir, monitorPath, {}, relativePaths);
        if (sandboxEnabled && sandboxSupported) {
            expected.prepend(QString(KEMUConst::QemuSandboxOptions));
            expected.prepend(u"-sandbox"_s);
        }
        QCOMPARE(qemuFile.readAll(), encodedArgs(expected));
        const QString tpmSocket = relativePaths ? QString(KEMUConst::SwtpmSocketFileName) : vmDir.filePath(KEMUConst::SwtpmSocketFileName);
        const QString tpmState = relativePaths ? u"."_s : vmDir.absolutePath();
        const QString fsSocket = relativePaths ? QString(KEMUConst::VirtiofsdSocketFileName) : vmDir.filePath(KEMUConst::VirtiofsdSocketFileName);
        QStringList expectedTpm = {u"socket"_s, u"--ctrl"_s, u"type=unixio,path="_s + tpmSocket, u"--terminate"_s, u"--tpmstate"_s, u"dir="_s + tpmState};
        if (encrypted)
            expectedTpm << u"--key"_s << TpmState::keyOption(0);
        expectedTpm << u"--tpm2"_s;
        QCOMPARE(tpmFile.readAll(), encodedArgs(expectedTpm));
        QCOMPARE(fsFile.readAll(),
                 encodedArgs({u"--socket-path="_s + fsSocket, u"--shared-dir="_s + config.sharedPath(), u"--cache=auto"_s, u"--sandbox=none"_s}));
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

    // A misspelled choice in a template falls back silently, "isa-vga" to
    // the PCI "vga", so every one must be a name VMConfig reads back as is.
    void shippedTemplateChoicesAreKnown()
    {
        const QStringList keys{u"gpu_type"_s,
                               u"machine_type"_s,
                               u"cpu_model"_s,
                               u"audio_device"_s,
                               u"disk_interface"_s,
                               u"net_card"_s,
                               u"usb_controller"_s,
                               u"pointer_device"_s,
                               u"keyboard_device"_s};
        const QDir dir(QFINDTESTDATA(u"../data/templates.d"_s));
        QVERIFY(dir.exists());
        int checked = 0;
        for (const QString &file : dir.entryList({u"*.json"_s}, QDir::Files)) {
            QFile f(dir.filePath(file));
            QVERIFY(f.open(QIODevice::ReadOnly));
            for (const QJsonValue &value : QJsonDocument::fromJson(f.readAll()).object().value(u"templates"_s).toArray()) {
                const QJsonObject defaults = value.toObject().value(u"qemu_defaults"_s).toObject();
                const QJsonObject readBack = VMConfig::fromJson(defaults).toJson();
                for (const QString &key : keys) {
                    if (!defaults.contains(key))
                        continue;
                    const QString choice = defaults.value(key).toString();
                    QVERIFY2(readBack.value(key).toString() == choice, qPrintable(file + u": "_s + key + u" \""_s + choice + u"\""_s));
                    ++checked;
                }
            }
        }
        QVERIFY(checked > 10);
    }
};

QTEST_GUILESS_MAIN(QemuCommandBuilderTest)

#include "qemucommandbuildertest.moc"
