/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCborMap>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <optional>

using namespace Qt::StringLiterals;

class Session;

// What GRAPHICS_INFO reports (section 7.5).
struct GraphicsInfo {
    // Capability sets the virtio-gpu device offers (VIRTIO_GPU_CAPSET_*), as
    // the guest kernel lists them. Unset when it cannot tell: no virtio-gpu
    // device, or Linux before 5.16.
    std::optional<QList<uint>> capsets;
    // Of an OpenGL context on the device; empty when none could be made.
    QString glRenderer;
    QString glVendor;
    QString glVersion;

    bool operator==(const GraphicsInfo &) const = default;
};

namespace GraphicsQuery
{
// The render node of the virtio-gpu device, e.g. /dev/dri/renderD128, or an
// empty string unless there is exactly one.
QString virtioRenderNode(const QString &sysfsDrm = u"/sys/class/drm"_s, const QString &devDri = u"/dev/dri"_s);

// The IDs in a VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs mask, ascending.
QList<uint> capsetIds(quint64 mask);
// For the log, e.g. "virgl, virgl2, venus, drm"; "none" for an empty list.
QString capsetNames(const QList<uint> &ids);

// Asks the guest kernel which capability sets the device offers.
std::optional<QList<uint>> capsets(const QString &renderNode);

// What kemu-agent --graphics-probe prints, and reading it back. Strings are
// cut to a length the protocol allows.
QByteArray probeOutput(const QString &renderer, const QString &vendor, const QString &version);
bool parseProbeOutput(const QByteArray &output, GraphicsInfo *info);

// The system's library directories, from the path of its C library: with
// a /lib path also its /usr counterpart, where distributions without a
// merged /usr keep libstdc++ and Mesa.
QStringList systemLibraryDirs(const QString &libcPath);
// LD_LIBRARY_PATH for the probe: what the agent inherited, then @p dirs.
QString probeLibraryPath(const QString &inherited, const QStringList &dirs);
// The last line of the probe's error output, for the log.
QString probeFailure(const QByteArray &errorOutput);

QCborMap message(const GraphicsInfo &info);
}

// Sends GRAPHICS_INFO once per session, as soon as the information is there.
class GraphicsReporter : public QObject
{
    Q_OBJECT

public:
    explicit GraphicsReporter(Session *session, QObject *parent = nullptr);

    void setInfo(const GraphicsInfo &info);
    bool hasInfo() const
    {
        return m_info.has_value();
    }

private:
    void report();

    Session *m_session;
    std::optional<GraphicsInfo> m_info;
    bool m_sentInSession = false;
};
