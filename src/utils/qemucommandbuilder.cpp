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

using namespace Qt::StringLiterals;

QStringList QemuCommandBuilder::build(const VMConfig &config,
                                      const QDir &vmDir,
                                      const QString &monitorSocketPath,
                                      const NetworkContext &networkContext,
                                      const bool relativePaths,
                                      const bool headless,
                                      const QString &incomingSavestatePath)
{
    QStringList args;

    // Use guest= form so commas in the VM name aren't parsed as further -name
    // options.
    args << u"-name"_s << u"guest=%1"_s.arg(config.name());

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

            args << u"-drive"_s << u"if=pflash,format=raw,readonly=on,unit=0,file=%1"_s.arg(ovmf.codePath);
            args << u"-drive"_s << u"if=pflash,format=raw,unit=1,file=%1"_s.arg(relativePaths ? QFileInfo(localVarsPath).fileName() : localVarsPath);
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
        args << u"-chardev"_s << u"socket,id=chrtpm,path=%1"_s.arg(relativePaths ? QFileInfo(sockPath).fileName() : sockPath);
        args << u"-tpmdev"_s << u"emulator,id=tpm0,chardev=chrtpm"_s;
        // CRB preferred for Q35 / Windows 11; TIS as fallback.
        args << u"-device"_s << (config.machineType() == VMSpecs::MachineType::Q35 ? u"tpm-crb,tpmdev=tpm0"_s : u"tpm-tis,tpmdev=tpm0"_s);
    }

    // --- Disk ---
    const QString driveId = u"sysdisk"_s;
    args << u"-drive"_s << u"file=%1,format=qcow2,if=none,id=%2"_s.arg(relativePaths ? QFileInfo(config.diskPath()).fileName() : config.diskPath(), driveId);

    if (config.diskInterface() == VMSpecs::DiskInterface::Nvme)
        args << u"-device"_s << u"%1,drive=%2,serial=serial0"_s.arg(VMConfig::toQemuString(config.diskInterface()), driveId);
    else
        args << u"-device"_s << u"%1,drive=%2"_s.arg(VMConfig::toQemuString(config.diskInterface()), driveId);

    // --- ISO ---
    if (config.machineType() == VMSpecs::MachineType::Q35) {
        args << u"-device"_s << u"virtio-scsi-pci,id=scsi0"_s;

        if (!config.isoPath().isEmpty()) {
            args << u"-drive"_s
                 << u"file=%1,format=raw,if=none,id=drive-cdrom0,media=cdrom,readonly=on"_s.arg(relativePaths ? QFileInfo(config.isoPath()).fileName()
                                                                                                              : config.isoPath());
            args << u"-device"_s << u"scsi-cd,drive=drive-cdrom0,id=cdrom0"_s;
        } else {
            args << u"-drive"_s << u"if=none,id=drive-cdrom0,media=cdrom"_s;
            args << u"-device"_s << u"scsi-cd,drive=drive-cdrom0,id=cdrom0"_s;
        }
    } else {
        if (!config.isoPath().isEmpty()) {
            args << u"-drive"_s
                 << u"file=%1,format=raw,if=none,id=drive-cdrom0,media=cdrom,readonly=on"_s.arg(relativePaths ? QFileInfo(config.isoPath()).fileName()
                                                                                                              : config.isoPath());
            args << u"-device"_s << u"ide-cd,drive=drive-cdrom0,id=cdrom0"_s;
        } else {
            args << u"-drive"_s << u"if=none,id=drive-cdrom0,media=cdrom"_s;
            args << u"-device"_s << u"ide-cd,drive=drive-cdrom0,id=cdrom0"_s;
        }
    }

    // --- Floppy ---
    if (config.machineType() != VMSpecs::MachineType::Q35) {
        if (!config.floppyPath().isEmpty()) {
            args << u"-drive"_s
                 << u"file=%1,format=raw,if=none,id=drive-fda,readonly=off"_s.arg(relativePaths ? QFileInfo(config.floppyPath()).fileName()
                                                                                                : config.floppyPath());
        } else {
            args << u"-drive"_s << u"if=none,id=drive-fda"_s;
        }
        args << u"-device"_s << u"floppy,drive=drive-fda,id=fda"_s;
    }

    // --- Cloud-Init NoCloud ISO ---
    const QString cidataIso = vmDir.absoluteFilePath(u"cidata.iso"_s);
    if (QFile::exists(cidataIso)) {
        const QString ciDevType = (config.machineType() == VMSpecs::MachineType::Q35) ? u"scsi-cd"_s : u"ide-cd"_s;
        args << u"-drive"_s
             << u"file=%1,format=raw,if=none,id=drive-cidata,media=cdrom,readonly=on"_s.arg(relativePaths ? QFileInfo(cidataIso).fileName() : cidataIso);
        args << u"-device"_s << u"%1,drive=drive-cidata,id=cidata"_s.arg(ciDevType);
    }

    // Boot order: Floppy (a), Disk (c) or ISO (d).
    // Q35 has no floppy controller, so floppy boot would silently fail.
    const bool canBootFromFloppy = config.bootFromFloppy() && config.machineType() != VMSpecs::MachineType::Q35;
    QString bootOrder;
    if (canBootFromFloppy)
        bootOrder = u"a"_s;
    else if (config.bootFromIso())
        bootOrder = u"d"_s;
    else
        bootOrder = u"c"_s;
    args << u"-boot"_s << bootOrder;

    // -- Shared Folders ---
    if (!config.sharedPath().isEmpty()) {
        const QString mountTag = config.sharedName().isEmpty() ? u"share"_s : config.sharedName();

        if (config.osType() == VMSpecs::OsType::Windows) {
            // -- virtio-fs (for Windows guests) --
            // virtiofsd must be started BEFORE QEMU (see MainWindow::startVirtiofsd).
            // QEMU connects via a vhost-user socket.
            const QString virtiofsSock = vmDir.absoluteFilePath(KEMUConst::VirtiofsdSocketFileName);

            args << u"-chardev"_s << u"socket,id=char_virtiofs,path=%1"_s.arg(relativePaths ? QFileInfo(virtiofsSock).fileName() : virtiofsSock);
            args << u"-device"_s << u"vhost-user-fs-pci,queue-size=1024,chardev=char_virtiofs,tag=%1"_s.arg(mountTag);

            // Shared memory for virtio-fs
            QString memObj = u"memory-backend-file,id=mem,size=%1M,mem-path=/dev/shm,share=on"_s.arg(config.ramMb());
            if (config.preallocMemory())
                memObj += u",prealloc=on"_s;
            args << u"-object"_s << memObj;
            args << u"-numa"_s << u"node,memdev=mem"_s;

        } else {
            // -- 9p / VirtFS (for Linux/BSD/Unix guests) --
            // No additional daemon required.
            // When QEMU runs inside a namespace (virtual network mode),
            // it sees itself as UID 0. security_model=none would make all
            // files root-owned in the guest. Use mapped-xattr instead,
            // which stores guest UIDs in xattrs on the host.
            const QString secModel = (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) ? u"mapped-xattr"_s : u"none"_s;
            args << u"-virtfs"_s
                 << u"local,path=%1,mount_tag=%2,security_model=%3,id=fs0"_s.arg(relativePaths ? QFileInfo(config.sharedPath()).fileName()
                                                                                               : config.sharedPath(),
                                                                                 mountTag,
                                                                                 secModel);
        }
    }

    // --- Graphics ---
    // 3D acceleration requires a GL display backend (GTK or SDL).
    // Disable it for headless mode and SPICE.
    const bool effective3d =
        config.gpu3dAcceleration() && config.gpuType() == VMSpecs::GpuType::Virtio && !headless && config.displayType() != VMSpecs::DisplayType::Spice;

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
    if (config.displayType() == VMSpecs::DisplayType::Spice) {
        const QString spiceSock = vmDir.absoluteFilePath(KEMUConst::SpiceSocketFileName);
        args << u"-spice"_s << u"unix=on,addr=%1,disable-ticketing=on"_s.arg(relativePaths ? QFileInfo(spiceSock).fileName() : spiceSock);
        args << u"-display"_s << u"none"_s;

        if (config.spiceAgent()) {
            args << u"-device"_s << u"virtio-serial-pci"_s;
            args << u"-chardev"_s << u"spicevmc,id=vdagent,name=vdagent"_s;
            args << u"-device"_s << u"virtserialport,chardev=vdagent,name=com.redhat.spice.0"_s;
        }

        // Only emit chardevs if a USB controller exists; otherwise the
        // matching usb-redir devices below are skipped and these would dangle.
        if (config.spiceUsbRedir() && config.usbController() != VMSpecs::UsbController::None) {
            for (int i = 0; i < config.spiceUsbRedirCount(); ++i) {
                const QString id = u"usbredir%1"_s.arg(i);
                args << u"-chardev"_s << u"spicevmc,id=%1,name=usbredir"_s.arg(id);
            }
        }
    } else if (config.displayType() == VMSpecs::DisplayType::Dbus) {
        if (headless) {
            args << u"-display"_s << u"none"_s;
        } else {
            const QString sockArg = relativePaths ? KEMUConst::DBUSSocketFileName : vmDir.absoluteFilePath(KEMUConst::DBUSSocketFileName);

            QString displayOpt = u"dbus,addr=unix:path=%1"_s.arg(sockArg);
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
            // qemu-vdagent chardev (NOT spicevmc, which is SPICE-only). QEMU
            // bridges that to /org/qemu/Display1/Clipboard on its D-Bus bus.
            // Without this block, the Clipboard D-Bus object exists but has
            // no peer on the guest side -- Grab/Request calls go nowhere.

            // NB: The port name "com.redhat.spice.0" is hardcoded in the
            // spice-vdagent daemon only for historical reasons.
            args << u"-device"_s << u"virtio-serial-pci"_s;
            args << u"-chardev"_s << u"qemu-vdagent,id=vdagent,name=vdagent,clipboard=on,mouse=off"_s;
            args << u"-device"_s << u"virtserialport,chardev=vdagent,name=com.redhat.spice.0"_s;
        }
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
        const bool useDbusAudio = (config.displayType() == VMSpecs::DisplayType::Dbus && !headless);

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
        QString netDevice = u"%1,netdev=nic0"_s.arg(VMConfig::toQemuString(config.networkCard()));
        if (!config.macAddress().isEmpty())
            netDevice.append(u",mac=%1"_s.arg(config.macAddress()));
        args << u"-device"_s << netDevice;

        if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork) {
            // Namespace-based: QEMU uses a pre-created tap device on the bridge.
            // QEMU itself must be launched inside the namespace via nsenter.
            // The caller MUST provide the tap name - otherwise we fail loud
            // rather than silently falling back to passt.
            Q_ASSERT_X(!networkContext.tapDevice.isEmpty(), "QemuCommandBuilder::build", "VirtualNetwork mode requires NetworkContext::tapDevice to be set");

            args << u"-netdev"_s << u"tap,id=nic0,ifname=%1,script=no,downscript=no"_s.arg(networkContext.tapDevice);
        } else if (networkContext.useSlirp) {
            // QEMU's built-in usermode networking. No external process, no
            // helper socket - just port forwards on the command line.
            QString netdev = u"user,id=nic0"_s;
            for (const auto &rule : config.portForwards()) {
                const QString proto = rule.protocol == PortForwardRule::Protocol::Tcp ? u"tcp"_s : u"udp"_s;
                netdev += u",hostfwd=%1:127.0.0.1:%2-:%3"_s.arg(proto).arg(rule.hostPort).arg(rule.guestPort);
            }
            args << u"-netdev"_s << netdev;
        } else {
            // Passt: user-mode networking via unix stream socket.
            const QString passtSock =
                networkContext.passtSocketPath.isEmpty() ? vmDir.absoluteFilePath(KEMUConst::PasstSocketFileName) : networkContext.passtSocketPath;
            args << u"-netdev"_s << u"stream,id=nic0,server=off,addr.type=unix,addr.path=%1"_s.arg(relativePaths ? QFileInfo(passtSock).fileName() : passtSock);
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
        if (!usbBus.isEmpty())
            args << u"-device"_s << u"usb-tablet,bus=%1"_s.arg(usbBus);
        break;
    }

    // --- Keyboard device ---
    if (config.keyboardDevice() == VMSpecs::KeyboardDevice::Usb && !usbBus.isEmpty())
        args << u"-device"_s << u"usb-kbd,bus=%1"_s.arg(usbBus);

    // usb-redir devices must come after the USB controller
    if (config.displayType() == VMSpecs::DisplayType::Spice && config.spiceUsbRedir() && config.usbController() != VMSpecs::UsbController::None) {
        for (int i = 0; i < config.spiceUsbRedirCount(); ++i) {
            args << u"-device"_s << u"usb-redir,chardev=usbredir%1"_s.arg(i);
        }
    }

    // --- Monitor / QMP ---
    args << u"-qmp"_s << u"unix:%1,server,nowait"_s.arg(relativePaths ? QFileInfo(monitorSocketPath).fileName() : monitorSocketPath);

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
                                             const NetworkContext &networkContext,
                                             const VirtualNetwork &network,
                                             const bool relativePaths)
{
    const QStringList args = build(config, vmDir, monitorSocketPath, networkContext, relativePaths);

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
            const QString gw = network.gateway.isEmpty() ? u"192.168.100.1"_s : network.gateway;

            script +=
                u"# Create network namespace\n"_s + u"unshare --user --net --map-root-user -- sleep infinity &\n"_s + u"NS_PID=$!\n"_s + u"sleep 0.5\n\n"_s;

            script += u"# Start NAT connector (must run before bridge setup)\n"_s;
            if (network.enableNat) {
                script +=
                    u"pasta --userns /proc/$NS_PID/ns/user \\\n"_s + u"  --netns /proc/$NS_PID/ns/net \\\n"_s + u"  --config-net -f &\n"_s + u"sleep 2\n\n"_s;
            }

            script += u"# Configure bridge\n"_s + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip link set lo up\n"_s
                + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip link add br0 type bridge\n"_s
                + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip link set dev br0 type bridge stp_state 0\n"_s
                + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip addr add %1/%2 dev br0\n"_s.arg(gw).arg(network.prefixLength)
                + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip link set br0 up\n"_s;

            if (network.enableNat) {
                script +=
            u"\n# Enable IP forwarding and setup NAT\n"_s +
            u"nsenter -t $NS_PID -U --preserve-credentials -n -- "
            u"sh -c 'echo 1 > /proc/sys/net/ipv4/ip_forward'\n"_s +
            u"NS_IFACE=$(nsenter -t $NS_PID -U --preserve-credentials -n -- "
            u"ip -o route show default | awk '{print $5}')\n"_s +
            u"nsenter -t $NS_PID -U --preserve-credentials -n -- "
            u"nft add table ip nat\n"_s +
            u"nsenter -t $NS_PID -U --preserve-credentials -n -- "
            u"nft add chain ip nat postrouting '{ type nat hook postrouting priority 100; }'\n"_s +
            u"nsenter -t $NS_PID -U --preserve-credentials -n -- "
            u"nft add rule ip nat postrouting oif \"$NS_IFACE\" masquerade\n"_s;
            }

            if (network.enableDhcp) {
                QString rangeStart = network.dhcpRangeStart;
                QString rangeEnd = network.dhcpRangeEnd;
                if (rangeStart.isEmpty()) {
                    QStringList octets = gw.split(u'.');
                    if (octets.size() == 4) {
                        QString prefix = octets[0] + u'.' + octets[1] + u'.' + octets[2];
                        rangeStart = prefix + u".100"_s;
                        rangeEnd = prefix + u".200"_s;
                    }
                }
                script += u"\n# Start DHCP server\n"_s + u"nsenter -t $NS_PID -U --preserve-credentials -n -- dnsmasq \\\n"_s
                    + u"  --interface=br0 --bind-interfaces \\\n"_s + u"  --dhcp-range=%1,%2,12h \\\n"_s.arg(rangeStart, rangeEnd)
                    + u"  --no-daemon --no-resolv --server=1.1.1.1 &\n"_s;
            }

            const QString tapName = networkContext.tapDevice.isEmpty() ? u"tap0"_s : networkContext.tapDevice;
            script += u"\n# Create tap device for VM\n"_s + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip tuntap add dev %1 mode tap\n"_s.arg(tapName)
                + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip link set %1 master br0\n"_s.arg(tapName)
                + u"nsenter -t $NS_PID -U --preserve-credentials -n -- ip link set %1 up\n"_s.arg(tapName) + u"sleep 1\n\n"_s;

        } else if (networkContext.useSlirp) {
            // Nothing to start - QEMU's own -netdev user is already in args.
        } else {
            const QString passtPath = relativePaths ? u"passt"_s : ExternalTools::instance().path(ExternalTools::Passt);
            const QString sockArg = relativePaths ? KEMUConst::PasstSocketFileName : vmDir.absoluteFilePath(KEMUConst::PasstSocketFileName);

            script += u"# Start passt for user-mode networking\n"_s + u"%1 \\\n"_s.arg(passtPath) + u"  --socket %1 \\\n"_s.arg(sockArg) + u"  -f -q"_s;

            QStringList tcpPorts, udpPorts;
            for (const auto &rule : config.portForwards()) {
                const QString mapping = (rule.hostPort != rule.guestPort) ? u"%1:%2"_s.arg(rule.hostPort).arg(rule.guestPort) : QString::number(rule.hostPort);
                if (rule.protocol == PortForwardRule::Protocol::Tcp)
                    tcpPorts << mapping;
                else
                    udpPorts << mapping;
            }
            if (!tcpPorts.isEmpty())
                script += u" \\\n  -t %1"_s.arg(tcpPorts.join(u","_s));
            if (!udpPorts.isEmpty())
                script += u" \\\n  -u %1"_s.arg(udpPorts.join(u","_s));

            script += u" &\n"_s + u"sleep 1\n\n"_s;
        }
    }

    // --- QEMU ---
    if (!relativePaths)
        script += u"# %1\n"_s.arg(ExternalTools::instance().version(ExternalTools::QemuSystem));

    const QString qemuPath = relativePaths ? u"qemu-system-x86_64"_s : ExternalTools::instance().path(ExternalTools::QemuSystem);

    if (config.displayType() == VMSpecs::DisplayType::Spice) {
        const QString spiceSock = relativePaths ? KEMUConst::SpiceSocketFileName : vmDir.absoluteFilePath(KEMUConst::SpiceSocketFileName);
        const QString vvFile = relativePaths ? KEMUConst::SpiceVVFileName : vmDir.absoluteFilePath(KEMUConst::SpiceVVFileName);
        const QString viewerPath = relativePaths ? u"remote-viewer"_s : ExternalTools::instance().path(ExternalTools::RemoteViewer);

        script += u"# Write SPICE connection file\n"_s + u"cat > %1 << EOF\n"_s.arg(vvFile) + u"[virt-viewer]\n"_s + u"type=spice\n"_s
            + u"unix-path=%1\n"_s.arg(spiceSock) + u"EOF\n\n"_s;

        script += u"# Start SPICE viewer (will connect once QEMU is ready)\n"_s + u"( sleep 2 && %1 %2 ) &\n\n"_s.arg(viewerPath, vvFile);
    } else if (config.displayType() == VMSpecs::DisplayType::Dbus) {
        const QString sockArg = relativePaths ? KEMUConst::DBUSSocketFileName : vmDir.absoluteFilePath(KEMUConst::DBUSSocketFileName);
        script += u"# D-Bus display: QEMU listens on a private Unix socket.\n"_s;
        script += u"# Inside KEMU, the integrated viewer connects automatically.\n"_s;
        script += u"# For standalone use, connect with a D-Bus display viewer:\n"_s;
        script += u"#   <viewer> --dbus-address unix:path=%1\n\n"_s.arg(sockArg);
    }

    if (config.networkMode() == VMSpecs::NetworkMode::VirtualNetwork && config.networkCard() != VMSpecs::NetworkCard::None) {
        script += u"exec nsenter -t $NS_PID -U --preserve-credentials -n -- \\\n  "_s;
    } else {
        script += u"exec "_s;
    }

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
