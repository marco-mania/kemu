/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QElapsedTimer>
#include <QFileInfo>
#include <QSet>
#include <QString>

#include <KProcess>

class KProcess;

using namespace Qt::StringLiterals;

class QemuCapabilities
{
public:
    static QemuCapabilities &instance();

    // Re-probes the currently resolved qemu-system binary. Call right after
    // ExternalTools::instance().refresh().
    void refresh();

    bool hasDisplay(const QString &name) const;
    bool hasAudiodev(const QString &name) const;
    // Device names as for -device, e.g. "qxl-vga", which needs SPICE.
    bool hasDevice(const QString &name) const;
    bool hasChardev(const QString &name) const;

    // Whether the binary was built with seccomp support. Unlike the lists
    // above this defaults to false: -sandbox on a QEMU without it is fatal.
    [[nodiscard]] bool hasSeccompSandbox() const
    {
        return m_seccompSandbox;
    }

    // Whether monitors can be created as objects (-object monitor-qmp, QEMU
    // 11.1+), which replaces the deprecated -mon. Defaults to false: -mon
    // still works on every version.
    [[nodiscard]] bool hasMonitorObject() const
    {
        return m_monitorObject;
    }

    // Whether virtio-vga-gl takes blob=on and hostmem= next to virgl (QEMU
    // 9.2+). The listing alone does not tell: QEMU has had both properties for
    // years, but up to 9.1 the device refuses blob resources once virgl is on,
    // and the VM does not start. The venus property came with virgl blob
    // support in 9.2 and serves as the marker. Defaults to false like the
    // sandbox: blob=on on a QEMU without it is fatal.
    //
    // A QEMU 9.2+ built against virglrenderer before 1.0 still lists
    // everything and fails at start ("old virglrenderer, blob resources
    // unsupported"); the listing cannot show the library version.
    [[nodiscard]] bool hasVirtioGpuBlob() const
    {
        return m_virtioGpuBlob;
    }

    // The render server of virglrenderer, or an empty string. Vulkan (Venus,
    // venus=on) runs in this separate program, which virglrenderer starts for
    // every such VM; without it the guest gets no Vulkan device. venus=on
    // needs the same QEMU as host blobs, see hasVirtioGpuBlob().
    [[nodiscard]] QString virglRenderServer() const
    {
        return m_virglRenderServer;
    }

    // virglrenderer starts the render server from this variable if it is set
    // (QEMU inherits it from KEMU), otherwise from the path it was built with.
    static constexpr const char *RenderServerEnvironment = "RENDER_SERVER_EXEC_PATH";

    // Where virglrenderer would start its render server from, for the QEMU at
    // qemuPath. The built-in path is <prefix>/libexec/virgl_render_server, or
    // <prefix>/lib/virgl_render_server on Arch. The library does not tell its
    // prefix, so this looks below the prefix of QEMU first, then below
    // systemPrefixes. A set RenderServerEnvironment is the only candidate, as
    // for virglrenderer. Returns the first program found, or an empty string.
    [[nodiscard]] static QString findVirglRenderServer(const QString &qemuPath, const QStringList &systemPrefixes);

    // Whether KVM can be told to honor the memory types the guest sets up
    // (-accel kvm,honor-guest-pat=, QEMU 11.0+ on x86). On Intel CPUs, KVM
    // otherwise forces write-back for host GPU memory the guest maps, which
    // the host itself maps write-combined on Meteor Lake and xe GPUs, and on
    // dedicated GPUs: the guest then reads stale values, and Vulkan waits for
    // fences forever. Linux 6.16 or later on the host makes it take effect.
    [[nodiscard]] bool hasKvmHonorGuestPat() const
    {
        return m_kvmHonorGuestPat;
    }

    // Property names in the replies of a QMP session over stdio, e.g.
    // "honor-guest-pat" from qom-list-properties. Exposed for tests.
    [[nodiscard]] static QSet<QString> parseQomPropertyList(const QString &output);

    // Whether the host kernel can map the GPU memory of host blobs into a KVM
    // guest. QEMU documents Linux 6.13 as the minimum; before it, KVM could not
    // map the memory of every GPU driver.
    [[nodiscard]] static bool hostKernelSupportsGpuBlobs();

    // Whether KVM on this host can honor the guest's memory types when told
    // to (KVM_X86_QUIRK_IGNORE_GUEST_PAT, Linux 6.16). See hasKvmHonorGuestPat().
    [[nodiscard]] static bool hostKernelHonorsGuestPat();

    // Property names of a "-device <type>,help" listing, e.g. "blob" from
    // "  blob=<bool>  - on/off (default: false)". Exposed for tests.
    [[nodiscard]] static QSet<QString> parsePropertyHelp(const QString &output);

    // Whether the property listing of virtio-vga-gl allows blob=on with virgl.
    [[nodiscard]] static bool allowsVirglBlob(const QSet<QString> &properties);

    // False when neither probe produced a listing. The Settings page can use
    // this to say "capabilities unknown" instead of silently offering
    // everything.
    [[nodiscard]] bool probeSucceeded() const
    {
        return m_probeSucceeded;
    }

private:
    QemuCapabilities() = default;

    static constexpr int kProbeTimeoutMs = 4000;
    static constexpr int kProbeRetryTimeoutMs = 8000;
    static constexpr int kGpuBlobKernelMajor = 6;
    static constexpr int kGpuBlobKernelMinor = 13;
    static constexpr int kGuestPatKernelMajor = 6;
    static constexpr int kGuestPatKernelMinor = 16;

    static bool hostKernelAtLeast(int major, int minor);

    bool runProbes(const QString &qemuPath, int timeoutMs);
    void seedSelfContainedDefaults();
    static void startProbe(KProcess &process, const QString &qemuPath, const QStringList &arguments);
    static QString collectOutput(KProcess &process, int remainingMs);
    static QSet<QString> collectProbe(KProcess &process, int remainingMs);
    static QSet<QString> collectDeviceProbe(KProcess &process, int remainingMs);
    static bool collectExitStatus(KProcess &process, int remainingMs);

    QSet<QString> m_displays;
    QSet<QString> m_audiodevs;
    QSet<QString> m_devices;
    QSet<QString> m_chardevs;
    bool m_seccompSandbox = false;
    bool m_monitorObject = false;
    bool m_virtioGpuBlob = false;
    QString m_virglRenderServer;
    bool m_kvmHonorGuestPat = false;
    bool m_probeSucceeded = false;
};
