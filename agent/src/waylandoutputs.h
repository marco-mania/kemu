/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QPoint>
#include <QSize>

#include <memory>
#include <vector>

#include "displaylayout.h"

struct wl_display;
struct wl_output;
struct wl_registry;
struct zxdg_output_manager_v1;
struct zxdg_output_v1;
class QSocketNotifier;

// The compositor's outputs, over a Wayland connection of our own: wl_output
// for name, mode, transform and scale, xdg-output for the logical position
// and size. Our own connection works the same whether Qt runs on Wayland or,
// on GNOME, on Xwayland - whose RandR would report every monitor with one
// scale (section 11.5).
class WaylandOutputs : public DisplayLayoutSource
{
    Q_OBJECT

public:
    // nullptr without a Wayland compositor.
    static WaylandOutputs *create(QObject *parent = nullptr);
    ~WaylandOutputs() override;

    QList<OutputInfo> outputs() const override;
    QString sourceName() const override;

    struct Output {
        WaylandOutputs *owner = nullptr;
        uint32_t globalName = 0;
        uint32_t version = 0;
        wl_output *output = nullptr;
        zxdg_output_v1 *xdgOutput = nullptr;
        QString name;
        QPoint compositorPosition;
        int32_t transform = 0;
        int32_t integerScale = 1;
        QSize modeSize;
        int refreshMilliHz = 0;
        bool hasLogical = false;
        QPoint logicalPosition;
        QSize logicalSize;
    };

    // Called from the protocol listeners.
    void handleGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    void handleGlobalRemove(uint32_t name);
    void outputChanged();

private:
    explicit WaylandOutputs(wl_display *display, QObject *parent);
    bool init();
    void dispatch();
    void addXdgOutput(Output &output);
    void destroyOutput(Output &output);

    wl_display *m_display;
    wl_registry *m_registry = nullptr;
    zxdg_output_manager_v1 *m_xdgManager = nullptr;
    std::vector<std::unique_ptr<Output>> m_outputs;
    QSocketNotifier *m_notifier = nullptr;
    bool m_initialised = false;
};
