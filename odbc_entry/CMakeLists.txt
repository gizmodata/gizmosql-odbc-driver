# Copyright (C) 2026 GizmoData LLC
#
# See "LICENSE" for license information.

cmake_minimum_required(VERSION 3.11)
set(CMAKE_CXX_STANDARD 17)

set(ODBC_ENTRY_SOURCES
    driver_singleton.h
    driver_singleton.cc
    odbc_entry.cc
)

# On Windows, add the .def file for symbol exports and system_dsn.cc
if (WIN32)
    list(APPEND ODBC_ENTRY_SOURCES
        exports/gizmosql-odbc.def
    )
endif()

add_library(gizmosql-odbc SHARED ${ODBC_ENTRY_SOURCES})

# Include paths
target_include_directories(gizmosql-odbc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/odbcabstraction/include
    ${CMAKE_SOURCE_DIR}/flight_sql/include
    ${CMAKE_SOURCE_DIR}/flight_sql
)

# Add Arrow library directory so the linker can find static Arrow libs
target_link_directories(gizmosql-odbc PRIVATE
    ${CMAKE_BINARY_DIR}/flight_sql/ApacheArrow-prefix/src/ApacheArrow-install/lib
)

# Link to internal static libraries (pulls in Arrow, gRPC, etc. transitively)
target_link_libraries(gizmosql-odbc PRIVATE
    gizmosql_odbc_spi_impl
    odbcabstraction
)

# Link ODBC installer library (needed for SQLGetPrivateProfileString)
if (APPLE)
    execute_process(
        COMMAND brew --prefix libiodbc
        OUTPUT_VARIABLE IODBC_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    target_link_libraries(gizmosql-odbc PRIVATE "-L${IODBC_PREFIX}/lib" -liodbcinst)
elseif (UNIX)
    target_link_libraries(gizmosql-odbc PRIVATE -lodbcinst)
elseif (WIN32)
    target_link_libraries(gizmosql-odbc PRIVATE odbccp32)
endif()

# Symbol export control (macOS / Linux — Windows uses .def file)
if (APPLE)
    set_target_properties(gizmosql-odbc PROPERTIES
        LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports/gizmosql-odbc.exports"
    )
elseif (UNIX)
    set_target_properties(gizmosql-odbc PROPERTIES
        LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/gizmosql-odbc.map"
    )
endif()

# Output to the same lib directory as other targets
set_target_properties(gizmosql-odbc PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/$<CONFIG>/lib
)

# Ensure Arrow external project is built before this target
add_dependencies(gizmosql-odbc ApacheArrow)
