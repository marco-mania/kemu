/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphicsstatus.h"

#include <QStringList>

using namespace Qt::StringLiterals;

namespace
{
constexpr qsizetype MaxShownLength = 120;
} // namespace

GraphicsStatus::OpenGl classifyRenderer(const QString &renderer, bool nativeContextOffered)
{
    const QString text = renderer.trimmed();
    if (text.isEmpty())
        return GraphicsStatus::OpenGl::Unknown;
    // Mesa: "virgl (Mesa Intel(R) …)", "zink Vulkan 1.3(Virtio-GPU Venus …)",
    // "llvmpipe (LLVM …)".
    if (text.startsWith(u"virgl"_s, Qt::CaseInsensitive))
        return GraphicsStatus::OpenGl::Virgl;
    if (text.startsWith(u"zink"_s, Qt::CaseInsensitive))
        return GraphicsStatus::OpenGl::Zink;
    static const QStringList software{u"llvmpipe"_s, u"softpipe"_s, u"swrast"_s, u"lavapipe"_s, u"Software Rasterizer"_s};
    for (const QString &name : software) {
        if (text.contains(name, Qt::CaseInsensitive))
            return GraphicsStatus::OpenGl::Software;
    }
    // Any other driver is a real GPU's. Inside virtio-gpu, that takes a
    // native context.
    return nativeContextOffered ? GraphicsStatus::OpenGl::NativeContext : GraphicsStatus::OpenGl::Other;
}

GraphicsStatus evaluateGraphics(const GraphicsRequest &request, const std::optional<KemuAgentProtocol::GuestGraphics> &guest)
{
    GraphicsStatus status;
    if (!guest)
        return status;

    if (guest->capsets) {
        status.venusOffered = guest->hasCapset(KemuAgentProtocol::Capset::Venus);
        status.nativeContextOffered = guest->hasCapset(KemuAgentProtocol::Capset::Drm);
    }
    status.renderer = plainGuestText(guest->glRenderer);
    status.openGl = classifyRenderer(status.renderer, status.nativeContextOffered.value_or(false));

    if (request.acceleration && status.openGl == GraphicsStatus::OpenGl::Software)
        status.warning = true;
    // Asked for, but the host leaves it out, or the guest does not use it.
    if (request.nativeContext
        && (status.nativeContextOffered == false || (status.nativeContextOffered == true && status.openGl == GraphicsStatus::OpenGl::Virgl)))
        status.warning = true;
    if (request.vulkan && status.venusOffered == false)
        status.warning = true;
    return status;
}

QString plainGuestText(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (const QChar c : text) {
        if (c.isSpace())
            result += u' ';
        // Without angle brackets, Qt never takes the text for markup.
        else if (c.isPrint() && c != u'<' && c != u'>')
            result += c;
    }
    result = result.simplified();
    if (result.size() > MaxShownLength)
        result = result.left(MaxShownLength - 1) + u'…';
    return result;
}
