# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Fixed
- Fix `SetParameters()` not handling `SQL_C_LONG` (type 4) and `SQL_C_SHORT` (type 5) C types for numeric Arrow types. pyodbc and other ODBC clients may send integer parameters as `SQL_C_LONG` (the generic/undecorated form) rather than `SQL_C_SLONG` (the explicitly signed form), causing "Cannot convert C type 4 to Arrow int64" errors on parameterized queries.
- Fix `SetParameters()` not handling `SQL_C_WCHAR` for numeric Arrow types. Power BI DirectQuery uses W (wide-char) ODBC functions and sends parameter values as wide strings; the driver now converts SQL_C_WCHAR to UTF-8 before parsing for all supported Arrow types (int16/32/64, float32/64, boolean, date32, timestamp, utf8).

## [v1.0.0] - 2026-02-24

Initial release.

- Implement `SQLBindParameter`, `SQLDescribeParam`, and `SQLNumParams` for parameterized queries, enabling Power BI DirectQuery mode via Arrow Flight SQL `PreparedStatement::SetParameters()`
- Fix empty Power BI navigation table: ODBC callers (Power Query) filter for table type "TABLE", but DuckDB returns "BASE TABLE" per SQL standard. The driver now maps "TABLE" → "BASE TABLE" when sending the filter to the Flight SQL server, and maps "BASE TABLE" → "TABLE" in the result set so Power Query recognizes the tables.
- Fix `SqlWCharToString` truncating Wide-char strings to half length: the ODBC `W` function length parameter is in characters (SQLWCHAR units), not bytes, but the conversion divided by `GetSqlWCharSize()`. This caused `SQLColumnsW`, `SQLTablesW`, and other catalog functions to send truncated search patterns (e.g., "memory" → "mem"), resulting in "no visible columns" in Power BI.
- ODBC driver for GizmoSQL via Arrow Flight SQL
- Self-contained shared library with all dependencies (Arrow, gRPC, Protobuf, abseil) statically linked
- Supported platforms: macOS (arm64), Linux (x86_64), Windows (x64)
- Apple notarized macOS dylib and Authenticode-signed Windows DLL
- Windows MSI installer via WiX Toolset (64-bit)
- Embedded VERSIONINFO resource in Windows DLL for ODBC Administrator version/company display
- Explicit CloseSession RPC on disconnect to properly release server-side sessions
- Server-side query cancellation via CancelFlightInfo RPC on SQLCancel
- Force static OpenSSL linking on all platforms including MSVC to prevent DLL error 126
- Ship `msvcp140_codecvt_ids.dll` in MSI to fix DLL error 126 on machines without VC++ Redistributable
- CI verification of Windows DLL dependencies via `dumpbin /dependents` and `LoadLibrary` smoke test
- Fix `SQLColAttributeW` and `SQLGetDescFieldW` returning UTF-8 instead of UTF-16, which caused Power Query to display Chinese characters for column names and fall back to `SQL_C_BINARY` binding for all columns
- Fix `SQLColAttribute`/`SQLColAttributeW` never writing `numericAttr` when `charAttr` is NULL, which caused Power Query to see `SQL_UNKNOWN_TYPE` for all columns
- Fix `SQLGetDiagFieldW` returning UTF-8 instead of UTF-16 for string diagnostic fields
- Cache Arrow ExternalProject build artifacts in CI to avoid rebuilding from source on every run
- Add `SQLColAttribute` numericAttr regression test to Linux integration tests
- Fix `m_currentArd` descriptor initialized to APD instead of ARD, which could cause column bindings to use the wrong descriptor
- Fix `SQL_ATTR_MAX_ROWS` incorrectly throwing read-only error instead of setting the max rows limit
- Fix `SQLColumns` crash (null pointer dereference) when server returns column schema fields without `ARROW:FLIGHT:SQL:TYPE_NAME` metadata
- Add metadata integration tests (SQLTables/SQLColumns) to CI exercising all four Flight SQL metadata endpoints (GetCatalogs, GetDbSchemas, GetTableTypes, GetTables)
- Fix Windows CI vcpkg cache never saving/restoring: `${{ env.VCPKG_INSTALLATION_ROOT }}` resolved to empty string (system env var not in GitHub Actions env context); changed to `${{ env.VCPKG_ROOT }}` which is explicitly exported via `$GITHUB_ENV`
