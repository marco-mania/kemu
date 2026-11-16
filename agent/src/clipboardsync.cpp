/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clipboardsync.h"

#include <QBuffer>
#include <QCborArray>
#include <QImage>
#include <QMimeData>
#include <QScopeGuard>
#include <QStandardPaths>

#include "clipboardbackend.h"
#include "logging.h"
#include "mimerules.h"
#include "transport.h"

using Protocol::Selection;

namespace
{
constexpr std::array kSelections{Selection::Clipboard, Selection::Primary};
// What file managers of GTK desktops read when files are pasted.
constexpr auto GnomeCopiedFiles = "x-special/gnome-copied-files"_L1;

const char *selectionName(Selection selection)
{
    return selection == Selection::Primary ? "PRIMARY" : "CLIPBOARD";
}

qint64 toCbor(quint64 value)
{
    return qint64(value);
}
} // namespace

ClipboardSync::ClipboardSync(Session *session, ClipboardBackend *backend, const Config &config, QObject *parent)
    : QObject(parent)
    , m_session(session)
    , m_backend(backend)
    , m_config(config)
{
    for (const Selection selection : kSelections) {
        QTimer &settle = state(selection).settle;
        settle.setSingleShot(true);
        settle.setInterval(selection == Selection::Primary ? m_config.primarySettleMs : m_config.settleMs);
        connect(&settle, &QTimer::timeout, this, [this, selection] {
            examineLocal(selection);
        });
    }
    m_timeouts.setInterval(std::max(100, m_config.requestTimeoutMs / 10));
    connect(&m_timeouts, &QTimer::timeout, this, &ClipboardSync::checkTimeouts);

    connect(m_backend, &ClipboardBackend::changed, this, &ClipboardSync::onLocalChanged);
    connect(m_session, &Session::policyChanged, this, &ClipboardSync::onPolicyChanged);
    connect(m_session, &Session::reset, this, &ClipboardSync::onReset);
    connect(m_session, &Session::message, this, &ClipboardSync::onMessage);
    connect(m_session->transport(), &Transport::drained, this, &ClipboardSync::pumpOutbound);
}

ClipboardSync::ClipboardSync(Session *session, ClipboardBackend *backend, QObject *parent)
    : ClipboardSync(session, backend, Config{}, parent)
{
}

ClipboardSync::State &ClipboardSync::state(Selection selection)
{
    return m_states[int(selection)];
}

const ClipboardSync::State &ClipboardSync::state(Selection selection) const
{
    return m_states[int(selection)];
}

ClipboardSync::Owner ClipboardSync::owner(Selection selection) const
{
    return state(selection).owner;
}

quint64 ClipboardSync::localSerial(Selection selection) const
{
    return state(selection).localSerial;
}

quint64 ClipboardSync::peerSerial(Selection selection) const
{
    return state(selection).peerSerial;
}

QStringList ClipboardSync::offeredTypes(Selection selection) const
{
    return state(selection).offeredTypes;
}

bool ClipboardSync::isFetching(Selection selection) const
{
    return state(selection).fetch.active;
}

bool ClipboardSync::offersFiles(Selection selection) const
{
    return state(selection).listing.has_value();
}

QString ClipboardSync::stagedDirectory(Selection selection) const
{
    return state(selection).stagedDirectory;
}

bool ClipboardSync::mayOfferFiles(Selection selection, const Session::Policy &policy) const
{
    return mayOffer(selection, policy) && m_session->hasCapability(Protocol::Capability::ClipboardFiles) && policy.files && policy.filesLimit > 0;
}

bool ClipboardSync::acceptsFiles(Selection selection, const Session::Policy &policy) const
{
    return acceptsPeer(selection, policy) && m_session->hasCapability(Protocol::Capability::ClipboardFiles) && policy.files && policy.filesLimit > 0;
}

ClipboardFiles::Staging *ClipboardSync::staging()
{
    if (!m_staging) {
        const QString root = m_config.stagingRoot.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/kemu-agent/clipboard"_s
            : m_config.stagingRoot;
        m_staging = std::make_unique<ClipboardFiles::Staging>(root, m_config.stagingGraceMs);
        if (!m_staging->isUsable())
            qCWarning(KEMU_AGENT) << "Clipboard: cannot use" << root << "for files from the host";
    }
    return m_staging.get();
}

// A receiver may be the sender of the signal that brings us here.
void ClipboardSync::dropFiles(Fetch &fetch, bool discard)
{
    if (!fetch.files)
        return;
    ClipboardFiles::Receiver *receiver = fetch.files.release();
    receiver->disconnect(this);
    receiver->cancel();
    if (discard && m_staging)
        m_staging->discard(receiver->directory());
    receiver->deleteLater();
    fetch.filesPending = false;
}

ClipboardFiles::Receiver *ClipboardSync::receiverOf(quint64 id) const
{
    for (const State &s : m_states) {
        if (s.fetch.files && s.fetch.files->owns(id))
            return s.fetch.files.get();
    }
    return nullptr;
}

int ClipboardSync::openFileRequests() const
{
    int open = 0;
    for (const State &s : m_states) {
        if (s.fetch.files && s.fetch.filesPending)
            open += m_config.fileWindow;
    }
    return open;
}

bool ClipboardSync::mayOffer(Selection selection, const Session::Policy &policy) const
{
    if (m_session->state() != Session::State::Ready || !policy.toHost)
        return false;
    if (selection == Selection::Clipboard)
        return m_session->hasCapability(Protocol::Capability::Clipboard);
    return m_session->hasCapability(Protocol::Capability::ClipboardPrimary) && policy.primary && m_backend->supportsPrimary();
}

bool ClipboardSync::acceptsPeer(Selection selection, const Session::Policy &policy) const
{
    if (m_session->state() != Session::State::Ready)
        return false;
    if (selection == Selection::Clipboard)
        return m_session->hasCapability(Protocol::Capability::Clipboard);
    return m_session->hasCapability(Protocol::Capability::ClipboardPrimary) && policy.primary && m_backend->supportsPrimary();
}

// =============================================================================
// Session events
// =============================================================================

void ClipboardSync::onPolicyChanged(const Session::Policy &previous)
{
    const Session::Policy &current = m_session->policy();
    for (const Selection selection : kSelections) {
        State &s = state(selection);

        const bool could = mayOffer(selection, previous);
        const bool can = mayOffer(selection, current);
        // Files switched on or off, or another limit: the offer in force
        // may gain or lose its files.
        const bool filesChanged = (previous.files != current.files || previous.filesLimit != current.filesLimit)
            && (s.listing || !localFilePaths(m_backend->mimeData(selection)).isEmpty());
        if (can && (!could || filesChanged)) {
            // Offer what the guest holds right away (section 5.5).
            examineLocal(selection);
        } else if (!can && could && s.owner == Owner::Local) {
            // The host drops our offer on its own.
            s.owner = Owner::None;
            s.offeredTypes.clear();
            s.cache.clear();
            s.listing.reset();
        }

        if (!acceptsPeer(selection, current) && s.owner == Owner::Peer) {
            cancelFetch(selection);
            s.owner = Owner::None;
            s.peerTypes.clear();
        } else if (s.fetch.filesPending && !acceptsFiles(selection, current)) {
            fileFetchEnded(selection, false);
        }
    }
}

void ClipboardSync::onReset()
{
    for (const Selection selection : kSelections) {
        State &s = state(selection);
        s.localSerial = 0;
        s.peerSerial = 0;
        s.owner = Owner::None;
        s.offeredTypes.clear();
        s.cache.clear();
        s.listing.reset();
        s.peerTypes.clear();
        dropFiles(s.fetch, true);
        s.fetch = Fetch();
        if (m_staging)
            m_staging->retire(s.stagedDirectory);
        s.stagedDirectory.clear();
    }
    m_inbound.clear();
    m_outbound.clear();
    m_nextOutbound = 0;
    m_nextRequestId = 0;
    m_timeouts.stop();
}

void ClipboardSync::onMessage(quint16 type, const QCborMap &body)
{
    switch (type) {
    case Protocol::ClipboardOffer:
        handleOffer(body);
        break;
    case Protocol::ClipboardRelease:
        handleRelease(body);
        break;
    case Protocol::ClipboardRequest:
        handleRequest(body);
        break;
    case Protocol::ClipboardData:
        handleData(body);
        break;
    case Protocol::ClipboardFail:
        handleFail(body);
        break;
    case Protocol::ClipboardCancel:
        handleCancel(body);
        break;
    case Protocol::ClipboardFileList:
        handleFileList(body);
        break;
    case Protocol::ClipboardFileRead:
        handleFileRead(body);
        break;
    case Protocol::ClipboardFileEntries:
    case Protocol::ClipboardFileData: {
        const auto id = Fields::uint(body, "id"_L1);
        if (!id) {
            badMessage(type);
        } else if (ClipboardFiles::Receiver *receiver = receiverOf(*id)) {
            if (type == Protocol::ClipboardFileEntries)
                receiver->handleEntries(*id, body);
            else
                receiver->handleData(*id, body);
        }
        break;
    }
    default:
        break;
    }
}

void ClipboardSync::onLocalChanged(Selection selection)
{
    // Let it settle: some applications set the clipboard twice, and
    // PRIMARY changes with every selected character (section 6.7).
    state(selection).settle.start();
}

std::optional<Selection> ClipboardSync::selectionOf(const QCborMap &body) const
{
    const auto value = Fields::uint(body, "selection"_L1);
    if (!value || *value >= quint64(Protocol::SelectionCount))
        return std::nullopt;
    return Selection(*value);
}

void ClipboardSync::badMessage(quint16 type)
{
    qCWarning(KEMU_AGENT).noquote() << "Clipboard: malformed" << Protocol::messageName(type);
    QCborMap body;
    body.insert("code"_L1, QString(Protocol::Reason::BadMessage));
    body.insert("type"_L1, type);
    m_session->send(Protocol::Error, body);
}

// =============================================================================
// Guest to host
// =============================================================================

std::optional<QByteArray> ClipboardSync::readType(const QMimeData *data, const QString &type) const
{
    if (!data)
        return std::nullopt;
    if (type == MimeRules::Utf8Text && !data->hasFormat(type)) {
        if (!data->hasText())
            return std::nullopt;
        return data->text().toUtf8();
    }
    if (type == MimeRules::Png && !data->hasFormat(type)) {
        const QImage image = qvariant_cast<QImage>(data->imageData());
        if (image.isNull())
            return std::nullopt;
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG"))
            return std::nullopt;
        return png;
    }
    if (!data->hasFormat(type))
        return std::nullopt;
    QByteArray bytes = data->data(type);
    if (type == MimeRules::UriList) {
        bytes = MimeRules::stripFileUris(bytes);
        if (bytes.isEmpty())
            return std::nullopt;
    }
    return bytes;
}

void ClipboardSync::examineLocal(Selection selection)
{
    State &s = state(selection);
    const QMimeData *data = m_backend->mimeData(selection);
    const QStringList formats = data ? data->formats() : QStringList();

    if (formats.isEmpty()) {
        // Cleared: the application that held it is gone.
        s.heldLocally = false;
        if (s.owner == Owner::Local)
            sendRelease(selection);
        return;
    }
    if (formats.contains(MimeRules::ProxyMarker))
        return; // the host's content, put there by us

    // Files (section 6.8). The host's files, staged here and taken over by
    // a clipboard manager without our marker, are still the host's.
    QStringList filePaths = localFilePaths(data);
    if (!filePaths.isEmpty() && m_staging && std::all_of(filePaths.cbegin(), filePaths.cend(), [this](const QString &path) {
            return m_staging->contains(path);
        })) {
        qCDebug(KEMU_AGENT) << "Clipboard:" << selectionName(selection) << "holds the host's files again, not offered";
        return;
    }
    std::optional<ClipboardFiles::Listing> listing;
    if (!filePaths.isEmpty() && mayOfferFiles(selection, m_session->policy())) {
        QString why;
        listing = ClipboardFiles::Listing::build(filePaths, m_session->policy().filesLimit, &why);
        if (!listing)
            qCInfo(KEMU_AGENT).noquote() << "Clipboard: the copied files are not shared:" << why;
    }

    QStringList types = MimeRules::outgoingTypes(formats, data->hasText(), data->hasImage());

    // Read small text ahead: it serves the host even after the source
    // application quit, and gives the offer its sizes.
    QHash<QString, QByteArray> cache;
    for (const QString &type : std::as_const(types)) {
        if (!type.startsWith("text/"_L1) && type != MimeRules::PasswordHint)
            continue;
        const auto bytes = readType(data, type);
        if (bytes && bytes->size() <= m_config.prefetchLimit)
            cache.insert(type, *bytes);
        else if (!bytes && type == MimeRules::UriList)
            types.removeAll(type); // only file: URIs
    }

    // Clipboard managers such as Klipper take over what we put there, but
    // drop our marker. That is still the host's content.
    if (s.owner == Owner::Peer && !s.peerText.isNull()) {
        const auto text = cache.contains(MimeRules::Utf8Text) ? std::optional(cache.value(MimeRules::Utf8Text)) : readType(data, MimeRules::Utf8Text);
        if (text && *text == s.peerText) {
            qCDebug(KEMU_AGENT) << "Clipboard:" << selectionName(selection) << "holds the host's content again, not offered";
            return;
        }
    }

    // New content from a guest application: whatever the host offered
    // before is outdated now.
    if (s.fetch.active)
        cancelFetch(selection);
    if (m_staging)
        m_staging->retire(s.stagedDirectory);
    s.stagedDirectory.clear();
    s.heldLocally = !types.isEmpty() || listing;

    if (types.isEmpty() && !listing) {
        qCDebug(KEMU_AGENT) << "Clipboard:" << selectionName(selection) << "holds nothing to share";
        if (s.owner == Owner::Local)
            sendRelease(selection);
        return;
    }
    if (!mayOffer(selection, m_session->policy())) {
        if (s.owner == Owner::Local) {
            s.owner = Owner::None;
            s.offeredTypes.clear();
            s.cache.clear();
            s.listing.reset();
        }
        return;
    }
    sendOffer(selection, types, cache, std::move(listing));
}

QStringList ClipboardSync::localFilePaths(const QMimeData *data) const
{
    if (!data || !data->hasFormat(MimeRules::UriList))
        return {};
    return ClipboardFiles::localFiles(data->data(MimeRules::UriList));
}

void ClipboardSync::sendOffer(Selection selection,
                              const QStringList &types,
                              const QHash<QString, QByteArray> &cache,
                              std::optional<ClipboardFiles::Listing> listing)
{
    State &s = state(selection);
    ++s.localSerial;
    s.owner = Owner::Local;
    s.offeredTypes = types;
    s.cache = cache;
    s.listing = std::move(listing);

    QCborArray typeArray;
    QCborMap sizes;
    for (const QString &type : types) {
        typeArray.append(type);
        if (cache.contains(type))
            sizes.insert(type, cache.value(type).size());
    }
    QCborMap body;
    body.insert("selection"_L1, int(selection));
    body.insert("serial"_L1, toCbor(s.localSerial));
    body.insert("seen"_L1, toCbor(s.peerSerial));
    body.insert("types"_L1, typeArray);
    if (!sizes.isEmpty())
        body.insert("sizes"_L1, sizes);
    if (s.listing)
        body.insert("files"_L1, QCborMap{{u"count"_s, s.listing->count()}, {u"size"_s, toCbor(s.listing->size())}});
    qCDebug(KEMU_AGENT) << "Clipboard: offering" << selectionName(selection) << s.localSerial << types
                        << (s.listing ? u"and %1 files and folders"_s.arg(s.listing->count()) : QString());
    m_session->send(Protocol::ClipboardOffer, body);
}

void ClipboardSync::sendRelease(Selection selection)
{
    State &s = state(selection);
    QCborMap body;
    body.insert("selection"_L1, int(selection));
    body.insert("serial"_L1, toCbor(s.localSerial));
    m_session->send(Protocol::ClipboardRelease, body);
    s.owner = Owner::None;
    s.offeredTypes.clear();
    s.cache.clear();
    s.listing.reset();
}

void ClipboardSync::handleRequest(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    const auto type = Fields::text(body, "type"_L1);
    if (!id || !selection || !serial || !type) {
        badMessage(Protocol::ClipboardRequest);
        return;
    }

    auto fail = [this, id](QLatin1StringView reason) {
        QCborMap answer;
        answer.insert("id"_L1, toCbor(*id));
        answer.insert("reason"_L1, QString(reason));
        m_session->send(Protocol::ClipboardFail, answer);
    };

    State &s = state(*selection);
    if (!mayOffer(*selection, m_session->policy()))
        return fail(Protocol::Reason::Denied);
    if (s.owner != Owner::Local || *serial != s.localSerial)
        return fail(Protocol::Reason::Stale);
    if (!s.offeredTypes.contains(*type))
        return fail(Protocol::Reason::NotOffered);
    if (m_outbound.size() >= m_config.maxServing)
        return fail(Protocol::Reason::Busy);

    std::optional<QByteArray> data;
    if (s.cache.contains(*type)) {
        data = s.cache.value(*type);
    } else {
        const QMimeData *current = m_backend->mimeData(*selection);
        if (current && !current->hasFormat(MimeRules::ProxyMarker))
            data = readType(current, *type);
    }
    if (!data)
        return fail(Protocol::Reason::Unavailable);
    if (quint64(data->size()) > m_session->peerMaxTransfer())
        return fail(Protocol::Reason::TooLarge);

    qCDebug(KEMU_AGENT) << "Clipboard: the host fetches" << *type << data->size() << "bytes";
    m_outbound.append(Outbound{*id, *data, 0});
    pumpOutbound();
}

// CLIPBOARD_FILE_LIST and CLIPBOARD_FILE_READ: one answer each, at once.
void ClipboardSync::handleFileList(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    if (!id || !selection || !serial) {
        badMessage(Protocol::ClipboardFileList);
        return;
    }
    const State &s = state(*selection);
    QLatin1StringView reason;
    if (!mayOfferFiles(*selection, m_session->policy()))
        reason = Protocol::Reason::Denied;
    else if (s.owner != Owner::Local || *serial != s.localSerial)
        reason = Protocol::Reason::Stale;
    else if (!s.listing)
        reason = Protocol::Reason::NotOffered;
    if (!reason.isEmpty()) {
        m_session->send(Protocol::ClipboardFail, QCborMap{{u"id"_s, toCbor(*id)}, {u"reason"_s, QString(reason)}});
        return;
    }
    QCborMap answer = s.listing->entriesBody(Fields::uint(body, "start"_L1).value_or(0));
    answer.insert("id"_L1, toCbor(*id));
    m_session->send(Protocol::ClipboardFileEntries, answer);
}

void ClipboardSync::handleFileRead(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    const auto file = Fields::uint(body, "file"_L1);
    const auto offset = Fields::uint(body, "offset"_L1);
    const auto length = Fields::uint(body, "length"_L1);
    if (!id || !selection || !serial || !file || !offset || !length) {
        badMessage(Protocol::ClipboardFileRead);
        return;
    }
    const State &s = state(*selection);
    QLatin1StringView reason;
    QByteArray data;
    if (!mayOfferFiles(*selection, m_session->policy())) {
        reason = Protocol::Reason::Denied;
    } else if (s.owner != Owner::Local || *serial != s.localSerial) {
        reason = Protocol::Reason::Stale;
    } else if (!s.listing) {
        reason = Protocol::Reason::NotOffered;
    } else {
        ClipboardFiles::Listing::ReadResult result = s.listing->read(*file, *offset, *length);
        reason = result.failure;
        data = std::move(result.data);
    }
    if (!reason.isEmpty()) {
        m_session->send(Protocol::ClipboardFail, QCborMap{{u"id"_s, toCbor(*id)}, {u"reason"_s, QString(reason)}});
        return;
    }
    m_session->send(Protocol::ClipboardFileData, QCborMap{{u"id"_s, toCbor(*id)}, {u"data"_s, data}});
}

void ClipboardSync::handleCancel(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    if (!id)
        return;
    for (qsizetype i = 0; i < m_outbound.size(); ++i) {
        if (m_outbound.at(i).id == *id) {
            m_outbound.removeAt(i);
            if (m_nextOutbound > i)
                --m_nextOutbound;
            return;
        }
    }
}

void ClipboardSync::pumpOutbound()
{
    if (m_pumping)
        return;
    m_pumping = true;
    const auto done = qScopeGuard([this] {
        m_pumping = false;
    });
    Transport *transport = m_session->transport();
    while (!m_outbound.isEmpty() && m_session->state() == Session::State::Ready && transport->pendingBytes() < m_config.pacingThreshold) {
        if (m_nextOutbound >= m_outbound.size())
            m_nextOutbound = 0;
        Outbound &transfer = m_outbound[m_nextOutbound];

        const qsizetype size = std::min(m_config.chunkSize, transfer.data.size() - transfer.offset);
        QCborMap body;
        body.insert("id"_L1, toCbor(transfer.id));
        body.insert("data"_L1, transfer.data.mid(transfer.offset, size));
        if (transfer.offset == 0)
            body.insert("size"_L1, transfer.data.size());
        transfer.offset += size;
        const bool last = transfer.offset >= transfer.data.size();
        if (last)
            body.insert("last"_L1, true);
        m_session->send(Protocol::ClipboardData, body);

        // Round robin, so one large transfer does not hold up the others.
        if (last)
            m_outbound.removeAt(m_nextOutbound);
        else
            ++m_nextOutbound;
    }
}

// =============================================================================
// Host to guest
// =============================================================================

void ClipboardSync::handleOffer(const QCborMap &body)
{
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    const auto seen = Fields::uint(body, "seen"_L1);
    const auto types = Fields::textList(body, "types"_L1);
    if (!selection || !serial || !seen || !types || types->size() > Protocol::MaxOfferTypes) {
        badMessage(Protocol::ClipboardOffer);
        return;
    }
    if (!acceptsPeer(*selection, m_session->policy()))
        return;

    State &s = state(*selection);
    if (*serial <= s.peerSerial)
        return; // stale
    if (s.owner == Owner::Local && *seen < s.localSerial)
        qCDebug(KEMU_AGENT) << "Clipboard: both sides copied at once; the host wins";

    s.peerSerial = *serial;
    s.owner = Owner::Peer;
    s.offeredTypes.clear();
    s.cache.clear();
    s.listing.reset();
    s.peerTypes.clear();
    for (const QString &type : *types) {
        if (MimeRules::isWellFormed(type))
            s.peerTypes.append(type);
    }
    qCDebug(KEMU_AGENT) << "Clipboard: the host offers" << selectionName(*selection) << *serial << s.peerTypes;
    startFetch(*selection, Fields::map(body, "sizes"_L1).value_or(QCborMap()), Fields::map(body, "files"_L1).value_or(QCborMap()));
}

void ClipboardSync::handleRelease(const QCborMap &body)
{
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    if (!selection || !serial) {
        badMessage(Protocol::ClipboardRelease);
        return;
    }
    State &s = state(*selection);
    if (s.owner != Owner::Peer || s.peerSerial != *serial)
        return;

    cancelFetch(*selection);
    s.owner = Owner::None;
    s.peerTypes.clear();
    s.peerText.clear();
    if (m_staging)
        m_staging->retire(s.stagedDirectory);
    s.stagedDirectory.clear();

    // Our proxy for that offer goes too, unless something replaced it.
    const QMimeData *current = m_backend->mimeData(*selection);
    if (current && current->data(MimeRules::ProxyMarker) == QByteArray::number(*serial)) {
        qCDebug(KEMU_AGENT) << "Clipboard: the host withdrew" << selectionName(*selection);
        m_backend->clear(*selection);
    }
}

void ClipboardSync::startFetch(Selection selection, const QCborMap &sizes, const QCborMap &files)
{
    cancelFetch(selection);
    State &s = state(selection);
    Fetch &fetch = s.fetch;
    fetch = Fetch();
    fetch.active = true;
    fetch.serial = s.peerSerial;

    // One image format is enough: the guest side converts on demand.
    const bool hasPng = s.peerTypes.contains(MimeRules::Png);
    bool imageTaken = false;
    for (const QString &type : std::as_const(s.peerTypes)) {
        if (!MimeRules::isShareable(type))
            continue;
        if (MimeRules::isImage(type)) {
            if (imageTaken || (hasPng && type != MimeRules::Png))
                continue;
            imageTaken = true;
        }
        const auto size = Fields::toUInt(sizes.value(type));
        if (size && (qint64(*size) > m_config.fetchBudget || qint64(*size) > m_config.maxReceive))
            continue;
        fetch.queue.append(type);
    }

    const auto fileCount = Fields::uint(files, "count"_L1);
    const auto fileSize = Fields::uint(files, "size"_L1);
    if (fileCount && fileSize && acceptsFiles(selection, m_session->policy()))
        startFileFetch(selection, *fileCount, *fileSize);

    pumpFetch();
    inboundDone(selection);
}

void ClipboardSync::startFileFetch(Selection selection, quint64 count, quint64 size)
{
    const quint64 limit = m_session->policy().filesLimit;
    ClipboardFiles::Staging *stage = staging();
    if (count == 0 || count > quint64(ClipboardFiles::MaxEntries) || size > limit) {
        qCInfo(KEMU_AGENT) << "Clipboard: the host's files exceed the limits, not fetched";
        return;
    }
    if (!stage->isUsable() || quint64(stage->freeBytes()) < size) {
        qCWarning(KEMU_AGENT) << "Clipboard: no room for" << size << "bytes of files from the host";
        return;
    }
    const QString directory = stage->create();
    if (directory.isEmpty())
        return;

    Fetch &fetch = state(selection).fetch;
    const quint64 serial = fetch.serial;
    ClipboardFiles::Receiver::Config config;
    config.sizeLimit = limit;
    config.window = m_config.fileWindow;
    config.timeoutMs = m_config.requestTimeoutMs;
    fetch.files = std::make_unique<ClipboardFiles::Receiver>(
        directory,
        count,
        size,
        config,
        [this, selection, serial](ClipboardFiles::Receiver::Kind kind, const QCborMap &fields) {
            const quint64 id = ++m_nextRequestId;
            QCborMap body = fields;
            body.insert("id"_L1, toCbor(id));
            body.insert("selection"_L1, int(selection));
            body.insert("serial"_L1, toCbor(serial));
            m_session->send(kind == ClipboardFiles::Receiver::Kind::List ? Protocol::ClipboardFileList : Protocol::ClipboardFileRead, body);
            return id;
        },
        [this](quint64 id) {
            m_session->send(Protocol::ClipboardCancel, QCborMap{{u"id"_s, toCbor(id)}});
        });
    fetch.filesPending = true;
    connect(fetch.files.get(), &ClipboardFiles::Receiver::finished, this, [this, selection] {
        fileFetchEnded(selection, true);
    });
    connect(fetch.files.get(), &ClipboardFiles::Receiver::failed, this, [this, selection](const QString &why) {
        qCInfo(KEMU_AGENT).noquote() << "Clipboard: the host's files did not arrive:" << why;
        fileFetchEnded(selection, false);
    });
    qCDebug(KEMU_AGENT) << "Clipboard: fetching" << count << "files and folders," << size << "bytes";
    fetch.files->start();
}

void ClipboardSync::fileFetchEnded(Selection selection, bool complete)
{
    Fetch &fetch = state(selection).fetch;
    if (!fetch.files)
        return;
    if (complete) {
        fetch.filePaths = fetch.files->topLevelPaths();
        fetch.filesPending = false;
    } else {
        // All or nothing: the offer goes on without files.
        dropFiles(fetch, true);
    }
    inboundDone(selection);
}

void ClipboardSync::pumpFetch()
{
    for (const Selection selection : kSelections) {
        Fetch &fetch = state(selection).fetch;
        // File reads share the limit of open requests (section 6.8.2).
        while (fetch.active && !fetch.queue.isEmpty() && m_inbound.size() < std::max(1, m_config.maxOpenRequests - openFileRequests())) {
            const QString type = fetch.queue.takeFirst();
            const quint64 id = ++m_nextRequestId;
            Inbound inbound;
            inbound.selection = selection;
            inbound.serial = fetch.serial;
            inbound.type = type;
            inbound.lastActivity.start();
            m_inbound.insert(id, inbound);

            QCborMap body;
            body.insert("id"_L1, toCbor(id));
            body.insert("selection"_L1, int(selection));
            body.insert("serial"_L1, toCbor(fetch.serial));
            body.insert("type"_L1, type);
            m_session->send(Protocol::ClipboardRequest, body);
        }
    }
    if (!m_inbound.isEmpty() && !m_timeouts.isActive())
        m_timeouts.start();
}

void ClipboardSync::cancelFetch(Selection selection)
{
    Fetch &fetch = state(selection).fetch;
    const QList<quint64> ids = m_inbound.keys();
    for (const quint64 id : ids) {
        if (m_inbound.value(id).selection != selection)
            continue;
        m_inbound.remove(id);
        m_session->send(Protocol::ClipboardCancel, QCborMap{{u"id"_s, toCbor(id)}});
    }
    dropFiles(fetch, true);
    fetch = Fetch();
    if (m_inbound.isEmpty())
        m_timeouts.stop();
}

void ClipboardSync::cancelInbound(quint64 id)
{
    const auto it = m_inbound.constFind(id);
    if (it == m_inbound.constEnd())
        return;
    const Selection selection = it->selection;
    m_inbound.erase(it);
    m_session->send(Protocol::ClipboardCancel, QCborMap{{u"id"_s, toCbor(id)}});
    inboundDone(selection);
}

void ClipboardSync::handleData(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    const auto data = Fields::bytes(body, "data"_L1);
    if (!id || !data) {
        badMessage(Protocol::ClipboardData);
        return;
    }
    auto it = m_inbound.find(*id);
    if (it == m_inbound.end())
        return; // cancelled, or not ours
    Inbound &inbound = *it;
    const Selection selection = inbound.selection;
    Fetch &fetch = state(selection).fetch;
    inbound.lastActivity.restart();

    const qint64 room = std::min(m_config.maxReceive, m_config.fetchBudget - fetch.bytes);
    const auto size = Fields::uint(body, "size"_L1);
    if (size && !inbound.sizeSeen) {
        inbound.sizeSeen = true;
        if (qint64(*size) > room) {
            qCDebug(KEMU_AGENT) << "Clipboard: skipping" << inbound.type << "of" << *size << "bytes";
            cancelInbound(*id);
            return;
        }
    }
    if (inbound.data.size() + data->size() > room) {
        qCDebug(KEMU_AGENT) << "Clipboard: skipping" << inbound.type << ", larger than the budget";
        cancelInbound(*id);
        return;
    }
    inbound.data += *data;

    if (Fields::boolean(body, "last"_L1).value_or(false)) {
        fetch.done.append({inbound.type, inbound.data});
        fetch.bytes += inbound.data.size();
        m_inbound.erase(it);
        inboundDone(selection);
    }
}

void ClipboardSync::handleFail(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    if (!id) {
        badMessage(Protocol::ClipboardFail);
        return;
    }
    if (ClipboardFiles::Receiver *receiver = receiverOf(*id)) {
        receiver->handleFail(*id, Fields::text(body, "reason"_L1).value_or(QString()));
        return;
    }
    const auto it = m_inbound.constFind(*id);
    if (it == m_inbound.constEnd())
        return;
    const Selection selection = it->selection;
    qCDebug(KEMU_AGENT) << "Clipboard: the host could not deliver" << it->type << Fields::text(body, "reason"_L1).value_or(QString());
    m_inbound.erase(it);
    inboundDone(selection);
}

void ClipboardSync::inboundDone(Selection selection)
{
    pumpFetch();
    if (m_inbound.isEmpty())
        m_timeouts.stop();

    const Fetch &fetch = state(selection).fetch;
    if (!fetch.active || !fetch.queue.isEmpty() || fetch.filesPending)
        return;
    for (const Inbound &inbound : std::as_const(m_inbound)) {
        if (inbound.selection == selection)
            return;
    }
    finishFetch(selection);
}

void ClipboardSync::finishFetch(Selection selection)
{
    State &s = state(selection);
    Fetch fetch = std::move(s.fetch);
    s.fetch = Fetch();
    // The receiver may have sent the signal that brought us here.
    QString stagedDirectory;
    if (ClipboardFiles::Receiver *receiver = fetch.files.release()) {
        stagedDirectory = receiver->directory();
        receiver->deleteLater();
    }
    if (fetch.done.isEmpty() && fetch.filePaths.isEmpty()) {
        if (m_staging)
            m_staging->discard(stagedDirectory);
        qCDebug(KEMU_AGENT) << "Clipboard: nothing of the host's offer arrived";
        return;
    }

    auto *data = new QMimeData;
    QByteArray text;
    bool imageSet = false;
    for (const auto &[type, bytes] : std::as_const(fetch.done)) {
        data->setData(type, bytes);
        if (type == MimeRules::Utf8Text)
            text = bytes;
        if (!imageSet && MimeRules::isImage(type)) {
            // Lets the clipboard offer every image format it can write.
            QImage image;
            if (image.loadFromData(bytes)) {
                data->setImageData(image);
                imageSet = true;
            }
        }
    }
    if (!text.isNull() && !data->hasFormat(MimeRules::PlainText))
        data->setText(QString::fromUtf8(text));
    if (!fetch.filePaths.isEmpty()) {
        // The staged copies, ahead of whatever other URIs the host sent.
        const QByteArray uris = ClipboardFiles::uriList(fetch.filePaths);
        data->setData(MimeRules::UriList, uris + data->data(MimeRules::UriList));
        data->setData(GnomeCopiedFiles, "copy\n" + uris.trimmed().replace("\r\n", "\n"));
    }
    data->setData(MimeRules::ProxyMarker, QByteArray::number(fetch.serial));
    if (m_staging)
        m_staging->retire(s.stagedDirectory);
    s.stagedDirectory = fetch.filePaths.isEmpty() ? QString() : stagedDirectory;

    s.peerText = text;
    s.heldLocally = false;
    qCDebug(KEMU_AGENT) << "Clipboard: host content on the guest" << selectionName(selection) << fetch.bytes << "bytes";
    m_backend->setMimeData(selection, data);
}

void ClipboardSync::checkTimeouts()
{
    QList<quint64> expired;
    for (auto it = m_inbound.cbegin(); it != m_inbound.cend(); ++it) {
        if (it->lastActivity.elapsed() > m_config.requestTimeoutMs)
            expired.append(it.key());
    }
    for (const quint64 id : std::as_const(expired)) {
        qCDebug(KEMU_AGENT) << "Clipboard: request" << id << "timed out";
        cancelInbound(id);
    }
}
