/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAbstractButton>

class QMimeData;

/**
 * @brief Fact about a VM, such as its memory, as a framed tile.
 *
 * A key figure shows its value in a larger font above a caption and a
 * small detail line. A card is smaller: the value, then caption and detail
 * on one line. Clicking an interactive tile leads to where the value is
 * changed. Colors come from the palette and the color scheme, so the tile
 * follows the theme.
 */
class SpecTile : public QAbstractButton
{
    Q_OBJECT

public:
    enum class Kind {
        KeyFigure,
        Card
    };

    SpecTile(const QIcon &icon, const QString &caption, QWidget *parent = nullptr);
    SpecTile(Kind kind, QWidget *parent = nullptr);

    void setIconName(const QString &name);
    /// A small icon on the corner of the icon or ring, such as a padlock; empty for none.
    void setEmblemName(const QString &name);
    void setCaption(const QString &caption);
    void setValue(const QString &value);
    /// Small line below the caption; @p attention draws it in the warning color.
    void setDetail(const QString &detail, bool attention = false);
    /// A value that stands for nothing, such as an empty drive, in a dimmed color.
    void setMuted(bool muted);
    /// Used share of a whole, from 0 to 1: a ring in place of the icon of a
    /// key figure, a bar below the value of a card. Negative hides it.
    void setUsage(double fraction);
    /// Whether it reacts to mouse and keyboard; just a display otherwise.
    void setInteractive(bool interactive);
    /// A small arrow that says a click opens a menu.
    void setMenuIndicator(bool shown);
    /// Takes a dropped local file with one of these suffixes; empty for none.
    void setDropSuffixes(const QStringList &suffixes);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void fileDropped(const QString &path);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

private:
    [[nodiscard]] QFont valueFont() const;
    [[nodiscard]] QFont detailFont() const;
    [[nodiscard]] QString secondLine() const;
    [[nodiscard]] int padding() const;
    [[nodiscard]] int iconExtent() const;
    [[nodiscard]] int barHeight() const;
    [[nodiscard]] int textHeight() const;
    void paintUsage(QPainter &painter, const QRect &area) const;
    [[nodiscard]] QString droppedFile(const QMimeData *mime) const;
    [[nodiscard]] int indicatorWidth() const;
    void contentChanged();

    Kind m_kind = Kind::KeyFigure;
    QString m_iconName;
    QString m_emblemName;
    QString m_caption;
    QString m_value;
    QString m_detail;
    double m_usage = -1;
    bool m_attention = false;
    bool m_muted = false;
    bool m_interactive = true;
    bool m_menuIndicator = false;
    bool m_dragOver = false;
    QStringList m_dropSuffixes;
};
