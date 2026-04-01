/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "cloudimagedeployer.h"

#include "utils/cloudinittemplates.h"
#include "utils/externaltools.h"
#include "utils/utils.h"

using namespace Qt::StringLiterals;

CloudImageDeployer::Result CloudImageDeployer::deploy(
    const QString& sourceImage,
    const QDir& vmDir,
    int diskSizeGb,
    const CloudDeployConfig& config)
{
    Result result;

    if (!prepareImage(sourceImage, vmDir, diskSizeGb, result.diskPath, result.errorMessage)) {
        return result;
    }

    QByteArray userData = generateUserData(config, result.errorMessage);
    if (userData.isEmpty())
        return result;

    QByteArray metaData = generateMetaData(config);

    QByteArray networkCfg = generateNetworkConfig(config, result.errorMessage);

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

void CloudImageDeployer::removeNoCloudIso(const QDir& vmDir)
{
    QFile::remove(noCloudIsoPath(vmDir));
}

bool CloudImageDeployer::hasNoCloudIso(const QDir& vmDir)
{
    return QFile::exists(noCloudIsoPath(vmDir));
}

QString CloudImageDeployer::noCloudIsoPath(const QDir& vmDir)
{
    return vmDir.absoluteFilePath(u"cidata.iso"_s);
}

bool CloudImageDeployer::prepareImage(const QString& sourceImage,
    const QDir& vmDir,
    int diskSizeGb,
    QString& diskPathOut,
    QString& errorOut)
{
    diskPathOut = vmDir.absoluteFilePath(u"disk.qcow2"_s);

    // If source is raw, convert to qcow2; otherwise copy
    QProcess proc;
    const QString qemuImg = ExternalTools::instance().path(ExternalTools::QemuImg);
    if (qemuImg.isEmpty()) {
        errorOut = u"qemu-img not found."_s;
        return false;
    }

    // Detect source format
    proc.start(qemuImg, { u"info"_s, u"--output=json"_s, sourceImage });
    if (!proc.waitForFinished(10000)) {
        errorOut = u"qemu-img info timed out."_s;
        return false;
    }

    QJsonDocument info = QJsonDocument::fromJson(proc.readAllStandardOutput());
    QString srcFormat = info.object()[u"format"_s].toString();
    if (srcFormat.isEmpty())
        srcFormat = u"qcow2"_s;

    if (srcFormat == u"qcow2"_s) {
        if (!QFile::copy(sourceImage, diskPathOut)) {
            errorOut = u"Failed to copy cloud image."_s;
            return false;
        }
        // Ensure writable
        QFile::setPermissions(diskPathOut,
            QFile::ReadOwner | QFile::WriteOwner | QFile::ReadGroup);
    } else {
        proc.start(qemuImg, { u"convert"_s, u"-f"_s, srcFormat, u"-O"_s, u"qcow2"_s, sourceImage, diskPathOut });
        if (!proc.waitForFinished(120000) || proc.exitCode() != 0) {
            errorOut = u"qemu-img convert failed: %1"_s.arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
            return false;
        }
    }

    // Resize to target size
    proc.start(qemuImg, { u"resize"_s, diskPathOut, u"%1G"_s.arg(diskSizeGb) });
    if (!proc.waitForFinished(30000) || proc.exitCode() != 0) {
        errorOut = u"qemu-img resize failed: %1"_s.arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return false;
    }

    return true;
}

QByteArray CloudImageDeployer::generateUserData(const CloudDeployConfig& config, QString& errorOut)
{
    QString templateId = config.templateId.isEmpty() ? u"default"_s : config.templateId;
    QString tpl = CloudInitTemplates::loadTemplate(templateId);

    if (tpl.isEmpty()) {
        errorOut = u"Cloud-Init template '%1' not found. Please check your installation."_s.arg(templateId);
        return { };
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
        vars[u"PASSWORD_HASH"_s] = { };
    }
    vars[u"SSH_KEYS"_s] = config.sshKeys;

    vars[u"PACKAGES"_s] = config.extraPackages;

    KTextTemplate::Engine engine;
    KTextTemplate::Template t = engine.newTemplate(tpl, u"cloud-init"_s);

    KTextTemplate::Context context(vars);

    return t->render(&context).toUtf8();
}

QByteArray CloudImageDeployer::generateMetaData(const CloudDeployConfig& config)
{
    QString yaml;
    QTextStream s(&yaml);

    s << u"instance-id: kemu-"_s << config.hostname << u"-"_s << QUuid::createUuid().toString(QUuid::WithoutBraces).left(8) << u"\n"_s;
    s << u"local-hostname: "_s << config.hostname << u"\n"_s;

    return yaml.toUtf8();
}

QByteArray CloudImageDeployer::generateNetworkConfig(const CloudDeployConfig& config, QString& errorOut)
{
    QString templateId = config.networkTemplateId.isEmpty() ? u"network-v2"_s : config.networkTemplateId;

    QString tpl = CloudInitTemplates::loadTemplate(templateId);

    if (tpl.isEmpty()) {
        errorOut = u"Network template '%1' not found. Please check your installation."_s.arg(templateId);
        return { };
    }

    QVariantHash vars;

    KTextTemplate::Engine engine;
    KTextTemplate::Template t = engine.newTemplate(tpl, u"network-config"_s);

    KTextTemplate::Context context(vars);

    return t->render(&context).toUtf8();
}

bool CloudImageDeployer::createNoCloudIso(const QDir& vmDir,
    const QByteArray& userData,
    const QByteArray& metaData,
    const QByteArray& networkConfig,
    QString& errorOut)
{
    QTemporaryDir tmpDir;
    if (!tmpDir.isValid()) {
        errorOut = u"Failed to create temp directory."_s;
        return false;
    }

    auto writeFile = [&](const QString& name, const QByteArray& content) -> bool {
        QFile f(tmpDir.filePath(name));
        if (!f.open(QIODevice::WriteOnly)) {
            errorOut = u"Failed to write %1."_s.arg(name);
            return false;
        }
        f.write(content);
        return true;
    };

    if (!writeFile(u"user-data"_s, userData))
        return false;
    if (!writeFile(u"meta-data"_s, metaData))
        return false;
    if (!writeFile(u"network-config"_s, networkConfig))
        return false;

    const QString tool = findIsoTool();
    if (tool.isEmpty()) {
        errorOut = u"No ISO tool found (install genisoimage, mkisofs, or xorriso)."_s;
        return false;
    }

    const QString outputIso = noCloudIsoPath(vmDir);

    QStringList args;
    if (tool.endsWith(u"xorriso"_s))
        args << u"-as"_s << u"genisoimage"_s;
    args << u"-output"_s << outputIso
         << u"-volid"_s << u"cidata"_s
         << u"-joliet"_s << u"-rock"_s
         << tmpDir.path();

    QProcess proc;
    proc.setProgram(tool);
    proc.setArguments(args);
    proc.start();
    if (!proc.waitForFinished(15000)) {
        proc.kill();
        errorOut = u"ISO creation timed out."_s;
        return false;
    }

    if (proc.exitCode() != 0 || !QFile::exists(outputIso)) {
        errorOut = u"ISO creation failed: %1"_s.arg(QString::fromUtf8(proc.readAllStandardError()).trimmed());
        return false;
    }

    return true;
}

QString CloudImageDeployer::findIsoTool()
{
    const auto& tools = ExternalTools::instance();
    if (tools.isAvailable(ExternalTools::GenIsoImage))
        return tools.path(ExternalTools::GenIsoImage);

    QString path = QStandardPaths::findExecutable(u"genisoimage"_s);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(u"mkisofs"_s);
    if (path.isEmpty())
        path = QStandardPaths::findExecutable(u"xorriso"_s);
    return path;
}
