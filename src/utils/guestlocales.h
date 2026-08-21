/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QLocale>
#include <QString>
#include <QTimeZone>

#include <KLocalizedString>

#include "utils/utils.h"

using namespace Qt::StringLiterals;

struct LocaleInfo {
    QString code; // e.g. "de_DE.UTF-8"
    QString displayName; // i18n()-translated
};

struct KeyboardInfo {
    QString layout; // e.g. "de"
    QString variant; // e.g. "nodeadkeys"
    QString displayName; // i18n()-translated
};

inline QList<LocaleInfo> availableLocales()
{
    return {
        {u"en_US.UTF-8"_s, i18n("English (United States)")},
        {u"en_GB.UTF-8"_s, i18n("English (United Kingdom)")},
        {u"de_DE.UTF-8"_s, i18n("German (Germany)")},
        {u"de_AT.UTF-8"_s, i18n("German (Austria)")},
        {u"de_CH.UTF-8"_s, i18n("German (Switzerland)")},
        {u"fr_FR.UTF-8"_s, i18n("French (France)")},
        {u"fr_CH.UTF-8"_s, i18n("French (Switzerland)")},
        {u"fr_BE.UTF-8"_s, i18n("French (Belgium)")},
        {u"es_ES.UTF-8"_s, i18n("Spanish (Spain)")},
        {u"es_MX.UTF-8"_s, i18n("Spanish (Mexico)")},
        {u"it_IT.UTF-8"_s, i18n("Italian (Italy)")},
        {u"pt_BR.UTF-8"_s, i18n("Portuguese (Brazil)")},
        {u"pt_PT.UTF-8"_s, i18n("Portuguese (Portugal)")},
        {u"ja_JP.UTF-8"_s, i18n("Japanese (Japan)")},
        {u"ko_KR.UTF-8"_s, i18n("Korean (Korea)")},
        {u"zh_CN.UTF-8"_s, i18n("Chinese Simplified (China)")},
        {u"zh_TW.UTF-8"_s, i18n("Chinese Traditional (Taiwan)")},
        {u"ru_RU.UTF-8"_s, i18n("Russian (Russia)")},
        {u"pl_PL.UTF-8"_s, i18n("Polish (Poland)")},
        {u"nl_NL.UTF-8"_s, i18n("Dutch (Netherlands)")},
        {u"nl_BE.UTF-8"_s, i18n("Dutch (Belgium)")},
        {u"sv_SE.UTF-8"_s, i18n("Swedish (Sweden)")},
        {u"nb_NO.UTF-8"_s, i18n("Norwegian Bokmål (Norway)")},
        {u"fi_FI.UTF-8"_s, i18n("Finnish (Finland)")},
        {u"da_DK.UTF-8"_s, i18n("Danish (Denmark)")},
        {u"cs_CZ.UTF-8"_s, i18n("Czech (Czechia)")},
        {u"hu_HU.UTF-8"_s, i18n("Hungarian (Hungary)")},
        {u"ro_RO.UTF-8"_s, i18n("Romanian (Romania)")},
        {u"tr_TR.UTF-8"_s, i18n("Turkish (Türkiye)")},
        {u"el_GR.UTF-8"_s, i18n("Greek (Greece)")},
        {u"uk_UA.UTF-8"_s, i18n("Ukrainian (Ukraine)")},
        {u"ar_SA.UTF-8"_s, i18n("Arabic (Saudi Arabia)")},
        {u"he_IL.UTF-8"_s, i18n("Hebrew (Israel)")},
        {u"hi_IN.UTF-8"_s, i18n("Hindi (India)")},
        {u"th_TH.UTF-8"_s, i18n("Thai (Thailand)")},
    };
}

inline QList<KeyboardInfo> availableKeyboards()
{
    return {
        {u"us"_s, {}, i18n("English (US)")},
        {u"gb"_s, {}, i18n("English (UK)")},
        {u"de"_s, {}, i18n("German")},
        {u"de"_s, u"nodeadkeys"_s, i18n("German (no dead keys)")},
        {u"at"_s, {}, i18n("Austrian")},
        {u"ch"_s, u"de"_s, i18n("Swiss German")},
        {u"ch"_s, u"fr"_s, i18n("Swiss French")},
        {u"fr"_s, {}, i18n("French")},
        {u"be"_s, {}, i18n("Belgian")},
        {u"es"_s, {}, i18n("Spanish")},
        {u"latam"_s, {}, i18n("Latin American")},
        {u"it"_s, {}, i18n("Italian")},
        {u"br"_s, {}, i18n("Portuguese (Brazil)")},
        {u"pt"_s, {}, i18n("Portuguese")},
        {u"jp"_s, {}, i18n("Japanese")},
        {u"kr"_s, {}, i18n("Korean")},
        {u"cn"_s, {}, i18n("Chinese")},
        {u"tw"_s, {}, i18n("Chinese (Taiwan)")},
        {u"ru"_s, {}, i18n("Russian")},
        {u"pl"_s, {}, i18n("Polish")},
        {u"nl"_s, {}, i18n("Dutch")},
        {u"se"_s, {}, i18n("Swedish")},
        {u"no"_s, {}, i18n("Norwegian")},
        {u"fi"_s, {}, i18n("Finnish")},
        {u"dk"_s, {}, i18n("Danish")},
        {u"cz"_s, {}, i18n("Czech")},
        {u"hu"_s, {}, i18n("Hungarian")},
        {u"ro"_s, {}, i18n("Romanian")},
        {u"tr"_s, {}, i18n("Turkish")},
        {u"gr"_s, {}, i18n("Greek")},
        {u"ua"_s, {}, i18n("Ukrainian")},
        {u"ara"_s, {}, i18n("Arabic")},
        {u"il"_s, {}, i18n("Hebrew")},
        {u"in"_s, {}, i18n("Hindi")},
        {u"th"_s, {}, i18n("Thai")},
    };
}

inline QString defaultTimezone()
{
    return QString::fromUtf8(QTimeZone::systemTimeZoneId());
}

inline QString defaultLocaleCode()
{
    return QLocale::system().name() + u".UTF-8"_s;
}

// Index into availableKeyboards(), or -1 when the host layout is unknown.
inline int defaultKeyboardIndex()
{
    const QString hostLayout = Utils::detectHostKeyboardLayout();
    const auto keyboards = availableKeyboards();
    for (int i = 0; i < keyboards.size(); ++i) {
        if (keyboards[i].layout == hostLayout && keyboards[i].variant.isEmpty())
            return i;
    }
    return -1;
}
