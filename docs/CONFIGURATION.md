# GizmoSQL ODBC Driver Configuration Reference

Complete reference for all connection properties, DSN configuration, and logging settings.

## Connection Properties

### Required Properties

| Property | Type | Description |
|----------|------|-------------|
| `host` | string | Hostname or IP address of the GizmoSQL server. If an IP address is provided with encryption enabled, the driver will attempt reverse DNS resolution. |
| `port` | int | Port number of the GizmoSQL server. |

### Authentication Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `user` | string | | Username for basic authentication. |
| `uid` | string | | Alias for `user`. Standard ODBC property name per SQLDriverConnect documentation. |
| `user id` | string | | Alias for `user`. Used by the Microsoft OLE DB to ODBC bridge (MSDASQL). |
| `password` | string | | Password for basic authentication. |
| `pwd` | string | | Alias for `password`. Standard ODBC property name. |
| `token` | string | | Bearer token for token-based authentication. |
| `authType` | string | | Authentication method: `basic`, `token`, or `external` (OAuth). When set to `external`, the driver initiates the server-side OAuth discovery flow. |

**Authentication precedence:**

1. If `authType=external` is set, OAuth authentication is used regardless of other credential properties.
2. If `user`/`uid` or `password`/`pwd` are set, basic authentication is used.
3. If `token` is set (and no user/password), token authentication is used.
4. If no credentials are provided, a no-op authentication handshake is performed.

### Encryption Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `useEncryption` | bool | `true` | Enable TLS/SSL encryption for the gRPC connection. When enabled, the driver connects via `grpc+tls://`; otherwise `grpc+tcp://`. |
| `disableCertificateVerification` | bool | `false` | Skip server certificate verification. **Not recommended for production.** |
| `trustedCerts` | string | | Path to a PEM file containing trusted CA certificates. Used when `useSystemTrustStore` is `false`. |
| `useSystemTrustStore` | bool | `true` (Windows), `false` (macOS/Linux) | Use the operating system's certificate store for TLS verification. On Windows, reads from the CA, MY, ROOT, and SPC certificate stores. |

### Data Handling Properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `StringColumnLength` | int | *(none)* | Maximum reported column length for string/varchar columns. When not set, the driver uses the server-reported length. Minimum value: 1. |
| `UseWideChar` | bool | `true` (Windows), `false` (macOS/Linux) | Use wide character (UTF-16) string bindings. Should be `true` for most Windows applications. |
| `UseExtendedFlightSQLBuffer` | bool | `false` | Enable extended buffer mode for large result sets. |
| `ChunkBufferCapacity` | int | `5` | Number of Arrow record batches to buffer in memory. Higher values may improve throughput at the cost of memory. Minimum value: 1. |
| `HideSQLTablesListing` | bool | `false` | Hide system SQL tables from `SQLTables()` results. |

### HTTP/2 Keepalive Properties

These properties control gRPC HTTP/2 ping frames, useful for maintaining long-lived connections through proxies and load balancers.

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `SendPingFrame` | bool | `false` | Enable HTTP/2 keepalive ping frames. All other ping properties are ignored unless this is `true`. |
| `PingFrameIntervalMilliseconds` | int | *(gRPC default)* | Interval between keepalive pings in milliseconds. Minimum value: 1000. Maps to gRPC `keepalive_time_ms`. |
| `PingFrameTimeoutMilliseconds` | int | *(gRPC default)* | Timeout waiting for a ping response in milliseconds. Minimum value: 1000. Maps to gRPC `keepalive_timeout_ms`. |
| `MaxPingsWithoutData` | int | *(gRPC default)* | Maximum number of pings that can be sent without receiving data before the connection is considered dead. Minimum value: 0. Maps to gRPC `http2.max_pings_without_data`. |

### Other Properties

| Property | Type | Description |
|----------|------|-------------|
| `dsn` | string | The DSN name. Set automatically when connecting via a DSN. |
| `driver` | string | The driver name. Set automatically by the ODBC driver manager. |

### Custom Properties

Any connection property not in the above list is passed as an HTTP header on gRPC calls. Property names are converted to lowercase (required by gRPC). Properties containing spaces are ignored with a warning, as they would crash gRPC.

## DSN Configuration

### macOS / Linux

**Driver registration** (`/usr/local/etc/odbcinst.ini` on macOS, `/etc/odbcinst.ini` on Linux):

```ini
[GizmoSQL ODBC Driver]
Driver = /path/to/libgizmosql-odbc.dylib   # or .so on Linux
```

**DSN definition** (`~/.odbc.ini` or `/etc/odbc.ini`):

```ini
[GizmoSQL]
Driver              = GizmoSQL ODBC Driver
host                = gizmosql.example.com
port                = 32010
uid                 = my-user
pwd                 = my-password
useEncryption       = true
useSystemTrustStore = false
trustedCerts        = /path/to/ca-bundle.pem
```

### Windows

Use the ODBC Data Source Administrator (`odbcad32.exe`) to create and configure DSNs through the graphical interface.

The driver's DSN configuration dialog has two tabs:

- **Common** — Data Source Name, Host, Port, Authentication Type (Basic/Token/OAuth), credentials
- **Advanced** — Encryption settings, certificate configuration, custom properties

### Connection Strings

Connection strings use semicolon-delimited key=value pairs:

```
Driver=GizmoSQL ODBC Driver;host=gizmosql.example.com;port=32010;uid=user;pwd=pass;useEncryption=true
```

Values containing semicolons or braces can be enclosed in braces: `pwd={p@ss;word}`.

## Logging Configuration

The driver reads logging settings from a file named `gizmosql-odbc.ini`, located in the same directory as the driver library.

### Configuration file format

```ini
[Driver]
LogEnabled=true
LogPath=/var/log/gizmosql-odbc
LogLevel=1
MaximumFileSize=16777216
FileQuantity=2
```

### Logging properties

| Property | Type | Default | Description |
|----------|------|---------|-------------|
| `LogEnabled` | bool | `false` | Enable or disable logging. |
| `LogPath` | string | *(required if enabled)* | Directory path for log file output. The directory must exist. |
| `LogLevel` | int | `1` | Log verbosity level. |
| `MaximumFileSize` | int | `16777216` (16 MB) | Maximum size of each log file in bytes. |
| `FileQuantity` | int | `1` | Number of rotating log files to keep. |

### Log levels

| Value | Level | Description |
|-------|-------|-------------|
| 0 | TRACE | Most detailed output, including all internal operations |
| 1 | DEBUG | Detailed debugging information |
| 2 | INFO | General informational messages |
| 3 | WARN | Warning conditions |
| 4 | ERROR | Error conditions only |
| 5 | OFF | Disable logging |

### Log file location

On Windows, place `gizmosql-odbc.ini` in the same directory as `gizmosql-odbc.dll`.

On macOS/Linux, place it in the same directory as the shared library (`libgizmosql-odbc.dylib` or `libgizmosql-odbc.so`). The driver uses its own module path to locate the config file.
