/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QIcon>
#include <QPushButton>
#include <QString>

#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

/**
 * @brief Icon picker for a virtual machine.
 *
 * Left click opens the bundled icon set, right click offers the system theme,
 * a custom image file and a reset. Until the user picks something, followOs()
 * keeps the icon in sync with the selected operating system.
 */
class VmIconSelector : public QPushButton
{
    Q_OBJECT

public:
    explicit VmIconSelector(QWidget *parent = nullptr);

    /// Sets the icon size and the surrounding button size in one go.
    void setSelectorSize(int px);

    /**
     * @param userChosen  Whether the stored icon represents a deliberate
     *                    choice. Pass false when it was derived automatically,
     *                    so followOs() keeps updating it.
     */
    void loadFrom(const VMConfig &config, bool userChosen = true);
    void applyTo(VMConfig &config) const;

    /**
     * Remembers the operating system and, unless the user has picked an icon
     * manually, derives the icon from it. @p themeFallback is used when the
     * icon set has nothing for this OS.
     */
    void followOs(VMSpecs::OsType osType, const QString &distroId, const QString &themeFallback = {});

    [[nodiscard]] bool isManuallySet() const;

Q_SIGNALS:
    void iconChanged();

private Q_SLOTS:
    void chooseFromIconSet();
    void chooseThemeIcon();
    void browseCustomIcon();
    void clearIcon();
    void showIconContextMenu(const QPoint &pos);

private:
    void applyOsDefault();
    void updateDisplay();

    QString m_themeIconName;
    QString m_iconSetFilename;
    QIcon m_customIcon;
    bool m_manuallySet = false;

    VMSpecs::OsType m_osType = VMSpecs::OsType::Linux;
    QString m_distroId;
    QString m_themeFallback;
};
