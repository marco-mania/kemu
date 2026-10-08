/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cloudimagedeployer.h"

#include "utils/cloudinittemplates.h"
#include "utils/externaltools.h"
#include "utils/isoimagebuilder.h"
#include "utils/utils.h"

#include <archive.h>
#include <archive_entry.h>

#include <QFile>
#include <QScopeGuard>

using namespace Qt::StringLiterals;

namespace
{

enum class UnpackResult {
    NotCompressed,
    Unpacked,
    Failed,
};

// Distributions ship some cloud images compressed (FreeBSD: .qcow2.xz).
// libarchive's raw format detects the compression by content and streams
// the payload; uncompressed images are left alone.
UnpackResult unpackImage(const QString &sourcePath, const QString &targetPath, QString &errorOut, const CloudImageDeployer::ProgressReport &progress)
{
    archive *a = archive_read_new();
    const auto freeArchive = qScopeGuard([a] {
        archive_read_free(a);
    });
    archive_read_support_filter_all(a);
    archive_read_support_format_raw(a);

    archive_entry *entry = nullptr;
    if (archive_read_open_filename(a, QFile::encodeName(sourcePath).constData(), 1 << 20) != ARCHIVE_OK
        || archive_read_next_header(a, &entry) != ARCHIVE_OK) {
        errorOut = i18n("Could not read the cloud image: %1", QString::fromLocal8Bit(archive_error_string(a)));
        return UnpackResult::Failed;
    }

    if (archive_filter_code(a, 0) == ARCHIVE_FILTER_NONE)
        return UnpackResult::NotCompressed;

    if (progress)
        progress(0, i18n("Decompressing the cloud image..."));

    QFile out(targetPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        errorOut = i18n("Could not create %1: %2", targetPath, out.errorString());
        return UnpackResult::Failed;
    }

    QByteArray buffer(1 << 20, Qt::Uninitialized);
    for (;;) {
        const la_ssize_t read = archive_read_data(a, buffer.data(), static_cast<size_t>(buffer.size()));
        if (read == 0)
            break;
        if (read < 0) {
            errorOut = i18n("Decompressing the cloud image failed: %1", QString::fromLocal8Bit(archive_error_string(a)));
            out.remove();
            return UnpackResult::Failed;
        }
        if (out.write(buffer.constData(), read) != read) {
            errorOut = i18n("Writing %1 failed: %2", targetPath, out.errorString());
            out.remove();
            return UnpackResult::Failed;
        }
    }

    return UnpackResult::Unpacked;
}

} // namespace

CloudImageDeployer::Result
CloudImageDeployer::deploy(const QString &sourceImage, const QDir &vmDir, int diskSizeGb, const CloudDeployConfig &config, const ProgressReport &progress)
{
    Result result;

    if (progress)
        progress(0, i18n("Preparing the disk image..."));

    if (!prepareImage(sourceImage, vmDir, diskSizeGb, result.diskPath, result.errorMessage, progress)) {
        return result;
    }

    if (progress)
        progress(2, i18n("Building the cloud-init image..."));

    const QByteArray userData = generateUserData(config, result.errorMessage);
    if (userData.isEmpty())
        return result;

    const QByteArray metaData = generateMetaData(config);

    const QByteArray networkCfg = generateNetworkConfig(config, result.errorMessage);

    if (!result.errorMessage.isEmpty() && networkCfg.isEmpty()) {
        return result;
    }

    if (!createNoCloudIso(vmDir, userData, metaData, networkCfg, result.errorMessage)) {
        return result;
    }

    result.noCloudIsoPath = noCloudIsoPath(vmDir);
    result.success = true;
    return result;
}

void CloudImageDeployer::removeNoCloudIso(const QDir &vmDir)
{
    QFile::remove(noCloudIsoPath(vmDir));
}

bool CloudImageDeployer::hasNoCloudIso(const QDir &vmDir)
{
    return QFile::exists(noCloudIsoPath(vmDir));
}

QString CloudImageDeployer::noCloudIsoPath(const QDir &vmDir)
{
    return vmDir.absoluteFilePath(u"cidata.iso"_s);
}

bool CloudImageDeployer::prepareImage(const QString &sourceImage,
                                      const QDir &vmDir,
                                      int diskSizeGb,
                                      QString &diskPathOut,
                                      QString &errorOut,
                                      const ProgressReport &progress)
{
    diskPathOut = vmDir.absoluteFilePath(u"disk.qcow2"_s);

    const QString qemuImg = ExternalTools::instance().path(ExternalTools::QemuImg);
    if (qemuImg.isEmpty()) {
        errorOut = i18n("qemu-img not found.");
        return false;
    }

    QString sourcePath = sourceImage;
    const QString unpackedPath = vmDir.absoluteFilePath(u"disk.unpacked"_s);
    const auto removeUnpacked = qScopeGuard([&unpackedPath] {
        QFile::remove(unpackedPath);
    });

    switch (unpackImage(sourceImage, unpackedPath, errorOut, progress)) {
    case UnpackResult::Failed:
        return false;
    case UnpackResult::Unpacked:
        sourcePath = unpackedPath;
        break;
    case UnpackResult::NotCompressed:
        break;
    }

    KProcess proc;
    proc.setOutputChannelMode(KProcess::SeparateChannels);

    // Detect source format
    proc.setProgram(qemuImg, {u"info"_s, u"--output=json"_s, sourcePath});
    if (proc.execute(10000) < 0) {
        errorOut = i18n("qemu-img info timed out or failed to start.");
        return false;
    }
    if (proc.exitStatus() != KProcess::NormalExit || proc.exitCode() != 0) {
        errorOut = i18n("Could not read the source image: %1", QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return false;
    }

    const QJsonDocument info = QJsonDocument::fromJson(proc.readAllStandardOutput());
    QString srcFormat = info.object()[u"format"_s].toString();
    if (srcFormat.isEmpty())
        srcFormat = u"qcow2"_s;

    if (QFile::exists(diskPathOut) && !QFile::remove(diskPathOut)) {
        errorOut = i18n("Could not remove the existing disk image at %1.", diskPathOut);
        return false;
    }

    if (srcFormat == u"qcow2"_s) {
        const bool placed = sourcePath == unpackedPath ? QFile::rename(unpackedPath, diskPathOut) : QFile::copy(sourceImage, diskPathOut);
        if (!placed) {
            errorOut = i18n("Failed to copy cloud image.");
            return false;
        }
        // Ensure writable
        QFile::setPermissions(diskPathOut, QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
    } else {
        proc.setProgram(qemuImg, {u"convert"_s, u"-f"_s, srcFormat, u"-O"_s, u"qcow2"_s, sourcePath, diskPathOut});
        if (proc.execute(120000) != 0) {
            errorOut = i18n("qemu-img convert failed: %1", QString::fromUtf8(proc.readAllStandardError()).trimmed());
            return false;
        }
    }

    // Resize to target size
    if (progress)
        progress(1, i18n("Resizing the disk image..."));

    // Resize to target size
    proc.setProgram(qemuImg, {u"resize"_s, diskPathOut, u"%1G"_s.arg(diskSizeGb)});
    if (proc.execute(30000) != 0) {
        errorOut = i18n("qemu-img resize failed: %1", QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return false;
    }

    return true;
}

QByteArray CloudImageDeployer::generateUserData(const CloudDeployConfig &config, QString &errorOut)
{
    const QString templateId = config.templateId.isEmpty() ? u"default"_s : config.templateId;
    const QString tpl = CloudInitTemplates::loadTemplate(templateId);

    if (tpl.isEmpty()) {
        errorOut = i18n("Cloud-Init template '%1' not found. Please check your installation.", templateId);
        return {};
    }

    QVariantHash vars;

    vars[u"DESKTOP_ENVIRONMENT"_s] = config.installDesktop;

    vars[u"HOSTNAME"_s] = config.hostname;
    vars[u"USERNAME"_s] = config.username;
    vars[u"TIMEZONE"_s] = config.timezone;
    vars[u"LOCALE"_s] = config.locale;
    vars[u"KB_LAYOUT"_s] = config.kbLayout.isEmpty() ? u"us"_s : config.kbLayout;
    vars[u"KB_VARIANT"_s] = config.kbVariant;

    // Language code without .UTF-8 suffix
    QString lang = config.locale;
    lang.remove(u".UTF-8"_s);
    vars[u"LANG"_s] = lang;
    vars[u"LANG_SHORT"_s] = lang.left(2);

    vars[u"SSH_PWAUTH"_s] = config.passwordLogin;
    if (!config.password.isEmpty()) {
        vars[u"PASSWORD_HASH"_s] = Utils::generateCloudInitPasswordHash(config.password);
    } else {
        vars[u"PASSWORD_HASH"_s] = {};
    }
    vars[u"SSH_KEYS"_s] = config.sshKeys;

    vars[u"PACKAGES"_s] = config.extraPackages;

    const KTextTemplate::Engine engine;
    const KTextTemplate::Template t = engine.newTemplate(tpl, u"cloud-init"_s);

    KTextTemplate::Context context(vars);

    return t->render(&context).toUtf8();
}

QByteArray CloudImageDeployer::generateMetaData(const CloudDeployConfig &config)
{
    QString yaml;
    QTextStream s(&yaml);

    s << u"instance-id: kemu-"_s << config.hostname << u"-"_s << QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) << u"\n"_s;
    s << u"local-hostname: "_s << config.hostname << u"\n"_s;

    return yaml.toUtf8();
}

QByteArray CloudImageDeployer::generateNetworkConfig(const CloudDeployConfig &config, QString &errorOut)
{
    const QString templateId = config.networkTemplateId.isEmpty() ? u"network-v2"_s : config.networkTemplateId;

    const QString tpl = CloudInitTemplates::loadTemplate(templateId);

    if (tpl.isEmpty()) {
        errorOut = i18n("Network template '%1' not found. Please check your installation.", templateId);
        return {};
    }

    const QVariantHash vars;

    const KTextTemplate::Engine engine;
    const KTextTemplate::Template t = engine.newTemplate(tpl, u"network-config"_s);

    KTextTemplate::Context context(vars);

    return t->render(&context).toUtf8();
}

bool CloudImageDeployer::createNoCloudIso(const QDir &vmDir,
                                          const QByteArray &userData,
                                          const QByteArray &metaData,
                                          const QByteArray &networkConfig,
                                          QString &errorOut)
{
    // The NoCloud datasource finds the seed by volume label "cidata" and
    // reads the three files from the image root.
    const QList<IsoImageBuilder::Entry> entries = {{u"user-data"_s, userData}, {u"meta-data"_s, metaData}, {u"network-config"_s, networkConfig}};

    QString isoError;
    if (!IsoImageBuilder::write(noCloudIsoPath(vmDir), u"cidata"_s, entries, isoError)) {
        errorOut = i18n("ISO creation failed: %1", isoError);
        return false;
    }

    return true;
}
