/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QTextStream>
#include <QUuid>

#include <KLocalizedString>
#include <KProcess>

#include <KTextTemplate/Context>
#include <KTextTemplate/Engine>
#include <KTextTemplate/Template>

using namespace Qt::StringLiterals;

struct CloudDeployConfig {
    QString username = u"user"_s;
    QString password;
    QString hostname = u"kemu-vm"_s;
    QString locale = u"en_US.UTF-8"_s;
    QString timezone;
    QString kbLayout;
    QString kbVariant;
    QStringList sshKeys;
    QStringList extraPackages;
    bool passwordLogin = true;
    bool installDesktop = false;
    QString templateId;
    QString networkTemplateId;
    // Chosen in the cloud configuration dialog; replaces the OS template's
    // templateId. Empty: the distribution's own.
    QString customTemplateId;
};

class CloudImageDeployer
{
public:
    struct Result {
        bool success = false;
        QString errorMessage;
        QString diskPath;
        QString noCloudIsoPath;
    };

    // Reports the phase the deployment has reached. Called from whatever
    // thread deploy() runs on.
    static constexpr int ProgressSteps = 3;
    using ProgressReport = std::function<void(int step, const QString &message)>;

    static Result deploy(const QString &sourceImage, const QDir &vmDir, int diskSizeGb, const CloudDeployConfig &config, const ProgressReport &progress = {});

    /// Renders a cloud-init template with the variables of @p config.
    /// @p errorOut is set and an empty string returned on template errors.
    static QString renderTemplate(const QString &templateText, const CloudDeployConfig &config, QString &errorOut);

    static void removeNoCloudIso(const QDir &vmDir);
    static bool hasNoCloudIso(const QDir &vmDir);
    static QString noCloudIsoPath(const QDir &vmDir);

private:
    CloudImageDeployer() = delete;

    static bool
    prepareImage(const QString &sourceImage, const QDir &vmDir, int diskSizeGb, QString &diskPathOut, QString &errorOut, const ProgressReport &progress);

    static QByteArray generateUserData(const CloudDeployConfig &config, QString &errorOut);
    static QByteArray generateMetaData(const CloudDeployConfig &config);
    static QByteArray generateNetworkConfig(const CloudDeployConfig &config, QString &errorOut);

    static bool createNoCloudIso(const QDir &vmDir, const QByteArray &userData, const QByteArray &metaData, const QByteArray &networkConfig, QString &errorOut);

};
