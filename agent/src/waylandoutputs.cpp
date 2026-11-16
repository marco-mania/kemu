/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "waylandoutputs.h"

#include <QAbstractEventDispatcher>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <cmath>

#include <wayland-client.h>

#include "wayland-xdg-output-unstable-v1-client-protocol.h"

#include "logging.h"

using namespace Qt::StringLiterals;

namespace
{
constexpr uint32_t kOutputVersion = 4; // name event
constexpr uint32_t kXdgOutputManagerVersion = 3;

using Output = WaylandOutputs::Output;

void registryGlobal(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    static_cast<WaylandOutputs *>(data)->handleGlobal(registry, name, interface, version);
}

void registryGlobalRemove(void *data, wl_registry *, uint32_t name)
{
    static_cast<WaylandOutputs *>(data)->handleGlobalRemove(name);
}

const wl_registry_listener s_registryListener = {registryGlobal, registryGlobalRemove};

void outputGeometry(void *data, wl_output *, int32_t x, int32_t y, int32_t, int32_t, int32_t, const char *, const char *, int32_t transform)
{
    auto *output = static_cast<Output *>(data);
    output->compositorPosition = QPoint(x, y);
    output->transform = transform;
}

void outputMode(void *data, wl_output *, uint32_t flags, int32_t width, int32_t height, int32_t refresh)
{
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        auto *output = static_cast<Output *>(data);
        output->modeSize = QSize(width, height);
        output->refreshMilliHz = refresh;
    }
}

void outputDone(void *data, wl_output *)
{
    static_cast<Output *>(data)->owner->outputChanged();
}

void outputScale(void *data, wl_output *, int32_t factor)
{
    static_cast<Output *>(data)->integerScale = std::max(factor, 1);
}

void outputName(void *data, wl_output *, const char *name)
{
    static_cast<Output *>(data)->name = QString::fromUtf8(name);
}

void outputDescription(void *, wl_output *, const char *)
{
}

const wl_output_listener s_outputListener = {outputGeometry, outputMode, outputDone, outputScale, outputName, outputDescription};

void xdgLogicalPosition(void *data, zxdg_output_v1 *, int32_t x, int32_t y)
{
    auto *output = static_cast<Output *>(data);
    output->logicalPosition = QPoint(x, y);
    output->hasLogical = true;
}

void xdgLogicalSize(void *data, zxdg_output_v1 *, int32_t width, int32_t height)
{
    static_cast<Output *>(data)->logicalSize = QSize(width, height);
}

void xdgDone(void *data, zxdg_output_v1 *)
{
    // Only sent before version 3; wl_output.done covers both since.
    static_cast<Output *>(data)->owner->outputChanged();
}

void xdgName(void *data, zxdg_output_v1 *, const char *name)
{
    auto *output = static_cast<Output *>(data);
    if (output->name.isEmpty()) // wl_output v4 has the authoritative one
        output->name = QString::fromUtf8(name);
}

void xdgDescription(void *, zxdg_output_v1 *, const char *)
{
}

const zxdg_output_v1_listener s_xdgOutputListener = {xdgLogicalPosition, xdgLogicalSize, xdgDone, xdgName, xdgDescription};

bool swapsAxes(int32_t transform)
{
    return transform == WL_OUTPUT_TRANSFORM_90 || transform == WL_OUTPUT_TRANSFORM_270 || transform == WL_OUTPUT_TRANSFORM_FLIPPED_90
        || transform == WL_OUTPUT_TRANSFORM_FLIPPED_270;
}

QString transformName(int32_t transform)
{
    switch (transform) {
    case WL_OUTPUT_TRANSFORM_90:
        return u"90"_s;
    case WL_OUTPUT_TRANSFORM_180:
        return u"180"_s;
    case WL_OUTPUT_TRANSFORM_270:
        return u"270"_s;
    case WL_OUTPUT_TRANSFORM_FLIPPED:
        return u"flipped"_s;
    case WL_OUTPUT_TRANSFORM_FLIPPED_90:
        return u"flipped-90"_s;
    case WL_OUTPUT_TRANSFORM_FLIPPED_180:
        return u"flipped-180"_s;
    case WL_OUTPUT_TRANSFORM_FLIPPED_270:
        return u"flipped-270"_s;
    default:
        return u"normal"_s;
    }
}
} // namespace

WaylandOutputs *WaylandOutputs::create(QObject *parent)
{
    if (qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") && qEnvironmentVariableIsEmpty("WAYLAND_SOCKET"))
        return nullptr;
    wl_display *display = wl_display_connect(nullptr);
    if (!display)
        return nullptr;
    auto *outputs = new WaylandOutputs(display, parent);
    if (!outputs->init()) {
        delete outputs;
        return nullptr;
    }
    return outputs;
}

WaylandOutputs::WaylandOutputs(wl_display *display, QObject *parent)
    : DisplayLayoutSource(parent)
    , m_display(display)
{
}

WaylandOutputs::~WaylandOutputs()
{
    delete m_notifier;
    for (auto &output : m_outputs)
        destroyOutput(*output);
    m_outputs.clear();
    if (m_xdgManager)
        zxdg_output_manager_v1_destroy(m_xdgManager);
    if (m_registry)
        wl_registry_destroy(m_registry);
    wl_display_disconnect(m_display);
}

bool WaylandOutputs::init()
{
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registryListener, this);

    // 1: globals. The xdg-output manager may come after the outputs.
    if (wl_display_roundtrip(m_display) < 0)
        return false;
    if (m_xdgManager) {
        for (auto &output : m_outputs)
            addXdgOutput(*output);
    }
    // 2: the outputs' own events.
    if (wl_display_roundtrip(m_display) < 0)
        return false;

    m_initialised = true;
    if (!m_xdgManager)
        qCInfo(KEMU_AGENT) << "Displays: the compositor has no xdg-output; positions may be off with scaling";

    m_notifier = new QSocketNotifier(wl_display_get_fd(m_display), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &WaylandOutputs::dispatch);
    // Events read along with others must not wait for the next wakeup.
    if (auto *dispatcher = QCoreApplication::eventDispatcher()) {
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            wl_display_dispatch_pending(m_display);
            wl_display_flush(m_display);
        });
    }
    return true;
}

void WaylandOutputs::dispatch()
{
    while (wl_display_prepare_read(m_display) != 0)
        wl_display_dispatch_pending(m_display);
    wl_display_flush(m_display);
    if (wl_display_read_events(m_display) < 0) {
        qCWarning(KEMU_AGENT) << "Displays: lost the Wayland connection";
        m_notifier->setEnabled(false);
        return;
    }
    wl_display_dispatch_pending(m_display);
    wl_display_flush(m_display);
}

void WaylandOutputs::handleGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    if (qstrcmp(interface, wl_output_interface.name) == 0) {
        auto output = std::make_unique<Output>();
        output->owner = this;
        output->globalName = name;
        output->version = std::min(version, kOutputVersion);
        output->output = static_cast<wl_output *>(wl_registry_bind(registry, name, &wl_output_interface, output->version));
        wl_output_add_listener(output->output, &s_outputListener, output.get());
        m_outputs.push_back(std::move(output));
        if (m_initialised && m_xdgManager)
            addXdgOutput(*m_outputs.back());
    } else if (qstrcmp(interface, zxdg_output_manager_v1_interface.name) == 0 && !m_xdgManager) {
        m_xdgManager = static_cast<zxdg_output_manager_v1 *>(
            wl_registry_bind(registry, name, &zxdg_output_manager_v1_interface, std::min(version, kXdgOutputManagerVersion)));
    }
}

void WaylandOutputs::handleGlobalRemove(uint32_t name)
{
    for (auto it = m_outputs.begin(); it != m_outputs.end(); ++it) {
        if ((*it)->globalName == name) {
            destroyOutput(**it);
            m_outputs.erase(it);
            outputChanged();
            return;
        }
    }
}

void WaylandOutputs::addXdgOutput(Output &output)
{
    if (output.xdgOutput)
        return;
    output.xdgOutput = zxdg_output_manager_v1_get_xdg_output(m_xdgManager, output.output);
    zxdg_output_v1_add_listener(output.xdgOutput, &s_xdgOutputListener, &output);
}

void WaylandOutputs::destroyOutput(Output &output)
{
    if (output.xdgOutput)
        zxdg_output_v1_destroy(output.xdgOutput);
    if (output.output) {
        if (output.version >= WL_OUTPUT_RELEASE_SINCE_VERSION)
            wl_output_release(output.output);
        else
            wl_output_destroy(output.output);
    }
    output.xdgOutput = nullptr;
    output.output = nullptr;
}

void WaylandOutputs::outputChanged()
{
    if (m_initialised)
        Q_EMIT changed();
}

QString WaylandOutputs::sourceName() const
{
    return m_xdgManager ? u"xdg-output"_s : u"wayland"_s;
}

QList<OutputInfo> WaylandOutputs::outputs() const
{
    QList<OutputInfo> result;
    for (const auto &output : m_outputs) {
        if (!output->modeSize.isValid())
            continue;
        OutputInfo info;
        info.name = output->name;
        info.modeSize = output->modeSize;
        info.refreshMilliHz = output->refreshMilliHz;
        info.transform = transformName(output->transform);

        // What the mode covers once rotated.
        const QSize rotated = swapsAxes(output->transform) ? output->modeSize.transposed() : output->modeSize;
        if (output->hasLogical && output->logicalSize.width() > 0 && output->logicalSize.height() > 0) {
            info.geometry = QRect(output->logicalPosition, output->logicalSize);
            info.scale = std::round(double(rotated.width()) / output->logicalSize.width() * 1000.0) / 1000.0;
        } else {
            info.scale = output->integerScale;
            info.geometry = QRect(output->compositorPosition, rotated / output->integerScale);
        }
        result.append(info);
    }
    return result;
}
