/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

enum class TemplateOrigin {
    BuiltIn,
    Downloaded,
    User
};

struct TemplateSearchDir {
    QString path;
    TemplateOrigin origin = TemplateOrigin::BuiltIn;
    // A downloaded package: its files never shadow those of other directories.
    bool package = false;
};

struct CloudInitEntry {
    QString id;
    TemplateOrigin origin = TemplateOrigin::BuiltIn;
    QString path;
    // Origin of the file this one shadows, e.g. a customized shipped template.
    std::optional<TemplateOrigin> replaces;
};

/**
 * @brief Where OS templates and cloud-init templates live, by origin.
 *
 * Templates the user created or customized win over downloaded ones, which
 * win over the ones shipped with KEMU. Only the user level is written to.
 */
class TemplateStore
{
public:
    /// Locations from the XDG data directories.
    TemplateStore();
    /// Explicit data directories, below which the kemu/... paths are used.
    TemplateStore(const QString &userDataDir, const QStringList &systemDataDirs);

    [[nodiscard]] QString globalDefaultsPath() const;

    /// Descending priority.
    [[nodiscard]] QList<TemplateSearchDir> templateDirs() const;
    [[nodiscard]] QList<TemplateSearchDir> cloudInitDirs() const;

    /// Empty if the id is invalid or no file exists.
    [[nodiscard]] QString locateCloudInit(const QString &id) const;
    /// The file in effect for every cloud-init template id, sorted by id.
    [[nodiscard]] QList<CloudInitEntry> cloudInitTemplates() const;
    /// network-config templates are named "network-*", the rest is user-data.
    [[nodiscard]] static bool isNetworkTemplate(const QString &id);

    [[nodiscard]] QString userTemplateDir() const;
    [[nodiscard]] QString userCloudInitDir() const;
    [[nodiscard]] QString downloadedTemplateDir() const;
    [[nodiscard]] QString downloadedCloudInitDir() const;

    [[nodiscard]] QStringList hiddenTemplateIds() const;
    bool setTemplateHidden(const QString &id, bool hidden, QString *error = nullptr);

    /// Writes one template, replacing any user definition with the same id.
    bool saveUserTemplate(const QJsonObject &tpl, QString *error = nullptr);
    /// Removes the user definition; a shipped or downloaded one shows again.
    bool removeUserTemplate(const QString &id, QString *error = nullptr);

    bool saveUserCloudInit(const QString &id, const QString &text, QString *error = nullptr);
    bool removeUserCloudInit(const QString &id, QString *error = nullptr);

    /// Ids end up in file names, so they are restricted to a safe set.
    [[nodiscard]] static bool isValidId(QStringView id);

    /// The definition of template @p id in the file at @p path.
    [[nodiscard]] static std::optional<QJsonObject> readTemplate(const QString &path, const QString &id);
    /// Translated description of the first problem, empty if the template is usable.
    [[nodiscard]] static QString validationError(const QJsonObject &tpl);
    /// The cloud-init templates a template refers to, as ImageDatabase resolves them.
    [[nodiscard]] static QStringList cloudInitIds(const QJsonObject &tpl);

private:
    [[nodiscard]] QString hiddenFilePath() const;

    QString m_userDataDir;
    QStringList m_systemDataDirs;
};
