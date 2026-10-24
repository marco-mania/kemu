/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <functional>
#include <optional>

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalSocket>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>
#include <QTimer>

/**
 * @brief Talks to the QEMU guest agent (qemu-ga) inside a running VM.
 *
 * QEMU connects a virtio-serial port to one end of a socketpair; this client
 * owns the other end. Whether the guest has opened the port is only known
 * to QEMU, which reports it through QMP (VSERPORT_CHANGE, query-chardev) -
 * the owner passes that on with setPortOpen().
 *
 * Before the first command and after any hiccup, the stream is brought back
 * in step with guest-sync-delimited: the agent answers it behind a 0xFF
 * byte, so anything left over from an earlier session can be skipped.
 */
class GuestAgentClient : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Waiting, // the guest has not opened the port: no agent running
        Syncing, // the port is open, the agent has not answered yet
        Connected,
        Unresponsive, // the port is open, but the agent does not answer
    };
    Q_ENUM(Status)

    using Callback = std::function<void(bool ok, const QJsonValue &result, const QString &error)>;

    // What the agent tells about the guest; empty fields are unknown.
    struct GuestInfo {
        QString hostName;
        QString osName; // e.g. "Ubuntu 24.04.1 LTS"
        QStringList addresses; // IPv4 first; no loopback or link-local

        bool operator==(const GuestInfo &other) const = default;
    };

    explicit GuestAgentClient(QObject *parent = nullptr);
    ~GuestAgentClient() override;

    // Takes ownership of @p socketFd, one end of a connected socketpair.
    void connectToVm(int socketFd);

    // From QMP: whether the guest side of the port is open.
    void setPortOpen(bool open);

    [[nodiscard]] Status status() const
    {
        return m_status;
    }
    [[nodiscard]] bool isConnected() const
    {
        return m_status == Status::Connected;
    }
    // From guest-info, once connected.
    [[nodiscard]] QString version() const
    {
        return m_version;
    }

    // Read once connected and every kInfoIntervalMs.
    [[nodiscard]] GuestInfo guestInfo() const
    {
        return m_info;
    }

    // Commands run one after the other. Fails at once unless connected.
    // A command still unanswered after @p timeoutMs counts as a hung agent.
    void sendCommand(const QString &command, const QJsonObject &arguments, Callback callback, int timeoutMs = kCommandTimeoutMs);

    // For commands the agent does not answer when they succeed, like
    // guest-shutdown. False unless connected.
    bool sendCommandNoReply(const QString &command, const QJsonObject &arguments);

    // Reads host name, OS and addresses again.
    void refreshGuestInfo();

    // Brings the stream back in step before the next command, e.g. after
    // the guest was put back to an earlier state.
    void resync();

    static constexpr int kCommandTimeoutMs = 10000;
    static constexpr int kRetryIntervalMs = 15000;
    static constexpr int kInfoIntervalMs = 30000;

    // Exposed for testing.
    [[nodiscard]] static QStringList addressesFromInterfaces(const QJsonArray &interfaces);

Q_SIGNALS:
    void statusChanged(GuestAgentClient::Status status);
    void guestInfoChanged();

private:
    struct Request {
        QString command;
        QJsonObject arguments;
        Callback callback;
        bool noReply = false;
        int timeoutMs = kCommandTimeoutMs;
    };

    void startSync();
    void sendNext();
    void writeRequest(const QString &command, const QJsonObject &arguments);
    void onReadyRead();
    void handleLine(const QByteArray &line);
    void onTimeout();
    void setStatus(Status status);
    void failAll(const QString &error);
    void setGuestInfo(const GuestInfo &info);

    QLocalSocket m_socket;
    QByteArray m_buffer;
    QTimer m_timeout;
    QTimer m_retry;
    QTimer m_infoTimer;

    Status m_status = Status::Waiting;
    bool m_portOpen = false;
    QString m_version;
    GuestInfo m_info;
    // A command went out without a reply to wait for: whatever the agent
    // still sends (an error) would be taken for the next reply.
    bool m_needsSync = false;

    // guest-sync-delimited in flight: skip input up to the 0xFF, then expect
    // the reply carrying m_syncId.
    bool m_syncing = false;
    bool m_skipToDelimiter = false;
    qint64 m_syncId = 0;

    QQueue<Request> m_queue;
    std::optional<Request> m_inFlight;
};
