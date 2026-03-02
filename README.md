# GizmoSQL ODBC Driver

The official ODBC driver for [GizmoSQL](https://gizmodata.com/gizmosql).

[![Build](https://github.com/gizmodata/gizmosql-odbc-driver/actions/workflows/ci.yml/badge.svg)](https://github.com/gizmodata/gizmosql-odbc-driver/actions/workflows/ci.yml)
[![License](https://img.shields.io/badge/License-Apache%202.0-blue.svg)](LICENSE)

## Overview

The GizmoSQL ODBC Driver enables BI tools, applications, and languages with ODBC support (Excel, Power BI, Tableau, Python/pyodbc, etc.) to connect to and query [GizmoSQL](https://gizmodata.com/gizmosql).

Key features:
- Full ODBC 3.x compliance
- Transaction support (`SQL_ATTR_AUTOCOMMIT`, `SQLEndTran`) via Flight SQL RPCs
- Power BI DirectQuery and Import mode support (via the [GizmoSQL Power BI Connector](https://github.com/gizmodata/gizmosql-powerbi-connector))
- Primary key and foreign key discovery (`SQLPrimaryKeys` / `SQLForeignKeys`) — enables Power BI and other BI tools to auto-detect table relationships
- TLS/SSL encryption with system trust store support
- Basic, token, and OAuth authentication
- HTTP/2 keepalive ping frames for long-running connections
- Cross-platform: macOS (arm64), Linux (x64/arm64), Windows (x64)

## Installation

### macOS (arm64)

#### Homebrew (recommended)

```bash
brew install gizmodata/tap/gizmosql-odbc
```

The driver is registered automatically. To create a DSN, add the following to `~/.odbc.ini`:

```ini
[GizmoSQL]
Driver        = GizmoSQL ODBC Driver
host          = localhost
port          = 31337
uid           = your-username
pwd           = your-password
useEncryption = true
```

#### Manual install

Download `libgizmosql-odbc.dylib` from the [latest release](https://github.com/gizmodata/gizmosql-odbc-driver/releases) and place it in a known location (e.g. `/usr/local/lib/`).

Register the driver in `$(brew --prefix)/etc/odbcinst.ini` (or `/usr/local/etc/odbcinst.ini`):

```ini
[GizmoSQL ODBC Driver]
Driver = /usr/local/lib/libgizmosql-odbc.dylib
```

Add a DSN in `~/.odbc.ini`:

```ini
[GizmoSQL]
Driver     = GizmoSQL ODBC Driver
host       = localhost
port       = 31337
uid        = your-username
pwd        = your-password
useEncryption = true

# For local development with self-signed certificates:
# disableCertificateVerification = true
```

### Linux (x64 / arm64)

#### Homebrew

```bash
brew install gizmodata/tap/gizmosql-odbc
```

The driver is registered automatically — see the macOS section above for DSN setup.

#### Manual install

Download `libgizmosql-odbc.so` for your architecture from the [latest release](https://github.com/gizmodata/gizmosql-odbc-driver/releases).

Register the driver in `/etc/odbcinst.ini`:

```ini
[GizmoSQL ODBC Driver]
Driver = /usr/local/lib/libgizmosql-odbc.so
```

Add a DSN in `/etc/odbc.ini` or `~/.odbc.ini`:

```ini
[GizmoSQL]
Driver     = GizmoSQL ODBC Driver
host       = localhost
port       = 31337
uid        = your-username
pwd        = your-password
useEncryption = true

# For local development with self-signed certificates:
# disableCertificateVerification = true
```

### Windows (x64)

#### Installer (recommended)

Download the MSI installer from the [latest release](https://github.com/gizmodata/gizmosql-odbc-driver/releases):

- `GizmoSQL-ODBC-Driver-x64.msi` for 64-bit Intel/AMD

The installer registers the ODBC driver automatically. After installation, use the ODBC Data Source Administrator (`odbcad32.exe`) to create a DSN.

#### Manual install

Download `gizmosql-odbc.dll` from the [latest release](https://github.com/gizmodata/gizmosql-odbc-driver/releases) and register manually:

```cmd
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\GizmoSQL ODBC Driver" /v Driver /t REG_SZ /d "C:\path\to\gizmosql-odbc.dll"
reg add "HKLM\SOFTWARE\ODBC\ODBCINST.INI\GizmoSQL ODBC Driver" /v Setup /t REG_SZ /d "C:\path\to\gizmosql-odbc.dll"
```

## Configuration

### Connection String Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `host` | string | *(required)* | Hostname or IP address of the GizmoSQL server |
| `port` | int | *(required)* | Port number of the GizmoSQL server |
| `user` / `uid` | string | | Username for basic authentication |
| `password` / `pwd` | string | | Password for basic authentication |
| `token` | string | | Bearer token for token-based authentication |
| `authType` | string | | Authentication method: `basic`, `token`, or `external` (OAuth) |
| `useEncryption` | bool | `true` | Enable TLS/SSL encryption |
| `disableCertificateVerification` | bool | `false` | Skip server certificate verification (not recommended for production) |
| `trustedCerts` | string | | Path to a PEM file with trusted CA certificates |
| `useSystemTrustStore` | bool | `true` (Windows), `false` (other) | Use the operating system's certificate store |
| `StringColumnLength` | int | | Maximum reported column length for string columns |
| `UseWideChar` | bool | `true` (Windows), `false` (other) | Use wide character (UTF-16) string bindings |
| `UseExtendedFlightSQLBuffer` | bool | `false` | Enable extended buffer mode for large result sets |
| `ChunkBufferCapacity` | int | `5` | Number of Arrow record batches to buffer (min: 1) |
| `HideSQLTablesListing` | bool | `false` | Hide system SQL tables from table listings |
| `SendPingFrame` | bool | `false` | Enable HTTP/2 keepalive ping frames |
| `PingFrameIntervalMilliseconds` | int | | Interval between keepalive pings in ms (min: 1000) |
| `PingFrameTimeoutMilliseconds` | int | | Timeout waiting for ping response in ms (min: 1000) |
| `MaxPingsWithoutData` | int | | Max pings allowed without data before connection is considered dead (min: 0) |

### Authentication

The driver supports three authentication methods:

1. **Basic authentication** — Set `user`/`uid` and `password`/`pwd`
2. **Token authentication** — Set `token` with a bearer token
3. **OAuth (external)** — Set `authType=external` to initiate the server-side OAuth discovery flow. The driver opens a browser for login and receives a token from the server. See [docs/OAUTH.md](docs/OAUTH.md) for details.

If both user/password and token are provided, user/password takes precedence. At least one authentication method must be configured.

## Usage Examples

### Python (pyodbc)

```python
import pyodbc

# Using a DSN
conn = pyodbc.connect("DSN=GizmoSQL")

# Using a connection string
conn = pyodbc.connect(
    "Driver=GizmoSQL ODBC Driver;"
    "host=localhost;"
    "port=31337;"
    "uid=gizmosql_user;"
    "pwd=gizmosql_password;"
    "useEncryption=true"
)

cursor = conn.cursor()
cursor.execute("SELECT * FROM my_table LIMIT 10")
for row in cursor.fetchall():
    print(row)

conn.close()
```

### Connection String

```
Driver=GizmoSQL ODBC Driver;host=gizmosql.example.com;port=31337;token=eyJhbGci...;useEncryption=true
```

## Building from Source

### Prerequisites

- CMake 3.11+
- C++17 compatible compiler
- Boost
- OpenSSL
- RapidJSON
- ODBC development headers (iODBC on macOS, unixODBC or iODBC on Linux)

Arrow Flight SQL, gRPC, and Protobuf are built automatically as a CMake ExternalProject — no separate installation needed.

### macOS

```bash
# Install dependencies
brew install cmake boost libiodbc openssl@3 rapidjson

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

### Linux

```bash
# Install dependencies (Debian/Ubuntu)
sudo apt-get install -y cmake libboost-all-dev libiodbc2-dev \
    libssl-dev rapidjson-dev

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --parallel
```

### Windows

Requires [vcpkg](https://github.com/microsoft/vcpkg) and Visual Studio 2022.

```cmd
@rem Set environment variables
set VCPKG_ROOT=C:\path\to\vcpkg
set ARROW_GIT_REPOSITORY=https://github.com/apache/arrow

@rem Install dependencies via vcpkg
%VCPKG_ROOT%\vcpkg.exe install --triplet x64-windows --x-install-root=%VCPKG_ROOT%/installed

@rem Build
mkdir build && cd build
cmake .. ^
    -DARROW_GIT_REPOSITORY=%ARROW_GIT_REPOSITORY% ^
    -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%/scripts/buildsystems/vcpkg.cmake ^
    -DVCPKG_TARGET_TRIPLET=x64-windows ^
    -DVCPKG_MANIFEST_MODE=OFF ^
    -G"Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_BUILD_TYPE=Release

cmake --build . --parallel 8 --config Release
```

Or use the provided build scripts:

```cmd
build_win64.bat
build_win32.bat
```

### Running Tests

After building, run the unit tests:

```bash
cd build
ctest --output-on-failure
```

## Troubleshooting / ODBC Trace

The driver includes built-in ODBC call tracing for diagnosing issues with Power BI, pyodbc, and other ODBC clients. Tracing is **off by default** and has zero overhead when disabled.

To enable tracing, set the `GIZMOSQL_ODBC_TRACE` environment variable before launching your application:

```cmd
@rem Windows (cmd)
set GIZMOSQL_ODBC_TRACE=1
start "" "C:\Program Files\Microsoft Power BI Desktop\bin\PBIDesktop.exe"
```

```powershell
# Windows (PowerShell)
$env:GIZMOSQL_ODBC_TRACE = "1"
& "C:\Program Files\Microsoft Power BI Desktop\bin\PBIDesktop.exe"
```

```bash
# macOS / Linux
export GIZMOSQL_ODBC_TRACE=1
python my_script.py
```

Trace output is written to `C:\odbc_trace.log` (Windows) or `/tmp/odbc_trace.log` (macOS/Linux). Each entry includes a timestamp and logs function name, parameters, and return values for all major ODBC calls (`SQLDriverConnectW`, `SQLExecDirectW`, `SQLFetch`, `SQLGetData`, `SQLColAttributeW`, `SQLGetTypeInfo`, `SQLColumnsW`, `SQLTablesW`, `SQLBindParameter`, etc.).

To disable tracing, unset the variable or set it to any value other than `1`.

## Logging

The driver reads logging configuration from `gizmosql-odbc.ini`, located in the same directory as the driver library.

Example `gizmosql-odbc.ini`:

```ini
[Driver]
LogEnabled=true
LogPath=/var/log/gizmosql-odbc
LogLevel=1
MaximumFileSize=16777216
FileQuantity=2
```

| Property | Description |
|----------|-------------|
| `LogEnabled` | Enable or disable logging (`true`/`false`) |
| `LogPath` | Directory path for log files |
| `LogLevel` | 0=TRACE, 1=DEBUG, 2=INFO, 3=WARN, 4=ERROR, 5=OFF |
| `MaximumFileSize` | Maximum size per log file in bytes (default: 16777216 / 16 MB) |
| `FileQuantity` | Number of rotating log files to keep (default: 1) |

## Acknowledgements

This project is a fork of the [Arrow Flight SQL ODBC Driver](https://github.com/dremio/flightsql-odbc) originally created by [Dremio](https://www.dremio.com/). We are grateful to the Dremio developers and the open-source community for building a solid ODBC foundation on top of Apache Arrow Flight SQL. Thank you for making this work possible.

## License

Licensed under the Apache License, Version 2.0. See [LICENSE](LICENSE) for details. See [NOTICE](NOTICE) for attribution.
