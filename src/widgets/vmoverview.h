/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QPixmap>
#include <QScrollArea>
#include <QString>

class QLabel;
class QVBoxLayout;
class VmOverviewBadge;
class VmOverviewGrid;

/**
 * @brief The details of a VM as tiles, ordered by importance.
 *
 * Below the header come the key figures as large tiles, then the hardware
 * and the drives as cards, and wide cards for facts with much text. The
 * grids drop columns when the view gets narrow.
 */
class VmOverview : public QScrollArea
{
    Q_OBJECT

public:
    struct Card {
        Card() = default;
        Card(const QString &iconName, const QString &value, const QString &caption = {}, const QString &detail = {})
            : iconName(iconName)
            , value(value)
            , caption(caption)
            , detail(detail)
        {
        }

        QString iconName;
        /// Small icon on the corner, such as a padlock.
        QString emblemName;
        QString value;
        QString caption;
        QString detail;
        QString toolTip;
        /// Used share from 0 to 1, see SpecTile::setUsage(); negative for none.
        double usage = -1;
        bool attention = false;
        bool muted = false;

        /// For the signals of a card that reacts; empty for a mere display.
        QString id;
        /// A click opens a menu, at VmOverview::cardActivated().
        bool menu = false;
        /// Suffixes of files it takes by drag and drop, see VmOverview::fileDropped().
        QStringList dropSuffixes;
    };

    enum class Tone {
        Positive,
        Neutral,
        Negative,
        Inactive
    };

    struct Content {
        QPixmap icon;
        QString name;
        QString status;
        Tone tone = Tone::Inactive;
        /// Rich text next to the status, such as the PID to copy.
        QString statusLinks;
        QString statusLinksToolTip;
        /// Rich text below the status, such as what the guest agent reports.
        QString system;
        QString systemToolTip;

        Card cpu;
        Card memory;
        Card systemDisk;
        QList<Card> hardware;
        QList<Card> media;
        QList<Card> wide;
    };

    explicit VmOverview(QWidget *parent = nullptr);

    /// Shown instead of the details while no VM is selected.
    void showPlaceholder(const QString &text);
    void setContent(const Content &content);

Q_SIGNALS:
    /// A link in the header, such as copy-pid: or copy-ip:.
    void linkActivated(const QString &link);
    /// A card with an id was clicked; @p anchor is its lower left corner, for a menu.
    void cardActivated(const QString &id, const QPoint &anchor);
    /// A file was dropped on the card @p id.
    void fileDropped(const QString &id, const QString &path);

protected:
    void changeEvent(QEvent *event) override;

private:
    void applyColors();

    QWidget *m_placeholder = nullptr;
    QLabel *m_placeholderText = nullptr;
    QWidget *m_content = nullptr;
    QLabel *m_icon = nullptr;
    QLabel *m_name = nullptr;
    VmOverviewBadge *m_status = nullptr;
    QLabel *m_statusLinks = nullptr;
    QLabel *m_system = nullptr;
    VmOverviewGrid *m_keyFigures = nullptr;
    VmOverviewGrid *m_hardware = nullptr;
    VmOverviewGrid *m_media = nullptr;
    VmOverviewGrid *m_wide = nullptr;
};
