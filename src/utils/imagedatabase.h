/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

#include <optional>

#include "const.h"
#include "utils/templatestore.h"
#include "utils/vmconfig.h"

using namespace Qt::StringLiterals;

struct CloudImageInfo {
    QString arch;
    QString url;
    QString format; // "qcow2", "raw", ...
    QString checksumUrl;
    QString defaultUser;
};

struct InstallerIsoInfo {
    QString arch;
    QString edition;
    QString url;
    QString note;
};

struct ReleaseInfo {
    QString version;
    bool quickDeploy = true;
    QString codename;
    bool lts = false;
    int minDiskGb = KEMUConst::DefaultMinDiskSizeGB;
    QList<CloudImageInfo> cloudImages;
    QList<InstallerIsoInfo> installerIsos;

    [[nodiscard]] QString displayString() const;
};

struct TemplateInfo {
    QString id;
    QString displayName;
    VMSpecs::OsType osType = VMSpecs::OsType::Linux;
    int sortOrder = KEMUConst::DefaultTemplateSortOrder;
    QString homepage;
    QString iconHint;
    QString cloudInitTemplate;
    QString networkTemplate;
    QList<ReleaseInfo> releases;
    VMConfig defaults;

    TemplateOrigin origin = TemplateOrigin::BuiltIn;
    QString sourcePath;
    // Origin of the definition this one replaces, e.g. a customized shipped one.
    std::optional<TemplateOrigin> replaces;
    bool hidden = false;
};

class ImageDatabase
{
public:
    ImageDatabase();

    // Loads templates.json and the templates of all origins, see TemplateStore.
    bool load();
    // templateDirs in descending priority; a file name in an earlier directory
    // shadows the same name in later ones (not across packages), and a later
    // loaded template id replaces an earlier one completely.
    bool load(const QString &defaultsPath, const QList<TemplateSearchDir> &templateDirs, const QStringList &hiddenIds = {});

    // Without hidden templates, for choosing one.
    [[nodiscard]] QList<TemplateInfo> templates() const;
    [[nodiscard]] QList<TemplateInfo> allTemplates() const;
    [[nodiscard]] QList<TemplateInfo> templatesForType(VMSpecs::OsType type) const;
    [[nodiscard]] const TemplateInfo *findById(const QString &id) const;
    [[nodiscard]] QString displayName(const QString &id) const;

    [[nodiscard]] QList<ReleaseInfo> releasesFor(const QString &tplId) const;
    [[nodiscard]] QList<CloudImageInfo> cloudImagesFor(const QString &tplId, const QString &version, bool allArchitectures = false) const;

    [[nodiscard]] VMConfig defaultsFor(const QString &tplId) const;
    [[nodiscard]] VMConfig globalDefaults() const;
    [[nodiscard]] bool hasCloudImages(const QString &tplId) const;
    [[nodiscard]] int minDiskGbFor(const QString &tplId, const QString &version) const;

    [[nodiscard]] static QString hostArch();
    [[nodiscard]] static QString imageCacheDir();
    [[nodiscard]] static QString localImagePath(const QString &url);
    [[nodiscard]] static bool isImageCached(const QString &url);
    [[nodiscard]] static bool isArchCompatible(const QString &imageArch);
    [[nodiscard]] bool isQuickDeployEnabled(const QString &tplId, const QString &version) const;

private:
    TemplateInfo parseTemplate(const QJsonObject &obj, const VMConfig &globalDefaults) const;
    ReleaseInfo parseRelease(const QJsonObject &obj) const;
    CloudImageInfo parseCloudImage(const QJsonObject &obj) const;

    static VMConfig overlayConfig(const VMConfig &base, const QJsonObject &overrides);

    QList<TemplateInfo> m_templates;
    VMConfig m_globalDefaults;
};
