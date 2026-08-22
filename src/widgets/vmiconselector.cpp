/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "widgets/vmiconselector.h"

#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QMenu>
#include <QPixmap>

#include <KIconDialog>
#include <KLocalizedString>
#include <KMessageBox>

#include "const.h"
#include "dialogs/iconchooserdialog.h"

using namespace Qt::StringLiterals;

VmIconSelector::VmIconSelector(QWidget *parent)
    : QPushButton(parent)
{
    setSelectorSize(112);
    setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
    setCursor(Qt::PointingHandCursor);
    setToolTip(i18n("Click: Choose an icon\nRight-click: More options"));
    setContextMenuPolicy(Qt::CustomContextMenu);

    connect(this, &QPushButton::clicked, this, &VmIconSelector::chooseFromIconSet);
    connect(this, &QPushButton::customContextMenuRequested, this, &VmIconSelector::showIconContextMenu);
}

void VmIconSelector::setSelectorSize(int px)
{
    setFixedSize(px + 20, px + 20);
    setIconSize(QSize(px, px));
    updateDisplay();
}

// --- Configuration ---

void VmIconSelector::loadFrom(const VMConfig &config, bool userChosen)
{
    m_themeIconName.clear();
    m_iconSetFilename.clear();
    m_customIcon = {};

    switch (config.iconSource()) {
    case VMConfig::IconSource::Custom:
        m_customIcon = config.customIcon();
        break;
    case VMConfig::IconSource::IconSet:
        m_iconSetFilename = config.iconIdentifier();
        break;
    case VMConfig::IconSource::SystemTheme:
        m_themeIconName = config.iconIdentifier();
        break;
    case VMConfig::IconSource::None:
        break;
    }

    // Anything stored counts as a deliberate choice, so following the OS stops.
    m_manuallySet = userChosen && config.iconSource() != VMConfig::IconSource::None;

    updateDisplay();
}

void VmIconSelector::applyTo(VMConfig &config) const
{
    if (!m_customIcon.isNull()) {
        config.setIconSource(VMConfig::IconSource::Custom);
        config.setCustomIcon(m_customIcon);
    } else if (!m_iconSetFilename.isEmpty()) {
        config.setIconSource(VMConfig::IconSource::IconSet);
        config.setIconIdentifier(m_iconSetFilename);
    } else if (!m_themeIconName.isEmpty()) {
        config.setIconSource(VMConfig::IconSource::SystemTheme);
        config.setIconIdentifier(m_themeIconName);
    } else {
        config.setIconSource(VMConfig::IconSource::None);
        config.setIconIdentifier({});
    }
}

void VmIconSelector::followOs(VMSpecs::OsType osType, const QString &distroId, const QString &themeFallback)
{
    m_osType = osType;
    m_distroId = distroId;
    m_themeFallback = themeFallback;

    if (m_manuallySet)
        return;

    applyOsDefault();
}

bool VmIconSelector::isManuallySet() const
{
    return m_manuallySet;
}

// --- Picking ---

void VmIconSelector::chooseFromIconSet()
{
    IconChooserDialog dlg(this);
    if (dlg.exec() != QDialog::Accepted)
        return;

    const QString path = dlg.selectedIconPath();
    if (path.isEmpty())
        return;

    const QDir baseDir(VMConfig::osIconDir());
    m_iconSetFilename = baseDir.relativeFilePath(path);
    m_themeIconName.clear();
    m_customIcon = {};
    m_manuallySet = true;

    updateDisplay();
    Q_EMIT iconChanged();
}

void VmIconSelector::chooseThemeIcon()
{
    const QString icon = KIconDialog::getIcon(KIconLoader::Desktop, KIconLoader::Application, false, 0, false, this, i18n("Choose Icon"));
    if (icon.isEmpty())
        return;

    m_themeIconName = icon;
    m_iconSetFilename.clear();
    m_customIcon = {};
    m_manuallySet = true;

    updateDisplay();
    Q_EMIT iconChanged();
}

void VmIconSelector::browseCustomIcon()
{
    const QString path =
        QFileDialog::getOpenFileName(this, i18n("Select Icon Image"), QDir::homePath(), i18n("Images (*.png *.svg *.svgz *.xpm *.ico *.jpg *.jpeg *.bmp)"));

    if (path.isEmpty())
        return;

    // Load as QIcon directly - handles all formats including SVG
    const QIcon icon(path);
    if (icon.isNull()) {
        KMessageBox::error(this, i18n("Failed to load icon from: %1", path));
        return;
    }

    m_customIcon = icon;
    m_themeIconName.clear();
    m_iconSetFilename.clear();
    m_manuallySet = true;

    updateDisplay();
    Q_EMIT iconChanged();
}

void VmIconSelector::clearIcon()
{
    m_manuallySet = false;

    // Go back to whatever the operating system suggests rather than to the
    // generic placeholder.
    applyOsDefault();
    Q_EMIT iconChanged();
}

void VmIconSelector::showIconContextMenu(const QPoint &pos)
{
    QMenu menu(this);
    menu.addAction(QIcon::fromTheme(u"preferences-desktop-icons"_s), i18n("Choose Icon..."), this, &VmIconSelector::chooseFromIconSet);
    menu.addAction(QIcon::fromTheme(u"preferences-desktop-icons"_s), i18n("Choose System Theme Icon..."), this, &VmIconSelector::chooseThemeIcon);
    menu.addAction(QIcon::fromTheme(u"document-open"_s), i18n("Load Custom Image..."), this, &VmIconSelector::browseCustomIcon);
    menu.addSeparator();
    menu.addAction(QIcon::fromTheme(u"edit-clear"_s), i18n("Reset Icon"), this, &VmIconSelector::clearIcon);
    menu.exec(mapToGlobal(pos));
}

// --- Display ---

void VmIconSelector::applyOsDefault()
{
    m_themeIconName.clear();
    m_iconSetFilename.clear();
    m_customIcon = {};

    const QString path = VMConfig::defaultIconFor(m_osType, m_distroId);
    if (!path.isEmpty() && QFile::exists(path)) {
        const QDir baseDir(VMConfig::osIconDir());
        m_iconSetFilename = baseDir.relativeFilePath(path);
    } else if (!m_themeFallback.isEmpty()) {
        m_themeIconName = m_themeFallback;
    }

    updateDisplay();
}

void VmIconSelector::updateDisplay()
{
    const QSize targetSize = iconSize();

    if (!m_customIcon.isNull()) {
        setIcon(m_customIcon);
        return;
    }

    if (!m_iconSetFilename.isEmpty()) {
        const QString dir = VMConfig::osIconDir();
        if (!dir.isEmpty()) {
            const QString fullPath = dir + u"/"_s + m_iconSetFilename;
            if (QFile::exists(fullPath)) {
                if (fullPath.endsWith(u".svg", Qt::CaseInsensitive)) {
                    setIcon(QIcon(fullPath));
                    return;
                }
                QPixmap pix(fullPath);
                if (!pix.isNull()) {
                    pix = pix.scaled(targetSize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                    setIcon(QIcon(pix));
                    return;
                }
            }
        }
        setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
        return;
    }

    if (!m_themeIconName.isEmpty()) {
        const QIcon themedIcon = QIcon::fromTheme(m_themeIconName);
        if (!themedIcon.isNull()) {
            const QPixmap scaled = themedIcon.pixmap(targetSize);
            setIcon(scaled.isNull() ? themedIcon : QIcon(scaled));
        } else {
            setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
        }
        return;
    }

    setIcon(QIcon::fromTheme(KEMUConst::VMDefaultIcon));
}
