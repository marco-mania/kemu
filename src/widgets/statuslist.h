/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QHash>
#include <QList>
#include <QScrollArea>
#include <QString>

#include <optional>
#include <utility>

class QLabel;
class QVBoxLayout;
class StatusSummaryBar;

/**
 * @brief List of things found or missing, with details to expand.
 *
 * Each item shows its title and a status pill. Expanded, it explains what the
 * item is, what to do about it, and the technical details in the fixed font.
 * Items that need attention start expanded. Colors come from the color scheme,
 * so the list follows light and dark themes and a change between them.
 */
class StatusList : public QScrollArea
{
    Q_OBJECT

public:
    enum class Status {
        Ok,
        Missing,
        Warning,
        Unknown
    };

    struct Item {
        Item(Status status, const QString &title);

        Status status;
        QString title;
        /// Pill text; empty for the one that goes with the status.
        QString statusText;
        /// What the item is.
        QString description;
        /// What to do about it, in the color of the status.
        QString hint;
        /// Key and value in the fixed font; an empty key lets the value span.
        QList<std::pair<QString, QString>> details;
    };

    explicit StatusList(QWidget *parent = nullptr);

    void clear();
    void addSection(const QString &title);
    void addItem(const Item &item);

    /// Shows above the list how many items are available.
    void setSummaryVisible(bool visible);

    /// The items as plain text, for example for a bug report.
    [[nodiscard]] QString toReport() const;

protected:
    void changeEvent(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    struct Entry {
        QString section;
        std::optional<Item> item;
    };

    void appendWidget(const Entry &entry, bool separator);
    void updateSummary();
    void rebuild();

    QList<Entry> m_entries;
    // Expansion the user chose, by title, kept across a refresh.
    QHash<QString, bool> m_expanded;
    QWidget *m_summary = nullptr;
    QLabel *m_summaryLabel = nullptr;
    StatusSummaryBar *m_summaryBar = nullptr;
    QVBoxLayout *m_layout = nullptr;
    bool m_summaryVisible = false;
    bool m_rebuildQueued = false;
};
