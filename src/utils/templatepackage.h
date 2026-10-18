/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

#include <optional>

/**
 * @brief OS templates and their cloud-init templates as one file to share.
 *
 * A package is a .kemu-template.tar.gz with template.json and <id>.yaml.tpl
 * files side by side, the layout a download service unpacks into one
 * directory per package.
 * A plain .json template file is read as well.
 */
struct TemplatePackage {
    QList<QJsonObject> templates;
    QMap<QString, QString> cloudInit; // by id

    [[nodiscard]] static std::optional<TemplatePackage> read(const QString &path, QString *error = nullptr);
    bool write(const QString &path, QString *error = nullptr) const;
};
