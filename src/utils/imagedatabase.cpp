/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "imagedatabase.h"
#include "const.h"

using namespace Qt::StringLiterals;

QString ReleaseInfo::displayString() const
{
    if (codename.isEmpty())
        return version;
    return u"%1 (%2)"_s.arg(version, codename);
}

ImageDatabase::ImageDatabase() = default;

void ImageDatabase::loadFromFile(const QString &jsonPath)
{
    m_templates.clear();

    m_globalDefaults = {};

    QFile file(jsonPath);
    if (!file.open(QIODevice::ReadOnly))
        return;

    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    const QJsonObject rootObj = doc.object();

    const VMConfig globalDefaults = VMConfig::fromJson(rootObj[u"global_qemu_defaults"_s].toObject());

    const QJsonArray arr = rootObj[u"templates"_s].toArray();
    for (const auto &val : arr)
        m_templates.append(parseTemplate(val.toObject(), m_globalDefaults));
}

QList<TemplateInfo> ImageDatabase::templates() const
{
    return m_templates;
}

QList<TemplateInfo> ImageDatabase::templatesForType(VMSpecs::OsType type) const
{
    QList<TemplateInfo> result;
    for (const auto &d : m_templates) {
        if (d.osType == type)
            result.append(d);
    }
    return result;
}

const TemplateInfo *ImageDatabase::findById(const QString &id) const
{
    for (const auto &d : m_templates) {
        if (d.id == id)
            return &d;
    }
    return nullptr;
}

QString ImageDatabase::displayName(const QString &id) const
{
    if (auto *d = findById(id))
        return d->displayName;
    return id;
}

QList<ReleaseInfo> ImageDatabase::releasesFor(const QString &tplId) const
{
    if (auto *d = findById(tplId))
        return d->releases;
    return {};
}

QList<CloudImageInfo> ImageDatabase::cloudImagesFor(const QString &tplId, const QString &version, bool allArchitectures) const
{
    auto *d = findById(tplId);
    if (!d)
        return {};

    const QString arch = hostArch();
    for (const auto &r : d->releases) {
        if (r.version == version) {
            if (allArchitectures)
                return r.cloudImages;
            QList<CloudImageInfo> filtered;
            for (const auto &img : r.cloudImages) {
                if (isArchCompatible(img.arch))
                    filtered.append(img);
            }
            return filtered;
        }
    }
    return {};
}

VMConfig ImageDatabase::defaultsFor(const QString &tplId) const
{
    if (auto *d = findById(tplId))
        return d->defaults;
    return {};
}

VMConfig ImageDatabase::globalDefaults() const
{
    return m_globalDefaults;
}

bool ImageDatabase::hasCloudImages(const QString &tplId) const
{
    auto *d = findById(tplId);
    if (!d)
        return false;
    for (const auto &r : d->releases) {
        if (!r.cloudImages.isEmpty())
            return true;
    }
    return false;
}

int ImageDatabase::minDiskGbFor(const QString &tplId, const QString &version) const
{
    auto *d = findById(tplId);
    if (!d || d->releases.isEmpty())
        return KEMUConst::DefaultMinDiskSizeGB;

    if (!version.isEmpty()) {
        for (const auto &r : d->releases) {
            if (r.version == version)
                return r.minDiskGb;
        }
    }

    // No version specified or not found: return minimum across all releases
    int minGb = d->releases.first().minDiskGb;
    for (const auto &r : d->releases)
        minGb = qMin(minGb, r.minDiskGb);
    return minGb;
}

QString ImageDatabase::hostArch()
{
    // QSysInfo::currentCpuArchitecture() returns "x86_64", "aarch64", etc.
    return QSysInfo::currentCpuArchitecture();
}

QString ImageDatabase::imageCacheDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + u"/images"_s;
}

QString ImageDatabase::localImagePath(const QString &url)
{
    const QString fileName = QUrl(url).fileName();
    return imageCacheDir() + u"/"_s + fileName;
}

bool ImageDatabase::isImageCached(const QString &url)
{
    return QFile::exists(localImagePath(url));
}

TemplateInfo ImageDatabase::parseTemplate(const QJsonObject &obj, const VMConfig &globalDefaults) const
{
    TemplateInfo d;
    d.id = obj[u"id"_s].toString();
    d.displayName = obj[u"display_name"_s].toString();
    d.homepage = obj[u"homepage"_s].toString();
    d.iconHint = obj[u"icon_hint"_s].toString();
    d.cloudInitTemplate = obj[u"cloud_init_template"_s].toString(d.id);
    d.networkTemplate = obj[u"network_template"_s].toString(d.id);

    const QString osTypeStr = obj[u"os_type"_s].toString();
    d.osType = VMConfig::parseOsType(osTypeStr);

    for (const auto &val : obj[u"releases"_s].toArray())
        d.releases.append(parseRelease(val.toObject()));

    d.defaults = overlayConfig(globalDefaults, obj[u"qemu_defaults"_s].toObject());

    return d;
}

bool ImageDatabase::isArchCompatible(const QString &imageArch)
{
    const QString host = hostArch();
    if (imageArch == host)
        return true;
    // x86_64 hosts can run i386/i686 images
    if (host == u"x86_64"_s && (imageArch == u"i386"_s || imageArch == u"i686"_s))
        return true;
    return false;
}

bool ImageDatabase::isQuickDeployEnabled(const QString &tplId, const QString &version) const
{
    auto *d = findById(tplId);
    if (!d)
        return false;
    for (const auto &r : d->releases) {
        if (r.version == version)
            return r.quickDeploy;
    }
    return false;
}

ReleaseInfo ImageDatabase::parseRelease(const QJsonObject &obj) const
{
    ReleaseInfo r;
    r.version = obj[u"version"_s].toString();
    r.quickDeploy = obj[u"quick_deploy"_s].toBool(true);
    r.codename = obj[u"codename"_s].toString();
    r.lts = obj[u"lts"_s].toBool(false);
    r.minDiskGb = obj[u"min_disk_gb"_s].toInt(KEMUConst::DefaultMinDiskSizeGB);

    for (const auto &val : obj[u"cloud_images"_s].toArray())
        r.cloudImages.append(parseCloudImage(val.toObject()));

    for (const auto &val : obj[u"installer_isos"_s].toArray()) {
        const QJsonObject o = val.toObject();
        InstallerIsoInfo iso;
        iso.arch = o[u"arch"_s].toString();
        iso.edition = o[u"edition"_s].toString();
        iso.url = o[u"url"_s].toString();
        iso.note = o[u"note"_s].toString();
        r.installerIsos.append(iso);
    }
    return r;
}

CloudImageInfo ImageDatabase::parseCloudImage(const QJsonObject &obj) const
{
    CloudImageInfo img;
    img.arch = obj[u"arch"_s].toString();
    img.url = obj[u"url"_s].toString();
    img.format = obj[u"format"_s].toString(u"qcow2"_s);
    img.checksumUrl = obj[u"checksum_url"_s].toString();
    img.defaultUser = obj[u"default_user"_s].toString();
    return img;
}

VMConfig ImageDatabase::overlayConfig(const VMConfig &base, const QJsonObject &overrides)
{
    if (overrides.isEmpty())
        return base;
    QJsonObject merged = base.toJson();
    for (auto it = overrides.begin(); it != overrides.end(); ++it)
        merged[it.key()] = it.value();
    return VMConfig::fromJson(merged);
}
