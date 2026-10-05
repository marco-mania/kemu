/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

/**
 * @brief Writes small ISO 9660 images in-process with libisofs.
 *
 * Replaces the call to genisoimage / mkisofs / xorriso: the files live in
 * memory, so no temporary directory and no helper binary are needed. Images
 * carry Rock Ridge and Joliet names, like "genisoimage -rock -joliet".
 *
 * Meant for the few small images KEMU builds itself (cloud-init NoCloud);
 * the whole image is streamed to disk, but every entry is held in memory.
 */
namespace IsoImageBuilder
{

struct Entry {
    QString name; // file name in the image's root directory
    QByteArray content;
};

/// Writes the image to isoPath, replacing an existing file. Returns false
/// and fills errorOut on failure.
bool write(const QString &isoPath, const QString &volumeId, const QList<Entry> &entries, QString &errorOut);

/// Version of the libisofs in use, e.g. "1.5.6".
[[nodiscard]] QString libraryVersion();

}
