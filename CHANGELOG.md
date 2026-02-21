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
