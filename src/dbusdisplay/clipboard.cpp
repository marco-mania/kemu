/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "clipboard.h"

#include <QBuffer>
#include <QCborArray>
#include <QCryptographicHash>
#include <QGuiApplication>
#include <QImage>
#include <QMimeData>
#include <QScopeGuard>

#include "kemu_dbusdisplay_debug.h"

using namespace KemuAgentProtocol;

const QString clipboardDirToString(ClipboardDirection dir)
{
    switch (dir) {
    case ClipboardDirection::ToHost:
        return u"ToHost"_s;
    case ClipboardDirection::ToGuest:
        return u"ToGuest"_s;
    case ClipboardDirection::Both:
        return u"Both"_s;
    case ClipboardDirection::Off:
    default:
        return u"Off"_s;
    }
}

ClipboardDirection clipboardDirFromString(const QString &s)
{
    if (s == u"ToHost"_s)
        return ClipboardDirection::ToHost;
    if (s == u"ToGuest"_s)
        return ClipboardDirection::ToGuest;
    if (s == u"Both"_s)
        return ClipboardDirection::Both;
    return ClipboardDirection::Off;
}

namespace
{
constexpr std::array kSelections{Selection::Clipboard, Selection::Primary};

QClipboard::Mode toMode(Selection selection)
{
    return selection == Selection::Primary ? QClipboard::Selection : QClipboard::Clipboard;
}

const char *selectionName(Selection selection)
{
    return selection == Selection::Primary ? "PRIMARY" : "CLIPBOARD";
}

qint64 toCbor(quint64 value)
{
    return qint64(value);
}
} // namespace

AgentClipboard::AgentClipboard(ClipboardDirection direction, QObject *parent)
    : AgentClipboard(direction, Config{}, parent)
{
}

AgentClipboard::AgentClipboard(ClipboardDirection direction, const Config &config, QObject *parent)
    : QObject(parent)
    , m_clipboard(QGuiApplication::clipboard())
    , m_config(config)
    , m_direction(direction)
{
    for (const Selection selection : kSelections) {
        QTimer &settle = state(selection).settle;
        settle.setSingleShot(true);
        // Applications sometimes set the clipboard twice; PRIMARY changes
        // with every selected character (section 6.7).
        settle.setInterval(selection == Selection::Primary ? m_config.primarySettleMs : m_config.settleMs);
        connect(&settle, &QTimer::timeout, this, [this, selection] {
            examineHost(selection, false);
        });
    }
    m_timeouts.setInterval(std::max(100, m_config.requestTimeoutMs / 10));
    connect(&m_timeouts, &QTimer::timeout, this, &AgentClipboard::checkTimeouts);
    connect(m_clipboard, &QClipboard::changed, this, &AgentClipboard::onHostChanged);
}

AgentClipboard::~AgentClipboard()
{
    // Without a viewer, nothing is shared.
    if (m_agent)
        m_agent->setPolicy({});
}

AgentClipboard::State &AgentClipboard::state(Selection selection)
{
    return m_states[int(selection)];
}

const AgentClipboard::State &AgentClipboard::state(Selection selection) const
{
    return m_states[int(selection)];
}

AgentClipboard::Owner AgentClipboard::owner(Selection selection) const
{
    return state(selection).owner;
}

quint64 AgentClipboard::localSerial(Selection selection) const
{
    return state(selection).localSerial;
}

quint64 AgentClipboard::peerSerial(Selection selection) const
{
    return state(selection).peerSerial;
}

bool AgentClipboard::isFetching(Selection selection) const
{
    return state(selection).fetch.active;
}

// =============================================================================
// Settings and sessions
// =============================================================================

void AgentClipboard::setAgent(KemuAgentClient *agent)
{
    if (m_agent == agent)
        return;
    if (m_agent) {
        m_agent->disconnect(this);
        m_agent->setPolicy({});
    }
    resetState();
    m_agent = agent;
    if (!m_agent)
        return;

    connect(m_agent, &KemuAgentClient::sessionStarted, this, &AgentClipboard::onSessionStarted);
    connect(m_agent, &KemuAgentClient::sessionEnded, this, &AgentClipboard::onSessionEnded);
    connect(m_agent, &KemuAgentClient::message, this, &AgentClipboard::onMessage);
    connect(m_agent, &KemuAgentClient::drained, this, &AgentClipboard::pumpOutbound);
    connect(m_agent, &QObject::destroyed, this, [this] {
        resetState();
    });
    m_agent->setPolicy(policyFor(m_direction, m_sharePrimary));
    if (m_agent->hasSession())
        onSessionStarted();
}

void AgentClipboard::setDirection(ClipboardDirection direction)
{
    if (m_direction == direction)
        return;
    const KemuAgentClient::Policy previous = policyFor(m_direction, m_sharePrimary);
    m_direction = direction;
    applyPolicy(previous);
}

void AgentClipboard::setSharePrimary(bool share)
{
    if (m_sharePrimary == share)
        return;
    const KemuAgentClient::Policy previous = policyFor(m_direction, m_sharePrimary);
    m_sharePrimary = share;
    applyPolicy(previous);
}

void AgentClipboard::setFilterPasswords(bool filter)
{
    if (m_filterPasswords == filter)
        return;
    m_filterPasswords = filter;
    for (const Selection selection : kSelections) {
        State &s = state(selection);
        if (!filter) {
            // A password already on the clipboard becomes available right away.
            examineHost(selection, true);
        } else if (s.owner == Owner::Local && s.offeredSecret) {
            // The guest may have fetched it already, but it is no longer handed out.
            qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: password entry withdrawn from the guest";
            sendRelease(selection);
            s.sharedHash.clear();
        }
    }
}

KemuAgentClient::Policy AgentClipboard::policyFor(ClipboardDirection direction, bool sharePrimary) const
{
    KemuAgentClient::Policy policy;
    policy.toHost = direction == ClipboardDirection::ToHost || direction == ClipboardDirection::Both;
    policy.toGuest = direction == ClipboardDirection::ToGuest || direction == ClipboardDirection::Both;
    // Not every host session has a selection to share (section 6.7).
    policy.primary = sharePrimary && direction != ClipboardDirection::Off && m_clipboard->supportsSelection();
    return policy;
}

bool AgentClipboard::mayOffer(Selection selection, const KemuAgentClient::Policy &policy) const
{
    if (!m_agent || !m_agent->hasSession() || !policy.toGuest)
        return false;
    if (selection == Selection::Clipboard)
        return m_agent->hasCapability(Capability::Clipboard);
    return policy.primary && m_agent->hasCapability(Capability::ClipboardPrimary) && m_clipboard->supportsSelection();
}

bool AgentClipboard::acceptsPeer(Selection selection, const KemuAgentClient::Policy &policy) const
{
    if (!m_agent || !m_agent->hasSession() || !policy.toHost)
        return false;
    if (selection == Selection::Clipboard)
        return m_agent->hasCapability(Capability::Clipboard);
    return policy.primary && m_agent->hasCapability(Capability::ClipboardPrimary) && m_clipboard->supportsSelection();
}

void AgentClipboard::applyPolicy(const KemuAgentClient::Policy &previous)
{
    const KemuAgentClient::Policy current = policyFor(m_direction, m_sharePrimary);
    for (const Selection selection : kSelections) {
        State &s = state(selection);
        // The host withdraws its own offers (section 5.5).
        if (mayOffer(selection, previous) && !mayOffer(selection, current) && s.owner == Owner::Local)
            sendRelease(selection);
        if (acceptsPeer(selection, previous) && !acceptsPeer(selection, current) && s.owner == Owner::Peer) {
            cancelFetch(selection);
            s.owner = Owner::None;
        }
    }
    if (m_agent)
        m_agent->setPolicy(current);
    for (const Selection selection : kSelections) {
        if (!mayOffer(selection, previous) && mayOffer(selection, current))
            examineHost(selection, true);
    }
}

void AgentClipboard::onSessionStarted()
{
    resetState();
    // The agent starts empty: offer what the host holds (section 5.5).
    for (const Selection selection : kSelections)
        examineHost(selection, true);
}

void AgentClipboard::onSessionEnded()
{
    resetState();
}

void AgentClipboard::resetState()
{
    for (const Selection selection : kSelections) {
        State &s = state(selection);
        s.localSerial = 0;
        s.peerSerial = 0;
        s.owner = Owner::None;
        s.offered.clear();
        s.offeredSecret = false;
        s.sharedHash.clear();
        s.peerTypes.clear();
        s.fetch = Fetch();
    }
    m_inbound.clear();
    m_outbound.clear();
    m_nextOutbound = 0;
    m_nextRequestId = 0;
    m_timeouts.stop();
}

void AgentClipboard::onMessage(quint16 type, const QCborMap &body)
{
    switch (type) {
    case ClipboardOffer:
        handleOffer(body);
        break;
    case ClipboardRelease:
        handleRelease(body);
        break;
    case ClipboardRequest:
        handleRequest(body);
        break;
    case ClipboardData:
        handleData(body);
        break;
    case ClipboardFail:
        handleFail(body);
        break;
    case ClipboardCancel:
        handleCancel(body);
        break;
    default:
        break;
    }
}

void AgentClipboard::onHostChanged(QClipboard::Mode mode)
{
    if (mode == QClipboard::Clipboard)
        state(Selection::Clipboard).settle.start();
    else if (mode == QClipboard::Selection)
        state(Selection::Primary).settle.start();
}

std::optional<Selection> AgentClipboard::selectionOf(const QCborMap &body) const
{
    const auto value = Fields::uint(body, "selection"_L1);
    if (!value || *value >= quint64(SelectionCount))
        return std::nullopt;
    return Selection(*value);
}

void AgentClipboard::badMessage(quint16 type)
{
    qCWarning(KEMU_DBUSDISPLAY).noquote() << "Clipboard: malformed" << messageName(type) << "from kemu-agent";
    if (m_agent)
        m_agent->send(Error, QCborMap{{u"code"_s, QString(Reason::BadMessage)}, {u"type"_s, int(type)}});
}

QByteArray AgentClipboard::contentHash(const Content &content)
{
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &[type, data] : content) {
        hash.addData(type.toUtf8());
        hash.addData(QByteArrayView("\0", 1));
        hash.addData(QByteArray::number(data.size()));
        hash.addData(data);
    }
    return hash.result();
}

bool AgentClipboard::isSecret(const Content &content)
{
    // Convention of KeePassXC, Klipper and others for password entries.
    for (const auto &[type, data] : content) {
        if (type == Mime::PasswordHint && data == "secret")
            return true;
    }
    return false;
}

// =============================================================================
// Host to guest
// =============================================================================

std::optional<AgentClipboard::Content> AgentClipboard::snapshot(const QMimeData *data) const
{
    if (!data)
        return std::nullopt;
    const QStringList formats = data->formats();
    QStringList types;
    QString image;
    for (const QString &format : formats) {
        if (!Mime::isShareable(format) || types.contains(format))
            continue;
        // One image format is enough: the guest converts on demand, and
        // every further one costs the source an encoding.
        if (Mime::isImage(format)) {
            if (image.isEmpty() || (format == Mime::Png && image != Mime::Png))
                image = format;
            continue;
        }
        types.append(format);
    }
    if (image.isEmpty() && data->hasImage())
        image = Mime::Png;
    if (!image.isEmpty())
        types.append(image);
    if (data->hasText() && !types.contains(Mime::Utf8Text)) {
        const qsizetype plain = types.indexOf(Mime::PlainText);
        types.insert(plain >= 0 ? plain : 0, QString(Mime::Utf8Text));
    }
    while (types.size() > MaxOfferTypes)
        types.removeAt(types.size() - (image.isEmpty() ? 1 : 2)); // the image at the end stays

    Content content;
    qint64 total = 0;
    for (const QString &type : std::as_const(types)) {
        QByteArray bytes;
        if (type == Mime::Utf8Text && !data->hasFormat(type)) {
            bytes = data->text().toUtf8();
        } else if (type == Mime::Png && !data->hasFormat(type)) {
            const QImage picture = qvariant_cast<QImage>(data->imageData());
            if (picture.isNull())
                continue;
            QBuffer buffer(&bytes);
            buffer.open(QIODevice::WriteOnly);
            picture.save(&buffer, "PNG");
        } else {
            bytes = data->data(type);
            if (type == Mime::UriList) {
                bytes = Mime::stripFileUris(bytes);
                if (bytes.isEmpty())
                    continue; // only local files
            }
        }
        if (total + bytes.size() > m_config.snapshotBudget) {
            qCDebug(KEMU_DBUSDISPLAY) << "Clipboard:" << type << "of" << bytes.size() << "bytes stays on the host";
            continue;
        }
        total += bytes.size();
        content.append({type, bytes});
    }
    return content;
}

void AgentClipboard::examineHost(Selection selection, bool force)
{
    State &s = state(selection);
    if (!mayOffer(selection, policyFor(m_direction, m_sharePrimary)))
        return;

    const QMimeData *data = m_clipboard->mimeData(toMode(selection));
    if (!data || data->formats().isEmpty()) {
        if (s.owner == Owner::Local)
            sendRelease(selection);
        return;
    }
    if (data->hasFormat(ProxyMarker))
        return; // the guest's content, put there by us

    const auto content = snapshot(data);
    if (!content || content->isEmpty()) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: nothing to share on the host" << selectionName(selection);
        if (s.owner == Owner::Local)
            sendRelease(selection);
        return;
    }

    // Wayland announces the selection again on every focus change, and
    // clipboard managers such as Klipper take over what we put there
    // without our marker: content already on both sides is not offered again.
    const QByteArray hash = contentHash(*content);
    if (!force && hash == s.sharedHash) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: host" << selectionName(selection) << "unchanged";
        return;
    }
    if (s.owner == Owner::Peer && !s.peerText.isNull()) {
        for (const auto &[type, bytes] : *content) {
            if (type == Mime::Utf8Text && bytes == s.peerText) {
                qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: host holds the guest's content again, not offered";
                return;
            }
        }
    }

    const bool secret = isSecret(*content);
    if (secret && m_filterPasswords) {
        // What the guest was offered before is no longer on the host
        // (section 6.2); the password itself stays here.
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: password manager entry not shared with the guest";
        if (s.owner == Owner::Local)
            sendRelease(selection);
        return;
    }

    // New host content: whatever the guest offered before is outdated.
    if (s.fetch.active)
        cancelFetch(selection);
    sendOffer(selection, *content, secret, hash);
}

void AgentClipboard::sendOffer(Selection selection, const Content &content, bool secret, const QByteArray &hash)
{
    State &s = state(selection);
    ++s.localSerial;
    s.owner = Owner::Local;
    s.offered = content;
    s.offeredSecret = secret;
    s.sharedHash = hash;

    QCborArray types;
    QCborMap sizes;
    for (const auto &[type, bytes] : content) {
        types.append(type);
        sizes.insert(type, bytes.size());
    }
    QCborMap body;
    body.insert("selection"_L1, int(selection));
    body.insert("serial"_L1, toCbor(s.localSerial));
    body.insert("seen"_L1, toCbor(s.peerSerial));
    body.insert("types"_L1, types);
    body.insert("sizes"_L1, sizes);
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: offering host" << selectionName(selection) << s.localSerial << types.toVariantList();
    m_agent->send(ClipboardOffer, body);
}

void AgentClipboard::sendRelease(Selection selection)
{
    State &s = state(selection);
    if (m_agent)
        m_agent->send(ClipboardRelease, QCborMap{{u"selection"_s, int(selection)}, {u"serial"_s, toCbor(s.localSerial)}});
    s.owner = Owner::None;
    s.offered.clear();
    s.offeredSecret = false;
}

void AgentClipboard::handleRequest(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    const auto type = Fields::text(body, "type"_L1);
    if (!id || !selection || !serial || !type) {
        badMessage(ClipboardRequest);
        return;
    }
    auto fail = [this, id](QLatin1StringView reason) {
        m_agent->send(ClipboardFail, QCborMap{{u"id"_s, toCbor(*id)}, {u"reason"_s, QString(reason)}});
    };

    const State &s = state(*selection);
    if (!mayOffer(*selection, policyFor(m_direction, m_sharePrimary)))
        return fail(Reason::Denied);
    if (s.owner != Owner::Local || *serial != s.localSerial)
        return fail(Reason::Stale);
    const auto it = std::find_if(s.offered.cbegin(), s.offered.cend(), [&](const auto &entry) {
        return entry.first == *type;
    });
    if (it == s.offered.cend())
        return fail(Reason::NotOffered);
    if (quint64(it->second.size()) > m_agent->peerMaxTransfer())
        return fail(Reason::TooLarge);
    if (m_outbound.size() >= m_config.maxServing)
        return fail(Reason::Busy);

    m_outbound.append(Outbound{*id, it->second, 0});
    pumpOutbound();
}

void AgentClipboard::handleCancel(const QCborMap &body)
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

void AgentClipboard::pumpOutbound()
{
    if (m_pumping || !m_agent)
        return;
    m_pumping = true;
    const auto done = qScopeGuard([this] {
        m_pumping = false;
    });
    while (!m_outbound.isEmpty() && m_agent && m_agent->hasSession() && m_agent->pendingBytes() < m_config.pacingThreshold) {
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
        m_agent->send(ClipboardData, body);

        // Round robin, so one large transfer does not hold up the others.
        if (last)
            m_outbound.removeAt(m_nextOutbound);
        else
            ++m_nextOutbound;
    }
}

// =============================================================================
// Guest to host
// =============================================================================

void AgentClipboard::handleOffer(const QCborMap &body)
{
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    const auto seen = Fields::uint(body, "seen"_L1);
    const auto types = Fields::textList(body, "types"_L1);
    if (!selection || !serial || !seen || !types || types->size() > MaxOfferTypes) {
        badMessage(ClipboardOffer);
        return;
    }
    State &s = state(*selection);
    if (*serial <= s.peerSerial)
        return; // stale
    if (!acceptsPeer(*selection, policyFor(m_direction, m_sharePrimary))) {
        // Refused for our own policy (section 6.2): an older guest content
        // on the host is outdated now.
        s.peerSerial = *serial;
        if (s.owner == Owner::Peer) {
            cancelFetch(*selection);
            s.owner = Owner::None;
            s.peerTypes.clear();
            s.peerText.clear();
            removeProxy(*selection, std::nullopt);
        }
        return;
    }
    if (s.owner == Owner::Local && *seen < s.localSerial) {
        // Both copied at once, and the host wins (section 6.2).
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest and host copied at once; the host's content stays";
        s.peerSerial = *serial;
        return;
    }

    s.peerSerial = *serial;
    s.owner = Owner::Peer;
    s.offered.clear();
    s.offeredSecret = false;
    s.peerTypes.clear();
    for (const QString &type : *types) {
        if (Mime::isWellFormed(type))
            s.peerTypes.append(type);
    }
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: the guest offers" << selectionName(*selection) << *serial << s.peerTypes;
    startFetch(*selection, Fields::map(body, "sizes"_L1).value_or(QCborMap()));
}

void AgentClipboard::handleRelease(const QCborMap &body)
{
    const auto selection = selectionOf(body);
    const auto serial = Fields::uint(body, "serial"_L1);
    if (!selection || !serial) {
        badMessage(ClipboardRelease);
        return;
    }
    State &s = state(*selection);
    if (s.owner != Owner::Peer || s.peerSerial != *serial)
        return;
    cancelFetch(*selection);
    s.owner = Owner::None;
    s.peerTypes.clear();
    s.peerText.clear();
    removeProxy(*selection, *serial);
}

void AgentClipboard::removeProxy(Selection selection, std::optional<quint64> serial)
{
    // Only our own proxy, and only the one for that offer.
    const QMimeData *current = m_clipboard->mimeData(toMode(selection));
    if (!current || !current->hasFormat(ProxyMarker))
        return;
    if (serial && current->data(ProxyMarker) != QByteArray::number(*serial))
        return;
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest content removed from the host" << selectionName(selection);
    m_clipboard->clear(toMode(selection));
}

void AgentClipboard::startFetch(Selection selection, const QCborMap &sizes)
{
    cancelFetch(selection);
    State &s = state(selection);
    Fetch &fetch = s.fetch;
    fetch.active = true;
    fetch.serial = s.peerSerial;

    // One image format is enough: Qt offers the others on the host.
    const bool hasPng = s.peerTypes.contains(Mime::Png);
    bool imageTaken = false;
    for (const QString &type : std::as_const(s.peerTypes)) {
        if (!Mime::isShareable(type))
            continue;
        if (Mime::isImage(type)) {
            if (imageTaken || (hasPng && type != Mime::Png))
                continue;
            imageTaken = true;
        }
        const auto size = Fields::toUInt(sizes.value(type));
        if (size && (qint64(*size) > m_config.fetchBudget || *size > KemuAgentClient::kMaxTransfer))
            continue;
        fetch.queue.append(type);
    }
    pumpFetch();
    inboundDone(selection);
}

void AgentClipboard::pumpFetch()
{
    if (!m_agent)
        return;
    for (const Selection selection : kSelections) {
        Fetch &fetch = state(selection).fetch;
        while (fetch.active && !fetch.queue.isEmpty() && m_inbound.size() < m_config.maxOpenRequests) {
            const QString type = fetch.queue.takeFirst();
            const quint64 id = ++m_nextRequestId;
            Inbound inbound{selection, type, {}, false, {}};
            inbound.lastActivity.start();
            m_inbound.insert(id, inbound);
            m_agent->send(ClipboardRequest,
                          QCborMap{{u"id"_s, toCbor(id)}, {u"selection"_s, int(selection)}, {u"serial"_s, toCbor(fetch.serial)}, {u"type"_s, type}});
        }
    }
    if (!m_inbound.isEmpty() && !m_timeouts.isActive())
        m_timeouts.start();
}

void AgentClipboard::cancelFetch(Selection selection)
{
    const QList<quint64> ids = m_inbound.keys();
    for (const quint64 id : ids) {
        if (m_inbound.value(id).selection != selection)
            continue;
        m_inbound.remove(id);
        if (m_agent)
            m_agent->send(ClipboardCancel, QCborMap{{u"id"_s, toCbor(id)}});
    }
    state(selection).fetch = Fetch();
    if (m_inbound.isEmpty())
        m_timeouts.stop();
}

void AgentClipboard::cancelInbound(quint64 id)
{
    const auto it = m_inbound.constFind(id);
    if (it == m_inbound.constEnd())
        return;
    const Selection selection = it->selection;
    m_inbound.erase(it);
    if (m_agent)
        m_agent->send(ClipboardCancel, QCborMap{{u"id"_s, toCbor(id)}});
    inboundDone(selection);
}

void AgentClipboard::handleData(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    const auto data = Fields::bytes(body, "data"_L1);
    if (!id || !data) {
        badMessage(ClipboardData);
        return;
    }
    const auto it = m_inbound.find(*id);
    if (it == m_inbound.end())
        return; // cancelled, or not ours
    Inbound &inbound = *it;
    const Selection selection = inbound.selection;
    Fetch &fetch = state(selection).fetch;
    inbound.lastActivity.restart();

    const qint64 room = std::min(qint64(KemuAgentClient::kMaxTransfer), m_config.fetchBudget - fetch.bytes);
    const auto size = Fields::uint(body, "size"_L1);
    if (size && !inbound.sizeSeen) {
        inbound.sizeSeen = true;
        if (qint64(*size) > room) {
            qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest" << inbound.type << "of" << *size << "bytes stays in the guest";
            cancelInbound(*id);
            return;
        }
    }
    if (inbound.data.size() + data->size() > room) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest" << inbound.type << "larger than the budget, stays in the guest";
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

void AgentClipboard::handleFail(const QCborMap &body)
{
    const auto id = Fields::uint(body, "id"_L1);
    if (!id) {
        badMessage(ClipboardFail);
        return;
    }
    const auto it = m_inbound.constFind(*id);
    if (it == m_inbound.constEnd())
        return;
    const Selection selection = it->selection;
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: the guest could not deliver" << it->type << Fields::text(body, "reason"_L1).value_or(QString());
    m_inbound.erase(it);
    inboundDone(selection);
}

void AgentClipboard::inboundDone(Selection selection)
{
    pumpFetch();
    if (m_inbound.isEmpty())
        m_timeouts.stop();
    const Fetch &fetch = state(selection).fetch;
    if (!fetch.active || !fetch.queue.isEmpty())
        return;
    for (const Inbound &inbound : std::as_const(m_inbound)) {
        if (inbound.selection == selection)
            return;
    }
    finishFetch(selection);
}

void AgentClipboard::finishFetch(Selection selection)
{
    State &s = state(selection);
    const Fetch fetch = std::exchange(s.fetch, Fetch());
    if (fetch.done.isEmpty()) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: nothing of the guest's offer arrived";
        return;
    }

    // The host copied while the guest's content was on its way, and that
    // copy waits for its offer: it is the newer one and wins (section 6.2).
    if (s.settle.isActive() && hostHoldsNewContent(selection)) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: the host copied meanwhile; the guest's content stays in the guest";
        s.peerText.clear();
        return;
    }

    if (m_filterPasswords && isSecret(fetch.done)) {
        // Refused for our own policy (section 6.2): the guest keeps it, and
        // an older guest content on the host is outdated now.
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: password manager entry stays in the guest";
        s.owner = Owner::None;
        s.peerText.clear();
        removeProxy(selection, std::nullopt);
        return;
    }

    auto *data = new QMimeData;
    QByteArray text;
    bool imageSet = false;
    for (const auto &[type, bytes] : fetch.done) {
        data->setData(type, bytes);
        if (type == Mime::Utf8Text)
            text = bytes;
        if (!imageSet && Mime::isImage(type)) {
            QImage picture;
            if (picture.loadFromData(bytes)) {
                data->setImageData(picture);
                imageSet = true;
            }
        }
    }
    if (!text.isNull() && !data->hasFormat(Mime::PlainText))
        data->setText(QString::fromUtf8(text));
    data->setData(ProxyMarker, QByteArray::number(fetch.serial));

    s.peerText = text;
    s.sharedHash = contentHash(fetch.done);
    qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: guest content on the host" << selectionName(selection) << fetch.bytes << "bytes";
    m_clipboard->setMimeData(data, toMode(selection));
}

bool AgentClipboard::hostHoldsNewContent(Selection selection) const
{
    const QMimeData *data = m_clipboard->mimeData(toMode(selection));
    if (!data || data->formats().isEmpty() || data->hasFormat(ProxyMarker))
        return false;
    const auto content = snapshot(data);
    return content && !content->isEmpty() && contentHash(*content) != state(selection).sharedHash;
}

void AgentClipboard::checkTimeouts()
{
    QList<quint64> expired;
    for (auto it = m_inbound.cbegin(); it != m_inbound.cend(); ++it) {
        if (it->lastActivity.elapsed() > m_config.requestTimeoutMs)
            expired.append(it.key());
    }
    for (const quint64 id : std::as_const(expired)) {
        qCDebug(KEMU_DBUSDISPLAY) << "Clipboard: request" << id << "to the guest timed out";
        cancelInbound(id);
    }
}
