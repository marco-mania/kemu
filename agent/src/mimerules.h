/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QByteArray>
#include <QLatin1StringView>
#include <QStringList>

using namespace Qt::StringLiterals;

// Which clipboard types cross the port, and how (section 6.6).
namespace MimeRules
{
inline constexpr auto Utf8Text = "text/plain;charset=utf-8"_L1;
inline constexpr auto PlainText = "text/plain"_L1;
inline constexpr auto Png = "image/png"_L1;
inline constexpr auto UriList = "text/uri-list"_L1;
inline constexpr auto PasswordHint = "x-kde-passwordManagerHint"_L1;
// Marks what this agent put on the guest clipboard for the host (private).
inline constexpr auto ProxyMarker = "application/x-kemu-agent-proxy"_L1;

// A MIME type as the protocol carries it: printable ASCII, at most 255
// bytes, with a "/" - or KDE's password manager hint.
bool isWellFormed(const QString &type);
// Well formed, not private to an implementation, not a local file
// operation.
bool isShareable(const QString &type);
bool isImage(const QString &type);

// The types to offer for what the guest clipboard holds, in the source's
// order. UTF-8 text and PNG are added when the source has text or an image
// in other forms; the total stays within the protocol's limit.
QStringList outgoingTypes(const QStringList &formats, bool hasText, bool hasImage);

// text/uri-list without file: URIs, which mean nothing on the other side.
QByteArray stripFileUris(const QByteArray &uriList);
}
