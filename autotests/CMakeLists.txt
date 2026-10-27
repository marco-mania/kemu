# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

include(ECMAddTests)

ecm_add_test(ovfparsertest.cpp ${CMAKE_SOURCE_DIR}/src/utils/ovfparser.cpp
             TEST_NAME ovfparsertest
             LINK_LIBRARIES Qt6::Core Qt6::Test)
target_include_directories(ovfparsertest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(templatestoretest.cpp ${CMAKE_SOURCE_DIR}/src/utils/templatestore.cpp
             TEST_NAME templatestoretest
             LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n)
target_include_directories(templatestoretest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(templatepackagetest.cpp ${CMAKE_SOURCE_DIR}/src/utils/templatepackage.cpp ${CMAKE_SOURCE_DIR}/src/utils/templatestore.cpp
             TEST_NAME templatepackagetest
             LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n LibArchive::LibArchive)
target_include_directories(templatepackagetest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(linkedclonestest.cpp ${CMAKE_SOURCE_DIR}/src/utils/linkedclones.cpp
             TEST_NAME linkedclonestest
             LINK_LIBRARIES Qt6::Core Qt6::Test)
target_include_directories(linkedclonestest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(diskimagechecktest.cpp ${CMAKE_SOURCE_DIR}/src/utils/diskimagecheck.cpp
             TEST_NAME diskimagechecktest
             LINK_LIBRARIES Qt6::Core Qt6::Test KF6::I18n KF6::CoreAddons)
target_include_directories(diskimagechecktest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(diskencryptiontest.cpp ${CMAKE_SOURCE_DIR}/src/utils/diskencryption.cpp
             TEST_NAME diskencryptiontest
             LINK_LIBRARIES Qt6::Core Qt6::Test)
target_include_directories(diskencryptiontest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(tpmstatetest.cpp ${CMAKE_SOURCE_DIR}/src/utils/tpmstate.cpp ${CMAKE_SOURCE_DIR}/src/utils/diskencryption.cpp
             TEST_NAME tpmstatetest
             LINK_LIBRARIES Qt6::Core Qt6::Test)
target_include_directories(tpmstatetest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# D-Bus display protocol tests use a local peer connection and no QEMU process.
ecm_add_test(dbusdisplaytest.cpp
             ${CMAKE_SOURCE_DIR}/src/dbusdisplay/listener.cpp
             ${CMAKE_SOURCE_DIR}/src/dbusdisplay/glibeventbridge.cpp
             TEST_NAME dbusdisplaytest
             LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Test PkgConfig::GIO)
target_include_directories(dbusdisplaytest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
ecm_qt_declare_logging_category(dbusdisplaytest
                                HEADER "kemu_dbusdisplay_debug.h"
                                IDENTIFIER "KEMU_DBUSDISPLAY"
                                CATEGORY_NAME "org.kde.kemu.dbusdisplay")
