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

# On Windows, add the .def file for symbol exports and version resource
if (WIN32)
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/version.rc.in
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
        @ONLY
    )
    list(APPEND ODBC_ENTRY_SOURCES
        exports/gizmosql-odbc.def
        ${CMAKE_CURRENT_BINARY_DIR}/version.rc
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
    target_link_libraries(gizmosql-odbc PRIVATE odbccp32 legacy_stdio_definitions)
endif()

# Symbol export control (macOS / Linux — Windows uses .def file)
if (APPLE)
    set_target_properties(gizmosql-odbc PROPERTIES
        LINK_FLAGS "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports/gizmosql-odbc.exports"
    )
elseif (UNIX)
    # -Bsymbolic-functions: resolve internal function calls to the library's
    # own definitions, preventing the ODBC Driver Manager from intercepting
    # calls between our exported ODBC functions (e.g. SQLAllocConnect calling
    # SQLAllocHandle internally).
    set_target_properties(gizmosql-odbc PROPERTIES
        LINK_FLAGS "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports/gizmosql-odbc.map -Wl,-Bsymbolic-functions"
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
