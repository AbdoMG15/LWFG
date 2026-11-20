cmake_minimum_required(VERSION 3.20)
project(lfg-linux LANGUAGES CXX)

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
