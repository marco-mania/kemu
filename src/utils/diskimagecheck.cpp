/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "diskimagecheck.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QtEndian>

#include <KLocalizedString>
#include <KProcess>

using namespace Qt::StringLiterals;

namespace DiskImageCheck
{

namespace
{
constexpr quint32 kQcow2Magic = 0x514649fb; // "QFI\xfb"
constexpr quint64 kQcow2IncompatDataFile = 1ULL << 2;
constexpr int kQcow2V2HeaderSize = 72;
constexpr int kQcow2V3HeaderSize = 104;
constexpr int kQcow2CryptMethodOffset = 32;
constexpr quint32 kQcow2CryptAes = 1;
constexpr quint32 kQcow2CryptLuks = 2;

quint32 cryptMethod(const QByteArray &header)
{
    return header.size() >= kQcow2CryptMethodOffset + 4 ? qFromBigEndian<quint32>(header.constData() + kQcow2CryptMethodOffset) : 0;
}
}

QString qcow2Problem(const QString &path)
{
    const QString name = QFileInfo(path).fileName();

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return i18n("%1 cannot be read.", name);

    const QByteArray header = file.read(kQcow2V3HeaderSize);
    if (header.size() < kQcow2V2HeaderSize || qFromBigEndian<quint32>(header.constData()) != kQcow2Magic)
        return i18n("%1 is not a qcow2 image.", name);

    const quint32 version = qFromBigEndian<quint32>(header.constData() + 4);
    if (version != 2 && version != 3)
        return i18n("%1 uses an unsupported qcow2 version.", name);
    if (version == 3 && header.size() < kQcow2V3HeaderSize)
        return i18n("%1 is not a qcow2 image.", name);

    if (qFromBigEndian<quint64>(header.constData() + 8) != 0)
        return i18n("%1 refers to a backing file.", name);

    if (version == 3 && (qFromBigEndian<quint64>(header.constData() + 72) & kQcow2IncompatDataFile))
        return i18n("%1 keeps its data in an external file.", name);

    // The old built-in AES encryption is broken and QEMU refuses it.
    const quint32 crypt = cryptMethod(header);
    if (crypt == kQcow2CryptAes)
        return i18n("%1 uses the outdated qcow2 AES encryption.", name);
    if (crypt != 0 && crypt != kQcow2CryptLuks)
        return i18n("%1 uses an unknown encryption.", name);

    return {};
}

bool qcow2IsEncrypted(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    return cryptMethod(file.read(kQcow2CryptMethodOffset + 4)) == kQcow2CryptLuks;
}

Inspection evaluate(const QJsonValue &info, const QString &path, const QString &allowedDir, bool allowBacking)
{
    static const QSet<QString> supportedFormats = {u"qcow2"_s, u"raw"_s, u"vmdk"_s, u"vdi"_s, u"vhdx"_s, u"vpc"_s};

    Inspection result;
    const QFileInfo image(path);
    const QString name = image.fileName();
    const QString imageCanonical = image.canonicalFilePath();
    const QString allowedCanonical = allowedDir.isEmpty() ? QString() : QFileInfo(allowedDir).canonicalFilePath();

    // A reference must name an existing local file: the image itself or one
    // inside the allowed folder. Colons would select a QEMU protocol.
    const auto isAllowed = [&](const QString &ref, const QString &baseDir) {
        if (ref.isEmpty() || ref.contains(u':'))
            return false;
        const QString canonical = QFileInfo(QDir(baseDir), ref).canonicalFilePath();
        if (canonical.isEmpty())
            return false;
        if (canonical == imageCanonical)
            return true;
        return !allowedCanonical.isEmpty() && canonical.startsWith(allowedCanonical + u'/');
    };

    const QJsonArray chain = info.isArray() ? info.toArray() : QJsonArray{info};
    if (chain.isEmpty() || !chain.first().isObject()) {
        result.problem = i18n("%1 is not a readable disk image.", name);
        return result;
    }

    for (qsizetype i = 0; i < chain.size(); ++i) {
        const QJsonObject obj = chain.at(i).toObject();
        const QString format = obj.value(u"format"_s).toString();
        if (i == 0)
            result.format = format;

        if (!supportedFormats.contains(format)) {
            result.problem = i18n("%1 uses the unsupported image format '%2'.", name, format);
            return result;
        }

        const QString fileName = obj.value(u"filename"_s).toString();
        if (i > 0 && !isAllowed(fileName, image.absolutePath())) {
            result.problem = i18n("%1 refers to %2, which is outside its folder.", name, fileName);
            return result;
        }
        const QString baseDir = i == 0 ? image.absolutePath() : QFileInfo(fileName).absolutePath();

        const QString backing = obj.value(u"backing-filename"_s).toString();
        if (!backing.isEmpty()) {
            if (!allowBacking) {
                result.problem = i18n("%1 refers to a backing file (%2).", name, backing);
                return result;
            }
            if (!isAllowed(backing, baseDir)) {
                result.problem = i18n("%1 refers to %2, which is outside its folder.", name, backing);
                return result;
            }
        }

        const QJsonObject data = obj.value(u"format-specific"_s).toObject().value(u"data"_s).toObject();

        const QString dataFile = data.value(u"data-file"_s).toString();
        if (!dataFile.isEmpty() && !isAllowed(dataFile, baseDir)) {
            result.problem = i18n("%1 keeps its data in another file (%2).", name, dataFile);
            return result;
        }

        for (const QJsonValue &extent : data.value(u"extents"_s).toArray()) {
            const QString extentFile = extent.toObject().value(u"filename"_s).toString();
            if (!isAllowed(extentFile, baseDir)) {
                result.problem = i18n("%1 refers to %2, which is outside its folder.", name, extentFile);
                return result;
            }
        }
    }

    return result;
}

Inspection inspect(const QString &qemuImg, const QString &path, const QString &allowedDir, bool allowBacking, int timeoutMs)
{
    Inspection result;
    const QString name = QFileInfo(path).fileName();

    if (qemuImg.isEmpty()) {
        result.problem = i18n("qemu-img not found. Please check Settings.");
        return result;
    }

    QStringList args{u"info"_s, u"--output=json"_s};
    if (allowBacking)
        args << u"--backing-chain"_s;
    args << path;

    KProcess process;
    process.setOutputChannelMode(KProcess::SeparateChannels);
    process.setProgram(qemuImg, args);
    process.start();

    if (!process.waitForFinished(timeoutMs) || process.exitStatus() != KProcess::NormalExit || process.exitCode() != 0) {
        process.kill();
        const QString stderrText = QString::fromUtf8(process.readAllStandardError()).trimmed();
        result.problem = stderrText.isEmpty() ? i18n("%1 is not a readable disk image.", name) : i18n("%1 is not a readable disk image: %2", name, stderrText);
        return result;
    }

    const QJsonDocument doc = QJsonDocument::fromJson(process.readAllStandardOutput());
    return evaluate(doc.isArray() ? QJsonValue(doc.array()) : QJsonValue(doc.object()), path, allowedDir, allowBacking);
}

}
