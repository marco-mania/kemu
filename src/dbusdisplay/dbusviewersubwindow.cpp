/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dbusviewersubwindow.h"

#include <QAction>
#include <QCloseEvent>
#include <QGuiApplication>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QWindow>

#include <KLocalizedString>
#include <KStandardAction>
#include <KToggleAction>

#include "dbusdisplay/displaywidget.h"
#include "dbusdisplay/listener.h"
#include "dbusdisplay/osdoverlay.h"
#include "dbusdisplay/screenshot.h"

namespace
{
constexpr qreal kZoomStep = 0.25;
constexpr qreal kMinZoom = 0.5;
constexpr qreal kMaxZoom = 5.0;
} // namespace

DbusViewerSubWindow::DbusViewerSubWindow(QemuDisplayListener *listener, QemuControlConnection *ctrl, const QString &consolePath, QWidget *parent)
    : QMainWindow(parent)
{
    m_display = new QemuDisplayWidget(listener, ctrl, consolePath, this);
    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidget(m_display);
    m_scrollArea->setWidgetResizable(false);
    m_scrollArea->setAlignment(Qt::AlignCenter);
    m_scrollArea->setFocusPolicy(Qt::NoFocus);
    setCentralWidget(m_scrollArea);

    // As in the main viewer, mirrored for the zero-copy path.
    m_osd = new OsdOverlay(m_scrollArea->viewport());
    connect(m_osd, &OsdOverlay::appearanceChanged, this, [this] {
        if (!m_display->isZeroCopyActive())
            return;
        const bool visible = m_osd->isVisible();
        m_display->setOverlayImage(visible ? m_osd->renderToImage() : QImage(), m_osd->mapTo(this, QPoint(0, 0)), visible);
    });

    QMenu *viewMenu = menuBar()->addMenu(i18n("&View"));
    m_fullscreenAction = viewMenu->addAction(i18n("&Fullscreen"));
    m_fullscreenAction->setCheckable(true);
    m_fullscreenAction->setShortcut(QKeySequence(Qt::Key_F11));
    connect(m_fullscreenAction, &QAction::toggled, this, &DbusViewerSubWindow::setFullscreen);
    connect(m_display, &QemuDisplayWidget::fullscreenToggleRequested, m_fullscreenAction, &QAction::trigger);
    m_showStatusBarAction = KStandardAction::showStatusbar(
        this,
        [this](bool shown) {
            setStatusBarShown(shown);
            Q_EMIT statusBarShownChanged(shown);
        },
        this);
    m_showStatusBarAction->setChecked(m_statusBarShown);
    viewMenu->addAction(m_showStatusBarAction);

    viewMenu->addSeparator();
    // Same texts and shortcuts as in the main viewer window.
    m_scaleAction = viewMenu->addAction(i18n("&Scale Display to Window"));
    m_scaleAction->setCheckable(true);
    m_scaleAction->setChecked(true);
    connect(m_scaleAction, &QAction::toggled, this, [this] {
        updateViewActions();
        updateDisplayScale();
    });

    m_fitWindowAction = viewMenu->addAction(QIcon::fromTheme(u"zoom-fit-best"_s), i18n("&Fit Window to Display"));
    m_fitWindowAction->setShortcut(QKeySequence(u"Ctrl+Shift+R"_s));
    connect(m_fitWindowAction, &QAction::triggered, this, &DbusViewerSubWindow::fitWindowToDisplay);

    viewMenu->addSeparator();
    m_zoomInAction = viewMenu->addAction(i18n("Zoom &In"));
    m_zoomInAction->setShortcut(QKeySequence::ZoomIn);
    connect(m_zoomInAction, &QAction::triggered, this, [this] {
        zoomBy(kZoomStep);
    });
    m_zoomOutAction = viewMenu->addAction(i18n("Zoom &Out"));
    m_zoomOutAction->setShortcut(QKeySequence::ZoomOut);
    connect(m_zoomOutAction, &QAction::triggered, this, [this] {
        zoomBy(-kZoomStep);
    });
    m_zoomResetAction = viewMenu->addAction(QIcon::fromTheme(u"zoom-original"_s), i18n("&Zoom to 100%"));
    connect(m_zoomResetAction, &QAction::triggered, this, [this] {
        m_zoom = 1.0;
        updateDisplayScale();
    });

    viewMenu->addSeparator();
    QAction *screenshotAction = viewMenu->addAction(QIcon::fromTheme(u"spectacle"_s), i18n("Save &Screenshot…"));
    connect(screenshotAction, &QAction::triggered, this, &DbusViewerSubWindow::saveScreenshot);

    viewMenu->addSeparator();
    connect(viewMenu->addAction(i18n("Show &Main Window")), &QAction::triggered, this, &DbusViewerSubWindow::mainWindowRequested);
    connect(viewMenu->addAction(QIcon::fromTheme(u"window-close"_s), i18n("&Close Display")), &QAction::triggered, this, [this] {
        close();
    });
    updateViewActions();

    // Only what this display shows; everything about the VM stays in the
    // main window.
    m_resolutionLabel = new QLabel(this);
    m_resolutionLabel->setAlignment(Qt::AlignCenter);
    m_resolutionLabel->setAccessibleName(i18nc("@info:whatsthis", "Guest resolution"));
    m_resolutionLabel->setForegroundRole(QPalette::PlaceholderText);
    m_resolutionLabel->setFixedWidth(m_resolutionLabel->fontMetrics().horizontalAdvance(u"8888 × 8888"_s) + 8);
    statusBar()->addPermanentWidget(m_resolutionLabel);
    showResolution({});

    connect(listener, &QemuDisplayListener::scanoutChanged, this, [this](int width, int height) {
        m_scanoutSize = QSize(width, height);
        showResolution(m_scanoutSize);
        updateDisplayScale();
    });
    connect(listener, &QemuDisplayListener::dmaBufScanout, this, [this](int width, int height) {
        m_scanoutSize = QSize(width, height);
        showResolution(m_scanoutSize);
        updateDisplayScale();
    });

    resize(960, 600);
    m_display->setFocus(Qt::OtherFocusReason);
}

DbusViewerSubWindow::Placement DbusViewerSubWindow::placement() const
{
    Placement placement;
    placement.geometry = normalGeometry();
    placement.screenName = screen() ? screen()->name() : QString();
    placement.fullscreen = isFullScreen();
    return placement;
}

void DbusViewerSubWindow::restorePlacement(const Placement &placement)
{
    QScreen *target = nullptr;
    for (QScreen *candidate : QGuiApplication::screens()) {
        if (candidate->name() == placement.screenName)
            target = candidate;
    }
    if (target) {
        winId(); // the native window has to exist to be put on a screen
        windowHandle()->setScreen(target);
    }

    // Wayland ignores the position; the size and the screen of a
    // fullscreen window still apply.
    if (placement.geometry.isValid()) {
        const QRect available = (target ? target : screen())->availableGeometry();
        if (available.intersects(placement.geometry))
            setGeometry(placement.geometry);
        else
            resize(placement.geometry.size());
    }
    if (placement.fullscreen)
        m_fullscreenAction->setChecked(true);
}

void DbusViewerSubWindow::setFullscreen(bool enabled)
{
    if (enabled)
        showFullScreen();
    else
        showNormal();
}

void DbusViewerSubWindow::updateDisplayScale()
{
    if (!m_display || !m_scrollArea || !m_scaleAction || m_scanoutSize.isEmpty())
        return;

    if (!m_scaleAction->isChecked() && !isFullScreen() && !isMaximized()) {
        m_display->setScale(m_zoom);
        return;
    }

    const QSize size = m_scrollArea->viewport()->size() * devicePixelRatio();
    const qreal scale = qMin(qreal(size.width()) / m_scanoutSize.width(), qreal(size.height()) / m_scanoutSize.height());
    if (scale > 0)
        m_display->setScale(scale);
}

void DbusViewerSubWindow::updateViewActions()
{
    const bool zoomable = !m_scaleAction->isChecked();
    m_zoomInAction->setEnabled(zoomable);
    m_zoomOutAction->setEnabled(zoomable);
    m_zoomResetAction->setEnabled(zoomable);
}

void DbusViewerSubWindow::zoomBy(qreal step)
{
    m_zoom = qBound(kMinZoom, m_zoom + step, kMaxZoom);
    updateDisplayScale();
}

void DbusViewerSubWindow::fitWindowToDisplay()
{
    if (m_scanoutSize.isEmpty())
        return;

    if (isFullScreen())
        m_fullscreenAction->setChecked(false);
    else if (isMaximized())
        showNormal();

    // Scaling to the window has no zoom of its own; native size is the
    // only sensible target there.
    if (m_scaleAction->isChecked())
        m_zoom = 1.0;

    // After leaving fullscreen, the menu bar is back on the next pass.
    QTimer::singleShot(0, this, [this] {
        const QSize content = (QSizeF(m_scanoutSize) * m_zoom / devicePixelRatio()).toSize();
        const int frame = 2 * m_scrollArea->frameWidth();
        const int menuHeight = menuBar()->isVisible() ? menuBar()->height() : 0;
        const int statusHeight = statusBar()->isVisible() ? statusBar()->height() : 0;
        resize(content.width() + frame, content.height() + frame + menuHeight + statusHeight);
    });
}

void DbusViewerSubWindow::saveScreenshot()
{
    const QString filePath = saveScreenshotInteractively(this, m_display->captureGuestFrame(), windowTitle());
    if (filePath.isEmpty())
        return;
    const QString message = i18n("Screenshot saved to %1", filePath);
    if (statusBar()->isVisible())
        statusBar()->showMessage(message);
    else
        m_osd->showMessage(message, 3000);
}

void DbusViewerSubWindow::setStatusBarShown(bool shown)
{
    m_statusBarShown = shown;
    {
        QSignalBlocker blocker(m_showStatusBarAction);
        m_showStatusBarAction->setChecked(shown);
    }
    if (isFullScreen())
        return;
    statusBar()->setVisible(shown);
    QTimer::singleShot(0, this, [this] {
        updateDisplayScale();
    });
}

void DbusViewerSubWindow::showResolution(const QSize &size)
{
    if (size.isEmpty()) {
        m_resolutionLabel->setText(u"–"_s);
        m_resolutionLabel->setToolTip(i18nc("@info:tooltip", "No display output from the guest"));
    } else {
        m_resolutionLabel->setText(u"%1 × %2"_s.arg(size.width()).arg(size.height()));
        m_resolutionLabel->setToolTip(i18nc("@info:tooltip", "Resolution reported by the guest"));
    }
}

void DbusViewerSubWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    QTimer::singleShot(0, this, [this] {
        updateDisplayScale();
    });
}

void DbusViewerSubWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange)
        return;

    {
        QSignalBlocker blocker(m_fullscreenAction);
        m_fullscreenAction->setChecked(isFullScreen());
    }
    menuBar()->setVisible(!isFullScreen());
    statusBar()->setVisible(!isFullScreen() && m_statusBarShown);
    QTimer::singleShot(0, this, [this] {
        updateDisplayScale();
    });
}

void DbusViewerSubWindow::closeEvent(QCloseEvent *event)
{
    Q_EMIT viewerClosed();
    event->accept();
}
