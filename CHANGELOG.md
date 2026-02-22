# Changelog

All notable changes to this project will be documented in this file.

## [v1.0.0] - 2026-02-21

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
