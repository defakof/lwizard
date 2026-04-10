cmake_minimum_required(VERSION 3.22)
project(lwizard LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_LWIZARD_UIBASE_CMAKE
  "${CMAKE_CURRENT_LIST_DIR}/../uibase_install/lib/cmake/mo2-uibase" ABSOLUTE)
set(mo2-uibase_DIR "${_LWIZARD_UIBASE_CMAKE}" CACHE PATH "mo2-uibase CMake package directory" FORCE)

# Portable MO2 layout: ModOrganizer.exe and plugins/ share the same folder (no bin/).
set(MO2_INSTALL_IS_BIN ON CACHE BOOL "Install runtime next to ModOrganizer.exe" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
find_package(mo2-cmake CONFIG REQUIRED)
find_package(mo2-uibase CONFIG REQUIRED)

add_library(lwizard SHARED)
mo2_configure_plugin(lwizard WARNINGS ON TRANSLATIONS OFF NO_SOURCES)
target_sources(lwizard PRIVATE
  src/lwizard_plugin.cpp
  src/lwizard_plugin.h
  src/bg3_localization_content.cpp
  src/bg3_localization_content.h
  src/lwizard_log.cpp
  src/lwizard_log.h
  src/lwizard_window.cpp
  src/lwizard_window.h
  resources/lwizard.qrc
)
target_link_libraries(lwizard PRIVATE mo2::uibase Qt6::Widgets)

mo2_install_plugin(lwizard)
