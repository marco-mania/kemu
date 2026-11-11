/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMainWindow>
#include <QRect>
#include <QSize>
#include <QString>

class QAction;
class QCloseEvent;
class QLabel;
class OsdOverlay;
class QMenu;
class QScrollArea;
class QResizeEvent;
class QemuControlConnection;
class QemuDisplayListener;
class QemuDisplayWidget;

// A view on one additional guest display. Controls the view only; the VM
// itself is controlled from the main viewer window.
class DbusViewerSubWindow : public QMainWindow
{
    Q_OBJECT

public:
    struct Placement {
        QRect geometry; // normal geometry, without the frame
        QString screenName;
        bool fullscreen = false;
    };

    DbusViewerSubWindow(QemuDisplayListener *listener, QemuControlConnection *ctrl, const QString &consolePath, QWidget *parent = nullptr);

    QemuDisplayWidget *displayWidget() const
    {
        return m_display;
    }

    Placement placement() const;
    // Call before show().
    void restorePlacement(const Placement &placement);
    void setStatusBarShown(bool shown);

Q_SIGNALS:
    void viewerClosed();
    void mainWindowRequested();
    void statusBarShownChanged(bool shown);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void updateDisplayScale();
    void updateViewActions();
    void setFullscreen(bool enabled);
    void zoomBy(qreal step);
    void fitWindowToDisplay();
    void saveScreenshot();
    void showResolution(const QSize &size);

    QScrollArea *m_scrollArea = nullptr;
    QemuDisplayWidget *m_display = nullptr;
    QAction *m_fullscreenAction = nullptr;
    QAction *m_showStatusBarAction = nullptr;
    QAction *m_scaleAction = nullptr;
    QAction *m_fitWindowAction = nullptr;
    QAction *m_zoomInAction = nullptr;
    QAction *m_zoomOutAction = nullptr;
    QAction *m_zoomResetAction = nullptr;
    QLabel *m_resolutionLabel = nullptr;
    OsdOverlay *m_osd = nullptr;
    bool m_statusBarShown = true;
    QSize m_scanoutSize;
    qreal m_zoom = 1.0;
};
