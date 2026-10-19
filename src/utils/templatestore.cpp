/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "templatestore.h"

#include "const.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>

#include <KLocalizedString>

#include <optional>

using namespace Qt::StringLiterals;

namespace
{

constexpr int kFormatVersion = 1;
const QString kCloudInitSuffix = u".yaml.tpl"_s;
// Not "<id>.json": a user file must not shadow a shipped file of that name,
// which may define other templates as well.
const QString kUserTemplateSuffix = u".user.json"_s;

std::optional<QJsonObject> readJsonObject(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    if (!doc.isObject())
        return std::nullopt;
    return doc.object();
}

bool writeFile(const QString &path, const QByteArray &data, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error)
            *error = i18n("Cannot create the folder %1.", QFileInfo(path).absolutePath());
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        if (error)
            *error = i18n("Cannot write %1: %2", path, file.errorString());
        return false;
    }
    return true;
}

bool writeJson(const QString &path, const QJsonObject &root, QString *error)
{
    return writeFile(path, QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

bool removeFile(const QString &path, QString *error)
{
    if (!QFile::exists(path) || QFile::remove(path))
        return true;
    if (error)
        *error = i18n("Cannot remove %1.", path);
    return false;
}

} // namespace

TemplateStore::TemplateStore()
    : m_userDataDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
{
    for (const QString &dir : QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation)) {
        if (QDir::cleanPath(dir) != QDir::cleanPath(m_userDataDir))
            m_systemDataDirs << dir;
    }
}

TemplateStore::TemplateStore(const QString &userDataDir, const QStringList &systemDataDirs)
    : m_userDataDir(userDataDir)
    , m_systemDataDirs(systemDataDirs)
{
}

QString TemplateStore::globalDefaultsPath() const
{
    // A copy in the user's data dir would pin the defaults across updates, so
    // it is only used when no system-wide file exists.
    for (const QString &dir : std::as_const(m_systemDataDirs)) {
        const QString path = QDir(dir).filePath(KEMUConst::TemplateDbFilePath);
        if (QFileInfo::exists(path))
            return path;
    }
    const QString userPath = QDir(m_userDataDir).filePath(KEMUConst::TemplateDbFilePath);
    return QFileInfo::exists(userPath) ? userPath : QString();
}

QString TemplateStore::userTemplateDir() const
{
    return QDir(m_userDataDir).filePath(KEMUConst::TemplateDropInDirPath);
}

QString TemplateStore::userCloudInitDir() const
{
    return QDir(m_userDataDir).filePath(KEMUConst::CloudInitTemplateDirPath);
}

QString TemplateStore::downloadedTemplateDir() const
{
    return QDir(m_userDataDir).filePath(KEMUConst::DownloadedTemplateDirPath);
}

QString TemplateStore::downloadedCloudInitDir() const
{
    return QDir(m_userDataDir).filePath(KEMUConst::DownloadedCloudInitDirPath);
}

QString TemplateStore::hiddenFilePath() const
{
    return QDir(m_userDataDir).filePath(KEMUConst::HiddenTemplatesFilePath);
}

QList<TemplateSearchDir> TemplateStore::templateDirs() const
{
    QList<TemplateSearchDir> dirs;
    dirs.append({userTemplateDir(), TemplateOrigin::User, false});

    // Loose files and one subdirectory per downloaded package.
    const QDir downloaded(downloadedTemplateDir());
    dirs.append({downloaded.path(), TemplateOrigin::Downloaded, true});
    for (const QString &package : downloaded.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        dirs.append({downloaded.filePath(package), TemplateOrigin::Downloaded, true});

    for (const QString &dir : std::as_const(m_systemDataDirs))
        dirs.append({QDir(dir).filePath(KEMUConst::TemplateDropInDirPath), TemplateOrigin::BuiltIn, false});
    return dirs;
}

QList<TemplateSearchDir> TemplateStore::cloudInitDirs() const
{
    QList<TemplateSearchDir> dirs;
    dirs.append({userCloudInitDir(), TemplateOrigin::User, false});
    dirs.append({downloadedCloudInitDir(), TemplateOrigin::Downloaded, false});

    // Packages bring the cloud-init templates their OS templates refer to.
    const QDir downloaded(downloadedTemplateDir());
    for (const QString &package : downloaded.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
        dirs.append({downloaded.filePath(package), TemplateOrigin::Downloaded, true});

    for (const QString &dir : std::as_const(m_systemDataDirs))
        dirs.append({QDir(dir).filePath(KEMUConst::CloudInitTemplateDirPath), TemplateOrigin::BuiltIn, false});
    return dirs;
}

QString TemplateStore::locateCloudInit(const QString &id) const
{
    if (!isValidId(id))
        return {};
    for (const TemplateSearchDir &dir : cloudInitDirs()) {
        const QString path = QDir(dir.path).filePath(id + kCloudInitSuffix);
        if (QFileInfo(path).isFile())
            return path;
    }
    return {};
}

QList<CloudInitEntry> TemplateStore::cloudInitTemplates() const
{
    QMap<QString, CloudInitEntry> entries;
    for (const TemplateSearchDir &dir : cloudInitDirs()) {
        for (const QFileInfo &file : QDir(dir.path).entryInfoList({u"*"_s + kCloudInitSuffix}, QDir::Files, QDir::Name)) {
            const QString id = file.fileName().chopped(kCloudInitSuffix.size());
            if (!isValidId(id))
                continue;
            auto it = entries.find(id);
            if (it == entries.end())
                entries.insert(id, {id, dir.origin, file.absoluteFilePath(), std::nullopt});
            else if (!it->replaces)
                it->replaces = dir.origin;
        }
    }
    return entries.values();
}

bool TemplateStore::isNetworkTemplate(const QString &id)
{
    return id.startsWith(u"network-"_s);
}

QStringList TemplateStore::hiddenTemplateIds() const
{
    const auto root = readJsonObject(hiddenFilePath());
    if (!root)
        return {};
    QStringList ids;
    for (const QJsonValue &value : root->value(u"hidden"_s).toArray()) {
        if (value.isString() && !ids.contains(value.toString()))
            ids << value.toString();
    }
    return ids;
}

bool TemplateStore::setTemplateHidden(const QString &id, bool hidden, QString *error)
{
    QStringList ids = hiddenTemplateIds();
    if (hidden == ids.contains(id))
        return true;
    if (hidden)
        ids << id;
    else
        ids.removeAll(id);
    ids.sort();
    return writeJson(hiddenFilePath(), {{u"version"_s, kFormatVersion}, {u"hidden"_s, QJsonArray::fromStringList(ids)}}, error);
}

bool TemplateStore::saveUserTemplate(const QJsonObject &tpl, QString *error)
{
    const QString id = tpl.value(u"id"_s).toString();
    if (!isValidId(id)) {
        if (error)
            *error = i18n("'%1' is not a valid template id. Use letters, digits, '.', '-' and '_'.", id);
        return false;
    }
    // Two user files defining the same id would leave it to the file names
    // which one wins.
    if (!removeUserTemplate(id, error))
        return false;

    const QJsonObject root{{u"version"_s, kFormatVersion}, {u"templates"_s, QJsonArray{tpl}}};
    return writeJson(QDir(userTemplateDir()).filePath(id + kUserTemplateSuffix), root, error);
}

bool TemplateStore::removeUserTemplate(const QString &id, QString *error)
{
    const QDir dir(userTemplateDir());
    for (const QFileInfo &entry : dir.entryInfoList({u"*.json"_s}, QDir::Files, QDir::Name)) {
        // Empty files mask a shipped file of the same name; leave them alone.
        if (entry.size() == 0)
            continue;
        const auto root = readJsonObject(entry.absoluteFilePath());
        if (!root)
            continue;

        const QJsonArray templates = root->value(u"templates"_s).toArray();
        QJsonArray kept;
        for (const QJsonValue &value : templates) {
            if (value.toObject().value(u"id"_s).toString() != id)
                kept.append(value);
        }
        if (kept.size() == templates.size())
            continue;

        if (kept.isEmpty()) {
            if (!removeFile(entry.absoluteFilePath(), error))
                return false;
        } else {
            QJsonObject rewritten = *root;
            rewritten[u"templates"_s] = kept;
            if (!writeJson(entry.absoluteFilePath(), rewritten, error))
                return false;
        }
    }
    return true;
}

bool TemplateStore::saveUserCloudInit(const QString &id, const QString &text, QString *error)
{
    if (!isValidId(id)) {
        if (error)
            *error = i18n("'%1' is not a valid template id. Use letters, digits, '.', '-' and '_'.", id);
        return false;
    }
    return writeFile(QDir(userCloudInitDir()).filePath(id + kCloudInitSuffix), text.toUtf8(), error);
}

bool TemplateStore::removeUserCloudInit(const QString &id, QString *error)
{
    if (!isValidId(id))
        return true;
    return removeFile(QDir(userCloudInitDir()).filePath(id + kCloudInitSuffix), error);
}

std::optional<QJsonObject> TemplateStore::readTemplate(const QString &path, const QString &id)
{
    const auto root = readJsonObject(path);
    if (!root)
        return std::nullopt;
    for (const QJsonValue &value : root->value(u"templates"_s).toArray()) {
        if (value.toObject().value(u"id"_s).toString() == id)
            return value.toObject();
    }
    return std::nullopt;
}

QString TemplateStore::validationError(const QJsonObject &tpl)
{
    const QString id = tpl.value(u"id"_s).toString();
    if (!isValidId(id))
        return i18n("'id' must be 1 to 64 letters, digits, '.', '-' or '_', starting with a letter or digit.");
    if (tpl.value(u"display_name"_s).toString().trimmed().isEmpty())
        return i18n("'display_name' is missing.");

    static const QStringList osTypes = {u"linux"_s, u"windows"_s, u"bsd"_s, u"dos"_s, u"other"_s};
    if (tpl.contains(u"os_type"_s) && !osTypes.contains(tpl.value(u"os_type"_s).toString()))
        return i18n("'os_type' must be one of %1.", osTypes.join(u", "_s));
    if (tpl.contains(u"sort_order"_s) && !tpl.value(u"sort_order"_s).isDouble())
        return i18n("'sort_order' must be a number.");
    if (tpl.contains(u"qemu_defaults"_s) && !tpl.value(u"qemu_defaults"_s).isObject())
        return i18n("'qemu_defaults' must be an object.");
    for (const QString &key : {u"cloud_init_template"_s, u"network_template"_s}) {
        if (tpl.contains(key) && !isValidId(tpl.value(key).toString()))
            return i18n("'%1' must name a cloud-init template by its id.", key);
    }

    const auto isWebUrl = [](const QJsonValue &value) {
        const QUrl url(value.toString(), QUrl::StrictMode);
        return url.isValid() && (url.scheme() == u"https"_s || url.scheme() == u"http"_s) && !url.host().isEmpty();
    };

    if (tpl.contains(u"releases"_s) && !tpl.value(u"releases"_s).isArray())
        return i18n("'releases' must be a list.");
    const QJsonArray releases = tpl.value(u"releases"_s).toArray();
    for (qsizetype i = 0; i < releases.size(); ++i) {
        // An empty version is fine: unversioned templates like "Other Linux".
        if (!releases.at(i).isObject())
            return i18n("Release %1 must be an object.", i + 1);
        const QJsonObject release = releases.at(i).toObject();
        for (const QString &listKey : {u"cloud_images"_s, u"installer_isos"_s}) {
            for (const QJsonValue &entry : release.value(listKey).toArray()) {
                const QJsonObject image = entry.toObject();
                // Installer ISOs may leave the URL empty and point to the download page in 'note'.
                const bool optional = listKey == u"installer_isos"_s && image.value(u"url"_s).toString().isEmpty();
                if (!optional && !isWebUrl(image.value(u"url"_s)))
                    return i18n("Release %1: every entry in '%2' needs an http or https 'url'.", i + 1, listKey);
                // Empty: no checksum published.
                if (!image.value(u"checksum_url"_s).toString().isEmpty() && !isWebUrl(image.value(u"checksum_url"_s)))
                    return i18n("Release %1: 'checksum_url' must be an http or https URL.", i + 1);
            }
        }
    }
    return {};
}

QStringList TemplateStore::cloudInitIds(const QJsonObject &tpl)
{
    const QString id = tpl.value(u"id"_s).toString();
    QStringList ids;
    for (const QString &key : {u"cloud_init_template"_s, u"network_template"_s}) {
        const QString ref = tpl.value(key).toString(id);
        if (isValidId(ref) && !ids.contains(ref))
            ids << ref;
    }
    return ids;
}

// Template IDs: ASCII letters, digits, '.', '_' and '-'
static bool isIdChar(QChar c)
{
    const ushort u = c.unicode();
    return (u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9') || u == u'.' || u == u'_' || u == u'-';
}

bool TemplateStore::isValidId(QStringView id)
{
    // 1 + up to 63 trailing characters
    if (id.isEmpty() || id.size() > 64)
        return false;

    // The first character must be a letter or digit
    const ushort first = id.front().unicode();
    const bool firstIsAlnum = (first >= u'a' && first <= u'z') || (first >= u'A' && first <= u'Z') || (first >= u'0' && first <= u'9');
    if (!firstIsAlnum)
        return false;

    // Single pass: validate characters and reject adjacent dots
    for (qsizetype i = 1; i < id.size(); ++i) {
        const QChar c = id.at(i);
        if (!isIdChar(c))
            return false;
        if (c == u'.' && id.at(i - 1) == u'.')
            return false; // ".." would allow directory traversal
    }
    return true;
}
