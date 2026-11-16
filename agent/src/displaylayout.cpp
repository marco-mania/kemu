/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "displaylayout.h"

#include <QCborArray>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <algorithm>

#include "protocol.h"

HeadMapper::HeadMapper(const QString &sysfsDrm)
    : m_root(sysfsDrm)
{
}

QHash<QString, uint> HeadMapper::heads() const
{
    const QDir root(m_root);
    static const QRegularExpression cardPattern(u"^card(\\d+)$"_s);

    QStringList virtioCards;
    const QStringList entries = root.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::System);
    for (const QString &entry : entries) {
        if (!cardPattern.match(entry).hasMatch())
            continue;
        const QFileInfo driver(root.filePath(entry + u"/device/driver"_s));
        if (QFileInfo(driver.symLinkTarget()).fileName() == u"virtio_gpu"_s)
            virtioCards.append(entry);
    }
    // With several virtio-gpu devices, nothing tells which one QEMU means.
    if (virtioCards.size() != 1)
        return {};

    const QRegularExpression connectorPattern(u"^%1-Virtual-(\\d+)$"_s.arg(virtioCards.first()));
    QList<int> numbers;
    for (const QString &entry : entries) {
        const auto match = connectorPattern.match(entry);
        if (match.hasMatch())
            numbers.append(match.captured(1).toInt());
    }
    std::sort(numbers.begin(), numbers.end());

    QHash<QString, uint> result;
    for (qsizetype i = 0; i < numbers.size(); ++i)
        result.insert(u"Virtual-%1"_s.arg(numbers.at(i)), uint(i));
    return result;
}

QRect pointerSpace(const QList<OutputInfo> &outputs)
{
    QRect space;
    for (const OutputInfo &output : outputs) {
        if (output.enabled && output.geometry.isValid())
            space = space.united(output.geometry);
    }
    return space;
}

QCborMap layoutMessage(const QList<OutputInfo> &outputs, quint64 generation, const QString &source)
{
    const QRect space = pointerSpace(outputs);
    QCborMap body;
    body.insert("generation"_L1, qint64(generation));
    body.insert("space"_L1,
                QCborMap{{u"x"_s, space.x()}, {u"y"_s, space.y()}, {u"width"_s, std::max(space.width(), 0)}, {u"height"_s, std::max(space.height(), 0)}});

    QCborArray list;
    for (const OutputInfo &output : outputs) {
        if (list.size() >= Protocol::MaxOutputs)
            break;
        QCborMap entry;
        entry.insert("name"_L1, output.name.left(255));
        if (output.head)
            entry.insert("head"_L1, qint64(*output.head));
        entry.insert("enabled"_L1, output.enabled);
        if (output.enabled) {
            entry.insert("x"_L1, output.geometry.x());
            entry.insert("y"_L1, output.geometry.y());
            entry.insert("width"_L1, std::max(output.geometry.width(), 0));
            entry.insert("height"_L1, std::max(output.geometry.height(), 0));
        }
        if (output.modeSize.isValid()) {
            QCborMap mode{{u"width"_s, output.modeSize.width()}, {u"height"_s, output.modeSize.height()}};
            if (output.refreshMilliHz > 0)
                mode.insert("refresh"_L1, output.refreshMilliHz);
            entry.insert("mode"_L1, mode);
        }
        entry.insert("scale"_L1, output.scale);
        if (output.transform != u"normal"_s)
            entry.insert("transform"_L1, output.transform);
        if (output.primary)
            entry.insert("primary"_L1, true);
        list.append(entry);
    }
    body.insert("outputs"_L1, list);
    if (!source.isEmpty())
        body.insert("source"_L1, source);
    return body;
}
