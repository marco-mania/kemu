/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "qemucommandbuilder.h"

#include "const.h"
#include "utils/externaltools.h"
#include "utils/qemucapabilities.h"
#include "utils/sandboxcapabilities.h"
#include "utils/utils.h"

#include "kemu_debug.h"

#include <QCryptographicHash>

using namespace Qt::StringLiterals;

namespace
{

/// Stable multicast group+port per virtual network for exported scripts.
QString mcastAddressFor(const QString &networkName)
{
    const QByteArray hash = QCryptographicHash::hash(networkName.toUtf8(), QCryptographicHash::Md5);
    const auto hi = static_cast<quint16>(static_cast<uchar>(hash.at(0)));
    const auto lo = static_cast<quint16>(static_cast<uchar>(hash.at(1)));
    const quint16 port = 1025 + static_cast<quint16>(((hi << 8) | lo) % 64000);
    return u"230.0.0.1:%1"_s.arg(port);
}

/// Value of a path option: the bare file name for portable scripts, with
/// commas doubled so QEMU does not split the value.
QString optPath(const QString &path, bool relative)
{
    return Utils::escapeQemuOptionValue(relative ? QFileInfo(path).fileName() : path);
}

} // namespace

QStringList QemuCommandBuilder::build(const VMConfig &config,
                                      const QDir &vmDir,
                                      const QString &monitorSocketPath,
                                      const QemuNetworkContext &networkContext,
                                      const bool relativePaths,
                                      const bool headless,
                                      const QString &incomingSavestatePath)
{
    QStringList args;

    // guest= does not protect against commas in the name, they still have to
    // be escaped.
    args << u"-name"_s << u"guest=%1"_s.arg(Utils::escapeQemuOptionValue(config.name()));

    // --- Chipset ---
    const QString accel = config.useKvm() ? u"kvm"_s : u"tcg"_s;

    if (config.machineType() == VMSpecs::MachineType::Q35) {
        QString machineTypeParam = u"type=q35,accel=%1,vmport=off"_s.arg(accel);
        machineTypeParam += config.useSecureBoot() ? u",smm=on"_s : u",smm=off"_s;
        if (config.osType() == VMSpecs::OsType::Windows)
            machineTypeParam += u",hpet=off"_s;
        if (!config.useAcpi())
            machineTypeParam += u",acpi=off"_s;
        args << u"-machine"_s << machineTypeParam;
    } else if (config.machineType() == VMSpecs::MachineType::PcISA) {
        args << u"-machine"_s << u"type=isapc,accel=%1"_s.arg(accel);
    } else {
        QString machineTypeParam = u"type=pc,accel=%1"_s.arg(accel);
        if (!config.useAcpi())
            machineTypeParam += u",acpi=off"_s;
        args << u"-machine"_s << machineTypeParam;
    }

    // --- CPU ---
    QString cpuArg = VMConfig::toQemuString(config.cpuModel());
    if (config.useKvm() && config.cpuModel() == VMSpecs::CpuModel::Host) {
        if (config.osType() == VMSpecs::OsType::Windows) {
            cpuArg += u",l3-cache=on,+hypervisor,+invtsc,migratable=no,hv_passthrough"_s;
        } else {
            cpuArg += u",l3-cache=on"_s;
        }
    }
    args << u"-cpu"_s << cpuArg;

    // -- SMP Topology --
    // Always 1 socket; 2 threads/core for even vCPU counts.
    const int sockets = 1;
    int cores = 1;
    int threads = 1;
    const int totalCpuCores = config.cpuCoreCount();

    if (totalCpuCores == 1) {
        cores = 1;
        threads = 1;
    } else if (totalCpuCores % 2 == 0) {
        threads = 2;
        cores = totalCpuCores / 2;
    } else {
        // Odd vCPU count - SMT cannot be distributed evenly, fall back to 1 thread
        // per core.
        threads = 1;
        cores = totalCpuCores;
    }

    // Pass the total number of CPUs (%1) as the first argument!
    args << u"-smp"_s << u"cpus=%1,cores=%2,threads=%3,sockets=%4"_s.arg(totalCpuCores).arg(cores).arg(threads).arg(sockets);

    if (config.useKvm())
        args << u"-global"_s << u"kvm-pit.lost_tick_policy=discard"_s;

    if (config.osType() == VMSpecs::OsType::Windows && config.machineType() == VMSpecs::MachineType::Q35) {
        args << u"-global"_s << u"ICH9-LPC.disable_s3=1"_s;
        args << u"-global"_s << u"ICH9-LPC.disable_s4=1"_s;
    }

    // --- RAM ---
    args << u"-m"_s << u"%1M"_s.arg(config.ramMb());
    if (config.useMemoryBallooning() && VMConfig::supportsPciBus(config.machineType())) {
        args << u"-device"_s << u"virtio-balloon-pci"_s;
    }

    // virtio-fs (Windows shared folder) requires an explicit memory-backend
    // object; in that case prealloc is set on the object itself below,
    // not via the legacy -mem-prealloc flag.
    const bool usesVirtiofs = !config.sharedPath().isEmpty() && config.osType() == VMSpecs::OsType::Windows;
    if (config.preallocMemory() && !usesVirtiofs)
        args << u"-mem-prealloc"_s;

    // --- RTC ---
    QString rtcParam = config.useLocalTime() ? u"base=localtime,clock=host"_s : u"base=utc,clock=host"_s;
    if (config.osType() == VMSpecs::OsType::Windows)
        rtcParam += u",driftfix=slew"_s;
    args << u"-rtc"_s << rtcParam;

    // --- UEFI / OVMF ---
    if (config.useUefi()) {
        const Utils::OvmfPaths ovmf = Utils::findOvmfPaths(config.useSecureBoot());
        const QString localVarsPath = vmDir.filePath(u"OVMF_VARS.fd"_s);

        if (!ovmf.codePath.isEmpty() && QFile::exists(localVarsPath)) {
            if (config.useSecureBoot())
                args << u"-global"_s << u"driver=cfi.pflash01,property=secure,value=on"_s;

            args << u"-drive"_s << u"if=pflash,format=raw,readonly=on,unit=0,file=%1"_s.arg(Utils::escapeQemuOptionValue(ovmf.codePath));
            args << u"-drive"_s << u"if=pflash,format=raw,unit=1,file=%1"_s.arg(optPath(localVarsPath, relativePaths));
        } else {
            if (ovmf.codePath.isEmpty())
                args << u"-drive"_s << u"if=pflash,file=UEFI_CODE_NOT_FOUND"_s;
            if (!QFile::exists(localVarsPath))
                args << u"-drive"_s << u"if=pflash,file=VM_VARS_FILE_MISSING"_s;
        }
    }

    // --- TPM ---
    if (config.useTpm()) {
        const QString sockPath = vmDir.absoluteFilePath(KEMUConst::SwtpmSocketFileName);
        args << u"-chardev"_s << u"socket,id=chrtpm,path=%1"_s.arg(optPath(sockPath, relativePaths));
        args << u"-tpmdev"_s << u"emulator,id=tpm0,chardev=chrtpm"_s;
        // CRB preferred for Q35 / Windows 11; TIS as fallback.
        args << u"-device"_s << (config.machineType() == VMSpecs::MachineType::Q35 ? u"tpm-crb,tpmdev=tpm0"_s : u"tpm-tis,tpmdev=tpm0"_s);
    }

    // Q35 uses ich9-ahci, where every port is a separate bus holding exactly
    // one unit; the pc chipsets use two IDE buses of two units each. QEMU's
    // automatic placement always picks ide.0, so a second device fails with
    // "bus supports only 1 units". Hand out the slots ourselves.
    const bool ahci = config.machineType() == VMSpecs::MachineType::Q35;
    int nextIdeSlot = 0;
    const auto takeIdeSlot = [&ahci, &nextIdeSlot]() {
        const int slot = nextIdeSlot++;
        return ahci ? u"bus=ide.%1"_s.arg(slot) : u"bus=ide.%1,unit=%2"_s.arg(slot / 2).arg(slot % 2);
    };

    // --- Disk ---
    const QString driveId = u"sysdisk"_s;
    args << u"-drive"_s << u"file=%1,format=qcow2,if=none,id=%2"_s.arg(optPath(config.diskPath(), relativePaths), driveId);

    if (config.diskInterface() == VMSpecs::DiskInterface::Nvme)
        args << u"-device"_s << u"%1,drive=%2,serial=serial0"_s.arg(VMConfig::toQemuString(config.diskInterface()), driveId);
    else if (config.diskInterface() == VMSpecs::DiskInterface::SataIde)
        args << u"-device"_s << u"%1,drive=%2,%3"_s.arg(VMConfig::toQemuString(config.diskInterface()), driveId, takeIdeSlot());
    else
        args << u"-device"_s << u"%1,drive=%2"_s.arg(VMConfig::toQemuString(config.diskInterface()), driveId);

    // --- ISO ---
    // Always AHCI/IDE, never virtio-scsi: Windows has no inbox virtio-scsi
    // driver, so an installer medium on that bus disappears as soon as WinPE
    // takes over from the firmware. Every guest OS we support can read an
    // AHCI CD-ROM out of the box.
    if (!config.isoPath().isEmpty()) {
        args << u"-drive"_s
             << u"file=%1,format=raw,if=none,id=drive-cdrom0,media=cdrom,readonly=on"_s.arg(optPath(config.isoPath(), relativePaths));
    } else {
        args << u"-drive"_s << u"if=none,id=drive-cdrom0,media=cdrom"_s;
    }
    args << u"-device"_s << u"ide-cd,drive=drive-cdrom0,id=cdrom0,%1"_s.arg(takeIdeSlot());

    // --- Floppy ---
    if (config.machineType() != VMSpecs::MachineType::Q35) {
        if (!config.floppyPath().isEmpty()) {
            args << u"-drive"_s
                 << u"file=%1,format=raw,if=none,id=drive-fda,readonly=off"_s.arg(optPath(config.floppyPath(), relativePaths));
        } else {
            args << u"-drive"_s << u"if=none,id=drive-fda"_s;
        }
        args << u"-device"_s << u"floppy,drive=drive-fda,id=fda"_s;
    }

    // --- Cloud-Init NoCloud ISO ---
    const QString cidataIso = vmDir.absoluteFilePath(u"cidata.iso"_s);
    if (QFile::exists(cidataIso)) {
        args << u"-drive"_s
             << u"file=%1,format=raw,if=none,id=drive-cidata,media=cdrom,readonly=on"_s.arg(optPath(cidataIso, relativePaths));
        args << u"-device"_s << u"ide-cd,drive=drive-cidata,id=cidata,%1"_s.arg(takeIdeSlot());
    }

    // Boot order: Floppy (a), Disk (c) or ISO (d).
    // Q35 has no floppy controller, so floppy boot would silently fail.
    const bool canBootFromFloppy = config.bootFromFloppy() && config.machineType() != VMSpecs::MachineType::Q35;
    QString bootOrder;
    if (canBootFromFloppy)
        bootOrder = u"a"_s;
    else if (config.bootFromIso() && !config.isoPath().isEmpty())
        bootOrder = u"d"_s;
    else
        bootOrder = u"c"_s;

    // An installer reboots into the system it has just written to disk.
    // "once" makes QEMU restore the normal order on the first reset, so the
    // guest does not land in the installer again.
    const bool installerFirstBoot =
        bootOrder == u"d"_s && config.deployState() == VMConfig::DeployState::Deploying && config.imageType() == VMConfig::ImageType::InstallerIso;

    if (installerFirstBoot)
        args << u"-boot"_s << u"order=c,once=d,menu=on"_s;
    else
        args << u"-boot"_s << bootOrder;

    // -- Shared Folders ---
    if (!config.sharedPath().isEmpty()) {
        const QString mountTag = config.sharedName().isEmpty() ? u"share"_s : config.sharedName();

        if (config.osType() == VMSpecs::OsType::Windows) {
            // -- virtio-fs (for Windows guests) --
            // virtiofsd must be started BEFORE QEMU (see MainWindow::startVirtiofsd).
            // QEMU connects via a vhost-user socket.
            const QString virtiofsSock = vmDir.absoluteFilePath(KEMUConst::VirtiofsdSocketFileName);

            args << u"-chardev"_s << u"socket,id=char_virtiofs,path=%1"_s.arg(optPath(virtiofsSock, relativePaths));
            args << u"-device"_s << u"vhost-user-fs-pci,queue-size=1024,chardev=char_virtiofs,tag=%1"_s.arg(Utils::escapeQemuOptionValue(mountTag));

            // Shared memory for virtio-fs
            QString memObj = u"memory-backend-file,id=mem,size=%1M,mem-path=/dev/shm,share=on"_s.arg(config.ramMb());
            if (config.preallocMemory())
                memObj += u",prealloc=on"_s;
            args << u"-object"_s << memObj;
            args << u"-numa"_s << u"node,memdev=mem"_s;

        } else {
            // -- 9p / VirtFS (for Linux/BSD/Unix guests) --
            // No additional daemon required. Always an absolute path.
            args << u"-virtfs"_s
                 << u"local,path=%1,mount_tag=%2,security_model=none,id=fs0"_s.arg(Utils::escapeQemuOptionValue(config.sharedPath()),
                                                                                   Utils::escapeQemuOptionValue(mountTag));
        }
    }

    // --- Graphics ---
    // 3D acceleration requires a GL display backend (GTK or SDL).
    // Disable it for headless mode.
    const bool effective3d = config.gpu3dAcceleration() && config.gpuType() == VMSpecs::GpuType::Virtio && !headless;

    const int mem = qBound(VMConfig::vgaMemMbLowerLimit(config.gpuType()), config.gpuMemMb(), VMConfig::vgaMemMbUpperLimit(config.gpuType()));

    if (config.gpuType() == VMSpecs::GpuType::Qxl) {
        args << u"-device"_s << u"qxl-vga,vgamem_mb=%1"_s.arg(mem);
    } else if (config.gpuType() == VMSpecs::GpuType::Virtio) {
        const QSize res = config.displayResolution();
        const QString device = effective3d ? u"virtio-vga-gl"_s : u"virtio-vga"_s;
        args << u"-device"_s << u"%1,xres=%2,yres=%3,max_hostmem=%4M"_s.arg(device).arg(res.width()).arg(res.height()).arg(mem);
    } else if (config.gpuType() == VMSpecs::GpuType::CirrusGD5446) {
        args << u"-device"_s << u"cirrus-vga,vgamem_mb=%1"_s.arg(mem);
    } else if (config.gpuType() == VMSpecs::GpuType::CirrusGD5430) {
        args << u"-device"_s << u"isa-cirrus-vga,vgamem_mb=%1"_s.arg(mem);
    } else if (config.gpuType() == VMSpecs::GpuType::VgaISA) {
        args << u"-device"_s << u"isa-vga,vgamem_mb=%1"_s.arg(mem);
    } else {
        args << u"-device"_s << u"VGA,vgamem_mb=%1"_s.arg(mem);
    }

    // --- Display Frontend ---
    if (config.displayType() == VMSpecs::DisplayType::Dbus) {
        // p2p: QEMU opens no window of its own and waits for a peer handed to
        // it via QMP add_client. "Headless" therefore only means "no viewer
        // attached yet" - the display object has to exist either way, or
        // reconnecting later would be impossible.
        QString displayOpt = u"dbus,p2p=on"_s;
        if (effective3d)
            displayOpt += u",gl=on"_s;
        if (config.audioDevice() != VMSpecs::AudioDevice::None) {
            const QString audioId = (config.audioDevice() == VMSpecs::AudioDevice::Hda) ? u"audio0"_s : u"snd0"_s;
            displayOpt += u",audiodev=%1"_s.arg(audioId);
        }
        args << u"-display"_s << displayOpt;

        // ---- Guest agent for clipboard (clipboard only; pointing is
        //      handled by the selected pointer device) ----
        // For -display dbus, the in-guest spice-vdagent talks to QEMU's
        // qemu-vdagent chardev. QEMU bridges that to /org/qemu/Display1/Clipboard
        // on its D-Bus bus.
        // Without this block, the Clipboard D-Bus object exists but has
        // no peer on the guest side -- Grab/Request calls go nowhere.
        // NB: The port name "com.redhat.spice.0" is hardcoded in the
        // spice-vdagent daemon only for historical reasons.
        args << u"-device"_s << u"virtio-serial-pci"_s;
        args << u"-chardev"_s << u"qemu-vdagent,id=vdagent,name=vdagent,clipboard=on,mouse=off"_s;
        args << u"-device"_s << u"virtserialport,chardev=vdagent,name=com.redhat.spice.0"_s;

    } else if (headless) {
        args << u"-display"_s << u"none"_s;
    } else {
        QString displayOpt = VMConfig::toQemuString(config.displayType());
        if (effective3d)
            displayOpt += u",gl=on"_s;
        args << u"-display"_s << displayOpt;
    }

    // --- Audio ---
    if (config.audioDevice() != VMSpecs::AudioDevice::None) {
        // Audio follows the display: with a D-Bus display it is routed to the
        // viewer, which may attach later.
        const bool useDbusAudio = (config.displayType() == VMSpecs::DisplayType::Dbus);

        QString hostAudioBackend = u"none"_s;
        const auto &qemu = QemuCapabilities::instance();
        const auto &caps = SandboxCapabilities::instance();
        if (caps.has(SandboxCapabilities::PipeWireAudio) && qemu.hasAudiodev(u"pipewire"_s))
            hostAudioBackend = u"pipewire"_s;
        else if (caps.has(SandboxCapabilities::PulseAudio) && qemu.hasAudiodev(u"pa"_s))
            hostAudioBackend = u"pa"_s;
        const QString audioBackend = useDbusAudio ? u"dbus"_s : hostAudioBackend;

        if (config.audioDevice() == VMSpecs::AudioDevice::Hda) {
            args << u"-audiodev"_s << u"%1,id=audio0"_s.arg(audioBackend);
            args << u"-device"_s << VMConfig::toQemuString(VMSpecs::AudioDevice::Hda);
            args << u"-device"_s << u"hda-duplex,audiodev=audio0"_s;
        } else {
            args << u"-audiodev"_s << u"%1,id=snd0"_s.arg(audioBackend);
            QString audioDevice = u"%1,audiodev=snd0"_s.arg(VMConfig::toQemuString(config.audioDevice()));
            if (config.audioDevice() == VMSpecs::AudioDevice::Sb16 && !config.sb16Config().isDefault()) {
                audioDevice += u",%1"_s.arg(config.sb16Config().toPropertyString());
            }
            args << u"-device"_s << audioDevice;
        }
    }

    // --- Network ---
    if (config.networkCard() != VMSpecs::NetworkCard::None) {
        QString netDevice = u"%1,netdev=%2"_s.arg(VMConfig::toQemuString(config.networkCard()), KEMUConst::NetdevId);
        if (!config.macAddress().isEmpty())
            netDevice.append(u",mac=%1"_s.arg(config.macAddress()));
        args << u"-device"_s << netDevice;

        if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            if (!networkContext.mcastAddress.isEmpty()) {
                // Standalone export: QEMU's built-in multicast network stands
                // in for the in-memory switch, which lives in the KEMU
                // process and is unreachable from a script. localaddr keeps
                // the group on the loopback interface; without it the guest's
                // frames leave for the physical network.
                args << u"-netdev"_s
                     << u"socket,id=%1,mcast=%2,localaddr=127.0.0.1"_s.arg(KEMUConst::NetdevId, networkContext.mcastAddress);
            } else {
                Q_ASSERT_X(networkContext.switchFd >= 0,
                           "QemuCommandBuilder::build",
                           "VirtualNetwork mode requires NetworkContext::switchFd or mcastAddress to be set");
                args << u"-netdev"_s << u"stream,id=%1,server=off,addr.type=fd,addr.str=%2"_s.arg(KEMUConst::NetdevId).arg(networkContext.switchFd);
            }
        } else if (networkContext.useSlirp) {
            // Standalone export: QEMU's built-in usermode networking stands
            // in for KEMU's in-process slirp instance. No external process,
            // no helper socket - just port forwards on the command line.
            QString netdev = u"user,id=%1"_s.arg(KEMUConst::NetdevId);
            for (const auto &rule : config.portForwards()) {
                const QString proto = rule.protocol == PortForwardRule::Protocol::Tcp ? u"tcp"_s : u"udp"_s;
                netdev += u",hostfwd=%1:127.0.0.1:%2-:%3"_s.arg(proto).arg(rule.hostPort).arg(rule.guestPort);
            }
            args << u"-netdev"_s << netdev;
        } else {
            // User mode: QEMU gets one end of the socketpair KEMU's
            // in-process slirp network (UserModeNetwork) listens on.
            Q_ASSERT_X(networkContext.userNetFd >= 0,
                       "QemuCommandBuilder::build",
                       "UserMode requires NetworkContext::userNetFd or useSlirp to be set");
            args << u"-netdev"_s << u"stream,id=%1,server=off,addr.type=fd,addr.str=%2"_s.arg(KEMUConst::NetdevId).arg(networkContext.userNetFd);
        }
    }

    // --- USB controller ---
    QString usbBus;
    if (config.usbController() == VMSpecs::UsbController::Xhci) {
        args << u"-device"_s << u"qemu-xhci,id=xhci"_s;
        usbBus = u"xhci.0"_s;
    } else if (config.usbController() == VMSpecs::UsbController::Ehci) {
        args << u"-device"_s << u"usb-ehci,id=ehci"_s;
        usbBus = u"ehci.0"_s;
    } else if (config.usbController() == VMSpecs::UsbController::Default) {
        args << u"-device"_s << u"pci-ohci,id=ohci"_s;
        usbBus = u"ohci.0"_s;
    }

    // --- Pointer device ---
    switch (config.pointerDevice()) {
    case VMSpecs::PointerDevice::Ps2:
        break;
    case VMSpecs::PointerDevice::UsbMouse:
        if (!usbBus.isEmpty())
            args << u"-device"_s << u"usb-mouse,bus=%1"_s.arg(usbBus);
        break;
    case VMSpecs::PointerDevice::UsbTablet:
        if (usbBus.isEmpty()) {
            qCWarning(KEMU) << "Pointer device usb-tablet requested without a USB controller - guest falls back to PS/2";
            break;
        } else {
            args << u"-device"_s << u"usb-tablet,bus=%1"_s.arg(usbBus);
        }
        break;
    case VMSpecs::PointerDevice::VirtioTablet:
        args << u"-device"_s << u"virtio-tablet-pci"_s;
        break;
    }

    // --- Keyboard device ---
    if (config.keyboardDevice() == VMSpecs::KeyboardDevice::Usb && !usbBus.isEmpty())
        args << u"-device"_s << u"usb-kbd,bus=%1"_s.arg(usbBus);

    // --- Monitor / QMP ---
    if (networkContext.monitorFd >= 0) {
        // Socketpair: nothing in the file system for other processes of this
        // user to connect to, and the monitor is up the moment QEMU starts.
        args << u"-chardev"_s << u"socket,id=%1,fd=%2"_s.arg(KEMUConst::QmpChardevId).arg(networkContext.monitorFd);
        args << u"-mon"_s << u"chardev=%1,mode=control"_s.arg(KEMUConst::QmpChardevId);
    } else {
        // Export scripts cannot inherit descriptors, so QEMU listens instead.
        args << u"-qmp"_s << u"unix:%1,server,nowait"_s.arg(optPath(monitorSocketPath, relativePaths));
    }

    // --- Extra Args ---
    if (!config.extraArgs().isEmpty())
        args << KProcess::splitCommand(config.extraArgs());

    // Resume from a previously saved VM state. Must come last so it cannot be
    // overridden by earlier defaults.
    if (!incomingSavestatePath.isEmpty()) {
        args << u"-incoming"_s << u"file:"_s + incomingSavestatePath;
    }

    return args;
}

QString QemuCommandBuilder::buildShellScript(const VMConfig &config,
                                             const QDir &vmDir,
                                             const QString &monitorSocketPath,
                                             const QemuNetworkContext &networkContext,
                                             const VirtualNetwork &network,
                                             const bool relativePaths)
{
    // A standalone script has no QMP peer to add_client the p2p display;
    // fall back to a self-contained display backend.
    VMConfig scriptConfig = config;
    if (scriptConfig.displayType() == VMSpecs::DisplayType::Dbus)
        scriptConfig.setDisplayType(VMSpecs::DisplayType::Gtk);

    // A standalone script cannot reach the in-memory switch inside the KEMU
    // process, so exported virtual networks run on QEMU's built-in multicast
    // networking: all exported VMs of the same network share one mcast group.
    // No descriptors in a script: the monitor listens and the network falls
    // back to QEMU's own backends.
    QemuNetworkContext scriptNetCtx = networkContext;
    scriptNetCtx.monitorFd = -1;
    scriptNetCtx.switchFd = -1;
    if (config.networkCard() != VMSpecs::NetworkCard::None && config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork)
        scriptNetCtx.mcastAddress = mcastAddressFor(network.name);

    // Same for user mode: the script cannot reach KEMU's in-process slirp
    // instance, so it falls back to QEMU's built-in "-netdev user".
    if (config.networkCard() != VMSpecs::NetworkCard::None && config.networkMode() == VMSpecs::NetworkMode::UserMode) {
        scriptNetCtx.useSlirp = true;
        scriptNetCtx.userNetFd = -1;
    }

    const QStringList args = build(scriptConfig, vmDir, monitorSocketPath, scriptNetCtx, relativePaths);

    QString script;

    if (relativePaths) {
        script += u"#!/bin/sh\n"_s + u"# Generated by %1 %2\n\n"_s.arg(QCoreApplication::applicationName(), QCoreApplication::applicationVersion())
            + u"cd \"$(dirname \"$0\")\" || exit 1\n\n"_s;
    }

    // --- swtpm ---
    if (config.useTpm()) {
        const QString swtpmPath = relativePaths ? u"swtpm"_s : ExternalTools::instance().path(ExternalTools::Swtpm);
        const QString sockArg = relativePaths ? KEMUConst::SwtpmSocketFileName : vmDir.absoluteFilePath(KEMUConst::SwtpmSocketFileName);
        const QString stateArg = relativePaths ? u"."_s : vmDir.absolutePath();

        script += u"# Start software TPM emulator\n"_s;
        if (!relativePaths)
            script += u"# %1\n"_s.arg(ExternalTools::instance().version(ExternalTools::Swtpm));
        script += u"%1 socket \\\n"_s.arg(swtpmPath) + u"  --ctrl type=unixio,path=%1 \\\n"_s.arg(sockArg) + u"  --terminate \\\n"_s
            + u"  --tpmstate dir=%1 \\\n"_s.arg(stateArg) + u"  --tpm2 &\n"_s + u"sleep 1\n\n"_s;
    }

    // --- virtiofsd (Windows shared folders) ---
    if (!config.sharedPath().isEmpty() && config.osType() == VMSpecs::OsType::Windows) {
        const QString virtiofsdPath = relativePaths ? u"virtiofsd"_s : ExternalTools::instance().path(ExternalTools::Virtiofsd);
        const QString sockArg = relativePaths ? KEMUConst::VirtiofsdSocketFileName : vmDir.absoluteFilePath(KEMUConst::VirtiofsdSocketFileName);

        script += u"# Start virtiofsd for shared folder\n"_s;
        script += u"%1 \\\n"_s.arg(virtiofsdPath) + u"  --socket-path=%1 \\\n"_s.arg(sockArg) + u"  --shared-dir=%1 \\\n"_s.arg(config.sharedPath())
            + u"  --cache=auto \\\n"_s + u"  --sandbox=none &\n"_s + u"sleep 1\n\n"_s;
    }

    // --- Network ---
    if (config.networkCard() != VMSpecs::NetworkCard::None) {
        if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            script += u"# Network '%1': QEMU's built-in multicast networking stands in for\n"_s.arg(network.name)
                + u"# KEMU's in-memory switch. Every exported VM of this network shares\n"_s
                + u"# the same group on this machine and reaches the others at L2; the\n"_s
                + u"# group stays on the loopback interface. There is no DHCP and\n"_s
                + u"# no internet access here - configure static addresses inside the\n"_s
                + u"# guests or run a DHCP server in one of them.\n\n"_s;
        } else if (networkContext.useSlirp) {
            // Nothing to start - QEMU's own -netdev user is already in args.
        }
    }

    // --- QEMU ---
    if (!relativePaths)
        script += u"# %1\n"_s.arg(ExternalTools::instance().version(ExternalTools::QemuSystem));

    const QString qemuPath = relativePaths ? u"qemu-system-x86_64"_s : ExternalTools::instance().path(ExternalTools::QemuSystem);

    if (config.displayType() == VMSpecs::DisplayType::Dbus) {
        script += u"# Inside KEMU, the integrated viewer connects automatically using D-Bus.\n"_s;
        script += u"# For standalone use another display type.\n"_s;
    }

    script += u"exec "_s;

    script += qemuPath + u" \\\n  "_s + argsToReadableString(args) + u"\n"_s;

    return script;
}

QString QemuCommandBuilder::argsToReadableString(const QStringList &args)
{
    QStringList formattedLines;
    QString currentLine;

    for (const QString &arg : args) {
        if (arg.startsWith(u'-')) {
            if (!currentLine.isEmpty())
                formattedLines << currentLine;
            currentLine = arg;
        } else {
            const QString quotedArg = Utils::quoteShellArg(arg);
            currentLine += currentLine.isEmpty() ? quotedArg : u" "_s + quotedArg;
        }
    }

    if (!currentLine.isEmpty())
        formattedLines << currentLine;

    return formattedLines.join(u" \\\n  "_s);
}
