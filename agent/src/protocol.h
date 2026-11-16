/*
 * SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QLatin1StringView>
#include <QtGlobal>

using namespace Qt::StringLiterals;

// Constants of the KEMU agent protocol, version 1 (docs/kemu-agent-protocol.md).
namespace Protocol
{
inline constexpr int Version = 1;

inline constexpr char Magic[4] = {'K', 'E', 'M', 'U'};
inline constexpr qsizetype HeaderSize = 12;
inline constexpr quint32 MaxBodySize = 1024 * 1024; // section 4.1
inline constexpr quint32 MaxHandshakeBodySize = 4096; // section 5.3
inline constexpr int MaxNesting = 8; // section 4.2
inline constexpr qsizetype MaxTextSize = 4096; // section 4.2
inline constexpr qsizetype MaxMimeTypeSize = 255; // section 6.6
inline constexpr int MaxOfferTypes = 64; // section 6.3
inline constexpr int MaxOutputs = 16; // section 7.1
inline constexpr qsizetype MaxChunkSize = 256 * 1024; // section 6.5
inline constexpr qsizetype NonceSize = 16;
inline constexpr quint64 DefaultMaxTransfer = 64ull * 1024 * 1024;

inline constexpr auto PortName = "org.kde.kemu.agent.0"_L1;

enum MessageType : quint16 {
    Hello = 0x0001,
    Welcome = 0x0002,
    HelloRequest = 0x0003,
    Ping = 0x0004,
    Pong = 0x0005,
    Goodbye = 0x0006,
    Error = 0x0007,
    Policy = 0x0010,

    ClipboardOffer = 0x0100,
    ClipboardRelease = 0x0101,
    ClipboardRequest = 0x0102,
    ClipboardData = 0x0103,
    ClipboardFail = 0x0104,
    ClipboardCancel = 0x0105,
    ClipboardFileList = 0x0110,
    ClipboardFileEntries = 0x0111,
    ClipboardFileRead = 0x0112,
    ClipboardFileData = 0x0113,

    DisplayLayout = 0x0200,
    DisplayLayoutQuery = 0x0201,
};

namespace Capability
{
inline constexpr auto Clipboard = "clipboard"_L1;
inline constexpr auto ClipboardPrimary = "clipboard.primary"_L1;
inline constexpr auto ClipboardFiles = "clipboard.files"_L1;
inline constexpr auto DisplayLayout = "display.layout"_L1;
}

// Selection values on the wire (section 6.1).
enum class Selection : quint8 {
    Clipboard = 0,
    Primary = 1,
};
inline constexpr int SelectionCount = 2;

// CLIPBOARD_FAIL reasons and ERROR codes (appendix C).
namespace Reason
{
inline constexpr auto Stale = "stale"_L1;
inline constexpr auto NotOffered = "not-offered"_L1;
inline constexpr auto TooLarge = "too-large"_L1;
inline constexpr auto Unavailable = "unavailable"_L1;
inline constexpr auto Denied = "denied"_L1;
inline constexpr auto Busy = "busy"_L1;
inline constexpr auto Changed = "changed"_L1;

inline constexpr auto UnsupportedVersion = "unsupported-version"_L1;
inline constexpr auto BadMessage = "bad-message"_L1;
inline constexpr auto Internal = "internal"_L1;
}

QLatin1StringView messageName(quint16 type);
}
