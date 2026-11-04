/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "spectile.h"

#include <algorithm>

#include <QDragEnterEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QMimeData>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>

#include <KColorScheme>

using namespace Qt::StringLiterals;

namespace
{
// As the disk usage bar always did: green, orange from 80 %, red from 90 %.
KColorScheme::ForegroundRole usageRole(double fraction)
{
    return fraction < 0.8 ? KColorScheme::PositiveText : fraction < 0.9 ? KColorScheme::NeutralText : KColorScheme::NegativeText;
}
}

SpecTile::SpecTile(const QIcon &icon, const QString &caption, QWidget *parent)
    : SpecTile(Kind::KeyFigure, parent)
{
    setIcon(icon);
    setCaption(caption);
}

SpecTile::SpecTile(Kind kind, QWidget *parent)
    : QAbstractButton(parent)
    , m_kind(kind)
{
    setAttribute(Qt::WA_Hover);
    // Equal shares of the row, whatever the values say.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    contentChanged();
}

void SpecTile::setIconName(const QString &name)
{
    if (name == m_iconName)
        return;
    m_iconName = name;
    setIcon(QIcon::fromTheme(name));
}

void SpecTile::setEmblemName(const QString &name)
{
    if (name == m_emblemName)
        return;
    m_emblemName = name;
    update();
}

void SpecTile::setCaption(const QString &caption)
{
    if (caption == m_caption)
        return;
    m_caption = caption;
    contentChanged();
}

void SpecTile::setValue(const QString &value)
{
    if (value == m_value)
        return;
    m_value = value;
    contentChanged();
}

void SpecTile::setDetail(const QString &detail, bool attention)
{
    if (detail == m_detail && attention == m_attention)
        return;
    m_detail = detail;
    m_attention = attention;
    contentChanged();
}

void SpecTile::setMuted(bool muted)
{
    if (muted == m_muted)
        return;
    m_muted = muted;
    update();
}

void SpecTile::setUsage(double fraction)
{
    fraction = fraction < 0 ? -1 : std::min(fraction, 1.0);
    if (fraction == m_usage)
        return;
    const bool shown = m_usage >= 0;
    m_usage = fraction;
    if (shown != (m_usage >= 0))
        contentChanged();
    else
        update();
}

void SpecTile::setInteractive(bool interactive)
{
    m_interactive = interactive;
    setFocusPolicy(interactive ? Qt::StrongFocus : Qt::NoFocus);
    update();
}

void SpecTile::setMenuIndicator(bool shown)
{
    if (shown == m_menuIndicator)
        return;
    m_menuIndicator = shown;
    update();
}

void SpecTile::setDropSuffixes(const QStringList &suffixes)
{
    m_dropSuffixes = suffixes;
    setAcceptDrops(!suffixes.isEmpty());
}

QString SpecTile::droppedFile(const QMimeData *mime) const
{
    const QList<QUrl> urls = mime->urls();
    if (urls.size() != 1 || !urls.first().isLocalFile())
        return {};
    const QFileInfo file(urls.first().toLocalFile());
    return file.isFile() && m_dropSuffixes.contains(file.suffix(), Qt::CaseInsensitive) ? file.absoluteFilePath() : QString();
}

void SpecTile::dragEnterEvent(QDragEnterEvent *event)
{
    if (droppedFile(event->mimeData()).isEmpty())
        return;
    event->acceptProposedAction();
    m_dragOver = true;
    update();
}

void SpecTile::dragLeaveEvent(QDragLeaveEvent *)
{
    m_dragOver = false;
    update();
}

void SpecTile::dropEvent(QDropEvent *event)
{
    m_dragOver = false;
    update();
    const QString path = droppedFile(event->mimeData());
    if (path.isEmpty())
        return;
    event->acceptProposedAction();
    Q_EMIT fileDropped(path);
}

int SpecTile::indicatorWidth() const
{
    return m_menuIndicator ? fontMetrics().height() / 2 + padding() : 0;
}

void SpecTile::contentChanged()
{
    setText(m_caption);
    setAccessibleName(m_value.isEmpty() ? m_caption : m_caption + u": "_s + m_value);
    setAccessibleDescription(m_detail);
    setMinimumWidth(minimumSizeHint().width());
    updateGeometry();
    update();
}

QFont SpecTile::valueFont() const
{
    QFont value = font();
    if (m_kind == Kind::KeyFigure) {
        value.setPointSizeF(value.pointSizeF() * 1.4);
        value.setWeight(QFont::DemiBold);
    } else {
        value.setWeight(QFont::Medium);
    }
    return value;
}

QFont SpecTile::detailFont() const
{
    return QFontDatabase::systemFont(QFontDatabase::SmallestReadableFont);
}

QString SpecTile::secondLine() const
{
    // A card says what it is on the line below its value.
    if (m_caption.isEmpty() || m_detail.isEmpty())
        return m_caption + m_detail;
    return m_caption + u" · "_s + m_detail;
}

int SpecTile::padding() const
{
    return fontMetrics().height() / 2;
}

int SpecTile::iconExtent() const
{
    const QStyle::PixelMetric metric = m_kind == Kind::KeyFigure ? QStyle::PM_LargeIconSize : QStyle::PM_ToolBarIconSize;
    return std::min(style()->pixelMetric(metric, nullptr, this), textHeight());
}

int SpecTile::barHeight() const
{
    return m_kind == Kind::Card && m_usage >= 0 ? std::max(4, fontMetrics().height() / 4) : 0;
}

int SpecTile::textHeight() const
{
    const int value = QFontMetrics(valueFont()).height();
    const int detail = QFontMetrics(detailFont()).height();
    if (m_kind == Kind::Card)
        return value + (barHeight() > 0 ? barHeight() + padding() / 2 : 0) + detail;
    return value + fontMetrics().height() + detail;
}

QSize SpecTile::sizeHint() const
{
    const QFontMetrics detail(detailFont());
    int text = QFontMetrics(valueFont()).horizontalAdvance(m_value);
    if (m_kind == Kind::Card)
        text = std::max(text, detail.horizontalAdvance(secondLine()));
    else
        text = std::max({text, fontMetrics().horizontalAdvance(m_caption), detail.horizontalAdvance(m_detail)});
    return {3 * padding() + iconExtent() + text + indicatorWidth(), 2 * padding() + textHeight()};
}

QSize SpecTile::minimumSizeHint() const
{
    // Anything elides rather than widening its row.
    const int text = fontMetrics().averageCharWidth() * 8;
    return {3 * padding() + iconExtent() + text + indicatorWidth(), 2 * padding() + textHeight()};
}

void SpecTile::mousePressEvent(QMouseEvent *event)
{
    if (!m_interactive) {
        event->ignore();
        return;
    }
    QAbstractButton::mousePressEvent(event);
}

void SpecTile::mouseReleaseEvent(QMouseEvent *event)
{
    if (!m_interactive) {
        event->ignore();
        return;
    }
    QAbstractButton::mouseReleaseEvent(event);
}

void SpecTile::paintUsage(QPainter &painter, const QRect &area) const
{
    QColor track = palette().color(QPalette::WindowText);
    track.setAlphaF(0.15);
    const QColor used = KColorScheme(QPalette::Active, KColorScheme::View).foreground(usageRole(m_usage)).color();

    if (m_kind == Kind::KeyFigure) {
        // A ring, filled clockwise from the top.
        const qreal width = area.width() / 6.0;
        const QRectF ring = QRectF(area).adjusted(width / 2, width / 2, -width / 2, -width / 2);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(track, width));
        painter.drawEllipse(ring);
        painter.setPen(QPen(used, width, Qt::SolidLine, Qt::FlatCap));
        painter.drawArc(ring, 90 * 16, -qRound(m_usage * 360 * 16));
        return;
    }

    const qreal radius = area.height() / 2.0;
    painter.setPen(Qt::NoPen);
    painter.setBrush(track);
    painter.drawRoundedRect(area, radius, radius);
    if (m_usage > 0) {
        painter.setBrush(used);
        painter.drawRoundedRect(QRectF(area.left(), area.top(), std::max(area.height() * 1.0, area.width() * m_usage), area.height()), radius, radius);
    }
}

void SpecTile::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPalette &palette = this->palette();
    const KColorScheme scheme(isEnabled() ? QPalette::Active : QPalette::Disabled, KColorScheme::View);

    // A view background with a light frame; the highlight shows it reacts.
    QColor frame = palette.color(QPalette::WindowText);
    frame.setAlphaF(0.2);
    if (m_dragOver || (m_interactive && isEnabled() && (underMouse() || hasFocus())))
        frame = palette.color(QPalette::Highlight);
    painter.setPen(frame);
    painter.setBrush(palette.color(QPalette::Base));
    const QRectF outline = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.drawRoundedRect(outline, 3, 3);
    if (m_interactive && isDown()) {
        QColor pressed = palette.color(QPalette::Highlight);
        pressed.setAlphaF(0.15);
        painter.setPen(Qt::NoPen);
        painter.setBrush(pressed);
        painter.drawRoundedRect(outline, 3, 3);
    }

    QRect content = rect().adjusted(padding(), padding(), -padding(), -padding());
    if (m_menuIndicator) {
        QStyleOption arrow;
        arrow.initFrom(this);
        const int size = fontMetrics().height() / 2;
        arrow.rect = QRect(content.right() - size + 1, content.center().y() - size / 2, size, size);
        style()->drawPrimitive(QStyle::PE_IndicatorArrowDown, &arrow, &painter, this);
        content.setRight(content.right() - indicatorWidth());
    }
    const int extent = iconExtent();
    const QRect iconArea(content.left(), content.center().y() - extent / 2, extent, extent);
    if (m_kind == Kind::KeyFigure && m_usage >= 0)
        paintUsage(painter, iconArea);
    else
        icon().paint(&painter, iconArea, Qt::AlignCenter, isEnabled() ? QIcon::Normal : QIcon::Disabled);
    if (!m_emblemName.isEmpty()) {
        const int size = extent / 2;
        QIcon::fromTheme(m_emblemName).paint(&painter, QRect(iconArea.right() - size + 1, iconArea.bottom() - size + 1, size, size));
    }

    const int left = content.left() + extent + padding();
    const int width = std::max(0, content.right() - left);
    int top = content.center().y() - textHeight() / 2;
    const auto line = [&](const QFont &font, const QColor &color, const QString &text) {
        const QFontMetrics metrics(font);
        painter.setFont(font);
        painter.setPen(color);
        painter.drawText(QRect(left, top, width, metrics.height()), Qt::AlignLeft | Qt::AlignVCenter, metrics.elidedText(text, Qt::ElideRight, width));
        top += metrics.height();
    };
    const QColor normal = scheme.foreground(KColorScheme::NormalText).color();
    const QColor inactive = scheme.foreground(KColorScheme::InactiveText).color();
    const QColor detail = m_attention ? scheme.foreground(KColorScheme::NeutralText).color() : inactive;

    line(valueFont(), m_muted ? inactive : normal, m_value);
    if (m_kind == Kind::KeyFigure) {
        line(font(), normal, m_caption);
        line(detailFont(), detail, m_detail);
        return;
    }
    if (barHeight() > 0) {
        top += padding() / 4;
        paintUsage(painter, QRect(left, top, width, barHeight()));
        top += barHeight() + padding() / 4;
    }
    line(detailFont(), detail, secondLine());
}
