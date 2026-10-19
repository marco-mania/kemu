/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QTimer>
#include <QWidget>

class QPlainTextEdit;

using namespace Qt::StringLiterals;

// =============================================================================
// DeploymentLogWindow - live view of a VM's deployment serial log
// =============================================================================
// Tails the file QEMU writes via "-serial file:" during an unattended
// deployment. Polling rather than QFileSystemWatcher: watchers report plain
// appends unreliably across filesystems, and re-arming them after the file is
// recreated costs more code than a 500 ms stat() on a few kilobytes.
//
// Strictly read-only, so the window can be opened, closed and reopened at any
// time - including long after the deployment finished, for a post-mortem on a
// cloud-init run that went wrong.
// =============================================================================

class DeploymentLogWindow : public QWidget
{
    Q_OBJECT

public:
    DeploymentLogWindow(const QString &vmName, const QString &logPath, QWidget *parent = nullptr);

private:
    void poll();
    void appendChunk(const QByteArray &bytes);
    [[nodiscard]] bool followingTail() const;

    QString m_logPath;
    QPlainTextEdit *m_view = nullptr;
    QTimer m_poll;

    // Bytes already consumed. Reset when the file shrinks - which is exactly
    // what a fresh deployment looks like, because QEMU truncates on open.
    qint64 m_offset = 0;

    // Carries decoder state across reads so a multi-byte sequence split
    // between two chunks does not decay into replacement characters.
    QStringDecoder m_decoder;

    static constexpr int kPollMs = 500;
    static constexpr qint64 kMaxChunkBytes = 1 << 20;
    // A runaway boot loop must not grow the widget without bound.
    static constexpr int kMaxBlocks = 20000;
};
