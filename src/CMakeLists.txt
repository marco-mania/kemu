# SPDX-FileCopyrightText: 2026 Marco Nelles <marco@maniatek.de>
#
# SPDX-License-Identifier: GPL-3.0-or-later

add_executable(${PROJECT_NAME})

function(kemu_add_wayland_protocol basename xml)
  set(header
      "${CMAKE_CURRENT_BINARY_DIR}/wayland-${basename}-client-protocol.h")
  set(source "${CMAKE_CURRENT_BINARY_DIR}/wayland-${basename}-protocol.c")
  add_custom_command(
    OUTPUT ${header}
    COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
    DEPENDS ${xml}
    VERBATIM)
  add_custom_command(
    OUTPUT ${source}
    COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${source}
    DEPENDS ${xml}
    VERBATIM)
  target_sources(${PROJECT_NAME} PRIVATE ${header} ${source})
endfunction()

# linux-dmabuf moved from unstable/ to stable/ in wayland-protocols 1.33. Both
# files declare zwp_linux_dmabuf_v1, so the generated basename stays the same
# either way and the #include does not have to care.
function(kemu_find_wayland_protocol_xml outvar)
  foreach(candidate ${ARGN})
    if(EXISTS "${candidate}")
      set(${outvar}
          "${candidate}"
          PARENT_SCOPE)
      return()
    endif()
  endforeach()
  message(FATAL_ERROR "No usable protocol XML found for ${outvar}")
endfunction()

if(TARGET Qt6::GuiPrivate)
  kemu_add_wayland_protocol(
    pointer-constraints-unstable-v1
    ${WAYLAND_PROTOCOLS_DIR}/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
  )
  kemu_add_wayland_protocol(
    relative-pointer-unstable-v1
    ${WAYLAND_PROTOCOLS_DIR}/unstable/relative-pointer/relative-pointer-unstable-v1.xml
  )

  # Zero-copy display path: viewporter does the crop/scale that sub-surfaces
  # cannot inherit, linux-dmabuf hands the guest's buffer to the compositor.
  kemu_add_wayland_protocol(
    viewporter ${WAYLAND_PROTOCOLS_DIR}/stable/viewporter/viewporter.xml)

  kemu_find_wayland_protocol_xml(
    KEMU_LINUX_DMABUF_XML
    ${WAYLAND_PROTOCOLS_DIR}/stable/linux-dmabuf/linux-dmabuf-v1.xml
    ${WAYLAND_PROTOCOLS_DIR}/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml)
  kemu_add_wayland_protocol(linux-dmabuf-v1 ${KEMU_LINUX_DMABUF_XML})
endif()

ecm_qt_declare_logging_category(
  ${PROJECT_NAME}
  HEADER
  "kemu_debug.h"
  IDENTIFIER
  "KEMU"
  CATEGORY_NAME
  "org.kde.kemu"
  DESCRIPTION
  "KEMU"
  EXPORT
  KEMU)

ecm_qt_declare_logging_category(
  ${PROJECT_NAME}
  HEADER
  "kemu_dbusdisplay_debug.h"
  IDENTIFIER
  "KEMU_DBUSDISPLAY"
  CATEGORY_NAME
  "org.kde.kemu.dbusdisplay"
  DESCRIPTION
  "KEMU DBusDisplay"
  EXPORT
  KEMU)

ecm_qt_install_logging_categories(EXPORT KEMU FILE kemu.categories DESTINATION
                                  "${KDE_INSTALL_LOGGINGCATEGORIESDIR}")

target_sources(
  ${PROJECT_NAME}
  PRIVATE main.cpp
          mainwindow.cpp
          mainwindow.h
          const.h
          runtime/vmruntimeinfo.h
          runtime/vmruntimemanager.cpp
          runtime/vmruntimemanager.h
          utils/utils.cpp
          utils/utils.h
          utils/vmconfig.cpp
          utils/vmconfig.h
          utils/qmpclient.cpp
          utils/qmpclient.h
          utils/externaltools.cpp
          utils/externaltools.h
          utils/qemucommandbuilder.cpp
          utils/qemucommandbuilder.h
          utils/snapshotmanager.cpp
          utils/snapshotmanager.h
          utils/imagedatabase.cpp
          utils/imagedatabase.h
          utils/cloudimagedeployer.cpp
          utils/cloudimagedeployer.h
          utils/cloudinittemplates.h
          utils/isoimagebuilder.cpp
          utils/isoimagebuilder.h
          utils/virtualnetwork.h
          utils/virtualnetworkmanager.cpp
          utils/virtualnetworkmanager.h
          utils/virtualswitch.cpp
          utils/virtualswitch.h
          utils/networkthread.cpp
          utils/networkthread.h
          utils/dhcpserver.cpp
          utils/dhcpserver.h
          utils/dnsserver.cpp
          utils/dnsserver.h
          utils/gatewayresponder.cpp
          utils/gatewayresponder.h
          utils/slirpuplink.cpp
          utils/slirpuplink.h
          utils/usermodenetwork.cpp
          utils/usermodenetwork.h
          utils/usbenumerator.h
          utils/usbenumerator.cpp
          utils/savestatemanager.cpp
          utils/savestatemanager.h
          utils/asynchelperprocess.cpp
          utils/asynchelperprocess.h
          utils/startupchain.cpp
          utils/startupchain.h
          utils/sandboxcapabilities.cpp
          utils/sandboxcapabilities.h
          utils/qemucapabilities.cpp
          utils/qemucapabilities.h
          utils/sessionguard.cpp
          utils/sessionguard.h
          utils/guestlocales.h
          utils/archivepaths.cpp
          utils/archivepaths.h
          utils/ovfparser.cpp
          utils/ovfparser.h
          utils/qemuimgprocess.h
          utils/qemuimgprocess.cpp
          utils/3rdparty/crypt_sha512.c
          models/virtualenvironmentmodel.cpp
          models/virtualenvironmentmodel.h
          jobs/createvmjob.cpp
          jobs/createvmjob.h
          jobs/shrinkjob.cpp
          jobs/shrinkjob.h
          jobs/exportjob.cpp
          jobs/exportjob.h
          jobs/importjob.cpp
          jobs/importjob.h
          jobs/ovaexportjob.cpp
          jobs/ovaexportjob.h
          jobs/ovaimportjob.cpp
          jobs/ovaimportjob.h
          jobs/downloadjob.cpp
          jobs/downloadjob.h
          jobs/clonejob.h
          jobs/clonejob.cpp
          jobs/clouddeployjob.h
          jobs/clouddeployjob.cpp
          jobs/archivestreamjob.h
          jobs/archivestreamjob.cpp
          jobs/diskimportjob.cpp
          jobs/diskimportjob.h
          dialogs/editvmdialog.cpp
          dialogs/editvmdialog.h
          dialogs/newvmdialog.cpp
          dialogs/newvmdialog.h
          dialogs/settingsdialog.cpp
          dialogs/settingsdialog.h
          dialogs/iconchooserdialog.cpp
          dialogs/iconchooserdialog.h
          dialogs/takesnapshotdialog.cpp
          dialogs/takesnapshotdialog.h
          dialogs/snapshotdialog.cpp
          dialogs/snapshotdialog.h
          dialogs/cloudconfigdialog.cpp
          dialogs/cloudconfigdialog.h
          dialogs/missingtoolsdialog.cpp
          dialogs/missingtoolsdialog.h
          dialogs/deploymentabortdialog.cpp
          dialogs/deploymentabortdialog.h
          dialogs/deploymentlogwindow.cpp
          dialogs/deploymentlogwindow.h
          dialogs/hostshutdowndialog.cpp
          dialogs/hostshutdowndialog.h
          dialogs/diskdialog.cpp
          dialogs/diskdialog.h
          dialogs/licenseagreementdialog.cpp
          dialogs/licenseagreementdialog.h
          widgets/virtualenvironmentitemdelegate.cpp
          widgets/virtualenvironmentitemdelegate.h
          widgets/virtualenvironmenttreeview.h
          widgets/performancemonitorwidget.h
          widgets/performancemonitorwidget.cpp
          widgets/vmiconselector.h
          widgets/vmiconselector.cpp
          widgets/ramspinbox.h
          widgets/steppedspinbox.h
          dbusdisplay/dbusviewerwindow.cpp
          dbusdisplay/dbusviewerwindow.h
          dbusdisplay/displaywidget.cpp
          dbusdisplay/displaywidget.h
          dbusdisplay/listener.cpp
          dbusdisplay/listener.h
          dbusdisplay/audiojittercore.h
          dbusdisplay/audiojitterbuffer.cpp
          dbusdisplay/audiojitterbuffer.h
          dbusdisplay/audiolistener.cpp
          dbusdisplay/audiolistener.h
          dbusdisplay/audioinlistener.cpp
          dbusdisplay/audioinlistener.h
          dbusdisplay/clipboard.cpp
          dbusdisplay/clipboard.h
          dbusdisplay/glibeventbridge.h
          dbusdisplay/glibeventbridge.cpp
          dbusdisplay/controlconnection.h
          dbusdisplay/controlconnection.cpp
          dbusdisplay/osdoverlay.h
          dbusdisplay/osdoverlay.cpp
          dbusdisplay/statusindicator.h
          dbusdisplay/statusindicator.cpp
          dbusdisplay/uniquefd.h
          dbusdisplay/gvariantptr.h
          dbusdisplay/pcmringbuffer.h
          dbusdisplay/waylandpointerlock.cpp
          dbusdisplay/waylandpointerlock.h
          dbusdisplay/waylandsubsurface.cpp
          dbusdisplay/waylandsubsurface.h)

target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                   ${CMAKE_CURRENT_BINARY_DIR})

target_link_libraries(
  ${PROJECT_NAME}
  PkgConfig::SLIRP
  PkgConfig::ISOFS
  ${UDEV_LIBRARIES}
  LibArchive::LibArchive
  PkgConfig::GIO
  PkgConfig::EGL
  PkgConfig::WAYLAND_CLIENT
  Qt6::Core
  Qt6::Gui
  Qt6::Widgets
  Qt6::Network
  Qt6::Svg
  Qt6::Multimedia
  Qt6::DBus
  Qt6::OpenGL
  Qt6::OpenGLWidgets
  KF6::CoreAddons
  KF6::I18n
  KF6::WidgetsAddons
  KF6::ConfigCore
  KF6::XmlGui
  KF6::ConfigWidgets
  KF6::IconThemes
  KF6::IconWidgets
  KF6::JobWidgets
  KF6::KIOWidgets
  KF6::TextTemplate
  KF6::DBusAddons
  KF6::Crash)

if(TARGET Qt6::GuiPrivate)
  target_link_libraries(${PROJECT_NAME} Qt6::GuiPrivate)
endif()

install(TARGETS ${PROJECT_NAME} ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
