# Changelog

All notable changes to this project will be documented in this file.

## [Unreleased]

### Added
- Transaction support via `SQL_ATTR_AUTOCOMMIT` and `SQLEndTran`, using Flight SQL `BeginTransaction`/`Commit`/`Rollback` RPCs. pyodbc, SQLAlchemy, and other clients that set `SQL_AUTOCOMMIT_OFF` now work correctly. When autocommit is OFF, transactions begin implicitly on first statement execution and are committed/rolled back via `SQLEndTran`. Open transactions are rolled back on disconnect (ODBC spec §SQLDisconnect).
- Add `defaultCatalog` connection property to specify which database/catalog to use at connection time. After authentication, the driver executes `USE <catalog>` to switch the server-side database and sets the ODBC `CURRENT_CATALOG` attribute. This allows Power BI and other ODBC clients to see tables from the correct database instead of the server's default (typically empty `memory`). The property is available in connection strings, DSN configuration UI, and DSN registry settings.
- Homebrew formula now auto-registers the driver in `odbcinst.ini` via `post_install`, eliminating manual driver registration. Users only need to create a DSN in `~/.odbc.ini`.
- pyodbc integration tests in CI verifying connection, transactions, commit, and rollback

### Fixed
- Fix OAuth (`authType=external`) requiring browser re-authentication on every Power BI query: the bearer token obtained from the OAuth flow was stored only per-connection and discarded on close. Power BI DirectQuery creates a new connection for each query, triggering the full browser-based OAuth flow every time. Bearer tokens are now cached to a file and reused across connections and processes. On each new connection the driver validates the cached token with a lightweight `ListActions` RPC — if the token has expired or the signing key has rotated, the cache is automatically cleared and a fresh browser-based OAuth flow is triggered.
- Fix OAuth (`authType=external`) browser-based authentication: rewrite to match the GizmoSQL server's 3-phase OAuth protocol — (1) discover OAuth URL via `x-gizmosql-oauth-url` gRPC response header, (2) initiate OAuth session and poll for token via HTTP REST endpoints (`/oauth/initiate`, `/oauth/token/{uuid}`), (3) exchange access token via Flight Handshake with username=`token`. Previously the driver tried to extract the OAuth URL from `FlightStatus::extra_info()` and polled via repeated gRPC handshakes, which didn't match the server protocol, causing "Token verification failed: Invalid input; too much fill".
- Fix "Unknown GetInfo type: 17" error when Power BI Navigator drills into catalogs: if the Flight SQL `GetSqlInfo` RPC failed (e.g., unrecognized info codes), `has_server_info_` was set to true but `LoadDefaultsForMissingEntries()` never ran, leaving the GetInfo cache empty. All subsequent `SQLGetInfo` calls then failed with "Unknown GetInfo type" for every ODBC info type. The server call is now wrapped in a try-catch so defaults are always populated regardless of server errors.

## [v1.0.0] - 2026-02-25

### Fixed
- Fix Power BI DirectQuery "UNSEARCHABLE" folding failure: `GetTypeName()` returned empty string when the Flight SQL server did not provide `ARROW:FLIGHT:SQL:TYPE_NAME` column metadata. Power BI's Mashup engine matches each column's `SQL_DESC_TYPE_NAME` (from `SQLColAttributeW`) against `TYPE_NAME` in `SQLGetTypeInfo` to determine searchability — an empty string matched nothing, causing all columns to be treated as UNSEARCHABLE and preventing query folding. The driver now falls back to computing the type name from the Arrow field's SQL data type (e.g., "WVARCHAR", "INTEGER").
- Fix `SQLGetTypeInfo` TYPE_NAME / DATA_TYPE inconsistency on Windows: `EnsureRightSqlCharType` remapped `DATA_TYPE` from `VARCHAR` to `WVARCHAR` but left `TYPE_NAME` as the raw server string "VARCHAR", creating a mismatch. TYPE_NAME is now updated to match the remapped data type.
- Fix `SQLColAttribute`/`SQLColAttributeW` sign extension for `numericAttr`: negative SQL data type codes (e.g., `SQL_WVARCHAR = -9`) were returned as unsigned values (65527) because the driver copied raw bytes into `SQLLEN` without sign-extending from `SQLSMALLINT`. Values are now properly sign-extended based on their actual size.
- Fix `SetParameters()` not handling `SQL_C_LONG` (type 4) and `SQL_C_SHORT` (type 5) C types for numeric Arrow types. pyodbc and other ODBC clients may send integer parameters as `SQL_C_LONG` (the generic/undecorated form) rather than `SQL_C_SLONG` (the explicitly signed form), causing "Cannot convert C type 4 to Arrow int64" errors on parameterized queries.
- Fix `SetParameters()` not handling `SQL_C_WCHAR` for numeric Arrow types. Power BI DirectQuery uses W (wide-char) ODBC functions and sends parameter values as wide strings; the driver now converts SQL_C_WCHAR to UTF-8 before parsing for all supported Arrow types (int16/32/64, float32/64, boolean, date32, timestamp, utf8).

### Changed
- `IsSearchable()` now always returns `SEARCHABILITY_ALL` for all column types and all types in `SQLGetTypeInfo`, since DuckDB supports WHERE, GROUP BY, and ORDER BY on all types. Previously, the driver relied on Flight SQL metadata which often defaulted to unsearchable.
- `GetLocalTypeName()` now falls back to computing the type name from the Arrow field type, matching the `GetTypeName()` fix.

### Added
- Wire up `SQLPrimaryKeys` and `SQLForeignKeys` to Flight SQL `GetPrimaryKeys`, `GetExportedKeys`, `GetImportedKeys`, and `GetCrossReference` RPCs. Previously these returned empty result sets, so Power BI could not discover primary key / foreign key relationships. The server returns `int32` for KEY_SEQ and `uint8` for UPDATE_RULE / DELETE_RULE, but ODBC expects `int16` — a new `CastField` transformer bridges the type gap via `arrow::compute::Cast`.
- Add integration tests for `SQLPrimaryKeys` and `SQLForeignKeys` (imported keys and exported keys paths) to verify end-to-end PK/FK discovery against a live GizmoSQL server.
- Optional ODBC trace logging to `C:\odbc_trace.log`, enabled by setting environment variable `GIZMOSQL_ODBC_TRACE=1`. Logs all major ODBC function calls with parameters and return values for debugging Power BI and other ODBC client interactions.

## [v1.0.0-rc1] - 2026-02-24

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
