# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(iidb_artwork_tests LANGUAGES CXX)
    enable_testing()
endif()

set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Network Test)
add_executable(iidb_artwork_tests
    iidb_artwork.cpp
    ../../lichii/configuration/iidb_artwork_dialog.cpp
    ../../lichii/configuration/iidb_artwork_dialog.h
    ../../lichii/configuration/iidb_key_dialog.cpp
    ../../lichii/game/artwork_image.cpp
    ../../lichii/game/iidb_client.cpp
)
target_compile_features(iidb_artwork_tests PRIVATE cxx_std_20)
target_compile_definitions(iidb_artwork_tests PRIVATE
    QT_USE_QSTRINGBUILDER
    QT_NO_CAST_FROM_ASCII
    QT_NO_CAST_TO_ASCII
    QT_NO_NARROWING_CONVERSIONS_IN_CONNECT
    QT_NO_PROCESS_COMBINED_ARGUMENT_START
    QT_NO_URL_CAST_FROM_STRING
)
target_include_directories(iidb_artwork_tests PRIVATE ../..)
target_link_libraries(iidb_artwork_tests PRIVATE Qt6::Widgets Qt6::Network Qt6::Test)
if(TARGET common)
    target_compile_definitions(iidb_artwork_tests PRIVATE IIDB_TEST_ARTWORK_FILES)
    target_sources(iidb_artwork_tests PRIVATE ../../qt_common/util/custom_game_metadata.cpp)
    target_link_libraries(iidb_artwork_tests PRIVATE common)
endif()
if(WIN32)
    target_link_libraries(iidb_artwork_tests PRIVATE advapi32)
endif()
add_test(NAME iidb_artwork COMMAND iidb_artwork_tests)
set_tests_properties(iidb_artwork PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
