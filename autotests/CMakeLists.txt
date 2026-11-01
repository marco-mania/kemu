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

ecm_add_test(controlconnectiontest.cpp
             ${CMAKE_SOURCE_DIR}/src/dbusdisplay/controlconnection.cpp
             ${CMAKE_SOURCE_DIR}/src/dbusdisplay/glibeventbridge.cpp
             TEST_NAME controlconnectiontest
             LINK_LIBRARIES Qt6::Core Qt6::Test PkgConfig::GIO)
target_include_directories(controlconnectiontest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
ecm_qt_declare_logging_category(controlconnectiontest
                                HEADER "kemu_dbusdisplay_debug.h"
                                IDENTIFIER "KEMU_DBUSDISPLAY"
                                CATEGORY_NAME "org.kde.kemu.dbusdisplay")

ecm_add_test(qemucommandbuildertest.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/qemucommandbuilder.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/vmconfig.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/externaltools.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/qemucapabilities.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/sandboxcapabilities.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/sessionguard.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/diskencryption.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/tpmstate.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/3rdparty/crypt_sha512.c
             TEST_NAME qemucommandbuildertest
             LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::DBus Qt6::Test
                            KF6::CoreAddons KF6::I18n KF6::ConfigCore)
target_include_directories(qemucommandbuildertest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
ecm_qt_declare_logging_category(qemucommandbuildertest
                                HEADER "kemu_debug.h"
                                IDENTIFIER "KEMU"
                                CATEGORY_NAME "org.kde.kemu")

ecm_add_test(framecapturetest.cpp
             ${CMAKE_SOURCE_DIR}/src/dbusdisplay/framecapture.cpp
             TEST_NAME framecapturetest
             LINK_LIBRARIES Qt6::Gui Qt6::OpenGL Qt6::Test)
target_include_directories(framecapturetest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# VM start orchestration and process exit decisions, without launching QEMU.
ecm_add_test(startupchaintest.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/startupchain.cpp
             TEST_NAME startupchaintest
             LINK_LIBRARIES Qt6::Core Qt6::Test)
target_include_directories(startupchaintest PRIVATE ${CMAKE_SOURCE_DIR}/src)

ecm_add_test(vmexitoutcometest.cpp
             TEST_NAME vmexitoutcometest
             LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Network Qt6::Test KF6::CoreAddons KF6::I18n)
target_include_directories(vmexitoutcometest PRIVATE ${CMAKE_SOURCE_DIR}/src)

# Stand-in helpers exercise the real KProcess/socket-readiness lifecycle.
ecm_add_test(asynchelperprocesstest.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/asynchelperprocess.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/utils.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/diskencryption.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/3rdparty/crypt_sha512.c
             TEST_NAME asynchelperprocesstest
             LINK_LIBRARIES Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Network Qt6::Test KF6::CoreAddons KF6::I18n KF6::ConfigCore)
target_include_directories(asynchelperprocesstest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
ecm_qt_declare_logging_category(asynchelperprocesstest
                                HEADER "kemu_debug.h"
                                IDENTIFIER "KEMU"
                                CATEGORY_NAME "org.kde.kemu")

# In-memory Ethernet framing, MAC binding, and isolation between networks.
ecm_add_test(virtualswitchtest.cpp
             ${CMAKE_SOURCE_DIR}/src/utils/virtualswitch.cpp
             TEST_NAME virtualswitchtest
             LINK_LIBRARIES Qt6::Core Qt6::Network Qt6::Test)
target_include_directories(virtualswitchtest PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
ecm_qt_declare_logging_category(virtualswitchtest
                                HEADER "kemu_debug.h"
                                IDENTIFIER "KEMU"
                                CATEGORY_NAME "org.kde.kemu")
