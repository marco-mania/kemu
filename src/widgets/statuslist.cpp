/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "statuslist.h"

#include <algorithm>
#include <functional>
#include <memory>

#include <QClipboard>
#include <QContextMenuEvent>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <KColorScheme>
#include <KLocalizedString>

using namespace Qt::StringLiterals;

namespace
{
using Status = StatusList::Status;

bool needsAttention(Status status)
{
    return status == Status::Missing || status == Status::Warning;
}

QString defaultStatusText(Status status)
{
    switch (status) {
    case Status::Ok:
        return i18nc("@info:status", "Available");
    case Status::Missing:
        return i18nc("@info:status", "Missing");
    case Status::Warning:
        return i18nc("@info:status", "Warning");
    case Status::Unknown:
        return i18nc("@info:status", "Unknown");
    }
    return {};
}

KColorScheme::ForegroundRole statusForeground(Status status)
{
    switch (status) {
    case Status::Ok:
        return KColorScheme::PositiveText;
    case Status::Missing:
    case Status::Warning:
        return KColorScheme::NeutralText;
    case Status::Unknown:
        return KColorScheme::InactiveText;
    }
    return KColorScheme::NormalText;
}

KColorScheme::BackgroundRole statusBackground(Status status)
{
    switch (status) {
    case Status::Ok:
        return KColorScheme::PositiveBackground;
    case Status::Missing:
    case Status::Warning:
        return KColorScheme::NeutralBackground;
    case Status::Unknown:
        return KColorScheme::AlternateBackground;
    }
    return KColorScheme::NormalBackground;
}

QColor schemeColor(KColorScheme::ForegroundRole role)
{
    return KColorScheme(QPalette::Active, KColorScheme::View).foreground(role).color();
}

// Text as well: on the Base background, labels draw with it.
void setTextColor(QWidget *widget, const QColor &color)
{
    QPalette palette = widget->palette();
    palette.setColor(QPalette::WindowText, color);
    palette.setColor(QPalette::Text, color);
    widget->setPalette(palette);
}

QLabel *wrappingLabel(const QString &text, const QColor &color)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    setTextColor(label, color);
    return label;
}

// The status as a word on a tinted, rounded background.
class StatusPill : public QLabel
{
public:
    StatusPill(Status status, const QString &text)
        : QLabel(text)
        , m_status(status)
    {
        setTextFormat(Qt::PlainText);
        setAlignment(Qt::AlignCenter);
        setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        const int vertical = fontMetrics().height() / 8;
        const int horizontal = fontMetrics().averageCharWidth();
        setContentsMargins(horizontal, vertical, horizontal, vertical);
        setTextColor(this, schemeColor(statusForeground(status)));
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(KColorScheme(QPalette::Active, KColorScheme::View).background(statusBackground(m_status)));
            const qreal radius = height() / 3.0;
            painter.drawRoundedRect(QRectF(rect()), radius, radius);
        }
        QLabel::paintEvent(event);
    }

private:
    Status m_status;
};

// Title and pill; expands to the explanation and the technical details.
class StatusRow : public QWidget
{
public:
    StatusRow(const StatusList::Item &item, bool expanded, bool separator, std::function<void(bool)> toggled)
        : m_separator(separator)
        , m_toggled(std::move(toggled))
    {
        const int padding = fontMetrics().height() / 3;
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(style()->pixelMetric(QStyle::PM_LayoutLeftMargin), padding, style()->pixelMetric(QStyle::PM_LayoutRightMargin), padding);
        layout->setSpacing(padding);

        auto *header = new QHBoxLayout;
        m_arrow = new QToolButton;
        m_arrow->setAutoRaise(true);
        m_arrow->setFocusPolicy(Qt::TabFocus);
        connect(m_arrow, &QToolButton::clicked, this, [this] {
            setExpanded(!m_expanded);
            m_toggled(m_expanded);
        });
        header->addWidget(m_arrow);
        auto *title = new QLabel(item.title);
        title->setTextFormat(Qt::PlainText);
        header->addWidget(title, 1);
        header->addWidget(new StatusPill(item.status, item.statusText.isEmpty() ? defaultStatusText(item.status) : item.statusText));
        layout->addLayout(header);

        m_body = new QWidget;
        auto *body = new QVBoxLayout(m_body);
        // Aligned with the title.
        body->setContentsMargins(m_arrow->sizeHint().width() + header->spacing(), 0, 0, 0);
        body->setSpacing(padding);
        const QColor inactive = schemeColor(KColorScheme::InactiveText);
        const bool hasBody = !item.description.isEmpty() || !item.hint.isEmpty() || !item.details.isEmpty();
        if (!item.description.isEmpty())
            body->addWidget(wrappingLabel(item.description, inactive));
        if (!item.hint.isEmpty()) {
            const KColorScheme::ForegroundRole role = item.status == Status::Ok ? KColorScheme::NormalText : statusForeground(item.status);
            body->addWidget(wrappingLabel(item.hint, schemeColor(role)));
        }
        if (!item.details.isEmpty()) {
            const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
            auto *grid = new QGridLayout;
            grid->setHorizontalSpacing(QFontMetrics(fixed).horizontalAdvance(u"  "_s));
            grid->setVerticalSpacing(0);
            grid->setColumnStretch(1, 1);
            for (int row = 0; row < item.details.size(); ++row) {
                const auto &[key, value] = item.details.at(row);
                QLabel *valueLabel = wrappingLabel(value, schemeColor(KColorScheme::NormalText));
                valueLabel->setFont(fixed);
                if (key.isEmpty()) {
                    grid->addWidget(valueLabel, row, 0, 1, 2);
                    continue;
                }
                QLabel *keyLabel = wrappingLabel(key, inactive);
                keyLabel->setFont(fixed);
                keyLabel->setWordWrap(false);
                grid->addWidget(keyLabel, row, 0, Qt::AlignTop);
                grid->addWidget(valueLabel, row, 1);
            }
            body->addLayout(grid);
        }
        layout->addWidget(m_body);

        // Keeps the title aligned with the others when there is nothing to expand.
        if (!hasBody) {
            QSizePolicy policy = m_arrow->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            m_arrow->setSizePolicy(policy);
            m_arrow->hide();
            expanded = false;
        }
        setExpanded(expanded);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override
    {
        // Only the header toggles; labels in the body take clicks to select text.
        const bool onHeader = !m_expanded || event->position().y() < m_body->geometry().top();
        if (!m_arrow->isHidden() && onHeader && event->button() == Qt::LeftButton)
            m_arrow->click();
        QWidget::mousePressEvent(event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        if (!m_separator)
            return;
        QPainter painter(this);
        QColor line = palette().color(QPalette::WindowText);
        line.setAlphaF(0.15);
        painter.setPen(line);
        painter.drawLine(0, 0, width(), 0);
    }

private:
    void setExpanded(bool expanded)
    {
        m_expanded = expanded;
        m_body->setVisible(expanded);
        m_arrow->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
        m_arrow->setAccessibleName(expanded ? i18nc("@action:button", "Hide details") : i18nc("@action:button", "Show details"));
        m_arrow->setToolTip(m_arrow->accessibleName());
    }

    QToolButton *m_arrow = nullptr;
    QWidget *m_body = nullptr;
    bool m_expanded = false;
    bool m_separator;
    std::function<void(bool)> m_toggled;
};
}

// How many items are available, as a bar split by status.
class StatusSummaryBar : public QWidget
{
public:
    StatusSummaryBar()
    {
        setFixedHeight(std::max(4, fontMetrics().height() / 3));
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setCounts(const QList<std::pair<Status, int>> &counts)
    {
        m_counts = counts;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        int total = 0;
        for (const auto &[status, count] : std::as_const(m_counts))
            total += count;
        if (total == 0)
            return;

        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const qreal radius = height() / 2.0;
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), radius, radius);
        painter.setClipPath(clip);
        qreal x = 0;
        for (const auto &[status, count] : std::as_const(m_counts)) {
            const qreal segment = width() * count / qreal(total);
            painter.fillRect(QRectF(x, 0, segment, height()), schemeColor(statusForeground(status)));
            x += segment;
        }
    }

private:
    QList<std::pair<Status, int>> m_counts;
};

StatusList::Item::Item(Status status, const QString &title)
    : status(status)
    , title(title)
{
}

StatusList::StatusList(QWidget *parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    rebuild();
}

void StatusList::clear()
{
    m_entries.clear();
    rebuild();
}

void StatusList::addSection(const QString &title)
{
    m_entries.append({title, std::nullopt});
    appendWidget(m_entries.constLast(), false);
}

void StatusList::addItem(const Item &item)
{
    const bool separator = !m_entries.isEmpty() && m_entries.constLast().item.has_value();
    m_entries.append({{}, item});
    appendWidget(m_entries.constLast(), separator);
    updateSummary();
}

void StatusList::setSummaryVisible(bool visible)
{
    m_summaryVisible = visible;
    updateSummary();
}

void StatusList::setExpandedByDefault(bool expanded)
{
    m_expandedByDefault = expanded;
    rebuild();
}

QString StatusList::toReport() const
{
    QStringList report;
    if (m_summaryVisible && m_summaryLabel)
        report << m_summaryLabel->text();
    for (const Entry &entry : m_entries) {
        if (!entry.item) {
            if (!report.isEmpty())
                report << QString();
            report << entry.section;
            continue;
        }
        const Item &item = *entry.item;
        report << u"  %1: %2"_s.arg(item.title, item.statusText.isEmpty() ? defaultStatusText(item.status) : item.statusText);
        if (!item.hint.isEmpty())
            report << u"      "_s + item.hint;
        qsizetype keyWidth = 0;
        for (const auto &[key, value] : item.details)
            keyWidth = std::max(keyWidth, key.size());
        for (const auto &[key, value] : item.details)
            report << u"      "_s + (key.isEmpty() ? value : key.leftJustified(keyWidth) + u"  "_s + value);
    }
    return report.join(u'\n') + u'\n';
}

void StatusList::changeEvent(QEvent *event)
{
    QScrollArea::changeEvent(event);
    // Colors are taken when the rows are made, so a new color scheme needs new rows.
    if (event->type() == QEvent::PaletteChange && !m_rebuildQueued) {
        m_rebuildQueued = true;
        QTimer::singleShot(0, this, &StatusList::rebuild);
    }
}

void StatusList::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(u"edit-copy"_s), i18nc("@action:inmenu", "Copy All"), this, [this] {
        QGuiApplication::clipboard()->setText(toReport());
    });
    menu.exec(event->globalPos());
}

void StatusList::appendWidget(const Entry &entry, bool separator)
{
    QWidget *widget = nullptr;
    if (!entry.item) {
        auto *label = new QLabel(entry.section);
        label->setTextFormat(Qt::PlainText);
        QFont font = label->font();
        font.setBold(true);
        label->setFont(font);
        setTextColor(label, schemeColor(KColorScheme::InactiveText));
        const int margin = style()->pixelMetric(QStyle::PM_LayoutLeftMargin);
        label->setContentsMargins(margin, fontMetrics().height() / 2, margin, 0);
        widget = label;
    } else {
        const QString title = entry.item->title;
        const bool expanded = m_expanded.value(title, m_expandedByDefault || needsAttention(entry.item->status));
        widget = new StatusRow(*entry.item, expanded, separator, [this, title](bool expanded) {
            m_expanded.insert(title, expanded);
        });
    }
    // Before the stretch that keeps the rows at the top.
    m_layout->insertWidget(m_layout->count() - 1, widget);
}

void StatusList::updateSummary()
{
    int available = 0;
    int total = 0;
    QList<std::pair<Status, int>> counts = {{Status::Ok, 0}, {Status::Warning, 0}, {Status::Unknown, 0}};
    for (const Entry &entry : std::as_const(m_entries)) {
        if (!entry.item)
            continue;
        ++total;
        const Status status = entry.item->status;
        available += status == Status::Ok ? 1 : 0;
        // Missing and warning share a color and a segment.
        counts[status == Status::Ok ? 0 : needsAttention(status) ? 1 : 2].second++;
    }
    m_summaryLabel->setText(i18ncp("@info", "%2 of %1 feature available", "%2 of %1 features available", total, available));
    m_summaryBar->setCounts(counts);
    m_summary->setVisible(m_summaryVisible && total > 0);
}

void StatusList::rebuild()
{
    m_rebuildQueued = false;
    auto *content = new QWidget;
    content->setBackgroundRole(QPalette::Base);
    content->setAutoFillBackground(true);
    m_layout = new QVBoxLayout(content);
    m_layout->setContentsMargins(0, 0, 0, fontMetrics().height() / 2);
    m_layout->setSpacing(0);

    m_summary = new QWidget;
    auto *summary = new QHBoxLayout(m_summary);
    m_summaryLabel = new QLabel;
    m_summaryBar = new StatusSummaryBar;
    summary->addWidget(m_summaryLabel);
    summary->addWidget(m_summaryBar, 1);
    m_layout->addWidget(m_summary);
    m_layout->addStretch();

    // Deletes the previous rows.
    setWidget(content);
    for (qsizetype i = 0; i < m_entries.size(); ++i)
        appendWidget(m_entries.at(i), i > 0 && m_entries.at(i).item && m_entries.at(i - 1).item);
    updateSummary();
}
