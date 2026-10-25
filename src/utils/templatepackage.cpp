/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "templatepackage.h"

#include "utils/templatestore.h"

extern "C" {
#include <archive.h>
#include <archive_entry.h>
}

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <KLocalizedString>

using namespace Qt::StringLiterals;

namespace
{

// Templates are a few kilobytes; anything far bigger is not one.
constexpr qint64 kMaxEntrySize = 1 << 20;
constexpr qint64 kMaxTotalSize = 8 << 20;
const QString kCloudInitSuffix = u".yaml.tpl"_s;
const QString kTemplateFileName = u"template.json"_s;

// A file holds {"templates": […]} or a single template object.
bool addTemplates(TemplatePackage &package, const QByteArray &data, const QString &name, QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (!doc.isObject()) {
        if (error)
            *error = i18n("%1 is not a valid template file: %2", name, parseError.errorString());
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.contains(u"templates"_s)) {
        for (const QJsonValue &value : root.value(u"templates"_s).toArray()) {
            if (value.isObject())
                package.templates << value.toObject();
        }
    } else if (root.contains(u"id"_s)) {
        package.templates << root;
    }
    return true;
}

QString archiveError(archive *a)
{
    const char *message = archive_error_string(a);
    return message ? QString::fromUtf8(message) : i18n("Unknown archive error.");
}

} // namespace

std::optional<TemplatePackage> TemplatePackage::read(const QString &path, QString *error)
{
    TemplatePackage package;

    if (path.endsWith(u".json"_s, Qt::CaseInsensitive)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxEntrySize) {
            if (error)
                *error = i18n("Cannot read %1.", path);
            return std::nullopt;
        }
        if (!addTemplates(package, file.readAll(), QFileInfo(path).fileName(), error))
            return std::nullopt;
    } else {
        archive *a = archive_read_new();
        archive_read_support_filter_all(a);
        archive_read_support_format_all(a);
        if (archive_read_open_filename(a, QFile::encodeName(path).constData(), 10240) != ARCHIVE_OK) {
            if (error)
                *error = i18n("Cannot open %1: %2", path, archiveError(a));
            archive_read_free(a);
            return std::nullopt;
        }

        qint64 total = 0;
        bool ok = true;
        archive_entry *entry = nullptr;
        int r;
        while ((r = archive_read_next_header(a, &entry)) == ARCHIVE_OK) {
            if (archive_entry_filetype(entry) != AE_IFREG)
                continue;
            // Only the file name counts; directories inside the package are ignored.
            const QString name = QFileInfo(QString::fromUtf8(archive_entry_pathname(entry))).fileName();
            const bool isTemplate = name.endsWith(u".json"_s, Qt::CaseInsensitive);
            const bool isCloudInit = name.endsWith(kCloudInitSuffix);
            if (!isTemplate && !isCloudInit)
                continue;

            QByteArray data;
            char buffer[16384];
            la_ssize_t n;
            while ((n = archive_read_data(a, buffer, sizeof(buffer))) > 0) {
                data.append(buffer, n);
                total += n;
                if (data.size() > kMaxEntrySize || total > kMaxTotalSize)
                    break;
            }
            if (n < 0 || data.size() > kMaxEntrySize || total > kMaxTotalSize) {
                if (error)
                    *error = n < 0 ? i18n("Cannot read %1: %2", path, archiveError(a)) : i18n("%1 is too large for a template package.", path);
                ok = false;
                break;
            }

            if (isTemplate) {
                if (!addTemplates(package, data, name, error)) {
                    ok = false;
                    break;
                }
            } else {
                const QString id = name.chopped(kCloudInitSuffix.size());
                if (TemplateStore::isValidId(id))
                    package.cloudInit.insert(id, QString::fromUtf8(data));
            }
        }
        if (ok && r != ARCHIVE_EOF) {
            if (error)
                *error = i18n("Cannot read %1: %2", path, archiveError(a));
            ok = false;
        }
        archive_read_free(a);
        if (!ok)
            return std::nullopt;
    }

    if (package.templates.isEmpty()) {
        if (error)
            *error = i18n("%1 contains no OS templates.", QFileInfo(path).fileName());
        return std::nullopt;
    }
    return package;
}

bool TemplatePackage::write(const QString &path, QString *error) const
{
    QJsonArray array;
    for (const QJsonObject &tpl : templates)
        array.append(tpl);
    QList<std::pair<QString, QByteArray>> files;
    files.append({kTemplateFileName, QJsonDocument(QJsonObject{{u"version"_s, 1}, {u"templates"_s, array}}).toJson(QJsonDocument::Indented)});
    for (auto it = cloudInit.cbegin(); it != cloudInit.cend(); ++it)
        files.append({it.key() + kCloudInitSuffix, it.value().toUtf8()});

    archive *a = archive_write_new();
    archive_write_add_filter_gzip(a);
    archive_write_set_format_pax_restricted(a);
    bool ok = archive_write_open_filename(a, QFile::encodeName(path).constData()) == ARCHIVE_OK;

    for (const auto &[name, data] : std::as_const(files)) {
        if (!ok)
            break;
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname(entry, name.toUtf8().constData());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_entry_set_size(entry, data.size());
        ok = archive_write_header(a, entry) == ARCHIVE_OK && archive_write_data(a, data.constData(), data.size()) == data.size();
        archive_entry_free(entry);
    }

    if (!ok && error)
        *error = i18n("Cannot write %1: %2", path, archiveError(a));
    if (archive_write_close(a) != ARCHIVE_OK && ok) {
        if (error)
            *error = i18n("Cannot write %1: %2", path, archiveError(a));
        ok = false;
    }
    archive_write_free(a);
    if (!ok)
        QFile::remove(path);
    return ok;
}
