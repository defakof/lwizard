cmake_minimum_required(VERSION 3.22)
project(lwizard LANGUAGES CXX)

# ---- bg3rustpaklib (Rust staticlib, ffi feature) ----------------------------
set(BG3RUSTPAKLIB_DIR "${CMAKE_CURRENT_LIST_DIR}/../bg3rustpaklib" CACHE PATH
    "Path to bg3rustpaklib source directory")

file(GLOB _RUST_MSI_BIN_DIRS LIST_DIRECTORIES true
    "$ENV{ProgramFiles}/Rust stable MSVC */bin")

find_program(CARGO_EXECUTABLE NAMES cargo cargo.exe REQUIRED
  HINTS
    "$ENV{USERPROFILE}/.cargo/bin"
    "$ENV{HOME}/.cargo/bin"
    ${_RUST_MSI_BIN_DIRS})
get_filename_component(_RUST_BIN_DIR "${CARGO_EXECUTABLE}" DIRECTORY)
if(WIN32)
  set(_RUST_BUILD_PATH "PATH=${_RUST_BIN_DIR};$ENV{PATH}")
else()
  set(_RUST_BUILD_PATH "PATH=${_RUST_BIN_DIR}:$ENV{PATH}")
endif()

set(_BG3PAK_LIB "${BG3RUSTPAKLIB_DIR}/target/release/bg3rustpaklib.lib")

add_custom_target(bg3rustpaklib_build ALL
  COMMAND "${CMAKE_COMMAND}" -E env "${_RUST_BUILD_PATH}"
          "${CARGO_EXECUTABLE}" rustc
          --release
          --features ffi
          --crate-type=staticlib
          --manifest-path "${BG3RUSTPAKLIB_DIR}/Cargo.toml"
  COMMENT "Building bg3rustpaklib (Rust staticlib)"
  BYPRODUCTS "${_BG3PAK_LIB}"
)

add_library(bg3rustpaklib STATIC IMPORTED GLOBAL)
set_target_properties(bg3rustpaklib PROPERTIES
  IMPORTED_LOCATION "${_BG3PAK_LIB}"
  INTERFACE_INCLUDE_DIRECTORIES "${BG3RUSTPAKLIB_DIR}/include"
)
add_dependencies(bg3rustpaklib bg3rustpaklib_build)
# ------------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LWIZARD_SOURCE_ROOT "${CMAKE_CURRENT_LIST_DIR}/src")

get_filename_component(_LWIZARD_UIBASE_CMAKE
  "${CMAKE_CURRENT_LIST_DIR}/../uibase_install/lib/cmake/mo2-uibase" ABSOLUTE)
set(mo2-uibase_DIR "${_LWIZARD_UIBASE_CMAKE}" CACHE PATH
    "mo2-uibase CMake package directory" FORCE)

# Portable MO2 layout: ModOrganizer.exe and plugins/ share the same folder (no bin/).
set(MO2_INSTALL_IS_BIN ON CACHE BOOL "Install runtime next to ModOrganizer.exe" FORCE)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(mo2-cmake CONFIG REQUIRED)
find_package(mo2-uibase CONFIG REQUIRED)

# Shared code (ModDataContent, Divine helper, log, Qt resources)
add_library(lwizard_common STATIC)
target_sources(lwizard_common PRIVATE
  src/core/bg3_localization_content.cpp
  src/core/bg3_localization_content.h
  src/core/lwizard_divine.cpp
  src/core/lwizard_divine.h
  src/core/lwizard_log.cpp
  src/core/lwizard_log.h
  src/core/lwizard_pak_reader.cpp
  src/core/lwizard_pak_reader.h
)
set_target_properties(lwizard_common PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  AUTOMOC ON
)
target_include_directories(lwizard_common PUBLIC "${LWIZARD_SOURCE_ROOT}")
target_link_libraries(lwizard_common PRIVATE
  mo2::uibase Qt6::Widgets
  bg3rustpaklib
  # Windows system libs required by the Rust standard library
  ws2_32 userenv ntdll bcrypt
)

add_library(lwizard SHARED)
mo2_configure_plugin(lwizard WARNINGS ON TRANSLATIONS OFF NO_SOURCES)
target_sources(lwizard PRIVATE
  src/plugins/lwizard_plugin.cpp
  src/plugins/lwizard_plugin.h
  src/services/lwizard_ai_translator.cpp
  src/services/lwizard_ai_translator.h
  src/services/lwizard_nexus_api.cpp
  src/services/lwizard_nexus_api.h
  src/ui/lwizard_html_delegate.cpp
  src/ui/lwizard_html_delegate.h
  src/ui/lwizard_modlist_ui_patch.cpp
  src/ui/lwizard_modlist_ui_patch.h
  src/ui/lwizard_nexus_tab.cpp
  src/ui/lwizard_nexus_tab.h
  src/ui/lwizard_translation_tab.cpp
  src/ui/lwizard_translation_tab.h
  src/ui/lwizard_window.cpp
  src/ui/lwizard_window.h
  resources/lwizard.qrc
)
set_target_properties(lwizard PROPERTIES AUTORCC ON)
target_include_directories(lwizard PRIVATE "${LWIZARD_SOURCE_ROOT}")
target_link_libraries(lwizard PRIVATE lwizard_common mo2::uibase Qt6::Widgets Qt6::Network)

mo2_install_plugin(lwizard)

add_library(lwizard_unpack SHARED)
mo2_configure_plugin(lwizard_unpack WARNINGS ON TRANSLATIONS OFF NO_SOURCES)
target_sources(lwizard_unpack PRIVATE
  src/plugins/lwizard_unpack_plugin.cpp
  src/plugins/lwizard_unpack_plugin.h
  src/ui/lwizard_unpack_dialog.cpp
  src/ui/lwizard_unpack_dialog.h
)
target_include_directories(lwizard_unpack PRIVATE "${LWIZARD_SOURCE_ROOT}")
target_link_libraries(lwizard_unpack PRIVATE lwizard_common mo2::uibase Qt6::Widgets)

mo2_install_plugin(lwizard_unpack)
