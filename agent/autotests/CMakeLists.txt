# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

include(ECMAddTests)

# The names are prefixed because KEMU's own tests live in the same build tree
# with BUILD_TESTING (clipboardfilestest exists on both sides).

ecm_add_tests(
    agentclipboardchoicetest.cpp
    agentclipboardfilestest.cpp
    agentframetest.cpp
    agentsessiontest.cpp
    agentclipboardsynctest.cpp
    agentlayouttest.cpp
    agentmimerulestest.cpp
    agentportconnectiontest.cpp
    LINK_LIBRARIES kemuagent_core Qt6::Test)
