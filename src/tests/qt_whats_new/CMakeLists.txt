# SPDX-FileCopyrightText: Copyright 2026 Lichii Project
# SPDX-License-Identifier: GPL-3.0-or-later

cmake_minimum_required(VERSION 3.16)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(whats_new_tests LANGUAGES CXX)
    enable_testing()
endif()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets Test)

add_executable(whats_new_tests
    whats_new.cpp
    ../../lichii/whats_new_dialog.cpp
    ../../lichii/whats_new_dialog.h
    ../../lichii/lichii.qrc
    ../../../dist/qt_themes/default/default.qrc
    ../../../dist/qt_themes/lichii/style.qrc
)
target_compile_features(whats_new_tests PRIVATE cxx_std_20)
target_compile_definitions(whats_new_tests PRIVATE QT_NO_CAST_FROM_ASCII QT_NO_CAST_TO_ASCII)
target_include_directories(whats_new_tests PRIVATE ../..)
target_link_libraries(whats_new_tests PRIVATE Qt6::Widgets Qt6::Test)

add_test(NAME whats_new COMMAND whats_new_tests)
set_tests_properties(whats_new PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
