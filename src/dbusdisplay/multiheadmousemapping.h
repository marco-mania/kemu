/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QMap>
#include <QObject>
#include <QPoint>
#include <QRect>
#include <QSet>
#include <QSize>
#include <QString>

#include <optional>
#include <utility>

class MultiheadMouseMapping : public QObject
{
    Q_OBJECT

public:
    enum class SecondaryPlacement {
        Right,
        Left,
        Above,
        Below,
        Custom,
        Automatic
    };

    explicit MultiheadMouseMapping(const QString &primaryConsolePath, QObject *parent = nullptr);

    void setDisplaySize(uint head, const QSize &size);
    void removeDisplay(uint head);
    QSize displaySize(uint head) const;
    bool hasDisplay(uint head) const;

    // QEMU replays the current surface to a new listener and follows every
    // Disable with a placeholder of the old size. Neither proves that the
    // guest drives a secondary head, so the first scanout after both is
    // not counted. An empty size is a Disable.
    void attachHead(uint head);
    void handleScanout(uint head, const QSize &size);

    void setSecondaryPlacement(SecondaryPlacement placement);
    SecondaryPlacement secondaryPlacement() const;
    void setSecondaryPosition(const QPoint &position);
    QPoint secondaryPosition() const;
    std::optional<QRect> displayGeometry(uint head) const;
    static QRect secondaryGeometry(const QSize &primary, const QSize &secondary, SecondaryPlacement placement, const QPoint &customPosition = {});

    // spice-vdagent in the guest knows the layout and places the pointer from
    // positions on each head's own console. Mapping and detection then rest.
    void setGuestAgentPointer(bool active);
    bool hasGuestAgentPointer() const;
    // The guest should then show the pointer where it was sent. It does not
    // when the guest scales its displays differently: spice-vdagent reads the
    // layout from XWayland, which scales all displays alike.
    void noteAgentPointerSent(uint head, const QPoint &position, qint64 msecs);

    std::optional<QPoint> mapToPrimary(uint head, const QPoint &position) const;
    const QString &primaryConsolePath() const;

    // The guest shows its cursor on one head at a time. Shown on another
    // head than the one under the host pointer, host and guest disagree
    // about where the pointer is.
    void setCursorVisible(uint head, bool visible);
    bool isCursorOnOtherHead(uint head) const;

    // Automatic placement learns the guest layout from the guest's own
    // cursor reports: once a position sent has stood for a moment, it and
    // where the guest shows the pointer form a sample.
    void notePointerSent(const QPoint &primaryPosition, qint64 msecs);
    void noteGuestPointer(uint head, const QPoint &position, qint64 msecs);
    std::optional<QPoint> detectedPosition() const;
    void setDetectedPosition(const QPoint &position);

Q_SIGNALS:
    void cursorHeadsChanged();
    void guestAgentPointerChanged(bool active);
    // Most recent positions missed. Reported again after a run of hits.
    void guestAgentPointerMisplaced();
    void secondaryPositionDetected(const QPoint &position);
    // Enough samples, but no position of display 2 explains them, e.g.
    // because the guest scales its displays. Reported once per attempt.
    void layoutDetectionFailed();

private:
    struct PointerSample {
        QPoint tablet;
        QPoint position;
    };
    struct PendingSample {
        PointerSample sample;
        uint head = 0;
    };
    struct AgentPointer {
        uint head = 0;
        QPoint position;
    };

    void storeDisplaySize(uint head, const QSize &size);
    void acceptPendingSample(qint64 msecs);
    void detectSecondaryPosition();
    void noteAgentReport(uint head, const QPoint &position, qint64 msecs);
    void acceptPendingAgentSample(qint64 msecs);
    void resetAgentChecks();

    QString m_primaryConsolePath;
    QMap<uint, QSize> m_displaySizes;
    QSet<uint> m_placeholderPending;
    QSet<uint> m_cursorHeads;
    QMap<uint, QList<PointerSample>> m_pointerSamples;
    std::optional<PendingSample> m_pendingSample;
    std::optional<QPoint> m_lastTablet;
    qint64 m_lastTabletMsecs = 0;
    std::optional<QPoint> m_detectedPosition;
    bool m_relearning = false;
    bool m_guestAgentPointer = false;
    std::optional<AgentPointer> m_agentSent;
    qint64 m_agentSentMsecs = 0;
    std::optional<std::pair<QPoint, QPoint>> m_pendingAgentSample;
    QList<bool> m_agentHits;
    bool m_agentMissReported = false;
    int m_failedFits = 0;
    bool m_failureReported = false;
    SecondaryPlacement m_secondaryPlacement = SecondaryPlacement::Right;
    QPoint m_secondaryPosition = QPoint(1920, 0);
};
