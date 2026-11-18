/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QFile>
#include <QString>

#define LIBISOFS_WITHOUT_LIBBURN
#include <libisofs/libisofs.h>

/// An image read back with libisofs, the way a Rock Ridge reader sees it.
class IsoReader
{
public:
    explicit IsoReader(const QString &path)
    {
        IsoDataSource *source = nullptr;
        if (iso_data_source_new_from_file(QFile::encodeName(path).constData(), &source) < 0)
            return;
        IsoReadOpts *opts = nullptr;
        iso_read_opts_new(&opts, 0);
        IsoReadImageFeatures *features = nullptr;
        if (iso_image_new("read", &m_image) >= 0 && iso_image_import(m_image, source, opts, &features) < 0) {
            iso_image_unref(m_image);
            m_image = nullptr;
        }
        if (features)
            iso_read_image_features_destroy(features);
        iso_read_opts_free(opts);
        iso_data_source_unref(source);
    }
    ~IsoReader()
    {
        if (m_image)
            iso_image_unref(m_image);
    }
    IsoReader(const IsoReader &) = delete;
    IsoReader &operator=(const IsoReader &) = delete;

    [[nodiscard]] bool isValid() const
    {
        return m_image != nullptr;
    }
    [[nodiscard]] QString volumeId() const
    {
        return QString::fromUtf8(iso_image_get_volume_id(m_image));
    }
    [[nodiscard]] IsoNode *node(const char *path) const
    {
        IsoNode *found = nullptr;
        return iso_tree_path_to_node(m_image, path, &found) == 1 ? found : nullptr;
    }
    [[nodiscard]] int mode(const char *path) const
    {
        IsoNode *n = node(path);
        return n ? int(iso_node_get_permissions(n)) : -1;
    }
    [[nodiscard]] int type(const char *path) const
    {
        IsoNode *n = node(path);
        return n ? int(iso_node_get_type(n)) : -1;
    }
    [[nodiscard]] QByteArray content(const char *path) const
    {
        IsoNode *n = node(path);
        if (!n || iso_node_get_type(n) != LIBISO_FILE)
            return {};
        IsoStream *stream = iso_file_get_stream(reinterpret_cast<IsoFile *>(n));
        if (iso_stream_open(stream) < 0)
            return {};
        QByteArray data;
        char buffer[4096];
        int got = 0;
        while ((got = iso_stream_read(stream, buffer, sizeof(buffer))) > 0)
            data.append(buffer, got);
        iso_stream_close(stream);
        return data;
    }
    [[nodiscard]] QString linkTarget(const char *path) const
    {
        IsoNode *n = node(path);
        if (!n || iso_node_get_type(n) != LIBISO_SYMLINK)
            return {};
        return QString::fromUtf8(iso_symlink_get_dest(reinterpret_cast<IsoSymlink *>(n)));
    }

private:
    IsoImage *m_image = nullptr;
};
