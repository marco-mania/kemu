/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QFile>
#include <QStandardPaths>
#include <QString>

#include "const.h"

using namespace Qt::StringLiterals;

namespace CloudInitTemplates
{

inline QString templateDir()
{
    return KEMUConst::CloudInitTemplateDirPath;
}

inline QString locateTemplate(const QString &templateId)
{
    const QString fileName = templateId + u".yaml.tpl"_s;
    return QStandardPaths::locate(QStandardPaths::GenericDataLocation, templateDir() + u"/"_s + fileName);
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
