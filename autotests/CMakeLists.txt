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
