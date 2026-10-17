/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovfparser.h"

#include "utils/utils.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

using namespace Qt::StringLiterals;

namespace
{

const QString kOvfNs = u"http://schemas.dmtf.org/ovf/envelope/1"_s;

// Namespace-aware lookup with a fallback for descriptors that declare no
// namespace at all.
QString ovfAttr(const QXmlStreamReader &xml, const QString &name)
{
    const QXmlStreamAttributes attrs = xml.attributes();
    if (attrs.hasAttribute(kOvfNs, name))
        return attrs.value(kOvfNs, name).toString();
    return attrs.value(u"ovf:"_s + name).toString();
}

// Collects the children of one <Item> so that element order does not matter.
// The CIM schema sorts them alphabetically, which puts AllocationUnits before
// ResourceType.
QHash<QString, QString> readItem(QXmlStreamReader &xml)
{
    QHash<QString, QString> fields;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == u"Item")
            break;
        if (xml.isStartElement())
            fields.insert(xml.name().toString(), xml.readElementText());
    }

    return fields;
}

QString normalizeMac(QString mac)
{
    mac.remove(u':');
    mac.remove(u'-');
    if (mac.length() != 12)
        return {};

    QStringList parts;
    for (int i = 0; i < 12; i += 2)
        parts << mac.mid(i, 2);
    const QString normalized = parts.join(u':').toLower();
    return Utils::isValidMacAddress(normalized) ? normalized : QString();
}

}

std::optional<OvfDescriptor> OvfParser::parseFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return std::nullopt;
    return parse(&file);
}

std::optional<OvfDescriptor> OvfParser::parse(QIODevice *device)
{
    QXmlStreamReader xml(device);

    OvfDescriptor result;
    QString systemId;
    QList<std::pair<QString, QString>> files; // ovf:id, ovf:href in document order
    QList<std::pair<QString, qint64>> diskRefs; // ovf:fileRef, capacity

    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement())
            continue;

        const QStringView element = xml.name();

        if (element == u"VirtualSystem") {
            if (systemId.isEmpty())
                systemId = ovfAttr(xml, u"id"_s);

        } else if (element == u"Name") {
            const QString text = xml.readElementText();
            if (!text.isEmpty())
                result.name = text;

        } else if (element == u"File") {
            const QString href = ovfAttr(xml, u"href"_s);
            if (!href.isEmpty())
                files.append({ovfAttr(xml, u"id"_s), href});

        } else if (element == u"Disk") {
            const qint64 capacity = ovfAttr(xml, u"capacity"_s).toLongLong();
            diskRefs.append({ovfAttr(xml, u"fileRef"_s), quantityToBytes(capacity, ovfAttr(xml, u"capacityAllocationUnits"_s), 1)});

        } else if (element == u"Item") {
            const QHash<QString, QString> item = readItem(xml);

            switch (item.value(u"ResourceType"_s).toInt()) {
            case 3:
                result.cpuCount = item.value(u"VirtualQuantity"_s).toInt();
                break;

            case 4:
                // Default to MiB, the common OVF case, when units are absent.
                result.memoryBytes = quantityToBytes(item.value(u"VirtualQuantity"_s).toLongLong(), item.value(u"AllocationUnits"_s), 1024LL * 1024);
                break;

            case 10: {
                // OVF carries the MAC in rasd:Address; rasd:MACAddress is a
                // non-standard extension some tools emit.
                QString raw = item.value(u"Address"_s);
                if (raw.isEmpty())
                    raw = item.value(u"MACAddress"_s);
                if (result.macAddress.isEmpty())
                    result.macAddress = normalizeMac(raw);
                break;
            }

            default:
                break;
            }
        }
    }

    if (xml.hasError())
        return std::nullopt;

    if (result.name.isEmpty())
        result.name = systemId;

    // Disks come from the DiskSection: taking the File references directly
    // would pick up ISOs and NVRAM blobs.
    for (const auto &[ref, capacity] : std::as_const(diskRefs)) {
        for (const auto &[id, href] : std::as_const(files)) {
            if (id == ref) {
                result.disks.append({href, capacity});
                break;
            }
        }
    }
    if (result.disks.isEmpty() && !files.isEmpty())
        result.disks.append({files.first().second, 0});

    return result;
}

qint64 OvfParser::quantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes)
{
    if (quantity <= 0)
        return 0;

    static const QRegularExpression powRx(uR"(byte\s*\*\s*2\^(\d+))"_s, QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch m = powRx.match(units);
    if (m.hasMatch())
        return quantity * (1LL << m.captured(1).toInt());

    const QString u = units.trimmed().toLower();
    if (u.isEmpty())
        return quantity * defaultUnitBytes;
    if (u == u"byte"_s || u == u"bytes"_s)
        return quantity;
    if (u.startsWith(u"kilo"_s))
        return quantity * 1024LL;
    if (u.startsWith(u"mega"_s))
        return quantity * 1024LL * 1024;
    if (u.startsWith(u"giga"_s))
        return quantity * 1024LL * 1024 * 1024;
    return quantity * defaultUnitBytes;
}
