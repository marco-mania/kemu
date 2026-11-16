/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QStringList>

#include <array>
#include <map>
#include <memory>
#include <optional>

#include "clipboardbackend.h"

class QSocketNotifier;
struct wl_display;
struct wl_registry;
struct wl_seat;

// The clipboard and primary selection of a Wayland session through
// ext-data-control or wlr-data-control, on a Wayland connection of its own:
// no window, no focus, and whatever platform Qt runs on.
//
// Reading another client's selection is synchronous (QMimeData has no other
// way), bounded by a timeout; our own selection is answered from the
// QMimeData we set, so the agent never waits for itself.
class DataControlBackend : public ClipboardBackend
{
    Q_OBJECT

public:
    enum class Variant {
        Ext, // ext_data_control_manager_v1
        Wlr, // zwlr_data_control_manager_v1
    };

    struct Limits {
        int readTimeoutMs = 2000; // without progress
        qsizetype maxRead = 64 * 1024 * 1024;
    };

    // nullptr when the compositor cannot be reached or lacks @p variant.
    static DataControlBackend *create(Variant variant, QObject *parent = nullptr);
    static DataControlBackend *create(Variant variant, const Limits &limits, QObject *parent = nullptr);
    ~DataControlBackend() override;

    QString name() const override;
    bool supportsPrimary() const override;
    const QMimeData *mimeData(::Protocol::Selection selection) const override;
    void setMimeData(::Protocol::Selection selection, QMimeData *data) override;
    void clear(::Protocol::Selection selection) override;

    // The protocol objects behind ext and wlr, which differ only in name.
    class Binding;
    // Another client's selection.
    class Offer;
    class OfferMimeData;

    // Called from the protocol listeners.
    void handleGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    void handleDataOffer(void *offer);
    void handleOfferType(void *offer, const char *type);
    void handleSelection(::Protocol::Selection selection, void *offer);
    void handleFinished();
    void handleSend(void *source, const char *type, int fd);
    void handleCancelled(void *source);

    // For OfferMimeData: the bytes of @p type, read through a pipe.
    std::optional<QByteArray> receive(void *offer, const QString &type) const;
    // Our own content as another client would read it.
    QByteArray bytesFor(const QMimeData &data, const QString &type) const;

Q_SIGNALS:
    // The compositor went away; the backend no longer works.
    void connectionLost();

private:
    DataControlBackend(Variant variant, wl_display *display, const Limits &limits, QObject *parent);
    bool init();
    void dispatch();
    void flush() const;
    void write(int fd, QByteArray bytes);

    struct SelectionState {
        void *ownSource = nullptr; // ours, until cancelled or replaced
        std::unique_ptr<Offer> offer; // another client's
        std::unique_ptr<OfferMimeData> offerData;
    };
    struct Writer {
        int fd = -1;
        QByteArray bytes;
        qsizetype written = 0;
        std::unique_ptr<QSocketNotifier> notifier;
    };

    SelectionState &state(::Protocol::Selection selection);
    const SelectionState &state(::Protocol::Selection selection) const;
    // Writes on as far as the pipe takes it; true when done.
    bool pump(Writer *writer);
    void finishWriter(Writer *writer);

    const Variant m_variant;
    const Limits m_limits;
    wl_display *m_display;
    wl_registry *m_registry = nullptr;
    wl_seat *m_seat = nullptr;
    uint32_t m_managerVersion = 0;
    std::unique_ptr<Binding> m_binding;
    QSocketNotifier *m_notifier = nullptr;
    bool m_lost = false;

    std::array<SelectionState, 2> m_selections;
    // Offers announced, not yet a selection.
    std::map<void *, std::unique_ptr<Offer>> m_newOffers;
    // Our sources, until cancelled: a replaced one may still be asked.
    std::map<void *, std::unique_ptr<QMimeData>> m_sources;
    std::map<Writer *, std::unique_ptr<Writer>> m_writers;
};
