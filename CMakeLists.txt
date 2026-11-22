cmake_minimum_required(VERSION 3.20)
project(lfg-linux VERSION 0.1.6 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(PkgConfig REQUIRED)
find_package(Vulkan 1.3 REQUIRED)

pkg_check_modules(PIPEWIRE  REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(LIBPORTAL REQUIRED IMPORTED_TARGET libportal)
pkg_check_modules(GLIB      REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0 gio-unix-2.0 gobject-2.0)
pkg_check_modules(XCB       REQUIRED IMPORTED_TARGET xcb xcb-shape xcb-randr)
pkg_check_modules(DRM       REQUIRED IMPORTED_TARGET libdrm)

set(LFG_SOURCES
    src/main.cpp
    src/vulkan_context.cpp
    src/frame_generator.cpp
    src/capture/portal_session.cpp
    src/capture/pipewire_capture.cpp
    src/capture/dmabuf_image_importer.cpp
    src/compute/lsfg_pipeline.cpp
    src/present/overlay_window.cpp
    src/present/control_panel.cpp
)

add_executable(lfg ${LFG_SOURCES})

target_include_directories(lfg PRIVATE include)

target_link_libraries(lfg PRIVATE
    Vulkan::Vulkan
    PkgConfig::PIPEWIRE
    PkgConfig::LIBPORTAL
    PkgConfig::GLIB
    PkgConfig::XCB
    PkgConfig::DRM
)

target_compile_options(lfg PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)

install(TARGETS lfg RUNTIME DESTINATION bin)
install(CODE "execute_process(COMMAND \"${CMAKE_COMMAND}\" -E create_symlink lfg \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/lwfg\")")
install(FILES assets/lfg-linux.desktop DESTINATION share/applications)
install(FILES assets/lfg-linux.png DESTINATION share/icons/hicolor/128x128/apps RENAME lfg-linux.png)
install(FILES assets/lfg-linux-32.png DESTINATION share/icons/hicolor/32x32/apps RENAME lfg-linux.png)
install(FILES assets/lfg-linux.png DESTINATION share/pixmaps RENAME lfg-linux.png)

set(CPACK_PACKAGE_NAME "lwfg")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "LWFG (Linux Windowed Frame Generation) - Lossless Scaling LSFG 3.1 Overlay for Linux")
set(CPACK_PACKAGE_CONTACT "LWFG")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")

set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_PACKAGE_SECTION "games")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "LWFG")

set(CPACK_RPM_PACKAGE_AUTOREQ "ON")
set(CPACK_RPM_PACKAGE_AUTOPROV "ON")
set(CPACK_RPM_PACKAGE_ARCHITECTURE "x86_64")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    /usr/share/applications
    /usr/share/icons
    /usr/share/icons/hicolor
    /usr/share/icons/hicolor/32x32
    /usr/share/icons/hicolor/32x32/apps
    /usr/share/icons/hicolor/128x128
    /usr/share/icons/hicolor/128x128/apps
    /usr/share/pixmaps
)

include(CPack)

