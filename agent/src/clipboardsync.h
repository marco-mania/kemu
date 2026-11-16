/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QCborMap>
#include <QElapsedTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QTimer>

#include <array>
#include <memory>
#include <optional>

#include "clipboardfiles.h"
#include "protocol.h"
#include "session.h"

class ClipboardBackend;
class QMimeData;

// The clipboard part of the protocol (section 6), between a Session and
// the guest's clipboard.
//
// Guest to host: a change on the guest clipboard becomes an offer once it
// has settled; the host fetches what it needs. Small text is read right
// away, so it survives the source application quitting.
//
// Host to guest: an offer from the host is fetched at once, within a
// budget, and put on the guest clipboard with a private marker type that
// keeps it from being offered back. Pasting applications are served from
// that QMimeData, so they never wait for the host.
class ClipboardSync : public QObject
{
    Q_OBJECT

public:
    struct Config {
        int settleMs = 50;
        int primarySettleMs = 250;
        int requestTimeoutMs = 10000;
        qsizetype prefetchLimit = 64 * 1024; // per text type, guest to host
        qint64 fetchBudget = 32ll * 1024 * 1024; // per host offer
        qint64 maxReceive = qint64(Protocol::DefaultMaxTransfer); // per item, what HELLO announces
        qsizetype chunkSize = 64 * 1024;
        qsizetype pacingThreshold = 128 * 1024;
        int maxOpenRequests = 8;
        int maxServing = 8;
        // Files (section 6.8). An empty root means the user's cache directory.
        QString stagingRoot;
        int stagingGraceMs = 10 * 60 * 1000;
        int fileWindow = 3;
    };

    enum class Owner {
        None,
        Local,
        Peer,
    };
    Q_ENUM(Owner)

    ClipboardSync(Session *session, ClipboardBackend *backend, const Config &config, QObject *parent = nullptr);
    ClipboardSync(Session *session, ClipboardBackend *backend, QObject *parent = nullptr);

    // Inspection, for the tests and the logs.
    Owner owner(Protocol::Selection selection) const;
    quint64 localSerial(Protocol::Selection selection) const;
    quint64 peerSerial(Protocol::Selection selection) const;
    QStringList offeredTypes(Protocol::Selection selection) const;
    bool isFetching(Protocol::Selection selection) const;
    bool offersFiles(Protocol::Selection selection) const;
    QString stagedDirectory(Protocol::Selection selection) const;

private:
    struct Inbound {
        Protocol::Selection selection;
        quint64 serial = 0;
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
        QList<std::pair<QString, QByteArray>> done;
        qint64 bytes = 0;
        std::unique_ptr<ClipboardFiles::Receiver> files;
        bool filesPending = false;
        QStringList filePaths; // staged top-level entries, once complete
    };
    struct State {
        quint64 localSerial = 0;
        quint64 peerSerial = 0;
        Owner owner = Owner::None;
        bool heldLocally = false; // a guest application holds the selection
        QStringList offeredTypes; // our offer in force
        QHash<QString, QByteArray> cache; // read ahead for that offer
        std::optional<ClipboardFiles::Listing> listing; // files of that offer
        QString stagedDirectory; // the host's files on our clipboard
        QStringList peerTypes;
        QByteArray peerText; // UTF-8 text of the host's content, to spot echoes
        Fetch fetch;
        QTimer settle;
    };

    State &state(Protocol::Selection selection);
    const State &state(Protocol::Selection selection) const;

    bool mayOffer(Protocol::Selection selection, const Session::Policy &policy) const;
    bool acceptsPeer(Protocol::Selection selection, const Session::Policy &policy) const;
    bool mayOfferFiles(Protocol::Selection selection, const Session::Policy &policy) const;
    bool acceptsFiles(Protocol::Selection selection, const Session::Policy &policy) const;

    void onPolicyChanged(const Session::Policy &previous);
    void onReset();
    void onMessage(quint16 type, const QCborMap &body);
    void onLocalChanged(Protocol::Selection selection);

    // Guest to host
    void examineLocal(Protocol::Selection selection);
    void
    sendOffer(Protocol::Selection selection, const QStringList &types, const QHash<QString, QByteArray> &cache, std::optional<ClipboardFiles::Listing> listing);
    void sendRelease(Protocol::Selection selection);
    void handleRequest(const QCborMap &body);
    void handleCancel(const QCborMap &body);
    void handleFileList(const QCborMap &body);
    void handleFileRead(const QCborMap &body);
    QStringList localFilePaths(const QMimeData *data) const;
    void pumpOutbound();
    std::optional<QByteArray> readType(const QMimeData *data, const QString &type) const;

    // Host to guest
    void handleOffer(const QCborMap &body);
    void handleRelease(const QCborMap &body);
    void handleData(const QCborMap &body);
    void handleFail(const QCborMap &body);
    void startFetch(Protocol::Selection selection, const QCborMap &sizes, const QCborMap &files);
    void startFileFetch(Protocol::Selection selection, quint64 count, quint64 size);
    void fileFetchEnded(Protocol::Selection selection, bool complete);
    void dropFiles(Fetch &fetch, bool discard);
    ClipboardFiles::Receiver *receiverOf(quint64 id) const;
    int openFileRequests() const;
    ClipboardFiles::Staging *staging();
    void pumpFetch();
    void cancelFetch(Protocol::Selection selection);
    void cancelInbound(quint64 id);
    void inboundDone(Protocol::Selection selection);
    void finishFetch(Protocol::Selection selection);
    void checkTimeouts();

    std::optional<Protocol::Selection> selectionOf(const QCborMap &body) const;
    void badMessage(quint16 type);

    Session *m_session;
    ClipboardBackend *m_backend;
    Config m_config;
    std::array<State, Protocol::SelectionCount> m_states;
    QHash<quint64, Inbound> m_inbound;
    QList<Outbound> m_outbound;
    qsizetype m_nextOutbound = 0;
    bool m_pumping = false;
    quint64 m_nextRequestId = 0;
    QTimer m_timeouts;
    std::unique_ptr<ClipboardFiles::Staging> m_staging;
};
