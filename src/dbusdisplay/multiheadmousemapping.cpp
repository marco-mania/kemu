/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "multiheadmousemapping.h"

#include <QLoggingCategory>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

// Off by default; QT_LOGGING_RULES="org.kde.kemu.dbusdisplay.multihead.debug=true"
Q_LOGGING_CATEGORY(lcMultihead, "org.kde.kemu.dbusdisplay.multihead", QtWarningMsg)

namespace
{
constexpr qint64 kSettleMsecs = 100;
constexpr qint64 kResponseMsecs = 250;
// Two scalings and libinput leave a pixel or two of rounding.
constexpr int kAgentTolerance = 6;
constexpr int kAgentWindow = 8;
constexpr int kMinSamples = 4;
constexpr int kMaxSamplesPerHead = 12;
constexpr int kFailedFitsBeforeReport = 3;

// QEMU truncates position * 0x7fff / extent; libinput spreads the 0x8000
// tablet steps over the whole guest desktop.
int tabletValue(int primaryPosition, int primaryExtent)
{
    return int(qint64(primaryPosition) * 0x7fff / primaryExtent);
}

double guestPosition(int primaryPosition, int primaryExtent, int desktopExtent)
{
    return double(tabletValue(primaryPosition, primaryExtent)) * desktopExtent / 0x8000;
}

struct AxisSample {
    int tablet;
    int position;
    bool secondary;
};

// Residual of a reported position if display 2 sat at offset. The guest
// reports whole pixels, so compare against their centre.
double residual(const AxisSample &sample, int offset, int primaryExtent, int secondaryExtent)
{
    const int start = std::min(0, offset);
    const int extent = std::max(primaryExtent, offset + secondaryExtent) - start;
    const double desktop = double(sample.tablet) * extent / 0x8000 + start;
    return desktop - (sample.secondary ? offset : 0) - (sample.position + 0.5);
}

struct AxisFit {
    int offset = 0;
    bool fits = false; // the best offset explains the samples
    bool unique = false; // and no offset apart from its neighbours does
};

// Offset of display 2 along one axis that best explains where the guest
// put the pointer, under the same model as guestPosition(). Ties go to the
// offset closest to the reference.
AxisFit fitOffset(const QList<AxisSample> &samples, int primaryExtent, int secondaryExtent, int reference)
{
    constexpr double maxRmsError = 2.0;
    constexpr double epsilon = 1e-6;
    const int range = primaryExtent + secondaryExtent;

    QList<double> rms;
    rms.reserve(2 * range + 1);
    int best = 0;
    for (int offset = -range; offset <= range; ++offset) {
        double error = 0;
        for (const AxisSample &sample : samples) {
            const double r = residual(sample, offset, primaryExtent, secondaryExtent);
            error += r * r;
        }
        rms.append(std::sqrt(error / samples.size()));
        const double bestRms = rms.at(best);
        if (rms.size() == 1 || rms.last() < bestRms - epsilon
            || (rms.last() <= bestRms + epsilon && std::abs(offset - reference) < std::abs(best - range - reference)))
            best = int(rms.size()) - 1;
    }

    AxisFit fit;
    fit.offset = best - range;
    fit.fits = rms.at(best) <= maxRmsError;

    // Offsets next to the best one fit almost as well. A fitting offset
    // outside that run means a second, different layout.
    qsizetype first = best;
    qsizetype last = best;
    while (first > 0 && rms.at(first - 1) <= maxRmsError)
        --first;
    while (last + 1 < rms.size() && rms.at(last + 1) <= maxRmsError)
        ++last;
    fit.unique = true;
    for (qsizetype i = 0; i < rms.size(); ++i) {
        if ((i < first || i > last) && rms.at(i) <= maxRmsError)
            fit.unique = false;
    }
    return fit;
}

// One primary step spans several desktop pixels, so the step nearest to
// the target can land on the neighbouring head. Aim at the pixel centre
// and keep away from inner seams.
int primaryCoordinate(int desktopPosition, int headStart, int headEnd, int primaryExtent, int desktopExtent)
{
    constexpr double seamMargin = 0.25;
    const double target = desktopPosition + 0.5;
    const double low = headStart > 0 ? headStart + seamMargin : 0.0;
    const double high = headEnd < desktopExtent ? headEnd - seamMargin : double(desktopExtent);
    const auto distance = [&](int position) {
        return std::abs(guestPosition(position, primaryExtent, desktopExtent) - target);
    };

    const int estimate = int(qBound(0L, std::lround(target * primaryExtent / desktopExtent), long(primaryExtent - 1)));
    int position = estimate;
    for (int candidate = estimate - 1; candidate <= estimate + 1; ++candidate) {
        if (candidate >= 0 && candidate < primaryExtent && distance(candidate) < distance(position))
            position = candidate;
    }
    while (position + 1 < primaryExtent && guestPosition(position, primaryExtent, desktopExtent) < low)
        ++position;
    while (position > 0 && guestPosition(position, primaryExtent, desktopExtent) >= high)
        --position;
    return position;
}
} // namespace

MultiheadMouseMapping::MultiheadMouseMapping(const QString &primaryConsolePath, QObject *parent)
    : QObject(parent)
    , m_primaryConsolePath(primaryConsolePath)
{
}

void MultiheadMouseMapping::setDisplaySize(uint head, const QSize &size)
{
    storeDisplaySize(head, size);
}

// Samples only hold for the display sizes they were taken with.
void MultiheadMouseMapping::storeDisplaySize(uint head, const QSize &size)
{
    if (displaySize(head) == size)
        return;
    if (size.isEmpty())
        m_displaySizes.remove(head);
    else
        m_displaySizes.insert(head, size);
    m_pointerSamples.clear();
    m_pendingSample.reset();
    m_lastTablet.reset();
    m_pendingAgentSample.reset();
    m_failedFits = 0;
    m_failureReported = false;
}

void MultiheadMouseMapping::removeDisplay(uint head)
{
    storeDisplaySize(head, {});
    m_placeholderPending.remove(head);
    setCursorVisible(head, false);
}

QSize MultiheadMouseMapping::displaySize(uint head) const
{
    return m_displaySizes.value(head);
}

bool MultiheadMouseMapping::hasDisplay(uint head) const
{
    return m_displaySizes.contains(head);
}

void MultiheadMouseMapping::attachHead(uint head)
{
    if (head == 0)
        return;
    storeDisplaySize(head, {});
    m_placeholderPending.insert(head);
}

void MultiheadMouseMapping::handleScanout(uint head, const QSize &size)
{
    if (size.isEmpty()) {
        storeDisplaySize(head, {});
        if (head != 0)
            m_placeholderPending.insert(head);
        setCursorVisible(head, false);
        return;
    }
    if (m_placeholderPending.remove(head))
        return;
    storeDisplaySize(head, size);
}

void MultiheadMouseMapping::setSecondaryPlacement(SecondaryPlacement placement)
{
    m_secondaryPlacement = placement;
}

MultiheadMouseMapping::SecondaryPlacement MultiheadMouseMapping::secondaryPlacement() const
{
    return m_secondaryPlacement;
}

void MultiheadMouseMapping::setSecondaryPosition(const QPoint &position)
{
    m_secondaryPosition = position;
}

QPoint MultiheadMouseMapping::secondaryPosition() const
{
    return m_secondaryPosition;
}

QRect MultiheadMouseMapping::secondaryGeometry(const QSize &primary, const QSize &secondary, SecondaryPlacement placement, const QPoint &customPosition)
{
    if (primary.isEmpty() || secondary.isEmpty())
        return {};

    QPoint origin;
    switch (placement) {
    case SecondaryPlacement::Right:
    case SecondaryPlacement::Automatic: // until the guest has told us
        origin.setX(primary.width());
        break;
    case SecondaryPlacement::Left:
        origin.setX(-secondary.width());
        break;
    case SecondaryPlacement::Above:
        origin.setY(-secondary.height());
        break;
    case SecondaryPlacement::Below:
        origin.setY(primary.height());
        break;
    case SecondaryPlacement::Custom:
        origin = customPosition;
        break;
    }
    return QRect(origin, secondary);
}

std::optional<QRect> MultiheadMouseMapping::displayGeometry(uint head) const
{
    const QSize primary = displaySize(0);
    const QSize size = displaySize(head);
    if (primary.isEmpty() || size.isEmpty())
        return std::nullopt;
    if (head == 0)
        return QRect(QPoint(), primary);
    if (head != 1)
        return std::nullopt;
    if (m_secondaryPlacement == SecondaryPlacement::Automatic && m_detectedPosition)
        return QRect(*m_detectedPosition, size);
    return secondaryGeometry(primary, size, m_secondaryPlacement, m_secondaryPosition);
}

void MultiheadMouseMapping::setGuestAgentPointer(bool active)
{
    if (m_guestAgentPointer == active)
        return;
    m_guestAgentPointer = active;
    // Positions sent before the switch reached the guest another way.
    m_pendingSample.reset();
    m_lastTablet.reset();
    resetAgentChecks();
    qCDebug(lcMultihead) << "Guest agent places the pointer:" << active;
    Q_EMIT guestAgentPointerChanged(active);
}

bool MultiheadMouseMapping::hasGuestAgentPointer() const
{
    return m_guestAgentPointer;
}

void MultiheadMouseMapping::noteAgentPointerSent(uint head, const QPoint &position, qint64 msecs)
{
    if (!m_guestAgentPointer)
        return;
    if (m_agentSent && m_agentSent->head == head && m_agentSent->position == position)
        return;
    acceptPendingAgentSample(msecs);
    m_agentSent = AgentPointer{head, position};
    m_agentSentMsecs = msecs;
}

void MultiheadMouseMapping::noteAgentReport(uint head, const QPoint &position, qint64 msecs)
{
    if (!m_agentSent || m_agentSent->head != head || msecs - m_agentSentMsecs > kResponseMsecs) {
        m_pendingAgentSample.reset();
        return;
    }
    const QSize size = displaySize(head);
    if (size.isEmpty() || position.x() <= 0 || position.y() <= 0 || position.x() >= size.width() - 1 || position.y() >= size.height() - 1)
        return;
    m_pendingAgentSample = std::pair{m_agentSent->position, position};
}

void MultiheadMouseMapping::acceptPendingAgentSample(qint64 msecs)
{
    if (!m_pendingAgentSample)
        return;
    const auto [sent, shown] = *std::exchange(m_pendingAgentSample, std::nullopt);
    if (msecs - m_agentSentMsecs < kSettleMsecs)
        return;
    const QPoint miss = shown - sent;
    const bool hit = qAbs(miss.x()) <= kAgentTolerance && qAbs(miss.y()) <= kAgentTolerance;
    qCDebug(lcMultihead) << "Agent placed" << sent << "at" << shown;
    m_agentHits.append(hit);
    if (m_agentHits.size() > kAgentWindow)
        m_agentHits.removeFirst();

    // Points near where the layouts agree hit even when the scales differ,
    // so half the window missing is enough.
    if (!m_agentMissReported && m_agentHits.count(false) >= kAgentWindow / 2) {
        m_agentMissReported = true;
        Q_EMIT guestAgentPointerMisplaced();
    } else if (m_agentMissReported && m_agentHits.size() == kAgentWindow && !m_agentHits.contains(false)) {
        m_agentMissReported = false;
    }
}

void MultiheadMouseMapping::resetAgentChecks()
{
    m_agentSent.reset();
    m_pendingAgentSample.reset();
    m_agentHits.clear();
    m_agentMissReported = false;
}

std::optional<QPoint> MultiheadMouseMapping::mapToPrimary(uint head, const QPoint &position) const
{
    if (m_guestAgentPointer)
        return std::nullopt;
    const auto primary = displayGeometry(0);
    const auto secondary = displayGeometry(1);
    if (!primary || !secondary)
        return std::nullopt;

    const auto geometry = displayGeometry(head);
    if (!geometry)
        return std::nullopt;

    const QRect desktop = primary->united(*secondary);
    const QPoint local(qBound(0, position.x(), geometry->width() - 1),
                       qBound(0, position.y(), geometry->height() - 1));
    const QPoint origin = geometry->topLeft() - desktop.topLeft();
    const int mappedX = primaryCoordinate(origin.x() + local.x(), origin.x(), origin.x() + geometry->width(), primary->width(), desktop.width());
    const int mappedY = primaryCoordinate(origin.y() + local.y(), origin.y(), origin.y() + geometry->height(), primary->height(), desktop.height());
    return QPoint(mappedX, mappedY);
}

const QString &MultiheadMouseMapping::primaryConsolePath() const
{
    return m_primaryConsolePath;
}

void MultiheadMouseMapping::setCursorVisible(uint head, bool visible)
{
    if (m_cursorHeads.contains(head) == visible)
        return;
    if (visible)
        m_cursorHeads.insert(head);
    else
        m_cursorHeads.remove(head);
    Q_EMIT cursorHeadsChanged();
}

bool MultiheadMouseMapping::isCursorOnOtherHead(uint head) const
{
    return !m_cursorHeads.isEmpty() && !m_cursorHeads.contains(head);
}

void MultiheadMouseMapping::notePointerSent(const QPoint &primaryPosition, qint64 msecs)
{
    if (m_guestAgentPointer)
        return;
    const QSize primary = displaySize(0);
    if (primary.isEmpty())
        return;
    // Repeats, e.g. from a click, leave the guest pointer where it is.
    const QPoint tablet(tabletValue(primaryPosition.x(), primary.width()), tabletValue(primaryPosition.y(), primary.height()));
    if (m_lastTablet == tablet)
        return;
    acceptPendingSample(msecs);
    m_lastTablet = tablet;
    m_lastTabletMsecs = msecs;
}

void MultiheadMouseMapping::noteGuestPointer(uint head, const QPoint &position, qint64 msecs)
{
    if (m_guestAgentPointer) {
        noteAgentReport(head, position, msecs);
        return;
    }
    if (!m_lastTablet)
        return;
    // A report long after the last position sent is the guest's own doing,
    // e.g. KWin moving the pointer after the displays were rearranged.
    if (msecs - m_lastTabletMsecs > kResponseMsecs) {
        qCDebug(lcMultihead) << "Ignoring unprompted cursor report on head" << head << position;
        m_pendingSample.reset();
        return;
    }
    // A cursor image overlapping a seam shows on both heads, and positions
    // outside every head are clamped onto the nearest edge.
    const QSize size = displaySize(head);
    if (head > 1 || size.isEmpty() || position.x() <= 0 || position.y() <= 0 || position.x() >= size.width() - 1 || position.y() >= size.height() - 1)
        return;
    m_pendingSample = PendingSample{{*m_lastTablet, position}, head};
}

std::optional<QPoint> MultiheadMouseMapping::detectedPosition() const
{
    return m_detectedPosition;
}

void MultiheadMouseMapping::setDetectedPosition(const QPoint &position)
{
    m_detectedPosition = position;
    m_relearning = false;
}

// The latest report counts once the position it answers has stood for the
// settle time: by then a report still in flight for an earlier position
// has been replaced.
void MultiheadMouseMapping::acceptPendingSample(qint64 msecs)
{
    if (!m_pendingSample)
        return;
    const PendingSample pending = *std::exchange(m_pendingSample, std::nullopt);
    if (msecs - m_lastTabletMsecs < kSettleMsecs)
        return;
    const QPoint &position = pending.sample.position;

    // A clear contradiction means the guest layout changed: start over.
    constexpr double maxDeviation = 8.0;
    const QSize primary = displaySize(0);
    const QSize secondary = displaySize(1);
    if (m_detectedPosition && !m_relearning && !primary.isEmpty() && !secondary.isEmpty()) {
        const bool onSecondary = pending.head == 1;
        const AxisSample x{pending.sample.tablet.x(), position.x(), onSecondary};
        const AxisSample y{pending.sample.tablet.y(), position.y(), onSecondary};
        if (std::abs(residual(x, m_detectedPosition->x(), primary.width(), secondary.width())) > maxDeviation
            || std::abs(residual(y, m_detectedPosition->y(), primary.height(), secondary.height())) > maxDeviation) {
            qCDebug(lcMultihead) << "Sample contradicts" << *m_detectedPosition << "- relearning";
            m_pointerSamples.clear();
            m_relearning = true;
        }
    }

    qCDebug(lcMultihead) << "Sample on head" << pending.head << "tablet" << pending.sample.tablet << "pointer" << position;
    auto &samples = m_pointerSamples[pending.head];
    samples.append(pending.sample);
    if (samples.size() > kMaxSamplesPerHead)
        samples.removeFirst();
    detectSecondaryPosition();
}

void MultiheadMouseMapping::detectSecondaryPosition()
{
    const QSize primary = displaySize(0);
    const QSize secondary = displaySize(1);
    const QList<PointerSample> onPrimary = m_pointerSamples.value(0);
    const QList<PointerSample> onSecondary = m_pointerSamples.value(1);
    if (primary.isEmpty() || secondary.isEmpty() || onPrimary.isEmpty() || onSecondary.isEmpty() || onPrimary.size() + onSecondary.size() < kMinSamples)
        return;

    QList<AxisSample> horizontal;
    QList<AxisSample> vertical;
    for (const auto &[samples, isSecondary] : {std::pair{&onPrimary, false}, std::pair{&onSecondary, true}}) {
        for (const PointerSample &sample : *samples) {
            horizontal.append({sample.tablet.x(), sample.position.x(), isSecondary});
            vertical.append({sample.tablet.y(), sample.position.y(), isSecondary});
        }
    }

    const QPoint reference = m_detectedPosition.value_or(QPoint(primary.width(), 0));
    const AxisFit x = fitOffset(horizontal, primary.width(), secondary.width(), reference.x());
    const AxisFit y = fitOffset(vertical, primary.height(), secondary.height(), reference.y());
    qCDebug(lcMultihead) << "Fit on" << onPrimary.size() << "+" << onSecondary.size() << "samples:" << QPoint(x.offset, y.offset) << "fits" << x.fits << y.fits
                         << "unique" << x.unique << y.unique;
    if (!x.fits || !y.fits) {
        if (++m_failedFits >= kFailedFitsBeforeReport && !m_failureReported) {
            m_failureReported = true;
            Q_EMIT layoutDetectionFailed();
        }
        return;
    }
    // Few samples can leave more than one layout possible; wait for more.
    if (!x.unique || !y.unique)
        return;

    const QPoint detected(x.offset, y.offset);
    m_relearning = false;
    m_failedFits = 0;
    m_failureReported = false;
    if (m_detectedPosition && (*m_detectedPosition - detected).manhattanLength() < 2)
        return;
    m_detectedPosition = detected;
    Q_EMIT secondaryPositionDetected(detected);
}
