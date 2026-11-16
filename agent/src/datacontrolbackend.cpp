/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "datacontrolbackend.h"

#include <QAbstractEventDispatcher>
#include <QBuffer>
#include <QCoreApplication>
#include <QImage>
#include <QImageReader>
#include <QImageWriter>
#include <QMimeData>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <wayland-client.h>

#include "logging.h"
#include "mimerules.h"
#include "wayland-ext-data-control-v1-client-protocol.h"
#include "wayland-wlr-data-control-unstable-v1-client-protocol.h"

using namespace Qt::StringLiterals;
using Selection = ::Protocol::Selection;

namespace
{
constexpr auto QtImage = "application/x-qt-image"_L1;
constexpr qsizetype ChunkSize = 64 * 1024;

bool isImageType(const QString &type)
{
    return type.startsWith(u"image/"_s);
}

// ext-data-control and wlr-data-control are the same protocol under two
// names (ext is the port of wlr to wayland-protocols). One set of wrappers
// for both, so the code below exists once.
#define KEMU_DATA_CONTROL_API(Api, p, managerDestroySince)                                                                                                     \
    struct Api {                                                                                                                                               \
        using Manager = p##_manager_v1;                                                                                                                        \
        using Device = p##_device_v1;                                                                                                                          \
        using Source = p##_source_v1;                                                                                                                          \
        using Offer = p##_offer_v1;                                                                                                                            \
        using DeviceListener = p##_device_v1_listener;                                                                                                         \
        using SourceListener = p##_source_v1_listener;                                                                                                         \
        using OfferListener = p##_offer_v1_listener;                                                                                                           \
        static constexpr uint32_t ManagerDestroySince = managerDestroySince;                                                                                   \
        static const wl_interface *managerInterface()                                                                                                          \
        {                                                                                                                                                      \
            return &p##_manager_v1_interface;                                                                                                                  \
        }                                                                                                                                                      \
        static Device *getDevice(Manager *manager, wl_seat *seat)                                                                                              \
        {                                                                                                                                                      \
            return p##_manager_v1_get_data_device(manager, seat);                                                                                              \
        }                                                                                                                                                      \
        static Source *createSource(Manager *manager)                                                                                                          \
        {                                                                                                                                                      \
            return p##_manager_v1_create_data_source(manager);                                                                                                 \
        }                                                                                                                                                      \
        static void destroyManager(Manager *manager)                                                                                                           \
        {                                                                                                                                                      \
            p##_manager_v1_destroy(manager);                                                                                                                   \
        }                                                                                                                                                      \
        static void addListener(Device *device, const DeviceListener *listener, void *data)                                                                    \
        {                                                                                                                                                      \
            p##_device_v1_add_listener(device, listener, data);                                                                                                \
        }                                                                                                                                                      \
        static void setSelection(Device *device, Source *source)                                                                                               \
        {                                                                                                                                                      \
            p##_device_v1_set_selection(device, source);                                                                                                       \
        }                                                                                                                                                      \
        static void setPrimarySelection(Device *device, Source *source)                                                                                        \
        {                                                                                                                                                      \
            p##_device_v1_set_primary_selection(device, source);                                                                                               \
        }                                                                                                                                                      \
        static void destroyDevice(Device *device)                                                                                                              \
        {                                                                                                                                                      \
            p##_device_v1_destroy(device);                                                                                                                     \
        }                                                                                                                                                      \
        static void addListener(Source *source, const SourceListener *listener, void *data)                                                                    \
        {                                                                                                                                                      \
            p##_source_v1_add_listener(source, listener, data);                                                                                                \
        }                                                                                                                                                      \
        static void offer(Source *source, const char *type)                                                                                                    \
        {                                                                                                                                                      \
            p##_source_v1_offer(source, type);                                                                                                                 \
        }                                                                                                                                                      \
        static void destroySource(Source *source)                                                                                                              \
        {                                                                                                                                                      \
            p##_source_v1_destroy(source);                                                                                                                     \
        }                                                                                                                                                      \
        static void addListener(Offer *offer, const OfferListener *listener, void *data)                                                                       \
        {                                                                                                                                                      \
            p##_offer_v1_add_listener(offer, listener, data);                                                                                                  \
        }                                                                                                                                                      \
        static void receive(Offer *offer, const char *type, int fd)                                                                                            \
        {                                                                                                                                                      \
            p##_offer_v1_receive(offer, type, fd);                                                                                                             \
        }                                                                                                                                                      \
        static void destroyOffer(Offer *offer)                                                                                                                 \
        {                                                                                                                                                      \
            p##_offer_v1_destroy(offer);                                                                                                                       \
        }                                                                                                                                                      \
    };

KEMU_DATA_CONTROL_API(ExtApi, ext_data_control, 1)
// wlr-data-control: the manager's destroy request came with version 2.
KEMU_DATA_CONTROL_API(WlrApi, zwlr_data_control, 2)
#undef KEMU_DATA_CONTROL_API

const wl_registry_listener s_registryListener = {
    [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
        static_cast<DataControlBackend *>(data)->handleGlobal(registry, name, interface, version);
    },
    [](void *, wl_registry *, uint32_t) {},
};
} // namespace

// =============================================================================
// Binding: the protocol objects of one variant
// =============================================================================

class DataControlBackend::Binding
{
public:
    virtual ~Binding() = default;
    virtual const char *interfaceName() const = 0;
    virtual void bindManager(wl_registry *registry, uint32_t name, uint32_t version) = 0;
    virtual bool hasManager() const = 0;
    virtual void createDevice(wl_seat *seat) = 0;
    virtual bool hasDevice() const = 0;
    virtual void destroyDevice() = 0;
    virtual void *createSource(const QStringList &types) = 0;
    virtual void destroySource(void *source) = 0;
    virtual void setSelection(Selection selection, void *source) = 0;
    virtual void receive(void *offer, const char *type, int fd) = 0;
    virtual void destroyOffer(void *offer) = 0;
};

namespace
{
template<typename Api>
class BindingImpl final : public DataControlBackend::Binding
{
public:
    BindingImpl(DataControlBackend *owner, uint32_t maxVersion)
        : m_owner(owner)
        , m_maxVersion(maxVersion)
    {
    }
    ~BindingImpl() override
    {
        destroyDevice();
        if (m_manager) {
            if (m_version >= Api::ManagerDestroySince)
                Api::destroyManager(m_manager);
            else
                wl_proxy_destroy(reinterpret_cast<wl_proxy *>(m_manager));
        }
    }

    const char *interfaceName() const override
    {
        return Api::managerInterface()->name;
    }
    void bindManager(wl_registry *registry, uint32_t name, uint32_t version) override
    {
        if (m_manager)
            return;
        m_version = std::min(version, m_maxVersion);
        m_manager = static_cast<typename Api::Manager *>(wl_registry_bind(registry, name, Api::managerInterface(), m_version));
    }
    bool hasManager() const override
    {
        return m_manager;
    }
    void createDevice(wl_seat *seat) override
    {
        m_device = Api::getDevice(m_manager, seat);
        Api::addListener(m_device, &s_deviceListener, this);
    }
    bool hasDevice() const override
    {
        return m_device;
    }
    void destroyDevice() override
    {
        if (m_device)
            Api::destroyDevice(m_device);
        m_device = nullptr;
    }
    void *createSource(const QStringList &types) override
    {
        typename Api::Source *source = Api::createSource(m_manager);
        Api::addListener(source, &s_sourceListener, this);
        for (const QString &type : types)
            Api::offer(source, type.toUtf8().constData());
        return source;
    }
    void destroySource(void *source) override
    {
        Api::destroySource(static_cast<typename Api::Source *>(source));
    }
    void setSelection(Selection selection, void *source) override
    {
        auto *typed = static_cast<typename Api::Source *>(source);
        if (selection == Selection::Primary)
            Api::setPrimarySelection(m_device, typed);
        else
            Api::setSelection(m_device, typed);
    }
    void receive(void *offer, const char *type, int fd) override
    {
        Api::receive(static_cast<typename Api::Offer *>(offer), type, fd);
    }
    void destroyOffer(void *offer) override
    {
        Api::destroyOffer(static_cast<typename Api::Offer *>(offer));
    }

private:
    static DataControlBackend *owner(void *data)
    {
        return static_cast<BindingImpl *>(data)->m_owner;
    }
    static void onDataOffer(void *data, typename Api::Device *, typename Api::Offer *offer)
    {
        Api::addListener(offer, &s_offerListener, data);
        owner(data)->handleDataOffer(offer);
    }
    static void onSelection(void *data, typename Api::Device *, typename Api::Offer *offer)
    {
        owner(data)->handleSelection(Selection::Clipboard, offer);
    }
    static void onFinished(void *data, typename Api::Device *)
    {
        owner(data)->handleFinished();
    }
    static void onPrimarySelection(void *data, typename Api::Device *, typename Api::Offer *offer)
    {
        owner(data)->handleSelection(Selection::Primary, offer);
    }
    static void onOffer(void *data, typename Api::Offer *offer, const char *type)
    {
        owner(data)->handleOfferType(offer, type);
    }
    static void onSend(void *data, typename Api::Source *source, const char *type, int32_t fd)
    {
        owner(data)->handleSend(source, type, fd);
    }
    static void onCancelled(void *data, typename Api::Source *source)
    {
        owner(data)->handleCancelled(source);
    }

    static constexpr typename Api::DeviceListener s_deviceListener = {onDataOffer, onSelection, onFinished, onPrimarySelection};
    static constexpr typename Api::SourceListener s_sourceListener = {onSend, onCancelled};
    static constexpr typename Api::OfferListener s_offerListener = {onOffer};

    DataControlBackend *const m_owner;
    const uint32_t m_maxVersion;
    uint32_t m_version = 0;
    typename Api::Manager *m_manager = nullptr;
    typename Api::Device *m_device = nullptr;
};
} // namespace

// =============================================================================
// Offers of other clients
// =============================================================================

class DataControlBackend::Offer
{
public:
    Offer(Binding *binding, void *handle)
        : binding(binding)
        , handle(handle)
    {
    }
    ~Offer()
    {
        binding->destroyOffer(handle);
    }
    Q_DISABLE_COPY_MOVE(Offer)

    Binding *const binding;
    void *const handle;
    QStringList types;
};

// What QMimeData asks for, read from the offer on demand. Text and image
// conversions as QMimeData expects them: text/plain from the UTF-8 type,
// application/x-qt-image from the best image type.
class DataControlBackend::OfferMimeData : public QMimeData
{
public:
    OfferMimeData(const DataControlBackend *backend, const Offer *offer)
        : m_backend(backend)
        , m_offer(offer)
    {
    }

    QStringList formats() const override
    {
        QStringList types = m_offer->types;
        if (!imageType().isEmpty() && !types.contains(QtImage))
            types.append(QtImage);
        return types;
    }

    bool hasFormat(const QString &type) const override
    {
        if (m_offer->types.contains(type))
            return true;
        if (type == MimeRules::PlainText)
            return m_offer->types.contains(MimeRules::Utf8Text);
        if (type == QtImage)
            return !imageType().isEmpty();
        return false;
    }

protected:
    QVariant retrieveData(const QString &type, QMetaType preferredType) const override
    {
        Q_UNUSED(preferredType)
        if (type == QtImage) {
            const QString source = imageType();
            const auto bytes = source.isEmpty() ? std::nullopt : read(source);
            return bytes ? QVariant(QImage::fromData(*bytes)) : QVariant();
        }
        if (type == MimeRules::PlainText) {
            const QString source = m_offer->types.contains(MimeRules::Utf8Text) ? QString(MimeRules::Utf8Text) : QString(MimeRules::PlainText);
            const auto bytes = m_offer->types.contains(source) ? read(source) : std::nullopt;
            return bytes ? QVariant(QString::fromUtf8(*bytes)) : QVariant();
        }
        if (!m_offer->types.contains(type))
            return {};
        const auto bytes = read(type);
        return bytes ? QVariant(*bytes) : QVariant();
    }

private:
    // PNG where offered, otherwise the first type Qt can decode.
    QString imageType() const
    {
        if (m_offer->types.contains(MimeRules::Png))
            return MimeRules::Png;
        const QList<QByteArray> readable = QImageReader::supportedMimeTypes();
        for (const QString &type : m_offer->types) {
            if (isImageType(type) && readable.contains(type.toLatin1()))
                return type;
        }
        return {};
    }

    // Each type crosses the pipe once.
    std::optional<QByteArray> read(const QString &type) const
    {
        const auto it = m_cache.constFind(type);
        if (it != m_cache.cend())
            return *it;
        const auto bytes = m_backend->receive(m_offer->handle, type);
        m_cache.insert(type, bytes);
        return bytes;
    }

    const DataControlBackend *const m_backend;
    const Offer *const m_offer;
    mutable QHash<QString, std::optional<QByteArray>> m_cache;
};

// =============================================================================
// DataControlBackend
// =============================================================================

DataControlBackend *DataControlBackend::create(Variant variant, QObject *parent)
{
    return create(variant, Limits(), parent);
}

DataControlBackend *DataControlBackend::create(Variant variant, const Limits &limits, QObject *parent)
{
    wl_display *display = wl_display_connect(nullptr);
    if (!display) {
        qCWarning(KEMU_AGENT) << "Clipboard: cannot connect to the Wayland compositor";
        return nullptr;
    }
    auto *backend = new DataControlBackend(variant, display, limits, parent);
    if (!backend->init()) {
        delete backend;
        return nullptr;
    }
    return backend;
}

DataControlBackend::DataControlBackend(Variant variant, wl_display *display, const Limits &limits, QObject *parent)
    : ClipboardBackend(parent)
    , m_variant(variant)
    , m_limits(limits)
    , m_display(display)
{
    if (variant == Variant::Ext)
        m_binding = std::make_unique<BindingImpl<ExtApi>>(this, 1);
    else
        m_binding = std::make_unique<BindingImpl<WlrApi>>(this, 2);
}

DataControlBackend::~DataControlBackend()
{
    delete m_notifier;
    for (auto &[raw, writer] : m_writers)
        ::close(writer->fd);
    m_writers.clear();
    for (SelectionState &s : m_selections) {
        s.offerData.reset();
        s.offer.reset();
    }
    m_newOffers.clear();
    for (auto &[source, data] : m_sources)
        m_binding->destroySource(source);
    m_sources.clear();
    m_binding.reset();
    if (m_seat)
        wl_seat_destroy(m_seat);
    if (m_registry)
        wl_registry_destroy(m_registry);
    wl_display_flush(m_display);
    wl_display_disconnect(m_display);
}

bool DataControlBackend::init()
{
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &s_registryListener, this);
    if (wl_display_roundtrip(m_display) < 0)
        return false;
    if (!m_binding->hasManager()) {
        qCWarning(KEMU_AGENT) << "Clipboard: the compositor does not offer" << m_binding->interfaceName();
        return false;
    }
    if (!m_seat) {
        qCWarning(KEMU_AGENT) << "Clipboard: the compositor has no seat";
        return false;
    }
    m_binding->createDevice(m_seat);
    // The current selections.
    if (wl_display_roundtrip(m_display) < 0)
        return false;

    m_notifier = new QSocketNotifier(wl_display_get_fd(m_display), QSocketNotifier::Read, this);
    connect(m_notifier, &QSocketNotifier::activated, this, &DataControlBackend::dispatch);
    // Events read along with others must not wait for the next wakeup.
    if (auto *dispatcher = QCoreApplication::eventDispatcher()) {
        connect(dispatcher, &QAbstractEventDispatcher::aboutToBlock, this, [this] {
            if (m_lost)
                return;
            wl_display_dispatch_pending(m_display);
            wl_display_flush(m_display);
        });
    }
    return true;
}

void DataControlBackend::dispatch()
{
    if (m_lost)
        return;
    while (wl_display_prepare_read(m_display) != 0)
        wl_display_dispatch_pending(m_display);
    wl_display_flush(m_display);
    if (wl_display_read_events(m_display) < 0 || wl_display_dispatch_pending(m_display) < 0) {
        qCWarning(KEMU_AGENT) << "Clipboard: lost the Wayland connection";
        m_lost = true;
        m_notifier->setEnabled(false);
        Q_EMIT connectionLost();
        return;
    }
    wl_display_flush(m_display);
}

void DataControlBackend::flush() const
{
    if (!m_lost)
        wl_display_flush(m_display);
}

QString DataControlBackend::name() const
{
    return m_variant == Variant::Ext ? u"ext-data-control"_s : u"wlr-data-control"_s;
}

bool DataControlBackend::supportsPrimary() const
{
    // wlr-data-control has the primary selection from version 2 on.
    return m_variant == Variant::Ext || m_managerVersion >= 2;
}

DataControlBackend::SelectionState &DataControlBackend::state(Selection selection)
{
    return m_selections[selection == Selection::Primary ? 1 : 0];
}

const DataControlBackend::SelectionState &DataControlBackend::state(Selection selection) const
{
    return m_selections[selection == Selection::Primary ? 1 : 0];
}

const QMimeData *DataControlBackend::mimeData(Selection selection) const
{
    const SelectionState &s = state(selection);
    if (s.ownSource) {
        const auto it = m_sources.find(s.ownSource);
        return it != m_sources.end() ? it->second.get() : nullptr;
    }
    return s.offerData.get();
}

void DataControlBackend::setMimeData(Selection selection, QMimeData *data)
{
    std::unique_ptr<QMimeData> owned(data);
    if (m_lost || !m_binding->hasDevice() || (selection == Selection::Primary && !supportsPrimary()))
        return;

    // What we offer: the types as set, the UTF-8 text type for text, and
    // every format Qt can write for an image.
    QStringList types;
    for (const QString &type : data->formats()) {
        if (type != QtImage && !types.contains(type))
            types.append(type);
    }
    if (data->hasText()) {
        for (const QString &type : {QString(MimeRules::Utf8Text), QString(MimeRules::PlainText)}) {
            if (!types.contains(type))
                types.append(type);
        }
    }
    if (data->hasImage()) {
        for (const QByteArray &type : QImageWriter::supportedMimeTypes()) {
            const QString name = QString::fromLatin1(type);
            if (!types.contains(name))
                types.append(name);
        }
    }

    void *source = m_binding->createSource(types);
    m_sources[source] = std::move(owned);
    SelectionState &s = state(selection);
    s.ownSource = source;
    // Whatever another client held there is no longer the selection.
    s.offerData.reset();
    s.offer.reset();
    m_binding->setSelection(selection, source);
    flush();
}

void DataControlBackend::clear(Selection selection)
{
    if (m_lost || !m_binding->hasDevice() || (selection == Selection::Primary && !supportsPrimary()))
        return;
    // The compositor cancels our source; until then it may still be asked.
    state(selection).ownSource = nullptr;
    m_binding->setSelection(selection, nullptr);
    flush();
}

void DataControlBackend::handleGlobal(wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    if (std::strcmp(interface, m_binding->interfaceName()) == 0) {
        m_binding->bindManager(registry, name, version);
        m_managerVersion = std::min<uint32_t>(version, m_variant == Variant::Ext ? 1 : 2);
    } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !m_seat) {
        // The first seat; virtual machines have one.
        m_seat = static_cast<wl_seat *>(wl_registry_bind(registry, name, &wl_seat_interface, 1));
    }
}

void DataControlBackend::handleDataOffer(void *offer)
{
    m_newOffers[offer] = std::make_unique<Offer>(m_binding.get(), offer);
}

void DataControlBackend::handleOfferType(void *offer, const char *type)
{
    const QString name = QString::fromUtf8(type);
    const auto it = m_newOffers.find(offer);
    if (it != m_newOffers.end()) {
        it->second->types.append(name);
        return;
    }
    for (SelectionState &s : m_selections) {
        if (s.offer && s.offer->handle == offer)
            s.offer->types.append(name);
    }
}

void DataControlBackend::handleSelection(Selection selection, void *offerHandle)
{
    std::unique_ptr<Offer> offer;
    if (offerHandle) {
        const auto it = m_newOffers.find(offerHandle);
        if (it != m_newOffers.end()) {
            offer = std::move(it->second);
            m_newOffers.erase(it);
        }
    }

    SelectionState &s = state(selection);
    // The selection is our own source: nothing new, and nothing to read
    // through a pipe from ourselves. The offer for it is dropped.
    if (s.ownSource)
        return;

    s.offerData.reset();
    s.offer = std::move(offer);
    if (s.offer)
        s.offerData = std::make_unique<OfferMimeData>(this, s.offer.get());
    Q_EMIT changed(selection);
}

void DataControlBackend::handleFinished()
{
    qCWarning(KEMU_AGENT) << "Clipboard: the compositor ended data-control for this seat";
    for (SelectionState &s : m_selections) {
        s.offerData.reset();
        s.offer.reset();
        s.ownSource = nullptr;
    }
    m_binding->destroyDevice();
    Q_EMIT connectionLost();
}

void DataControlBackend::handleCancelled(void *source)
{
    for (SelectionState &s : m_selections) {
        if (s.ownSource == source)
            s.ownSource = nullptr;
    }
    m_sources.erase(source);
    m_binding->destroySource(source);
}

void DataControlBackend::handleSend(void *source, const char *type, int fd)
{
    const auto it = m_sources.find(source);
    if (it == m_sources.end()) {
        ::close(fd);
        return;
    }
    write(fd, bytesFor(*it->second, QString::fromUtf8(type)));
}

QByteArray DataControlBackend::bytesFor(const QMimeData &data, const QString &type) const
{
    if (type != QtImage && data.formats().contains(type))
        return data.data(type);
    if ((type == MimeRules::Utf8Text || type == MimeRules::PlainText) && data.hasText())
        return data.text().toUtf8();
    if (isImageType(type) && data.hasImage()) {
        const QImage image = qvariant_cast<QImage>(data.imageData());
        const QList<QByteArray> formats = QImageWriter::imageFormatsForMimeType(type.toLatin1());
        if (image.isNull() || formats.isEmpty())
            return {};
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, formats.first().constData()))
            return {};
        return bytes;
    }
    return {};
}

void DataControlBackend::write(int fd, QByteArray bytes)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    auto writer = std::make_unique<Writer>();
    writer->fd = fd;
    writer->bytes = std::move(bytes);
    Writer *raw = writer.get();
    m_writers[raw] = std::move(writer);
    if (pump(raw))
        return;
    // The reader takes its time: go on whenever the pipe has room.
    raw->notifier = std::make_unique<QSocketNotifier>(fd, QSocketNotifier::Write);
    connect(raw->notifier.get(), &QSocketNotifier::activated, this, [this, raw] {
        pump(raw);
    });
}

bool DataControlBackend::pump(Writer *writer)
{
    while (writer->written < writer->bytes.size()) {
        const qsizetype chunk = std::min(ChunkSize, writer->bytes.size() - writer->written);
        const ssize_t n = ::write(writer->fd, writer->bytes.constData() + writer->written, size_t(chunk));
        if (n > 0) {
            writer->written += n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && errno == EAGAIN) // EWOULDBLOCK on Linux
            return false;
        // The reader went away (EPIPE) or something else broke.
        break;
    }
    finishWriter(writer);
    return true;
}

void DataControlBackend::finishWriter(Writer *writer)
{
    ::close(writer->fd);
    const auto it = m_writers.find(writer);
    if (it == m_writers.end())
        return;
    // Possibly called from the notifier's own signal.
    if (QSocketNotifier *notifier = it->second->notifier.release()) {
        notifier->setEnabled(false);
        notifier->deleteLater();
    }
    m_writers.erase(it);
}

std::optional<QByteArray> DataControlBackend::receive(void *offer, const QString &type) const
{
    if (m_lost)
        return std::nullopt;
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0)
        return std::nullopt;
    m_binding->receive(offer, type.toUtf8().constData(), fds[1]);
    ::close(fds[1]);
    flush();

    // The other client writes straight into the pipe; nothing of ours needs
    // dispatching meanwhile, and nothing is, so the offer stays valid.
    QByteArray bytes;
    char buffer[ChunkSize];
    for (;;) {
        pollfd pfd{fds[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, m_limits.readTimeoutMs);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready <= 0) {
            qCWarning(KEMU_AGENT) << "Clipboard: no data for" << type << "within" << m_limits.readTimeoutMs << "ms";
            ::close(fds[0]);
            return std::nullopt;
        }
        const ssize_t n = ::read(fds[0], buffer, sizeof buffer);
        if (n < 0 && (errno == EINTR || errno == EAGAIN))
            continue;
        if (n < 0) {
            ::close(fds[0]);
            return std::nullopt;
        }
        if (n == 0)
            break;
        if (bytes.size() + n > m_limits.maxRead) {
            qCWarning(KEMU_AGENT) << "Clipboard:" << type << "is larger than" << m_limits.maxRead << "bytes, not read";
            ::close(fds[0]);
            return std::nullopt;
        }
        bytes.append(buffer, n);
    }
    ::close(fds[0]);
    return bytes;
}
