/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ovfparser.h"

#include <QFile>
#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QXmlStreamReader>

#include <algorithm>
#include <limits>

using namespace Qt::StringLiterals;

namespace
{

constexpr qint64 kMaxBytes = std::numeric_limits<qint64>::max();

const QString kOvf1Ns = u"http://schemas.dmtf.org/ovf/envelope/1"_s;
const QString kOvf2Ns = u"http://schemas.dmtf.org/ovf/envelope/2"_s;

// Namespace-aware lookup with fallbacks for descriptors that bind the OVF
// namespace to the "ovf" prefix only loosely or not at all.
QString ovfAttr(const QXmlStreamReader &xml, const QString &name)
{
    const QXmlStreamAttributes attrs = xml.attributes();
    for (const QString &ns : {kOvf1Ns, kOvf2Ns}) {
        if (attrs.hasAttribute(ns, name))
            return attrs.value(ns, name).toString();
    }
    if (attrs.hasAttribute(u"ovf:"_s + name))
        return attrs.value(u"ovf:"_s + name).toString();
    return attrs.value(name).toString();
}

// Collects the children of one hardware item so that element order does not
// matter. The CIM schema sorts them alphabetically, which puts
// AllocationUnits before ResourceType.
QHash<QString, QString> readItem(QXmlStreamReader &xml)
{
    const QString element = xml.name().toString();
    QHash<QString, QString> fields;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isEndElement() && xml.name() == element)
            break;
        if (xml.isStartElement())
            fields.insert(xml.name().toString(), xml.readElementText().trimmed());
    }

    return fields;
}

QString normalizeMac(QString mac)
{
    static const QRegularExpression hexRx(u"^[0-9a-fA-F]{12}$"_s);

    mac.remove(u':');
    mac.remove(u'-');
    if (!hexRx.match(mac).hasMatch())
        return {};

    QStringList parts;
    for (int i = 0; i < 12; i += 2)
        parts << mac.mid(i, 2);
    return parts.join(u':').toLower();
}

// base^exponent, or 0 on overflow.
qint64 checkedPower(qint64 base, int exponent)
{
    qint64 result = 1;
    for (int i = 0; i < exponent; ++i) {
        if (result > kMaxBytes / base)
            return 0;
        result *= base;
    }
    return result;
}

qint64 unitBytes(const QString &units, qint64 defaultUnitBytes)
{
    static const QRegularExpression programmaticRx(uR"(^byte\s*(?:\*\s*(\d+)\s*(?:\^\s*(\d+))?)?$)"_s, QRegularExpression::CaseInsensitiveOption);

    const QString u = units.trimmed();
    if (u.isEmpty())
        return defaultUnitBytes;

    const QRegularExpressionMatch m = programmaticRx.match(u);
    if (m.hasMatch()) {
        if (!m.hasCaptured(1))
            return 1;
        const qint64 base = m.captured(1).toLongLong();
        const int exponent = m.hasCaptured(2) ? m.captured(2).toInt() : 1;
        return base > 0 && exponent <= 64 ? checkedPower(base, exponent) : 0;
    }

    // Textual units are binary in practice: VirtualBox writes "MegaBytes" for
    // MiB, and "GB" in hand-written descriptors means GiB.
    const QString lower = u.toLower();
    if (lower == u"bytes"_s)
        return 1;
    if (lower.startsWith(u"kilo"_s) || lower == u"kb"_s || lower == u"kib"_s)
        return 1LL << 10;
    if (lower.startsWith(u"mega"_s) || lower == u"mb"_s || lower == u"mib"_s)
        return 1LL << 20;
    if (lower.startsWith(u"giga"_s) || lower == u"gb"_s || lower == u"gib"_s)
        return 1LL << 30;
    if (lower.startsWith(u"tera"_s) || lower == u"tb"_s || lower == u"tib"_s)
        return 1LL << 40;
    return defaultUnitBytes;
}

struct FileRef {
    QString href;
    QString compression;
    qint64 chunkSize = 0;
};

struct DiskRef {
    QString diskId;
    QString fileRef;
    qint64 capacityBytes = 0;
};

struct Attachment {
    QString hostResource;
    QString parent;
    int address = std::numeric_limits<int>::max();
};
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
    QHash<QString, FileRef> files;
    QStringList fileOrder;
    QList<DiskRef> diskSection;
    QStringList controllers; // InstanceIDs in document order
    QList<Attachment> attachments;

    // Only the first VirtualSystem counts; a VirtualSystemCollection would
    // otherwise mix the hardware of all its machines.
    QStringList stack;
    qsizetype systemDepth = -1;

    while (!xml.atEnd()) {
        xml.readNext();

        if (xml.isEndElement()) {
            if (stack.size() == systemDepth + 1)
                systemDepth = -2; // first system done
            stack.removeLast();
            continue;
        }
        if (!xml.isStartElement())
            continue;

        const QString element = xml.name().toString();
        const bool inSystem = systemDepth >= 0 && stack.size() > systemDepth;

        if (element == u"VirtualSystem") {
            ++result.virtualSystemCount;
            if (systemDepth == -1) {
                systemDepth = stack.size();
                systemId = ovfAttr(xml, u"id"_s);
            }

        } else if (element == u"Name" && inSystem && stack.size() == systemDepth + 1) {
            const QString text = xml.readElementText().simplified();
            if (!text.isEmpty())
                result.name = text;
            continue; // readElementText() consumed the end element

        } else if (element == u"File") {
            const QString id = ovfAttr(xml, u"id"_s);
            const QString href = ovfAttr(xml, u"href"_s);
            if (!href.isEmpty() && !files.contains(id)) {
                files.insert(id, {href, ovfAttr(xml, u"compression"_s), ovfAttr(xml, u"chunkSize"_s).toLongLong()});
                fileOrder << id;
            }

        } else if (element == u"Disk") {
            const qint64 capacity = ovfAttr(xml, u"capacity"_s).trimmed().toLongLong();
            diskSection.append(
                {ovfAttr(xml, u"diskId"_s), ovfAttr(xml, u"fileRef"_s), quantityToBytes(capacity, ovfAttr(xml, u"capacityAllocationUnits"_s), 1)});

        } else if (inSystem && (element == u"Item" || element == u"StorageItem" || element == u"EthernetPortItem")) {
            const QHash<QString, QString> item = readItem(xml);

            switch (item.value(u"ResourceType"_s).toInt()) {
            case 3:
                result.cpuCount = item.value(u"VirtualQuantity"_s).toInt();
                break;

            case 4:
                // Default to MiB, the common OVF case, when units are absent.
                result.memoryBytes = quantityToBytes(item.value(u"VirtualQuantity"_s).toLongLong(), item.value(u"AllocationUnits"_s), 1LL << 20);
                break;

            case 5: // IDE
            case 6: // SCSI
            case 20: // other storage: SATA, NVMe
                controllers << item.value(u"InstanceID"_s);
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

            case 17: { // disk drive
                bool ok = false;
                const int address = item.value(u"AddressOnParent"_s).toInt(&ok);
                attachments.append({item.value(u"HostResource"_s), item.value(u"Parent"_s), ok ? address : std::numeric_limits<int>::max()});
                break;
            }

            default:
                break;
            }
            continue; // readItem() consumed the end element
        }

        stack << element;
    }

    if (xml.hasError())
        return std::nullopt;

    if (result.name.isEmpty())
        result.name = systemId.simplified();

    const auto diskFor = [&files](const DiskRef &ref) -> std::optional<OvfDisk> {
        if (ref.fileRef.isEmpty())
            return OvfDisk{{}, ref.capacityBytes, {}, 0};
        const auto it = files.constFind(ref.fileRef);
        if (it == files.constEnd())
            return std::nullopt;
        return OvfDisk{it->href, ref.capacityBytes, it->compression, it->chunkSize};
    };

    // There is no boot order in OVF 1.x: the first disk on the first
    // controller is what a BIOS boots from.
    std::stable_sort(attachments.begin(), attachments.end(), [&controllers](const Attachment &a, const Attachment &b) {
        const auto rank = [&controllers](const Attachment &x) {
            const qsizetype i = controllers.indexOf(x.parent);
            return i < 0 ? controllers.size() : i;
        };
        return std::pair(rank(a), a.address) < std::pair(rank(b), b.address);
    });

    static const QRegularExpression hostResourceRx(u"^(?:ovf:)?/(disk|file)/(.+)$"_s);
    QStringList attached;
    for (const Attachment &a : std::as_const(attachments)) {
        const QRegularExpressionMatch m = hostResourceRx.match(a.hostResource);
        if (!m.hasMatch() || attached.contains(a.hostResource))
            continue;
        attached << a.hostResource;

        if (m.captured(1) == u"file"_s) {
            const auto it = files.constFind(m.captured(2));
            if (it != files.constEnd())
                result.disks.append({it->href, 0, it->compression, it->chunkSize});
            continue;
        }
        for (const DiskRef &ref : std::as_const(diskSection)) {
            if (ref.diskId == m.captured(2)) {
                if (const auto disk = diskFor(ref))
                    result.disks.append(*disk);
                break;
            }
        }
    }

    // Descriptors without disk items: fall back to the DiskSection.
    if (attachments.isEmpty()) {
        for (const DiskRef &ref : std::as_const(diskSection)) {
            if (const auto disk = diskFor(ref))
                result.disks.append(*disk);
        }
    }

    // No DiskSection at all: the first referenced file that is not an
    // installation medium or an NVRAM blob.
    if (diskSection.isEmpty() && result.disks.isEmpty()) {
        for (const QString &id : std::as_const(fileOrder)) {
            const FileRef &file = files[id];
            if (!file.href.endsWith(u".iso"_s, Qt::CaseInsensitive) && !file.href.endsWith(u".nvram"_s, Qt::CaseInsensitive)) {
                result.disks.append({file.href, 0, file.compression, file.chunkSize});
                break;
            }
        }
    }

    return result;
}

qint64 OvfParser::quantityToBytes(qint64 quantity, const QString &units, qint64 defaultUnitBytes)
{
    if (quantity <= 0)
        return 0;

    const qint64 unit = unitBytes(units, defaultUnitBytes);
    if (unit <= 0 || quantity > kMaxBytes / unit)
        return 0;
    return quantity * unit;
}
