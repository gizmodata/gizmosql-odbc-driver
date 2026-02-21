# Copyright (C) 2020-2022 Dremio Corporation
# Copyright (C) 2026 GizmoData LLC
#
# See "LICENSE" for license information.
#

cmake_minimum_required(VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

# Version — single source of truth for DLL resource and MSI installer
set(GIZMOSQL_VERSION_MAJOR 1)
set(GIZMOSQL_VERSION_MINOR 0)
set(GIZMOSQL_VERSION_PATCH 0)
set(GIZMOSQL_VERSION_TWEAK 0)

project(gizmosql_odbc)

# Add Boost dependencies. Should be pre-installed (Brew on Mac).
find_package(Boost REQUIRED)
include_directories(${Boost_INCLUDE_DIRS})

# Add ODBC dependencies.
if (APPLE)
    # Find iODBC include path from Homebrew (works for both Intel and Apple Silicon)
    execute_process(
        COMMAND brew --prefix libiodbc
        OUTPUT_VARIABLE IODBC_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    set(ODBC_INCLUDE_DIRS ${IODBC_PREFIX}/include)
    add_compile_definitions(HAVE_LONG_LONG SQLCOLATTRIBUTE_SQLLEN WITH_IODBC)
else()
    find_package(ODBC REQUIRED)
endif()
include_directories(${ODBC_INCLUDE_DIRS})

if(CMAKE_BUILD_TYPE STREQUAL "Release")
    add_compile_definitions(NDEBUG)
endif()

# Fetch and include GTest
# Adapted from Google's documentation: https://google.github.io/googletest/quickstart-cmake.html#set-up-a-project
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
# For Windows: Prevent overriding the parent project's compiler/linker settings
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_subdirectory(flight_sql)
add_subdirectory(odbcabstraction)
add_subdirectory(odbc_entry)
