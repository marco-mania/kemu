/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "deploymentlogwindow.h"

#include <QClipboard>
#include <QDialogButtonBox>
#include <QFile>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStringConverter>
#include <QTextCursor>
#include <QVBoxLayout>

#include <KLocalizedString>

DeploymentLogWindow::DeploymentLogWindow(const QString &vmName, const QString &logPath, QWidget *parent)
    : QWidget(parent, Qt::Window)
    , m_logPath(logPath)
    , m_decoder(QStringDecoder::Utf8)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(i18nc("@title:window", "Deployment Log - %1", vmName));
    resize(900, 520);

    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_view->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    m_view->setMaximumBlockCount(kMaxBlocks);
    m_view->setPlaceholderText(i18n("Waiting for output..."));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    QPushButton *copyButton = buttons->addButton(i18n("Copy to Clipboard"), QDialogButtonBox::ActionRole);
    connect(copyButton, &QPushButton::clicked, this, [this]() {
        QGuiApplication::clipboard()->setText(m_view->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QWidget::close);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_view);
    layout->addWidget(buttons);

    connect(&m_poll, &QTimer::timeout, this, &DeploymentLogWindow::poll);
    m_poll.start(kPollMs);
    poll();
}

void DeploymentLogWindow::poll()
{
    QFile file(m_logPath);
    if (!file.exists())
        return; // nothing written yet, or the VM was removed - keep waiting

    const qint64 size = file.size();

    if (size < m_offset) {
        // Truncated: a new deployment started. Show that run from the top.
        m_view->clear();
        m_offset = 0;
        m_decoder.resetState();
    }

    if (size == m_offset)
        return;

    if (!file.open(QIODevice::ReadOnly) || !file.seek(m_offset))
        return;

    const QByteArray chunk = file.read(kMaxChunkBytes);
    m_offset += chunk.size();
    appendChunk(chunk);
}

void DeploymentLogWindow::appendChunk(const QByteArray &bytes)
{
    QString text = m_decoder.decode(bytes);
    if (text.isEmpty())
        return;

    // A serial console emits CR and ANSI colour/cursor sequences that
    // QPlainTextEdit would otherwise render as stray glyphs and blank lines.
    static const QRegularExpression ansi(u"\u001B\\[[0-9;?]*[A-Za-z]"_s);
    text.remove(ansi);
    text.replace(u"\r\n"_s, u"\n"_s);
    text.remove(u'\r');

    // Decide before inserting: appending moves the maximum.
    const bool follow = followingTail();

    QTextCursor cursor(m_view->document());
    cursor.movePosition(QTextCursor::End);
    cursor.insertText(text);

    if (follow)
        m_view->verticalScrollBar()->setValue(m_view->verticalScrollBar()->maximum());
}

bool DeploymentLogWindow::followingTail() const
{
    const QScrollBar *bar = m_view->verticalScrollBar();
    // A little slack: the user counts as following until they deliberately
    // scroll up to read something.
    return bar->value() >= bar->maximum() - 4;
}
