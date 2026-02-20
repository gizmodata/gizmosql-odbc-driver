# GizmoSQL ODBC Driver

## Build

```bash
# macOS (requires: brew install cmake boost libiodbc grpc protobuf openssl@3 rapidjson)
cmake -B build -DCMAKE_BUILD_TYPE=Release -DOPENSSL_ROOT_DIR=$(brew --prefix openssl@3)
cmake --build build --parallel $(sysctl -n hw.ncpu)

# Linux (requires: cmake libboost-all-dev unixodbc-dev libgrpc++-dev libprotobuf-dev libssl-dev rapidjson-dev)
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel $(nproc)
```

Arrow 23.0.1 is built automatically as a CMake ExternalProject and statically linked into the single shared library (`libgizmosql-odbc.dylib` / `.so` / `.dll`).

## Run Tests

```bash
# Unit tests (always run these first)
ctest --test-dir build --output-on-failure

# Integration tests — IMPORTANT: run locally before pushing to avoid long CI failures
# Requires Docker for the GizmoSQL server container
docker run -d --name gizmosql-test -p 31337:31337 \
  -e GIZMOSQL_USERNAME=gizmosql_user \
  -e GIZMOSQL_PASSWORD=gizmosql_password \
  -e DATABASE_FILENAME=/tmp/test.duckdb \
  -e TLS_ENABLED=0 \
  gizmodata/gizmosql:latest

# Wait for server, then test with isql (unixODBC) or iodbctest (iODBC)
echo "SELECT 42 AS answer;" | isql -v "GizmoSQL Test"

# Clean up
docker rm -f gizmosql-test
```

## Architecture

- `flight_sql/` — SPI layer: Arrow Flight SQL client, accessors, type conversions
- `odbcabstraction/` — ODBC abstraction: handles, descriptors, statement management, diagnostics
- `odbc_entry/` — Thin `extern "C"` shim exporting all ODBC SQL* functions → shared library
- Build produces one shared library with all dependencies (Arrow, gRPC, Protobuf) statically linked

## Key Patterns

### Arrow 23 FlightCallOptions
All Flight SQL RPC calls (Prepare, Execute, Close, DoGet) require `FlightCallOptions` with auth headers. Arrow 23 changed `PreparedStatement::Execute()` and `Close()` to accept `FlightCallOptions` as a parameter (defaulting to empty). Always pass `call_options_` — never rely on the default.

### DDL/DML Lazy Execution
Flight SQL `Execute()` (GetFlightInfo) only plans queries. For DDL/DML with empty endpoints, the server defers execution to `DoGet()`. Fix: detect empty endpoints and fall back to `ExecuteUpdate()`.

### Symbol Interposition
When unixODBC DM loads the driver, internal calls between exported ODBC functions get interposed via PLT. Fix: `static` internal functions + `-Bsymbolic-functions` on Linux. macOS uses `-two_levelnamespace` by default.

### SQLGetData Return Values
`ResultSet::GetData()` returns `true` when data was fetched (maps to `SQL_SUCCESS`), `false` only when no more data is available (maps to `SQL_NO_DATA`). The ODBC handle wrapper automatically upgrades `SQL_SUCCESS` to `SQL_SUCCESS_WITH_INFO` when diagnostics contain truncation warnings.

## CI Notes
- Windows uses `windows-2022` (not `windows-latest`) for sufficient disk space
- Windows ARM64 build is disabled — Arrow 23 SSE intrinsics don't cross-compile
- Linux integration tests use GizmoSQL Docker service container on port 31337
- `isql` exits 0 even when SQL statements fail — CI greps output for `[ISQL]ERROR`
- macOS links against iODBC; Linux links against unixODBC
