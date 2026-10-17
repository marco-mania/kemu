# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

include(ECMAddTests)

ecm_add_test(ovfparsertest.cpp ${CMAKE_SOURCE_DIR}/src/utils/ovfparser.cpp
             TEST_NAME ovfparsertest
             LINK_LIBRARIES Qt6::Core Qt6::Test)
target_include_directories(ovfparsertest PRIVATE ${CMAKE_SOURCE_DIR}/src)
