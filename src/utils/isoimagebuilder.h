/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <utility>

/**
 * @brief Writes small ISO 9660 images in-process with libisofs.
 *
 * Replaces the call to genisoimage / mkisofs / xorriso: no temporary
 * directory and no helper binary are needed. Images carry Rock Ridge and
 * Joliet names, like "genisoimage -rock -joliet", and Rock Ridge keeps the
 * files' permissions (directories are 0555, owners root).
 *
 * Meant for the few small images KEMU builds itself (cloud-init NoCloud, the
 * kemu-agent medium); the image is streamed to disk, entries with content are
 * held in memory, entries with a local path are read from disk while writing.
 */
namespace IsoImageBuilder
{

struct Entry {
    Entry() = default;
    /// A file with the given content.
    Entry(QString name_, QByteArray content_ = {}, int mode_ = -1)
        : name(std::move(name_))
        , content(std::move(content_))
        , mode(mode_)
    {
    }
    /// A file, or a directory with everything below it, from disk.
    [[nodiscard]] static Entry fromDisk(QString name, QString localPath)
    {
        Entry entry(std::move(name));
        entry.localPath = std::move(localPath);
        return entry;
    }
    /// A symbolic link.
    [[nodiscard]] static Entry symlink(QString name, QString target)
    {
        Entry entry(std::move(name));
        entry.symlinkTarget = std::move(target);
        return entry;
    }

    /// Path in the image; "/" separates directories, which are made as
    /// needed. No empty, "." or ".." components.
    QString name;
    /// The file's content, unless localPath or symlinkTarget is set.
    QByteArray content;
    /// Permission bits; -1 means 0444 for content, the file's own for
    /// localPath.
    int mode = -1;
    /// A file, or a directory with everything below it, taken from disk
    /// (symbolic links inside stay links, special files are left out).
    QString localPath;
    /// Makes the entry a symbolic link to this target.
    QString symlinkTarget;
};

/// Writes the image to isoPath, replacing an existing file. Returns false
/// and fills errorOut on failure.
bool write(const QString &isoPath, const QString &volumeId, const QList<Entry> &entries, QString &errorOut);

/// Version of the libisofs in use, e.g. "1.5.6".
[[nodiscard]] QString libraryVersion();

}
