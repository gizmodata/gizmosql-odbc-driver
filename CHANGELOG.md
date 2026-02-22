# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Fixed
- Fix `ODBCStatement` constructor initializing `m_currentArd` to the APD (`m_builtInApd`) instead of the ARD (`m_builtInArd`), causing all `SQLFetch`/`SQLFetchScroll` row-binding operations to read from the wrong descriptor
- Fix `getCTypeForSQLType` using `SQL_C_INTERVAL_*` case labels (C-type constants, negative values) instead of `SQL_INTERVAL_*` SQL-type constants (100-range), causing interval-typed columns to always throw "Unknown SQL type" when `SQLGetData` was called with `SQL_C_DEFAULT`
- Fix `SQL_ATTR_MAX_ROWS` statement attribute rejecting writes with "read-only attribute"; it is a valid, settable ODBC attribute that limits rows returned by `SQLFetch`
- Fix DDL/DML statements never executing when the server returns a `FlightInfo` with empty endpoints; `Execute()` and `ExecutePrepared()` now detect empty endpoints and fall back to `ExecuteUpdate()` so the statement actually runs on the server

## [v1.0.0] - 2026-02-22

Initial release.

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
