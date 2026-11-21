/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "graphicsinfo.h"

#include <QCborArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "drmdevice.h"
#include "logging.h"
#include "protocol.h"
#include "session.h"

namespace
{
// From drm/virtgpu_drm.h, which not every build environment has.
struct VirtgpuGetparam {
    quint64 param;
    quint64 value; // user pointer to an int
};
constexpr quint64 ParamSupportedCapsetIds = 7; // VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs
constexpr unsigned long IoctlGetparam = _IOWR('d', 0x40 + 0x03, VirtgpuGetparam); // DRM_IOCTL_VIRTGPU_GETPARAM
static_assert(sizeof(VirtgpuGetparam) == 16);

// Well below the 4096 bytes of section 4.2; the strings are short.
constexpr qsizetype MaxStringLength = 255;

QString cut(const QString &text)
{
    return text.left(MaxStringLength);
}
} // namespace

QString GraphicsQuery::virtioRenderNode(const QString &sysfsDrm, const QString &devDri)
{
    const QDir root(sysfsDrm);
    static const QRegularExpression renderPattern(u"^renderD\\d+$"_s);

    QStringList nodes;
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &entry : entries) {
        if (!renderPattern.match(entry).hasMatch())
            continue;
        if (DrmDevice::isVirtioGpu(root.filePath(entry)))
            nodes.append(entry);
    }
    // As for the heads: with several devices, nothing tells which one QEMU
    // shows.
    if (nodes.size() != 1)
        return {};
    return QDir(devDri).filePath(nodes.first());
}

QList<uint> GraphicsQuery::capsetIds(quint64 mask)
{
    QList<uint> ids;
    for (uint id = 0; id < 64; ++id) {
        if (mask & (quint64(1) << id))
            ids.append(id);
    }
    return ids;
}

QString GraphicsQuery::capsetNames(const QList<uint> &ids)
{
    // VIRTIO_GPU_CAPSET_* (section 7.5)
    static const QHash<uint, QString> names{{1, u"virgl"_s}, {2, u"virgl2"_s}, {3, u"gfxstream"_s}, {4, u"venus"_s}, {5, u"cross-domain"_s}, {6, u"drm"_s}};
    QStringList list;
    for (const uint id : ids)
        list.append(names.value(id, QString::number(id)));
    return list.isEmpty() ? u"none"_s : list.join(u", "_s);
}

std::optional<QList<uint>> GraphicsQuery::capsets(const QString &renderNode)
{
    if (renderNode.isEmpty())
        return std::nullopt;
    const int fd = ::open(QFile::encodeName(renderNode).constData(), O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        qCDebug(KEMU_AGENT) << "Graphics: cannot open" << renderNode;
        return std::nullopt;
    }
    int mask = 0;
    VirtgpuGetparam request{ParamSupportedCapsetIds, quint64(reinterpret_cast<quintptr>(&mask))};
    const int result = ::ioctl(fd, IoctlGetparam, &request);
    ::close(fd);
    // Linux before 5.16 does not know the parameter.
    if (result != 0)
        return std::nullopt;
    return capsetIds(quint32(mask));
}

QByteArray GraphicsQuery::probeOutput(const QString &renderer, const QString &vendor, const QString &version)
{
    const QJsonObject object{{u"renderer"_s, cut(renderer)}, {u"vendor"_s, cut(vendor)}, {u"version"_s, cut(version)}};
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

bool GraphicsQuery::parseProbeOutput(const QByteArray &output, GraphicsInfo *info)
{
    const QJsonDocument document = QJsonDocument::fromJson(output.trimmed());
    if (!document.isObject())
        return false;
    const QJsonObject object = document.object();
    const QString renderer = cut(object.value(u"renderer"_s).toString());
    if (renderer.isEmpty())
        return false;
    info->glRenderer = renderer;
    info->glVendor = cut(object.value(u"vendor"_s).toString());
    info->glVersion = cut(object.value(u"version"_s).toString());
    return true;
}

QStringList GraphicsQuery::systemLibraryDirs(const QString &libcPath)
{
    const QFileInfo libc(libcPath);
    if (libcPath.isEmpty() || !libc.isAbsolute())
        return {};
    const QString dir = libc.absolutePath();
    if (dir.startsWith(u"/lib"_s))
        return {u"/usr"_s + dir, dir};
    return {dir};
}

QString GraphicsQuery::probeLibraryPath(const QString &inherited, const QStringList &dirs)
{
    QStringList path = inherited.split(u':', Qt::SkipEmptyParts);
    for (const QString &dir : dirs) {
        if (!path.contains(dir))
            path.append(dir);
    }
    return path.join(u':');
}

QString GraphicsQuery::probeFailure(const QByteArray &errorOutput)
{
    const QList<QByteArray> lines = errorOutput.trimmed().split('\n');
    const QString last = QString::fromUtf8(lines.last()).trimmed();
    return cut(last.isEmpty() ? u"no reason given"_s : last);
}

QCborMap GraphicsQuery::message(const GraphicsInfo &info)
{
    QCborMap body;
    if (info.capsets) {
        QCborArray ids;
        for (const uint id : std::as_const(*info.capsets))
            ids.append(qint64(id));
        body.insert(u"capsets"_s, ids);
    }
    if (!info.glRenderer.isEmpty()) {
        QCborMap gl{{u"renderer"_s, info.glRenderer}};
        if (!info.glVendor.isEmpty())
            gl.insert(u"vendor"_s, info.glVendor);
        if (!info.glVersion.isEmpty())
            gl.insert(u"version"_s, info.glVersion);
        body.insert(u"gl"_s, gl);
    }
    return body;
}

GraphicsReporter::GraphicsReporter(Session *session, QObject *parent)
    : QObject(parent)
    , m_session(session)
{
    connect(m_session, &Session::ready, this, &GraphicsReporter::report);
    connect(m_session, &Session::reset, this, [this] {
        m_sentInSession = false;
    });
}

void GraphicsReporter::setInfo(const GraphicsInfo &info)
{
    m_info = info;
    m_sentInSession = false;
    report();
}

void GraphicsReporter::report()
{
    if (!m_info || m_sentInSession)
        return;
    if (m_session->state() != Session::State::Ready || !m_session->hasCapability(Protocol::Capability::DisplayGraphics))
        return;
    m_sentInSession = true;
    qCDebug(KEMU_AGENT) << "Graphics: reporting" << m_info->glRenderer;
    m_session->send(Protocol::GraphicsInfo, GraphicsQuery::message(*m_info));
}
