/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QString>

#include "utils/templatestore.h"

using namespace Qt::StringLiterals;

namespace CloudInitTemplates
{

// User templates first, then downloaded, then shipped ones; see TemplateStore.
inline QString locateTemplate(const QString &templateId)
{
    return TemplateStore().locateCloudInit(templateId);
}

inline QString loadTemplate(const QString &templateId)
{
    QString path = locateTemplate(templateId);
    if (path.isEmpty() && templateId != u"default"_s)
        path = locateTemplate(u"default"_s);

    if (path.isEmpty())
        return {};

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    return QString::fromUtf8(f.readAll());
}

} // namespace CloudInitTemplates
