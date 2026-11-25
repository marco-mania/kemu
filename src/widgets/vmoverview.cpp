/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vmoverview.h"

#include <algorithm>
#include <functional>

#include <QEvent>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QVBoxLayout>

#include <KColorScheme>

#include "utils/utils.h"
#include "widgets/spectile.h"

using namespace Qt::StringLiterals;

namespace
{
QColor toneColor(VmOverview::Tone tone)
{
    const KColorScheme scheme(QPalette::Active, KColorScheme::Window);
    switch (tone) {
    case VmOverview::Tone::Positive:
        return scheme.foreground(KColorScheme::PositiveText).color();
    case VmOverview::Tone::Neutral:
        return scheme.foreground(KColorScheme::NeutralText).color();
    case VmOverview::Tone::Negative:
        return scheme.foreground(KColorScheme::NegativeText).color();
    case VmOverview::Tone::Inactive:
        break;
    }
    return scheme.foreground(KColorScheme::InactiveText).color();
}

void setLabelText(QLabel *label, const QString &text)
{
    if (label->text() != text)
        label->setText(text);
    label->setVisible(!text.isEmpty());
}
}

// The state of the VM in its color, on a tint of it.
class VmOverviewBadge : public QLabel
{
public:
    VmOverviewBadge()
    {
        QFont bold = font();
        bold.setWeight(QFont::DemiBold);
        setFont(bold);
        const int horizontal = fontMetrics().averageCharWidth();
        setContentsMargins(horizontal, 1, horizontal, 1);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    }

    void setTone(VmOverview::Tone tone)
    {
        m_tone = tone;
        Utils::setTextColor(this, toneColor(tone));
        update();
    }

    VmOverview::Tone tone() const
    {
        return m_tone;
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QColor tint = toneColor(m_tone);
            tint.setAlphaF(0.15);
            painter.setPen(Qt::NoPen);
            painter.setBrush(tint);
            painter.drawRoundedRect(QRectF(rect()), 3, 3);
        }
        QLabel::paintEvent(event);
    }

private:
    VmOverview::Tone m_tone = VmOverview::Tone::Inactive;
};

// Framed like the tiles, for the header, and clickable like them.
class VmOverviewFrame : public QWidget
{
public:
    explicit VmOverviewFrame(std::function<void()> activated)
        : m_activated(std::move(activated))
    {
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
    }

    void activate()
    {
        m_activated();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        // As SpecTile: the border shows hover and focus, a tint the press.
        QColor frame = palette().color(QPalette::WindowText);
        frame.setAlphaF(0.2);
        if (isEnabled() && (underMouse() || hasFocus()))
            frame = palette().color(QPalette::Highlight);
        QColor fill = palette().color(QPalette::Base);
        if (m_pressed) {
            QColor pressed = palette().color(QPalette::Highlight);
            pressed.setAlphaF(0.15);
            painter.setBrush(fill);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
            fill = pressed;
        }
        painter.setPen(frame);
        painter.setBrush(fill);
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }

    // Clicks on the labels with text to select or links reach the frame
    // through VmOverview::eventFilter() instead.
    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton) {
            QWidget::mousePressEvent(event);
            return;
        }
        m_pressed = true;
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton || !m_pressed) {
            QWidget::mouseReleaseEvent(event);
            return;
        }
        m_pressed = false;
        update();
        if (rect().contains(event->position().toPoint()))
            activate();
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        switch (event->key()) {
        case Qt::Key_Space:
        case Qt::Key_Return:
        case Qt::Key_Enter:
            activate();
            break;
        default:
            QWidget::keyPressEvent(event);
        }
    }

    bool event(QEvent *event) override
    {
        switch (event->type()) {
        case QEvent::HoverEnter:
        case QEvent::HoverLeave:
        case QEvent::FocusIn:
        case QEvent::FocusOut:
            update();
            break;
        default:
            break;
        }
        return QWidget::event(event);
    }

private:
    std::function<void()> m_activated;
    bool m_pressed = false;
};

// Tiles in as many equal columns as fit, up to a maximum.
class VmOverviewGrid : public QWidget
{
public:
    VmOverviewGrid(VmOverview *overview, SpecTile::Kind kind, int maxColumns, int tileWidth)
        : m_overview(overview)
        , m_kind(kind)
        , m_maxColumns(maxColumns)
        , m_tileWidth(tileWidth)
        , m_layout(new QGridLayout(this))
    {
        m_layout->setContentsMargins(0, 0, 0, 0);
    }

    void setCards(const QList<VmOverview::Card> &cards)
    {
        while (m_tiles.size() < cards.size()) {
            auto *tile = new SpecTile(m_kind, this);
            const qsizetype index = m_tiles.size();
            connect(tile, &SpecTile::clicked, m_overview, [this, tile, index] {
                Q_EMIT m_overview->cardActivated(m_ids.value(index), tile->mapToGlobal(tile->rect().bottomLeft()));
            });
            connect(tile, &SpecTile::fileDropped, m_overview, [this, index](const QString &path) {
                Q_EMIT m_overview->fileDropped(m_ids.value(index), path);
            });
            m_tiles.append(tile);
        }
        m_ids.clear();
        for (qsizetype i = 0; i < m_tiles.size(); ++i) {
            SpecTile *tile = m_tiles.at(i);
            if (i >= cards.size()) {
                tile->hide();
                continue;
            }
            const VmOverview::Card &card = cards.at(i);
            tile->setIconName(card.iconName);
            tile->setEmblemName(card.emblemName);
            tile->setValue(card.value);
            tile->setCaption(card.caption);
            tile->setDetail(card.detail, card.attention);
            tile->setMuted(card.muted);
            tile->setUsage(card.usage);
            tile->setToolTip(card.toolTip);
            tile->setInteractive(!card.id.isEmpty());
            tile->setMenuIndicator(card.menu);
            tile->setDropSuffixes(card.id.isEmpty() ? QStringList() : card.dropSuffixes);
            m_ids.append(card.id);
            tile->show();
        }
        const bool countChanged = m_count != cards.size();
        m_count = cards.size();
        setVisible(m_count > 0);
        if (countChanged)
            relayout(true);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        relayout(false);
    }

private:
    void relayout(bool force)
    {
        const int columns = std::clamp(width() / m_tileWidth, 1, m_maxColumns);
        if (columns == m_columns && !force)
            return;
        m_columns = columns;

        while (QLayoutItem *item = m_layout->takeAt(0))
            delete item;
        for (int i = 0; i < m_count; ++i)
            m_layout->addWidget(m_tiles.at(i), i / columns, i % columns);
        for (int column = 0; column < m_maxColumns; ++column)
            m_layout->setColumnStretch(column, column < columns ? 1 : 0);
    }

    VmOverview *m_overview;
    SpecTile::Kind m_kind;
    int m_maxColumns;
    int m_tileWidth;
    QGridLayout *m_layout;
    QList<SpecTile *> m_tiles;
    QStringList m_ids;
    qsizetype m_count = 0;
    int m_columns = 0;
};

VmOverview::VmOverview(QWidget *parent)
    : QScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setWidgetResizable(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto *container = new QWidget;
    auto *outer = new QVBoxLayout(container);
    const int margin = fontMetrics().height() * 3 / 4;
    outer->setContentsMargins(margin, margin, margin, margin);

    m_placeholder = new QWidget;
    auto *placeholder = new QVBoxLayout(m_placeholder);
    auto *placeholderIcon = new QLabel;
    placeholderIcon->setPixmap(QIcon::fromTheme(u"computer"_s).pixmap(style()->pixelMetric(QStyle::PM_LargeIconSize) * 2));
    placeholderIcon->setAlignment(Qt::AlignCenter);
    m_placeholderText = new QLabel;
    m_placeholderText->setAlignment(Qt::AlignCenter);
    m_placeholderText->setWordWrap(true);
    placeholder->addStretch();
    placeholder->addWidget(placeholderIcon);
    placeholder->addWidget(m_placeholderText);
    placeholder->addStretch(2);
    outer->addWidget(m_placeholder, 1);

    m_content = new QWidget;
    auto *content = new QVBoxLayout(m_content);
    content->setContentsMargins(0, 0, 0, 0);

    // Header: icon, name, state and what the guest is, framed like the tiles.
    // A click on it opens the VM's settings, on their first page.
    auto *headerFrame = new VmOverviewFrame([this] {
        Q_EMIT cardActivated(u"general"_s, m_header->mapToGlobal(m_header->rect().bottomLeft()));
    });
    m_header = headerFrame;
    auto *header = new QHBoxLayout(headerFrame);
    const int padding = fontMetrics().height() / 2;
    header->setContentsMargins(padding, padding, padding, padding);
    m_icon = new QLabel;
    m_icon->setAlignment(Qt::AlignTop);
    header->addWidget(m_icon);
    auto *headerText = new QVBoxLayout;
    headerText->setSpacing(fontMetrics().height() / 6);
    m_name = new QLabel;
    QFont nameFont = m_name->font();
    nameFont.setPointSizeF(nameFont.pointSizeF() + 3);
    nameFont.setBold(true);
    m_name->setFont(nameFont);
    m_name->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_name->installEventFilter(this);
    headerText->addWidget(m_name);
    auto *statusRow = new QHBoxLayout;
    m_status = new VmOverviewBadge;
    statusRow->addWidget(m_status);
    m_statusLinks = new QLabel;
    m_statusLinks->setTextFormat(Qt::RichText);
    m_statusLinks->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    connect(m_statusLinks, &QLabel::linkActivated, this, &VmOverview::linkActivated);
    connect(m_statusLinks, &QLabel::linkHovered, this, [this](const QString &link) {
        m_hoveredLink = link;
    });
    m_statusLinks->installEventFilter(this);
    statusRow->addWidget(m_statusLinks);
    statusRow->addStretch();
    headerText->addLayout(statusRow);
    m_system = new QLabel;
    m_system->setTextFormat(Qt::RichText);
    m_system->setWordWrap(true);
    m_system->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard | Qt::TextSelectableByMouse);
    connect(m_system, &QLabel::linkActivated, this, &VmOverview::linkActivated);
    connect(m_system, &QLabel::linkHovered, this, [this](const QString &link) {
        m_hoveredLink = link;
    });
    m_system->installEventFilter(this);
    headerText->addWidget(m_system);
    header->addLayout(headerText, 1);
    content->addWidget(headerFrame);
    content->addSpacing(margin / 3);

    // From the most important facts down: tile sizes follow.
    const int cardWidth = fontMetrics().averageCharWidth() * 28;
    m_keyFigures = new VmOverviewGrid(this, SpecTile::Kind::KeyFigure, 3, fontMetrics().averageCharWidth() * 18);
    m_hardware = new VmOverviewGrid(this, SpecTile::Kind::Card, 2, cardWidth);
    m_media = new VmOverviewGrid(this, SpecTile::Kind::Card, 2, cardWidth);
    m_wide = new VmOverviewGrid(this, SpecTile::Kind::Card, 1, 1);
    // A little more room between the groups than within them.
    for (VmOverviewGrid *grid : {m_keyFigures, m_hardware, m_media, m_wide}) {
        content->addWidget(grid);
        content->addSpacing(margin / 3);
    }

    content->addStretch();
    outer->addWidget(m_content);

    setWidget(container);
    applyColors();
    m_content->hide();
}

// The header's labels take mouse clicks for selecting text and following
// links. A plain click elsewhere on them still belongs to the header.
bool VmOverview::eventFilter(QObject *watched, QEvent *event)
{
    const bool press = event->type() == QEvent::MouseButtonPress;
    if (press || event->type() == QEvent::MouseButtonRelease) {
        const auto *mouse = static_cast<QMouseEvent *>(event);
        const QPoint pos = mouse->globalPosition().toPoint();
        if (mouse->button() == Qt::LeftButton) {
            if (press)
                m_pressPos = pos;
            else if (m_hoveredLink.isEmpty() && (pos - m_pressPos).manhattanLength() < QGuiApplication::styleHints()->startDragDistance())
                // Once the label is done with the release, as the settings
                // dialog is modal.
                QTimer::singleShot(0, m_header, [this] {
                    m_header->activate();
                });
        }
    }
    return QScrollArea::eventFilter(watched, event);
}

void VmOverview::showPlaceholder(const QString &text)
{
    m_placeholderText->setText(text);
    m_content->hide();
    m_placeholder->show();
}

void VmOverview::setContent(const Content &content)
{
    m_placeholder->hide();
    m_content->show();

    m_icon->setPixmap(content.icon);
    setLabelText(m_name, content.name);
    setLabelText(m_status, u"● "_s + content.status);
    if (m_status->tone() != content.tone)
        m_status->setTone(content.tone);
    setLabelText(m_statusLinks, content.statusLinks);
    m_statusLinks->setToolTip(content.statusLinksToolTip);
    setLabelText(m_system, content.system);
    m_system->setToolTip(content.systemToolTip);

    m_keyFigures->setCards({content.cpu, content.memory, content.systemDisk});
    m_hardware->setCards(content.hardware);
    m_media->setCards(content.media);
    m_wide->setCards(content.wide);
}

void VmOverview::changeEvent(QEvent *event)
{
    QScrollArea::changeEvent(event);
    if (event->type() == QEvent::PaletteChange)
        applyColors();
}

void VmOverview::applyColors()
{
    // Colors are set on the labels, so they follow a new color scheme here.
    const QColor inactive = toneColor(Tone::Inactive);
    for (QLabel *label : {m_placeholderText, m_system, m_statusLinks})
        Utils::setTextColor(label, inactive);
    m_status->setTone(m_status->tone());
}
