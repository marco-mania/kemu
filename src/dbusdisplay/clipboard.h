/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCborMap>
#include <QClipboard>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>

#include <array>
#include <memory>
#include <optional>

#include "utils/clipboardfiles.h"
#include "utils/kemuagentclient.h"
#include "utils/kemuagentprotocol.h"

class QMimeData;

using namespace Qt::StringLiterals;

enum class ClipboardDirection {
    Off,
    ToHost, // Guest -> Host
    ToGuest, // Host -> Guest
    Both
};

const QString clipboardDirToString(ClipboardDirection dir);
ClipboardDirection clipboardDirFromString(const QString &s);

// What the clipboard menu of the viewer can offer. Without a port, or for a
// guest system kemu-agent does not run on, nothing can work. While the agent
// is away, everything stays open: the settings apply once it comes. A
// connected agent says what it can.
struct ClipboardMenuState {
    enum class Agent {
        NoChannel, // the VM has no kemu-agent port
        UnsupportedGuest, // no kemu-agent for the guest system, and none connected
        Away, // not running, connecting or not responding
        NoGuestClipboard, // connected, without access to the clipboard of the guest session
        Ready,
    };
    Agent agent = Agent::NoChannel;
    // False only when a connected agent lacks them.
    bool guestSelection = true;
    bool guestFiles = true;
};

// guestSupported: kemu-agent exists for the guest system (MainWindow knows
// it from the VM configuration). An agent that connects counts for more.
ClipboardMenuState clipboardMenuState(const KemuAgentClient *agent, bool guestSupported);

/**
 * @brief Shares the host clipboard with the guest through kemu-agent.
 *
 * The clipboard part of the KEMU agent protocol (section 6), on top of a
 * KemuAgentClient. Each side announces what it holds with an offer; the
 * other side fetches it.
 *
 * Host to guest: when the host clipboard changes, everything shareable is
 * read at once (within a budget) into a snapshot. Wayland announces the
 * selection again on every focus change; identical content is not offered
 * twice. Requests are served from the snapshot.
 *
 * Guest to host: an offer from the guest is fetched at once, within a
 * budget, and placed on the host clipboard with a private marker type that
 * keeps it from being offered back. QMimeData hands data to pasting
 * applications synchronously, so there is no waiting for the guest then.
 *
 * When both sides copy at the same moment, the host wins (section 6.2).
 */
class AgentClipboard : public QObject
{
    Q_OBJECT

public:
    struct Config {
        int settleMs = 50;
        int primarySettleMs = 250;
        int requestTimeoutMs = 10000;
        qint64 snapshotBudget = 32ll * 1024 * 1024; // host content read per offer
        qint64 fetchBudget = 32ll * 1024 * 1024; // guest content fetched per offer
        qsizetype chunkSize = 64 * 1024;
        qsizetype pacingThreshold = 128 * 1024;
        int maxOpenRequests = 8;
        int maxServing = 8;
        // Files (section 6.8). An empty root means KEMU's cache directory.
        QString stagingRoot;
        int stagingGraceMs = 10 * 60 * 1000;
        int fileWindow = 3;
    };

    enum class Owner {
        None,
        Local, // the host's offer is in force
        Peer, // the guest's offer is in force
    };
    Q_ENUM(Owner)

    // Marks guest content on the host clipboard (private, never offered).
    static constexpr auto ProxyMarker = "application/x-kemu-guest-proxy"_L1;

    explicit AgentClipboard(ClipboardDirection direction, QObject *parent = nullptr);
    AgentClipboard(ClipboardDirection direction, const Config &config, QObject *parent = nullptr);
    ~AgentClipboard() override;

    // The agent of the VM; nullptr without one. Follows its sessions.
    void setAgent(KemuAgentClient *agent);
    [[nodiscard]] KemuAgentClient *agent() const
    {
        return m_agent;
    }

    void setDirection(ClipboardDirection direction);
    [[nodiscard]] ClipboardDirection direction() const
    {
        return m_direction;
    }
    // Keep entries that password managers mark as secret
    // (x-kde-passwordManagerHint) on their side, in both directions.
    void setFilterPasswords(bool filter);
    // Share the primary selection (select, middle-click) as well.
    void setSharePrimary(bool share);
    // Share copied files and folders as well (section 6.8), up to @p limit
    // bytes per copy. Off by default: the guest then writes files on the host.
    void setShareFiles(bool share);
    void setFilesLimit(quint64 limit);

    // Inspection, for the tests and the logs.
    [[nodiscard]] Owner owner(KemuAgentProtocol::Selection selection) const;
    [[nodiscard]] quint64 localSerial(KemuAgentProtocol::Selection selection) const;
    [[nodiscard]] quint64 peerSerial(KemuAgentProtocol::Selection selection) const;
    [[nodiscard]] bool isFetching(KemuAgentProtocol::Selection selection) const;
    [[nodiscard]] bool offersFiles(KemuAgentProtocol::Selection selection) const;
    [[nodiscard]] QString stagedDirectory(KemuAgentProtocol::Selection selection) const;

private:
    using Content = QList<std::pair<QString, QByteArray>>;

    struct Inbound {
        KemuAgentProtocol::Selection selection;
        QString type;
        QByteArray data;
        bool sizeSeen = false;
        QElapsedTimer lastActivity;
    };
    struct Outbound {
        quint64 id = 0;
        QByteArray data;
        qsizetype offset = 0;
    };
    struct Fetch {
        bool active = false;
        quint64 serial = 0;
        QStringList queue;
        Content done;
        qint64 bytes = 0;
        std::unique_ptr<ClipboardFiles::Receiver> files;
        bool filesPending = false;
        QStringList filePaths; // staged top-level entries, once complete
    };
    struct State {
        quint64 localSerial = 0;
        quint64 peerSerial = 0;
        Owner owner = Owner::None;
        Content offered; // snapshot of the host's offer in force
        bool offeredSecret = false;
        std::optional<ClipboardFiles::Listing> listing; // files of that offer
        QString stagedDirectory; // the guest's files on the host clipboard
        QByteArray sharedHash; // content already on both sides
        QByteArray peerText; // UTF-8 text of the guest's content, to spot echoes
        QStringList peerTypes;
        Fetch fetch;
        QTimer settle;
    };

    State &state(KemuAgentProtocol::Selection selection);
    const State &state(KemuAgentProtocol::Selection selection) const;
    [[nodiscard]] KemuAgentClient::Policy policyFor(ClipboardDirection direction, bool sharePrimary) const;
    [[nodiscard]] bool mayOffer(KemuAgentProtocol::Selection selection, const KemuAgentClient::Policy &policy) const;
    [[nodiscard]] bool acceptsPeer(KemuAgentProtocol::Selection selection, const KemuAgentClient::Policy &policy) const;
    [[nodiscard]] bool mayOfferFiles(KemuAgentProtocol::Selection selection, const KemuAgentClient::Policy &policy) const;
    [[nodiscard]] bool acceptsFiles(KemuAgentProtocol::Selection selection, const KemuAgentClient::Policy &policy) const;
    void applyPolicy(const KemuAgentClient::Policy &previous);

    void onSessionStarted();
    void onSessionEnded();
    void onMessage(quint16 type, const QCborMap &body);
    void onHostChanged(QClipboard::Mode mode);

    // Host to guest
    void examineHost(KemuAgentProtocol::Selection selection, bool force);
    std::optional<Content> snapshot(const QMimeData *data) const;
    void sendOffer(KemuAgentProtocol::Selection selection,
                   const Content &content,
                   bool secret,
                   const QByteArray &hash,
                   std::optional<ClipboardFiles::Listing> listing);
    void sendRelease(KemuAgentProtocol::Selection selection);
    void handleRequest(const QCborMap &body);
    void handleCancel(const QCborMap &body);
    void handleFileList(const QCborMap &body);
    void handleFileRead(const QCborMap &body);
    [[nodiscard]] QStringList localFilePaths(const QMimeData *data) const;
    [[nodiscard]] bool isStagedEcho(const QStringList &paths) const;
    void pumpOutbound();

    // Guest to host
    void handleOffer(const QCborMap &body);
    void handleRelease(const QCborMap &body);
    void handleData(const QCborMap &body);
    void handleFail(const QCborMap &body);
    void startFetch(KemuAgentProtocol::Selection selection, const QCborMap &sizes, const QCborMap &files);
    void startFileFetch(KemuAgentProtocol::Selection selection, quint64 count, quint64 size);
    void fileFetchEnded(KemuAgentProtocol::Selection selection, bool complete);
    void dropFiles(Fetch &fetch, bool discard);
    void retireStaged(State &s);
    [[nodiscard]] ClipboardFiles::Receiver *receiverOf(quint64 id) const;
    [[nodiscard]] int openFileRequests() const;
    ClipboardFiles::Staging *staging();
    void pumpFetch();
    void cancelFetch(KemuAgentProtocol::Selection selection);
    void cancelInbound(quint64 id);
    void inboundDone(KemuAgentProtocol::Selection selection);
    void finishFetch(KemuAgentProtocol::Selection selection);
    void removeProxy(KemuAgentProtocol::Selection selection, std::optional<quint64> serial);
    [[nodiscard]] bool hostHoldsNewContent(KemuAgentProtocol::Selection selection) const;
    void checkTimeouts();

    void resetState();
    std::optional<KemuAgentProtocol::Selection> selectionOf(const QCborMap &body) const;
    void badMessage(quint16 type);
    static QByteArray contentHash(const Content &content);
    static bool isSecret(const Content &content);

    QPointer<KemuAgentClient> m_agent;
    QClipboard *m_clipboard;
    Config m_config;
    ClipboardDirection m_direction;
    bool m_filterPasswords = true;
    bool m_sharePrimary = false;
    bool m_shareFiles = false;
    quint64 m_filesLimit = 256ull * 1024 * 1024;

    std::array<State, KemuAgentProtocol::SelectionCount> m_states;
    QHash<quint64, Inbound> m_inbound;
    QList<Outbound> m_outbound;
    qsizetype m_nextOutbound = 0;
    bool m_pumping = false;
    quint64 m_nextRequestId = 0;
    QTimer m_timeouts;
    std::unique_ptr<ClipboardFiles::Staging> m_staging;
};
